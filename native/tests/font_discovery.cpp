#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include "../src/font_discovery.h"

// Maps installed images without executing their DllMain. Never starts a user
// database or writes to a platform installation.
int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    std::wstring directory=argv[1];
    auto text=LoadLibraryExW((directory+L"/txtedui.dll").c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
    if (!text) return 3;
    Image image(text);
    auto imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image.base+
        image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    HMODULE core=nullptr; void** ctorSlot=nullptr;
    for (;imports->Name && !ctorSlot;++imports) {
        if (!imports->OriginalFirstThunk) continue;
        auto names=reinterpret_cast<IMAGE_THUNK_DATA64*>(image.base+imports->OriginalFirstThunk);
        auto slots=reinterpret_cast<void**>(image.base+imports->FirstThunk);
        for (;names->u1.AddressOfData;++names,++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto name=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image.base+names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(name->Name),"??0Font@core@@QEAA@XZ")) continue;
            auto provider=reinterpret_cast<char*>(image.base+imports->Name);
            std::wstring path=directory+L"/";
            while (*provider) path.push_back(*provider++);
            core=LoadLibraryExW(path.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
            ctorSlot=slots; break;
        }
    }
    if (!core || !ctorSlot) return 4;
    auto previous=*ctorSlot;
    DWORD protect,ignored;
    if (!VirtualProtect(ctorSlot,sizeof(void*),PAGE_READWRITE,&protect)) return 5;
    *ctorSlot=reinterpret_cast<void*>(GetProcAddress(core,"??0Font@core@@QEAA@XZ"));
    VirtualProtect(ctorSlot,sizeof(void*),protect,&ignored);
    ResolvedEditor resolved{};
    bool found=discoverEditor(image,Image(core),resolved);
    bool source=false;
    if (auto lookup=Image(core).unique({0x41,0x8b,0x45,0,0x48,0x8b,0x4b,0x78,0x48,0x23,0xc8,0x48,0xc1,0xe1,4},true))
        source=sourceLayout(Image(core),Image(core).function(lookup).data);
    printf("discovery=%u source_layout=%u controllers=%u getter=%lx loader=%lx slot=%lx setter=%lx\n",
        found,source,resolved.tableCount,resolved.getter,resolved.loadFont,resolved.loaderSlot,resolved.setFont);
    bool rejected=false;
    if (found) {
        auto getter=image.base+resolved.getter;
        if (!VirtualProtect(getter,1,PAGE_EXECUTE_READWRITE,&protect)) return 6;
        auto original=*getter; *getter=0x90;
        ResolvedEditor changed{};
        rejected=!discoverEditor(image,Image(core),changed);
        *getter=original; VirtualProtect(getter,1,protect,&ignored);
    }
    printf("changed_layout_rejected=%u\n",rejected);
    VirtualProtect(ctorSlot,sizeof(void*),PAGE_READWRITE,&protect);
    *ctorSlot=previous; VirtualProtect(ctorSlot,sizeof(void*),protect,&ignored);
    FreeLibrary(text); FreeLibrary(core);
    return found && source && rejected?0:1;
}
