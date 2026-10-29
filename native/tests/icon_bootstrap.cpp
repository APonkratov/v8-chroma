// Production bootstrap on an 8.5 host, with available/failing icon components.
// The icon module's fixture separately checks real image matching and cleanup.
#include "../src/protocol.h"
#include "../src/icon_theme.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

extern "C" DWORD WINAPI ThemeInitialize(void*);
extern "C" DWORD WINAPI ThemeApply(void*);
extern "C" DWORD WINAPI ThemeSnapshot(void*);
extern "C" DWORD WINAPI ThemeStop(void*);
struct FrameResult { DWORD text, background, brush, pen; BOOL invariants; };
extern "C" __declspec(dllimport) FrameResult frame_probe();
extern "C" __declspec(dllimport) BOOL graphics_probe(const ProbeConfig*);
extern "C" __declspec(dllimport) void graphics_icon_probe();

struct IconState { unsigned mode; };
static IconState prepared;
static unsigned preparations, iconDraws, activeMode;
static bool available;
bool initializeIconTheme(HMODULE) { return true; }
IconState* prepareIconTheme(unsigned mode,DWORD& error) {
    ++preparations;
    error=available?ERROR_SUCCESS:(mode?ERROR_NOT_ENOUGH_MEMORY:ERROR_INVALID_PARAMETER);
    prepared.mode=mode;
    return available?&prepared:nullptr;
}
void activateIconTheme(IconState* state) { activeMode=state?state->mode:0; }
void discardIconTheme(IconState*) {}
IconStats iconStats() { IconStats stats={}; stats.mode=activeMode; return stats; }
void drawIconSource(void* c,void* p,void(__cdecl* original)(void*,void*)) {
    ++iconDraws; original(c,p);
}
void drawIconMask(void* c,void* p,void(__cdecl* original)(void*,void*)) {
    ++iconDraws; original(c,p);
}
void drawIconMaskSurface(void* c,void* s,double x,double y,
                        void(__cdecl* original)(void*,void*,double,double)) {
    ++iconDraws; original(c,s,x,y);
}
static bool snapshotMatches(const wchar_t* path,unsigned generation,unsigned requestedMode) {
    if (ThemeSnapshot(nullptr)) return false;
    FILE* file=_wfopen(path,L"rb");
    if (!file) return false;
    char bytes[16384]={}; fread(bytes,1,sizeof(bytes)-1,file); fclose(file);
    char expected[64]; sprintf(expected,"# generation=%u\n",generation);
    bool base=strstr(bytes,expected) && strstr(bytes,"# base_patched_imports=14\n");
    if (available) {
        sprintf(expected,"# icon_mode=%u\n",requestedMode);
        return base && strstr(bytes,"# patched_imports=17\n") &&
            strstr(bytes,"# icon_patched_imports=3\n") && strstr(bytes,"# icon_status=0\n") &&
            strstr(bytes,expected) && strstr(bytes,"# icon_pack=1\n");
    }
    return base && strstr(bytes,"# patched_imports=14\n") &&
        strstr(bytes,"# icon_patched_imports=0\n") &&
        strstr(bytes,"# icon_status=8\n") && strstr(bytes,"# icon_mode=0\n") &&
        strstr(bytes,"# icon_pack=0\n");
}
int wmain(int argc,wchar_t** argv) {
    if (argc!=3 && argc!=4) return 2;
    available=argc==4 && !wcscmp(argv[3],L"available");
    ProbeConfig config={};
    config.size=sizeof(config); config.version=3; config.mode=1;
    config.mapCount=1; config.maps[0]={Text,0,0x123456};
    strcpy(config.themeId,"bootstrap-test");
    config.colors.glyphBackground=0xFFFFFF;
    config.colors.glyphForeground=0xDADADA;
    config.colors.selectionBackground=0x536AC2;
    config.colors.selectionForeground=0xFFFFFF;
    config.iconMode=static_cast<DWORD>(_wtoi(argv[2])); config.iconPack=1;
    GetModuleFileNameW(nullptr,config.targetDir,260);
    auto slash=wcsrchr(config.targetDir,L'\\'); if (!slash) return 3; *slash=0;
    if (!GetFullPathNameW(argv[1],520,config.logPath,nullptr)) return 4;
    DWORD result=ThemeInitialize(&config);
    if (result) { printf("ThemeInitialize failed: %lu\n",result); return 5; }
    bool ok=preparations==1 && snapshotMatches(config.logPath,1,config.iconMode);
    auto frame=frame_probe();
    ok &= frame.invariants && frame.text==0x123456 && graphics_probe(&config);
    graphics_icon_probe(); ok &= iconDraws==(available?3u:0u);
    // A failed optional component must not prevent subsequent color updates.
    config.maps[0].target=0x654321;
    config.iconMode=config.iconMode==1?2:1;
    ok &= ThemeApply(&config)==ERROR_SUCCESS;
    frame=frame_probe();
    ok &= preparations==(available?2u:1u) && frame.invariants && frame.text==0x654321;
    ok &= graphics_probe(&config) && snapshotMatches(config.logPath,2,config.iconMode);
    ok &= ThemeStop(nullptr)==ERROR_SUCCESS;
    frame=frame_probe();
    ok &= frame.invariants && frame.text==0;
    // Stop keeps the import hooks but disables their replacement state.
    unsigned before=iconDraws; graphics_icon_probe();
    ok &= activeMode==0 && iconDraws==before+(available?3u:0u);
    printf("8.5 host icons %s: bootstrap, imports, status, apply, stop: %s\n",available?"available":"preparation failure",ok?"PASS":"FAIL");
    return ok?0:1;
}
