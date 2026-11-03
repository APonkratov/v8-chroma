#pragma once
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <elf.h>
#include <unistd.h>
#include <signal.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace linux_trace {
using Clock=std::chrono::steady_clock;
inline void require(bool ok,const char* operation){if(!ok)throw std::runtime_error(std::string(operation)+": "+strerror(errno));}
class Trace {
    pid_t pid;
    bool attached=false,stopped=false,saved=false,callRunning=false,singleStep=false;
    uintptr_t completion=0;
    user_regs_struct original{};
    std::vector<unsigned char> xstate=std::vector<unsigned char>(65536);
    size_t xstateSize=0;
    int deferredSignal=0;
    std::chrono::milliseconds timeout;
    sigset_t previousMask{};
    bool masked=false;
    void trap(){
        auto deadline=Clock::now()+timeout;
        bool warned=false;
        for(;;){
            int status=0;pid_t result=waitpid(pid,&status,__WALL|WNOHANG);
            if(result<0&&errno==EINTR)continue;
            if(result<0&&errno==ECHILD){attached=false;callRunning=false;stopped=false;}
            require(result>=0,"Ожидание процесса");
            if(result){
                if(!WIFSTOPPED(status)){attached=false;callRunning=false;stopped=false;throw std::runtime_error("Конфигуратор завершился при подключении");}
                stopped=true;
                int signal=WSTOPSIG(status);
                bool interrupt=signal==SIGTRAP&&(status>>16)==PTRACE_EVENT_STOP;
                if(signal==SIGTRAP){
                    if(!callRunning)return;
                    user_regs_struct regs{};
                    require(ptrace(PTRACE_GETREGS,pid,nullptr,&regs)!=-1,"Проверка завершения вызова");
                    if(!interrupt&&regs.rip==completion){callRunning=false;return;}
                }
                if(!callRunning){
                    deferredSignal=signal;
                    if(signal==SIGSEGV||signal==SIGILL||signal==SIGBUS)throw std::runtime_error("Системный вызов загрузчика завершился ошибкой");
                }
                // A remote libc call cannot be rolled back. Keep tracing it,
                // including after an interrupt-stop or a handled signal.
                int deliver=callRunning&&!interrupt?signal:0;
                require(ptrace(singleStep?PTRACE_SINGLESTEP:PTRACE_CONT,pid,nullptr,reinterpret_cast<void*>(static_cast<intptr_t>(deliver)))!=-1,"Продолжение процесса");
                stopped=false;
            }
            if(Clock::now()>=deadline){
                if(!callRunning)throw std::runtime_error("Превышено время подключения к процессу");
                if(!warned){std::cerr<<"Системный вызов загрузчика ещё выполняется; ожидаю безопасного возврата\n";warned=true;}
            }
            usleep(1000);
        }
    }
public:
    explicit Trace(pid_t id,std::chrono::milliseconds limit=std::chrono::seconds(5)):pid(id),timeout(limit){
        // Defer ordinary cancellation of the helper until the target context
        // is safe. SIGKILL cannot be handled by a userspace ptrace client.
        sigset_t signals;sigemptyset(&signals);for(int s:{SIGINT,SIGTERM,SIGHUP})sigaddset(&signals,s);
        require(!sigprocmask(SIG_BLOCK,&signals,&previousMask),"Защита восстановления потока");masked=true;
        try{
            require(ptrace(PTRACE_SEIZE,pid,nullptr,nullptr)!=-1,"Linux запретил подключение к процессу (ptrace)");attached=true;
            require(ptrace(PTRACE_INTERRUPT,pid,nullptr,nullptr)!=-1,"Остановка потока");trap();
            require(ptrace(PTRACE_GETREGS,pid,nullptr,&original)!=-1,"Чтение регистров");
            iovec area{xstate.data(),xstate.size()};
            require(ptrace(PTRACE_GETREGSET,pid,reinterpret_cast<void*>(NT_X86_XSTATE),&area)!=-1,"Чтение SIMD-регистров");
            xstateSize=area.iov_len;saved=true;
        }catch(...){detach();throw;}
    }
    void detach()noexcept{
        singleStep=false;
        // Error cleanup has the same completion barrier as the normal path.
        // Never restore while libc owns the temporary stack or internal locks.
        while(attached&&callRunning){
            if(stopped){
                if(ptrace(PTRACE_CONT,pid,nullptr,nullptr)==-1){if(errno==ESRCH){attached=false;break;}usleep(1000);continue;}
                stopped=false;
            }
            try{trap();}catch(...){if(attached&&callRunning)usleep(1000);}
        }
        if(attached){
            if(!stopped&&ptrace(PTRACE_INTERRUPT,pid,nullptr,nullptr)!=-1){try{trap();}catch(...){}}
            if(stopped&&saved){
                iovec area{xstate.data(),xstateSize};
                ptrace(PTRACE_SETREGSET,pid,reinterpret_cast<void*>(NT_X86_XSTATE),&area);
                ptrace(PTRACE_SETREGS,pid,nullptr,&original);
            }
            ptrace(PTRACE_DETACH,pid,nullptr,reinterpret_cast<void*>(static_cast<intptr_t>(deferredSignal)));attached=false;
        }
        if(masked){masked=false;sigprocmask(SIG_SETMASK,&previousMask,nullptr);}
    }
    ~Trace(){detach();}
    void put(uintptr_t address,const void* bytes,size_t size){
        auto source=static_cast<const unsigned char*>(bytes);
        while(size){
            size_t count=std::min(size,sizeof(long));long value=0;
            if(count<sizeof(long)){errno=0;value=ptrace(PTRACE_PEEKDATA,pid,reinterpret_cast<void*>(address),nullptr);require(!errno,"Чтение памяти");}
            memcpy(&value,source,count);require(ptrace(PTRACE_POKEDATA,pid,reinterpret_cast<void*>(address),reinterpret_cast<void*>(value))!=-1,"Запись памяти");
            address+=count;source+=count;size-=count;
        }
    }
    unsigned long long execute(uintptr_t instruction,unsigned long long function,uintptr_t stack,const std::array<unsigned long long,6>& args,bool syscall){
        auto regs=original;
        regs.rip=instruction;regs.rax=function;regs.orig_rax=static_cast<unsigned long long>(-1);
        regs.rdi=args[0];regs.rsi=args[1];regs.rdx=args[2];regs.r10=args[3];regs.rcx=args[3];regs.r8=args[4];regs.r9=args[5];
        if(stack)regs.rsp=stack&~15ULL;
        require(ptrace(PTRACE_SETREGS,pid,nullptr,&regs)!=-1,"Подготовка вызова");
        singleStep=syscall;
        require(ptrace(singleStep?PTRACE_SINGLESTEP:PTRACE_CONT,pid,nullptr,nullptr)!=-1,"Вызов загрузчика");
        stopped=false;callRunning=!syscall;completion=instruction+3;
        trap();singleStep=false;require(ptrace(PTRACE_GETREGS,pid,nullptr,&regs)!=-1,"Результат вызова");return regs.rax;
    }
};
}
