#include "../src/protocol.h"
#include <stdio.h>
#include <wchar.h>
#include <string.h>
template<class T> T load(HMODULE m,const char* name) { auto p=GetProcAddress(m,name); T f; memcpy(&f,&p,sizeof(f)); return f; }
int wmain(int argc,wchar_t** argv) {
    if (argc<5) return 2;
    HMODULE cairo=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    HMODULE frame=LoadLibraryW(L"frame.dll"),graphics=LoadLibraryW(L"grphcs.dll");
    HMODULE browser=LoadLibraryW(L"WebKit.dll"),probe=LoadLibraryW(argv[2]);
    if (!cairo || !frame || !graphics || !browser || !probe) { printf("Load failed: %lu\n",GetLastError()); return 3; }
    auto initialize=load<DWORD(WINAPI*)(void*)>(probe,"ThemeInitialize");
    auto apply=load<DWORD(WINAPI*)(void*)>(probe,"ThemeApply");
    auto stop=load<DWORD(WINAPI*)(void*)>(probe,"ThemeStop");
    auto render=load<BOOL(*)(const ProbeConfig*)>(graphics,"real_selection_probe");
    auto renderChrome=load<BOOL(*)(const ProbeConfig*)>(graphics,"chrome_probe");
    auto renderComparison=load<BOOL(*)(const ProbeConfig*)>(graphics,"comparison_probe");
    auto renderBrowser=load<BOOL(*)(const ProbeConfig*)>(browser,"browser_probe");
    auto systemColor=load<DWORD(*)(int)>(frame,"system_color_probe");
    bool ok=initialize && apply && stop && render && renderBrowser && systemColor && renderChrome && renderComparison;
    for (int i=3;ok && i<argc;++i) {
        ProbeConfig cfg={}; FILE* file=_wfopen(argv[i],L"rb");
        if (!file) return 4;
        size_t read=fread(&cfg,1,sizeof(cfg),file); fclose(file);
        if (read!=sizeof(cfg)) return 5;
        DWORD result=i==3?initialize(&cfg):apply(&cfg);
        if (result) { printf("Theme call failed: %lu\n",result); ok=false; break; }
        ok=render(&cfg)!=0;
        ok &= renderBrowser(&cfg)!=0;
        ok &= renderChrome(&cfg)!=0;
        ok &= renderComparison(&cfg)!=0;
        auto nativeRgb=[](int index)->DWORD { COLORREF c=GetSysColor(index); return (GetRValue(c)<<16)|(GetGValue(c)<<8)|GetBValue(c); };
        ok &= systemColor(COLOR_ACTIVECAPTION)==(cfg.mode?cfg.colors.captionBackground:nativeRgb(COLOR_ACTIVECAPTION));
        ok &= systemColor(COLOR_CAPTIONTEXT)==(cfg.mode?cfg.colors.captionText:nativeRgb(COLOR_CAPTIONTEXT));
        ok &= systemColor(COLOR_SCROLLBAR)==(cfg.mode?cfg.colors.rasterBackground:nativeRgb(COLOR_SCROLLBAR));
    }
    if (stop) stop(nullptr);
    printf("Production IAT with real Cairo: %s\n",ok?"PASS":"FAIL");
    return ok?0:1;
}
