#include <dlfcn.h>
extern "C" void* platform_lookup(void* library, const char* name) {
    return dlsym(library, name);
}
