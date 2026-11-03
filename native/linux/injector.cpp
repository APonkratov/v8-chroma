// Same-user x86-64 loader. Never modifies existing executable instructions.
// A short call creates a detached pthread; dlopen runs after the UI thread is
// restored, so a loader lock held by that thread cannot deadlock the injection.
#include "trace.h"
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <elf.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <limits.h>

extern "C" const unsigned char v8_remote_begin[], v8_remote_end[];
using Clock = std::chrono::steady_clock;
using linux_trace::require;
using linux_trace::Trace;
struct Blob {
    unsigned long long open, lookup, result, done, thread, handle, reserved[2];
    char library[4096], config[4096], symbol[64];
};
static_assert(offsetof(Blob, library) == 64 && offsetof(Blob, symbol) == 8256);
static unsigned long long started(pid_t pid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string line; std::getline(input, line);
    auto end = line.rfind(')');
    if (end == std::string::npos) throw std::runtime_error("Процесс завершён");
    std::istringstream fields(line.substr(end + 1));
    std::string value;
    for (unsigned i = 0; i <= 19; ++i) fields >> value;
    return std::stoull(value);
}
static void eligible(pid_t pid, unsigned long long expected) {
    auto root = "/proc/" + std::to_string(pid);
    struct stat meta{};
    require(!stat(root.c_str(), &meta), "Процесс недоступен");
    if (meta.st_uid != getuid() || getuid() != geteuid()) throw std::runtime_error("Разрешены только процессы текущего пользователя");
    std::ifstream command(root + "/cmdline", std::ios::binary);
    std::string exe, mode; std::getline(command, exe, '\0'); std::getline(command, mode, '\0');
    for (auto& c : mode) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    char path[PATH_MAX]{};
    ssize_t size = readlink((root + "/exe").c_str(), path, sizeof(path) - 1);
    require(size > 0, "EXE недоступен");
    std::string actual(path, static_cast<size_t>(size));
    if (actual.substr(actual.find_last_of('/') + 1) != "1cv8" || mode != "DESIGNER" || started(pid) != expected)
        throw std::runtime_error("Идентичность Конфигуратора изменилась");
}
static unsigned long long relocated(pid_t pid, const char* name) {
    void* symbol = dlsym(RTLD_DEFAULT, name);
    Dl_info info{};
    require(symbol && dladdr(symbol, &info), "Не найден системный API загрузчика");
    struct stat local{};
    require(!stat(info.dli_fname, &local), "Системная библиотека недоступна");
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream columns(line);
        std::string range, access, offset, device; unsigned long long inode;
        columns >> range >> access >> offset >> device >> inode;
        unsigned majorNumber = 0, minorNumber = 0;
        if (sscanf(device.c_str(), "%x:%x", &majorNumber, &minorNumber) != 2) continue;
        if (inode != local.st_ino || makedev(majorNumber, minorNumber) != local.st_dev || std::stoull(offset, nullptr, 16)) continue;
        auto base = std::stoull(range.substr(0, range.find('-')), nullptr, 16);
        return base + reinterpret_cast<uintptr_t>(symbol) - reinterpret_cast<uintptr_t>(info.dli_fbase);
    }
    throw std::runtime_error("Системная библиотека процесса отличается от установленной; перезапустите этот Конфигуратор");
}
int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::runtime_error("Ожидались PID, starttime, библиотека и конфигурация");
        pid_t pid = static_cast<pid_t>(std::stoul(argv[1]));
        auto expected = std::stoull(argv[2]);
        eligible(pid, expected);
        Blob blob{};
        if (strlen(argv[3]) >= sizeof(blob.library) || strlen(argv[4]) >= sizeof(blob.config)) throw std::runtime_error("Слишком длинный путь");
        strcpy(blob.library, argv[3]); strcpy(blob.config, argv[4]); strcpy(blob.symbol, "v8_chroma_start");
        blob.open = relocated(pid, "dlopen"); blob.lookup = relocated(pid, "dlsym");
        auto create = relocated(pid, "pthread_create"), detach = relocated(pid, "pthread_detach");
        auto gadget = relocated(pid, "getpid");
        auto* local = static_cast<const unsigned char*>(dlsym(RTLD_DEFAULT, "getpid"));
        unsigned pos = 0;
        for (; pos < 32 && !(local[pos] == 0x0f && local[pos + 1] == 0x05); ++pos) {}
        if (pos == 32) throw std::runtime_error("Не найден системный syscall trampoline");
        gadget += pos;
        Trace trace(pid); eligible(pid, expected);
        int memory = open(("/proc/" + std::to_string(pid) + "/mem").c_str(), O_RDONLY | O_CLOEXEC);
        require(memory >= 0, "Чтение ответа загрузчика");
        struct Close { int fd; ~Close() { close(fd); } } closeMemory{memory};
        auto syscall = [&](unsigned long long number, std::array<unsigned long long, 6> args) {
            auto value = trace.execute(gadget, number, 0, args, true);
            if (value >= static_cast<unsigned long long>(-4095)) { errno = static_cast<int>(-value); require(false, "Выделение памяти загрузчика"); }
            return value;
        };
        auto region = syscall(SYS_mmap, {0, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, static_cast<unsigned long long>(-1), 0});
        auto stack = syscall(SYS_mmap, {0, 1048576, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, static_cast<unsigned long long>(-1), 0});
        size_t payloadSize = static_cast<size_t>(v8_remote_end - v8_remote_begin);
        if (payloadSize > 512) throw std::runtime_error("Некорректный размер trampoline");
        trace.put(region, v8_remote_begin, payloadSize);
        const unsigned char call[] = {0xff, 0xd0, 0xcc};
        trace.put(region + 512, call, sizeof(call));
        trace.put(region + 4096, &blob, sizeof(blob));
        syscall(SYS_mprotect, {region, 4096, PROT_READ | PROT_EXEC, 0, 0, 0});
        auto result = trace.execute(region + 512, create, stack + 1048560,
            {region + 4096 + offsetof(Blob, thread), 0, region, region + 4096, 0, 0}, false);
        if (result) throw std::runtime_error("Не удалось создать поток загрузчика");
        require(pread(memory, &blob, sizeof(blob), static_cast<off_t>(region + 4096)) == sizeof(blob), "Чтение pthread identity");
        result = trace.execute(region + 512, detach, stack + 1048560, {blob.thread, 0, 0, 0, 0, 0}, false);
        if (result) throw std::runtime_error("Не удалось отделить поток загрузчика");
        syscall(SYS_munmap, {stack, 1048576, 0, 0, 0, 0});
        trace.detach();
        auto deadline = Clock::now() + std::chrono::seconds(10);
        do {
            require(pread(memory, &blob, sizeof(blob), static_cast<off_t>(region + 4096)) == sizeof(blob), "Чтение результата загрузчика");
            if (blob.done) {
                if (blob.result) throw std::runtime_error("Движок не поддержал отрисовку этого процесса, код " + std::to_string(blob.result));
                return 0;
            }
            usleep(10000);
        } while (Clock::now() < deadline);
        throw std::runtime_error("Движок не подтвердил загрузку; Конфигуратор продолжает работу");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
