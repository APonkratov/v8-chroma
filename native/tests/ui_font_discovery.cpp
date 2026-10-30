#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/font_discovery.h"
#include "../src/ui_font_discovery.h"

// Optional integration fixture: maps an installed core without running DllMain.
int wmain(int argc,wchar_t** argv) {
    (void)&discoverEditor; (void)&sourceLayout;
    if (argc!=2) return 2;
    HMODULE module=nullptr;
    for (auto name:{L"/core83.dll",L"/core85.dll"}) {
        module=LoadLibraryExW((std::wstring(argv[1])+name).c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
        if (module) break;
    }
    if (!module) return 3;
    Image core(module);
    auto event=reinterpret_cast<BYTE*>(GetProcAddress(module,"?kDefaultStyleChangedEvent@core@@3U_GUID@@B"));
    ResolvedStyle resolved{};
    if (!discoverStyle(core,event,resolved)) return 4;
    printf("style_discovery=1 dispatch=%s getter=%lx\n",resolved.tail?"tail":"call",static_cast<DWORD>(resolved.getter-core.base));
    ResolvedStyle wrong{};
    if (discoverStyle(core,event+1,wrong)) return 5;
    struct Changed { BYTE* location; BYTE original; };
    std::vector<Changed> changed;
    while (discoverStyle(core,event,resolved)) {
        if (changed.size()==16) return 6;
        auto location=resolved.call+resolved.length-1;
        DWORD old,ignored;
        if (!VirtualProtect(location,1,PAGE_READWRITE,&old)) return 7;
        changed.push_back({location,*location}); *location=0x20;
        VirtualProtect(location,1,old,&ignored);
    }
    for (auto& item:changed) {
        DWORD old,ignored;
        if (!VirtualProtect(item.location,1,PAGE_READWRITE,&old)) return 8;
        *item.location=item.original;
        VirtualProtect(item.location,1,old,&ignored);
    }
    bool restored=discoverStyle(core,event,resolved);
    printf("wrong_guid_rejected=1 changed_dispatches_rejected=%zu restored=%u\n",changed.size(),restored);
    FreeLibrary(module);
    return !changed.empty() && restored?0:1;
}
