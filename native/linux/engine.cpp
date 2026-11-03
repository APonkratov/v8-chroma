// glibc auditing scopes bindings to vendor Cairo or the platform's own lookup
// modules. GTK bindings stay native; each Cairo has its own original pointers.
#include "protocol.h"
#include "icons.h"
#include "fonts.h"
#include <link.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <climits>
#include <initializer_list>
#include <dlfcn.h>
#include <algorithm>
#include <string>

namespace {
bool enabled;
dev_t cairoDevice;
ino_t cairoInode;
dev_t binderDevice[2]{};
ino_t binderInode[2]{};
char configPath[PATH_MAX], statusPath[PATH_MAX], pendingPath[PATH_MAX];
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
ProbeConfig config{};
bool valid;
unsigned generation, hookMask;
unsigned long long observed, changed;
long long checkedAt, publishedAt;
struct stat previous{};
const char* problem = "not-loaded";
uintptr_t rgbAddress[2]{}, rgbaAddress[2]{}, stopRgbAddress[2]{}, stopAddress[2]{}, meshRgbAddress[2]{}, meshAddress[2]{};
uintptr_t sourceAddress[2]{}, surfaceAddress[2]{}, maskAddress[2]{}, maskSurfaceAddress[2]{};
bool iconsAvailable;

long long now() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec * 1000000000LL + value.tv_nsec;
}
bool readExact(int fd, void* data, size_t size) {
    auto* out = static_cast<unsigned char*>(data);
    while (size) {
        ssize_t count = read(fd, out, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        out += count; size -= static_cast<size_t>(count);
    }
    return true;
}
bool acceptable(const ProbeConfig& value) {
    if (value.size != sizeof(value) || value.version != 3 || value.mode > 1 ||
        value.mapCount > 256 || value.iconMode > 2 || value.iconPack > 1 ||
        (value.iconMode && value.iconPack != 1) || !value.themeId[0] ||
        !memchr(value.themeId, 0, sizeof(value.themeId))) return false;
    for (const char* p = value.themeId; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
    for (auto color : value.colors) if (color > 0xFFFFFF) return false;
    for (unsigned i = 0; i < value.mapCount; ++i) {
        const auto& entry = value.maps[i];
        if (entry.kind < 1 || entry.kind > 8 || entry.source > 0xFFFFFF || entry.target > 0xFFFFFF) return false;
    }
    return true;
}
std::string jsonText(const char* text) {
    std::string result;
    for(auto p=reinterpret_cast<const unsigned char*>(text);*p;++p) {
        if(*p=='"'||*p=='\\'){result+='\\';result+=static_cast<char>(*p);}
        else if(*p<32){char encoded[7];snprintf(encoded,sizeof(encoded),"\\u%04x",*p);result+=encoded;}
        else result+=static_cast<char>(*p);
    }
    return result;
}
void publish(long long time) {
    char data[3072];
    auto icons=linux_icons::stats();
    auto fonts=linux_fonts::stats();
    int length = snprintf(data, sizeof(data),
        "{\"schema\":1,\"pid\":%ld,\"theme\":\"%s\",\"generation\":%u,"
        "\"hook_mask\":%u,\"observed_calls\":%llu,\"mapped_calls\":%llu,\"error\":\"%s\","
        "\"icon_mode\":%u,\"icons_available\":%s,\"icon_calls\":%llu,\"icon_matches\":%llu,"
        "\"fonts_available\":%u,\"font_generation\":%u,\"font_refresh\":%u,\"font_error\":\"%s\","
        "\"editor_enabled\":%s,\"ui_enabled\":%s,\"ui_fallback\":%s,\"editor_points10\":%u,\"editor_family\":\"%s\",\"ui_family\":\"%s\","
        "\"editor_calls\":%llu,\"ui_calls\":%llu,\"font_conversions\":%llu,\"font_last_source\":\"%s\",\"ui_last_source\":\"%s\"}\n",
        static_cast<long>(getpid()), valid ? config.themeId : "standard", generation,
        __atomic_load_n(&hookMask, __ATOMIC_RELAXED), observed, changed, problem, icons.mode,
        iconsAvailable?"true":"false", static_cast<unsigned long long>(icons.calls),static_cast<unsigned long long>(icons.matches),
        fonts.available,fonts.generation,fonts.refresh,fonts.error,fonts.editorEnabled?"true":"false",fonts.uiEnabled?"true":"false",fonts.uiFallback?"true":"false",fonts.points10,
        jsonText(fonts.editor).c_str(),jsonText(fonts.ui).c_str(),static_cast<unsigned long long>(fonts.editorCalls),static_cast<unsigned long long>(fonts.uiCalls),
        static_cast<unsigned long long>(fonts.conversions),jsonText(fonts.source).c_str(),jsonText(fonts.uiSource).c_str());
    if(length<0||static_cast<size_t>(length)>=sizeof(data))return;
    int fd = open(pendingPath, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        const char* cursor = data;
        int remaining = length;
        while (remaining > 0) {
            ssize_t count = write(fd, cursor, static_cast<size_t>(remaining));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            cursor += count; remaining -= static_cast<int>(count);
        }
        close(fd);
        if (!remaining) { int result = rename(pendingPath, statusPath); (void)result; }
    }
    publishedAt = time;
}
void reload(long long time) {
    if (time - checkedAt < 100000000LL) return;
    checkedAt = time;
    int fd = open(configPath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat metadata{};
    ProbeConfig candidate{};
    bool ok = fd >= 0 && !fstat(fd, &metadata) && S_ISREG(metadata.st_mode) &&
        metadata.st_uid == getuid() && !(metadata.st_mode & 0022) && metadata.st_size == sizeof(candidate);
    bool same = ok && previous.st_ino == metadata.st_ino && previous.st_dev == metadata.st_dev &&
        previous.st_mtim.tv_sec == metadata.st_mtim.tv_sec && previous.st_mtim.tv_nsec == metadata.st_mtim.tv_nsec;
    if (same) { close(fd); return; }
    ok = ok && readExact(fd, &candidate, sizeof(candidate)) && acceptable(candidate);
    if (fd >= 0) close(fd);
    if (ok) {
        if (!valid || memcmp(&candidate, &config, sizeof(config))) ++generation;
        config = candidate; previous = metadata; problem = "";
    } else {
        previous = {}; problem = "invalid-config";
    }
    valid = ok;
    linux_icons::configure(ok ? candidate.iconMode : 0);
    publish(time);
}
unsigned channel(double n) { return static_cast<unsigned>(fmax(0., fmin(1., n)) * 255. + .5); }
void color(unsigned kind, double& r, double& g, double& b) {
    int saved = errno;
    pthread_mutex_lock(&lock);
    auto time = now(); reload(time);
    ++observed;
    // Preserve unknown/diagnostic colors bit-for-bit, including out-of-range
    // values; only an exact, finite palette match is converted to 8-bit RGB.
    if (valid && config.mode && std::isfinite(r) && std::isfinite(g) && std::isfinite(b)) {
        unsigned source = (channel(r) << 16) | (channel(g) << 8) | channel(b);
        for (unsigned i = 0; i < config.mapCount; ++i) {
            const auto& entry = config.maps[i];
            if (entry.kind != kind || entry.source != source) continue;
            if (entry.target != source) {
                r = ((entry.target >> 16) & 255) / 255.;
                g = ((entry.target >> 8) & 255) / 255.;
                b = (entry.target & 255) / 255.;
                ++changed;
            }
            break;
        }
    }
    if (time - publishedAt > 1000000000LL) publish(time);
    pthread_mutex_unlock(&lock);
    errno = saved;
}
using Rgb = void (*)(void*, double, double, double);
using Rgba = void (*)(void*, double, double, double, double);
using StopRgb = void (*)(void*, double, double, double, double);
using Stop = void (*)(void*, double, double, double, double, double);
using MeshRgb = void (*)(void*, unsigned, double, double, double);
using Mesh = void (*)(void*, unsigned, double, double, double, double);
template<unsigned Bank> void source(void* context,void* pattern) {
    linux_icons::source(Bank,context,pattern,reinterpret_cast<void(*)(void*,void*)>(sourceAddress[Bank]));
}
template<unsigned Bank> void mask(void* context,void* pattern) {
    linux_icons::source(Bank,context,pattern,reinterpret_cast<void(*)(void*,void*)>(maskAddress[Bank]));
}
template<unsigned Bank> void surface(void* context,void* image,double x,double y) {
    linux_icons::surface(Bank,context,image,x,y,reinterpret_cast<void(*)(void*,void*,double,double)>(surfaceAddress[Bank]));
}
template<unsigned Bank> void maskSurface(void* context,void* image,double x,double y) {
    linux_icons::surface(Bank,context,image,x,y,reinterpret_cast<void(*)(void*,void*,double,double)>(maskSurfaceAddress[Bank]));
}
template<unsigned Bank> void rgb(void* context, double r, double g, double b) {
    color(5, r, g, b);
    reinterpret_cast<Rgb>(__atomic_load_n(&rgbAddress[Bank], __ATOMIC_RELAXED))(context, r, g, b);
}
template<unsigned Bank> void rgba(void* context, double r, double g, double b, double a) {
    color(6, r, g, b);
    reinterpret_cast<Rgba>(__atomic_load_n(&rgbaAddress[Bank], __ATOMIC_RELAXED))(context, r, g, b, a);
}
template<unsigned Bank> void stopRgb(void* pattern, double offset, double r, double g, double b) {
    color(7, r, g, b);
    reinterpret_cast<StopRgb>(__atomic_load_n(&stopRgbAddress[Bank], __ATOMIC_RELAXED))(pattern, offset, r, g, b);
}
template<unsigned Bank> void stop(void* pattern, double offset, double r, double g, double b, double a) {
    color(7, r, g, b);
    reinterpret_cast<Stop>(__atomic_load_n(&stopAddress[Bank], __ATOMIC_RELAXED))(pattern, offset, r, g, b, a);
}
template<unsigned Bank> void meshRgb(void* pattern, unsigned corner, double r, double g, double b) {
    color(8, r, g, b);
    reinterpret_cast<MeshRgb>(__atomic_load_n(&meshRgbAddress[Bank], __ATOMIC_RELAXED))(pattern, corner, r, g, b);
}
template<unsigned Bank> void mesh(void* pattern, unsigned corner, double r, double g, double b, double a) {
    color(8, r, g, b);
    reinterpret_cast<Mesh>(__atomic_load_n(&meshAddress[Bank], __ATOMIC_RELAXED))(pattern, corner, r, g, b, a);
}
}

#define AUDIT_EXPORT extern "C" __attribute__((visibility("default")))
AUDIT_EXPORT unsigned int la_version(unsigned int) {
    // Child processes (including WebKit) inherit environment, not eligibility.
    char args[4096]{};
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    ssize_t count = fd >= 0 ? read(fd, args, sizeof(args) - 1) : 0;
    if (fd >= 0) close(fd);
    const char* path = getenv("V8_CHROMA_CONFIG");
    const char* cairo = getenv("V8_CHROMA_CAIRO");
    struct stat identity{};
    char* mode = count > 0 ? static_cast<char*>(memchr(args, 0, static_cast<size_t>(count))) : nullptr;
    const char* name = strrchr(args, '/'); name = name ? name + 1 : args;
    enabled = mode && mode + 1 < args + count && !strcmp(name, "1cv8") &&
        !strcasecmp(mode + 1, "DESIGNER") && getuid() == geteuid() && path && path[0] == '/' &&
        strlen(path) + 16 < sizeof(configPath) && cairo && !stat(cairo, &identity);
    if (enabled) {
        strcpy(configPath, path);
        snprintf(statusPath, sizeof(statusPath), "%s.status", path);
        snprintf(pendingPath, sizeof(pendingPath), "%s.status.pending", path);
        cairoDevice = identity.st_dev; cairoInode = identity.st_ino;
        // 1C can resolve Cairo from the global scope: on Ubuntu these lookups
        // return system Cairo even though libcairo-v8 is also loaded. Scope
        // that bank to the platform's own loader modules, never GTK bindings.
        const char* slash = strrchr(cairo, '/');
        if (slash && slash - cairo < PATH_MAX - 32) {
            unsigned index = 0;
            for (auto module : {"core83.so", "grphcs.so"}) {
                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%.*s/%s", static_cast<int>(slash - cairo), cairo, module);
                struct stat binder{};
                if (!stat(path, &binder)) { binderDevice[index] = binder.st_dev; binderInode[index] = binder.st_ino; }
                ++index;
            }
        }
    }
    return LAV_CURRENT;
}
AUDIT_EXPORT unsigned int la_objopen(struct link_map* map, Lmid_t, uintptr_t* cookie) {
    struct stat identity{};
    *cookie = enabled && !stat(map->l_name, &identity) && identity.st_dev == cairoDevice && identity.st_ino == cairoInode;
    if (enabled) {
        for (unsigned i = 0; i < 2; ++i)
            if (binderInode[i] && identity.st_dev == binderDevice[i] && identity.st_ino == binderInode[i]) *cookie |= 2;
        const char* base = strrchr(map->l_name, '/'); base = base ? base + 1 : map->l_name;
        if (!strncmp(base, "libcairo.so.", 12)) *cookie |= 4;
    }
    return enabled ? LA_FLG_BINDTO | LA_FLG_BINDFROM : 0;
}
AUDIT_EXPORT void la_preinit(uintptr_t*) {
    if (!enabled) return;
    pthread_mutex_lock(&lock); reload(now()); pthread_mutex_unlock(&lock);
}
AUDIT_EXPORT uintptr_t la_symbind64(Elf64_Sym* symbol, unsigned int, uintptr_t* reference, uintptr_t* definition,
                                  unsigned int*, const char* name) {
    if (!enabled || !((*definition & 1) || ((*definition & 4) && (*reference & 2)))) return symbol->st_value;
    unsigned bank = (*definition & 1) ? 0 : 1;
    struct Hook { const char* name; uintptr_t* original; uintptr_t replacement[2]; unsigned mask; };
    const Hook hooks[] = {
        {"cairo_set_source_rgb", rgbAddress, {reinterpret_cast<uintptr_t>(&rgb<0>), reinterpret_cast<uintptr_t>(&rgb<1>)}, 1},
        {"cairo_set_source_rgba", rgbaAddress, {reinterpret_cast<uintptr_t>(&rgba<0>), reinterpret_cast<uintptr_t>(&rgba<1>)}, 2},
        {"cairo_pattern_add_color_stop_rgb", stopRgbAddress, {reinterpret_cast<uintptr_t>(&stopRgb<0>), reinterpret_cast<uintptr_t>(&stopRgb<1>)}, 4},
        {"cairo_pattern_add_color_stop_rgba", stopAddress, {reinterpret_cast<uintptr_t>(&stop<0>), reinterpret_cast<uintptr_t>(&stop<1>)}, 8},
        {"cairo_mesh_pattern_set_corner_color_rgb", meshRgbAddress, {reinterpret_cast<uintptr_t>(&meshRgb<0>), reinterpret_cast<uintptr_t>(&meshRgb<1>)}, 16},
        {"cairo_mesh_pattern_set_corner_color_rgba", meshAddress, {reinterpret_cast<uintptr_t>(&mesh<0>), reinterpret_cast<uintptr_t>(&mesh<1>)}, 32},
        {"cairo_set_source", sourceAddress, {reinterpret_cast<uintptr_t>(&source<0>), reinterpret_cast<uintptr_t>(&source<1>)}, 64},
        {"cairo_set_source_surface", surfaceAddress, {reinterpret_cast<uintptr_t>(&surface<0>), reinterpret_cast<uintptr_t>(&surface<1>)}, 128},
        {"cairo_mask", maskAddress, {reinterpret_cast<uintptr_t>(&mask<0>), reinterpret_cast<uintptr_t>(&mask<1>)}, 256},
        {"cairo_mask_surface", maskSurfaceAddress, {reinterpret_cast<uintptr_t>(&maskSurface<0>), reinterpret_cast<uintptr_t>(&maskSurface<1>)}, 512},
    };
    for (const auto& hook : hooks) if (!strcmp(name, hook.name)) {
        __atomic_store_n(&hook.original[bank], symbol->st_value, __ATOMIC_RELAXED);
        __atomic_fetch_or(&hookMask, hook.mask, __ATOMIC_RELAXED);
        return hook.replacement[bank];
    }
    return symbol->st_value;
}

namespace {
bool liveStarted, repaintPending, fontRefreshPending;
struct List { void* data; List* next; List* previous; };
struct Gtk {
    void* handle;
    unsigned (*idle)(int (*)(void*), void*);
    List* (*windows)();
    void (*draw)(void*);
    void (*freeList)(void*);
} gtk{};
int repaint(void*) {
    if(__atomic_exchange_n(&fontRefreshPending,false,__ATOMIC_ACQ_REL))linux_fonts::refresh();
    auto* windows = gtk.windows();
    for (auto* item = windows; item; item = item->next) gtk.draw(item->data);
    gtk.freeList(windows);
    __atomic_store_n(&repaintPending, false, __ATOMIC_RELEASE);
    return 0;
}
void* refreshWorker(void*) {
    unsigned previousGeneration = 0;
    bool previousValid = false;
    for (;;) {
        bool fontChange=linux_fonts::reload();
        if(fontChange)__atomic_store_n(&fontRefreshPending,true,__ATOMIC_RELEASE);
        pthread_mutex_lock(&lock);
        reload(now());
        bool change = fontChange || generation != previousGeneration || valid != previousValid;
        previousGeneration = generation; previousValid = valid;
        if (now() - publishedAt > 1000000000LL) publish(now());
        pthread_mutex_unlock(&lock);
        if (change && gtk.idle && !__atomic_exchange_n(&repaintPending, true, __ATOMIC_ACQ_REL)) {
            if (!gtk.idle(repaint, nullptr)) __atomic_store_n(&repaintPending, false, __ATOMIC_RELEASE);
        }
        usleep(100000);
    }
}
struct Writable { uintptr_t begin, end; };
struct Symbol { uintptr_t original, replacement; unsigned mask, bank; };
struct Patch { uintptr_t* address; Symbol symbol; };
struct Scan {
    dev_t device; ino_t inode;
    Symbol symbols[20]{}; unsigned symbolCount = 0;
    Writable writable[4096]{}; unsigned writableCount = 0;
    Patch patches[64]{}; unsigned patchCount = 0;
    bool overflow = false;
};
bool writable(const Scan& scan, uintptr_t address) {
    for (unsigned i = 0; i < scan.writableCount; ++i)
        if (address >= scan.writable[i].begin && address + sizeof(uintptr_t) <= scan.writable[i].end) return true;
    return false;
}
int scanModule(dl_phdr_info* module, size_t, void* context) {
    auto& scan = *static_cast<Scan*>(context);
    struct stat identity{};
    if (stat(module->dlpi_name, &identity) || identity.st_dev != scan.device || identity.st_ino != scan.inode) return 0;
    for (unsigned i = 0; i < module->dlpi_phnum; ++i) {
        const auto& segment = module->dlpi_phdr[i];
        if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_W)) continue;
        if (segment.p_memsz > 8 * 1024 * 1024) { scan.overflow = true; return 1; }
        uintptr_t begin = (module->dlpi_addr + segment.p_vaddr + 7) & ~uintptr_t(7);
        uintptr_t end = module->dlpi_addr + segment.p_vaddr + segment.p_memsz;
        for (uintptr_t address = begin; address + sizeof(uintptr_t) <= end; address += sizeof(uintptr_t)) {
            if (!writable(scan, address)) continue;
            auto value = __atomic_load_n(reinterpret_cast<uintptr_t*>(address), __ATOMIC_ACQUIRE);
            for (unsigned s = 0; s < scan.symbolCount; ++s) {
                if (value != scan.symbols[s].original) continue;
                if (scan.patchCount == 64) { scan.overflow = true; return 1; }
                scan.patches[scan.patchCount++] = {reinterpret_cast<uintptr_t*>(address), scan.symbols[s]};
                break;
            }
        }
    }
    return 1;
}
bool coherent(const Scan& scan) {
    if (scan.overflow) return false;
    // Require the RGB/RGBA/gradient entries of one compact dispatch table.
    // Matching an isolated pointer is never sufficient to modify a module.
    for (unsigned i = 0; i < scan.patchCount; ++i) {
        const auto& first = scan.patches[i];
        if (first.symbol.mask != 1) continue;
        unsigned mask = 0;
        for (unsigned j = 0; j < scan.patchCount; ++j) {
            const auto& next = scan.patches[j];
            auto a = reinterpret_cast<uintptr_t>(first.address), b = reinterpret_cast<uintptr_t>(next.address);
            if (next.symbol.bank == first.symbol.bank && (a > b ? a - b : b - a) < 1024) mask |= next.symbol.mask;
        }
        if ((mask & 11) == 11) return true;
    }
    return false;
}
void liveStatus(unsigned mask, const char* error) {
    pthread_mutex_lock(&lock);
    __atomic_store_n(&hookMask, mask, __ATOMIC_RELEASE);
    problem = error; publish(now());
    pthread_mutex_unlock(&lock);
}
}

