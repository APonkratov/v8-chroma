// Own tracee: a delayed libc-style call holds a mutex across the soft deadline.
#include "trace.h"
#include <sys/mman.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <dlfcn.h>
#include <atomic>
#include <immintrin.h>
#include <new>

using linux_trace::require;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t delivered,deliveredUser;
static void signalHandler(int number){if(number==SIGUSR1)++deliveredUser;else ++delivered;}
extern "C" __attribute__((noinline)) unsigned long long slowCall(){
    pthread_mutex_lock(&mutex);
    raise(SIGTRAP); // Must not be confused with the return breakpoint.
    usleep(150000);
    asm volatile("pxor %%xmm0,%%xmm0; pxor %%xmm1,%%xmm1; pxor %%xmm2,%%xmm2; pxor %%xmm3,%%xmm3;"
                 "pxor %%xmm4,%%xmm4; pxor %%xmm5,%%xmm5; pxor %%xmm6,%%xmm6; pxor %%xmm7,%%xmm7;"
                 "pxor %%xmm8,%%xmm8; pxor %%xmm9,%%xmm9; pxor %%xmm10,%%xmm10; pxor %%xmm11,%%xmm11;"
                 "pxor %%xmm12,%%xmm12; pxor %%xmm13,%%xmm13; pxor %%xmm14,%%xmm14; pxor %%xmm15,%%xmm15;"
                 :::"xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15");
    pthread_mutex_unlock(&mutex);return 42;
}
struct Shared{std::atomic<int> ready{0},stop{0};};
int main(){
    pid_t child=-1;
    try{
        auto memory=mmap(nullptr,sizeof(Shared),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
        require(memory!=MAP_FAILED,"fixture mmap");auto shared=new(memory)Shared;
        child=fork();require(child>=0,"fork");
        if(!child){
            signal(SIGTRAP,signalHandler);
            signal(SIGUSR1,signalHandler);
            __m128d values=_mm_set_pd(2.0,4.0);shared->ready=1;
            while(!shared->stop){
                asm volatile("" : "+x"(values));
                alignas(16) double check[2];_mm_store_pd(check,values);
                if(check[0]!=4||check[1]!=2)_exit(3);
            }
            if(pthread_mutex_trylock(&mutex))_exit(4);
            pthread_mutex_unlock(&mutex);_exit(delivered==1&&deliveredUser==1?0:5);
        }
        while(!shared->ready)usleep(1000);
        auto code=static_cast<const unsigned char*>(dlsym(RTLD_DEFAULT,"getpid"));
        unsigned offset=0;for(;offset<32&&!(code[offset]==0x0f&&code[offset+1]==0x05);++offset){}
        require(offset<32,"syscall gadget");auto gadget=reinterpret_cast<uintptr_t>(code+offset);
        linux_trace::Trace trace(child,std::chrono::milliseconds(25));
        // Delivery must not replace the pending syscall single-step with CONT.
        require(kill(child,SIGUSR1)==0,"queue signal before remote syscall");
        auto syscall=[&](unsigned long long number,std::array<unsigned long long,6> args){
            auto result=trace.execute(gadget,number,0,args,true);
            require(result<static_cast<unsigned long long>(-4095),"remote syscall");return result;
        };
        auto region=syscall(SYS_mmap,{0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,static_cast<unsigned long long>(-1),0});
        auto stack=syscall(SYS_mmap,{0,1048576,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,static_cast<unsigned long long>(-1),0});
        const unsigned char call[]={0xff,0xd0,0xcc};trace.put(region,call,sizeof(call));
        syscall(SYS_mprotect,{region,4096,PROT_READ|PROT_EXEC,0,0,0});
        auto began=linux_trace::Clock::now();
        auto result=trace.execute(region,reinterpret_cast<uintptr_t>(&slowCall),stack+1048560,{},false);
        require(result==42&&linux_trace::Clock::now()-began>=std::chrono::milliseconds(150),"completion barrier");
        syscall(SYS_munmap,{stack,1048576,0,0,0,0});syscall(SYS_munmap,{region,4096,0,0,0,0});
        trace.detach();shared->stop=1;
        int status=0;auto deadline=linux_trace::Clock::now()+std::chrono::seconds(3);pid_t ended=0;
        while(!(ended=waitpid(child,&status,WNOHANG))&&linux_trace::Clock::now()<deadline)usleep(1000);
        require(ended==child,"tracee resumed");child=-1;
        require(WIFEXITED(status)&&WEXITSTATUS(status)==0,"mutex, SIMD and signal restored");
        munmap(memory,sizeof(Shared));
        std::cout<<"Linux trace: pending syscall signal, delayed call completion, unrelated SIGTRAP, mutex, SIMD and resumed tracee: PASS\n";return 0;
    }catch(const std::exception& error){
        if(child>0){kill(child,SIGKILL);waitpid(child,nullptr,0);}
        std::cerr<<error.what()<<'\n';return 1;
    }
}