AUDIT_EXPORT unsigned v8_chroma_start(const char* path) {
    if (__atomic_exchange_n(&liveStarted, true, __ATOMIC_ACQ_REL)) return 114;
    char command[4096]{}, executable[PATH_MAX]{};
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    ssize_t count = fd >= 0 ? read(fd, command, sizeof(command) - 1) : 0;
    if (fd >= 0) close(fd);
    char* mode = count > 0 ? static_cast<char*>(memchr(command, 0, static_cast<size_t>(count))) : nullptr;
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    char* slash = strrchr(executable, '/');
    if (!path || path[0] != '/' || strlen(path) + 16 >= sizeof(configPath) || length <= 0 || !slash ||
        strcmp(slash + 1, "1cv8") || !mode || mode + 1 >= command + count || strcasecmp(mode + 1, "DESIGNER") || getuid() != geteuid()) {
        liveStarted = false; return 22;
    }
    *slash = 0;
    char cairo[PATH_MAX], graphics[PATH_MAX];
    if (snprintf(cairo, sizeof(cairo), "%s/libcairo-v8.so", executable) >= static_cast<int>(sizeof(cairo)) ||
        snprintf(graphics, sizeof(graphics), "%s/grphcs.so", executable) >= static_cast<int>(sizeof(graphics))) {
        liveStarted = false; return 22;
    }
    strcpy(configPath, path);
    snprintf(statusPath, sizeof(statusPath), "%s.status", path);
    snprintf(pendingPath, sizeof(pendingPath), "%s.status.pending", path);
    pthread_mutex_lock(&lock); reload(now()); pthread_mutex_unlock(&lock);
    if (!valid) { liveStarted = false; return 22; }
    const char* names[] = {"cairo_set_source_rgb", "cairo_set_source_rgba", "cairo_pattern_add_color_stop_rgb",
        "cairo_pattern_add_color_stop_rgba", "cairo_mesh_pattern_set_corner_color_rgb", "cairo_mesh_pattern_set_corner_color_rgba",
        "cairo_set_source", "cairo_set_source_surface", "cairo_mask", "cairo_mask_surface"};
    Scan scan{};
    bool ready = false;
    enabled = true;
    for (unsigned attempt = 0; attempt < 50 && !ready; ++attempt) {
        scan = {};
        struct stat module{};
        if (!stat(graphics, &module)) { scan.device = module.st_dev; scan.inode = module.st_ino; }
        for (unsigned bank = 0; bank < 2; ++bank) {
            void* handle = dlopen(bank ? "libcairo.so.2" : cairo, RTLD_NOW | RTLD_NOLOAD);
            if (!handle) continue;
            bool iconApi=linux_icons::initialize(bank,handle);
            if (bank==1) iconsAvailable=iconApi;
            for (unsigned index = 0; index < 10; ++index) {
                const char* name = names[index];
                void* original = dlsym(handle, name);
                if (!original) continue;
                Elf64_Sym symbol{}; symbol.st_value = reinterpret_cast<uintptr_t>(original);
                uintptr_t reference = 2, definition = bank ? 4 : 1;
                uintptr_t replacement = la_symbind64(&symbol, 0, &reference, &definition, nullptr, name);
                scan.symbols[scan.symbolCount++] = {symbol.st_value, replacement, 1u << index, bank};
            }
            dlclose(handle);
        }
        FILE* maps = fopen("/proc/self/maps", "r");
        if (maps) {
            char line[1024], permissions[8]; unsigned long begin, end;
            while (fgets(line, sizeof(line), maps) && scan.writableCount < 4096) {
                if (sscanf(line, "%lx-%lx %7s", &begin, &end, permissions) == 3 && permissions[0] == 'r' && permissions[1] == 'w')
                    scan.writable[scan.writableCount++] = {begin, end};
            }
            fclose(maps);
        }
        dl_iterate_phdr(scanModule, &scan);
        ready = coherent(scan);
        if (!ready) usleep(100000);
    }
    if (!ready) {
        liveStatus(0, "unsupported-renderer"); liveStarted = false; return 95;
    }
    unsigned installed = 0, mask = 0;
    for (; installed < scan.patchCount; ++installed) {
        const auto& patch = scan.patches[installed];
        uintptr_t expected = patch.symbol.original;
        if (!__atomic_compare_exchange_n(patch.address, &expected, patch.symbol.replacement, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
        mask |= patch.symbol.mask;
    }
    if (installed != scan.patchCount) {
        while (installed) {
            const auto& patch = scan.patches[--installed]; uintptr_t expected = patch.symbol.replacement;
            __atomic_compare_exchange_n(patch.address, &expected, patch.symbol.original, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
        liveStatus(0, "renderer-changed"); liveStarted = false; return 11;
    }
    iconsAvailable=iconsAvailable&&(mask&960);
    liveStatus(mask, "");
    linux_fonts::initialize(executable,path);
    gtk.handle = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (gtk.handle) {
        gtk.idle = reinterpret_cast<decltype(gtk.idle)>(dlsym(gtk.handle, "g_idle_add"));
        gtk.windows = reinterpret_cast<decltype(gtk.windows)>(dlsym(gtk.handle, "gtk_window_list_toplevels"));
        gtk.draw = reinterpret_cast<decltype(gtk.draw)>(dlsym(gtk.handle, "gtk_widget_queue_draw"));
        gtk.freeList = reinterpret_cast<decltype(gtk.freeList)>(dlsym(gtk.handle, "g_list_free"));
        if (!gtk.windows || !gtk.draw || !gtk.freeList) gtk.idle = nullptr;
    }
    pthread_t worker;
    if (!pthread_create(&worker, nullptr, refreshWorker, nullptr)) pthread_detach(worker);
    return 0;
}
