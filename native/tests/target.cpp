#include "../src/protocol.h"
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <dwmapi.h>
struct FrameResult { DWORD text, background, brush, pen; BOOL invariants; };
extern "C" __declspec(dllimport) FrameResult frame_probe();
extern "C" __declspec(dllimport) BOOL graphics_probe(const ProbeConfig*);
int wmain(int argc,wchar_t** argv) {
    if (argc!=3 && argc!=4) return 2;
    ProbeConfig cfg={};
    FILE* configFile=_wfopen(argv[2],L"rb");
    if (!configFile) return 9;
    size_t read=fread(&cfg,1,sizeof(cfg),configFile); fclose(configFile);
    if (read!=sizeof(cfg) || cfg.size!=sizeof(cfg) || cfg.version!=3) return 10;
    auto expected=[&](DWORD kind,DWORD source) {
        if (cfg.mode) for (DWORD i=0;i<cfg.mapCount;++i)
            if (cfg.maps[i].kind==kind && cfg.maps[i].source==source) return cfg.maps[i].target;
        return source;
    };
    auto color=[](DWORD rgb) { return RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255); };
    HWND window=CreateWindowExW(0,L"STATIC",L"Theme probe title test",WS_OVERLAPPEDWINDOW,0,0,320,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if (!window) return 7;
    const DWORD attributes[]={20,34,35,36};
    DWORD original[4]={}; unsigned supported=0;
    for (unsigned i=0;i<4;++i)
        if (SUCCEEDED(DwmGetWindowAttribute(window,attributes[i],&original[i],sizeof(DWORD)))) supported|=1u<<i;
    wchar_t eventName[100]; swprintf(eventName,100,L"Local\\ThemeProbeReady-%lu",GetCurrentProcessId());
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,eventName);
    if (!event || WaitForSingleObject(event,30000)!=WAIT_OBJECT_0) return 3;
    CloseHandle(event);
    if (argc==4) {
        swprintf(eventName,100,L"Local\\ThemeProbeContinue-%lu",GetCurrentProcessId());
        HANDLE hold=CreateEventW(nullptr,TRUE,FALSE,eventName);
        if (!hold || WaitForSingleObject(hold,300000)!=WAIT_OBJECT_0) return 12;
        CloseHandle(hold);
    }
    // Invalid protocol fields must be rejected even when a valid instance is active.
    auto initializeAddress=GetProcAddress(GetModuleHandleW(L"ThemeProbe.dll"),"ThemeInitialize");
    DWORD(WINAPI* initialize)(void*)=nullptr; memcpy(&initialize,&initializeAddress,sizeof(initialize));
    if (!initialize) return 11;
    auto applyAddress=GetProcAddress(GetModuleHandleW(L"ThemeProbe.dll"),"ThemeApply");
    DWORD(WINAPI* apply)(void*)=nullptr; memcpy(&apply,&applyAddress,sizeof(apply));
    if (!apply) return 11;
    bool protocolValid=true;
    for (unsigned test=0;test<7;++test) {
        ProbeConfig bad=cfg;
        if (test==0) bad.version=1;
        if (test==1) bad.size=sizeof(cfg)-4;
        if (test==2) bad.colors.rasterForeground=0x1000000;
        if (test==3) bad.colors.darkCaption=2;
        if (test==4) bad.mapCount=257;
        if (test==5) memset(bad.themeId,'a',sizeof(bad.themeId));
        if (test==6) { bad.mapCount=1; bad.maps[0]={0,0,0}; }
        protocolValid &= initialize(&bad)==ERROR_INVALID_PARAMETER;
        protocolValid &= apply(&bad)==ERROR_INVALID_PARAMETER;
    }
    FrameResult result=frame_probe();
    bool themed=cfg.mode!=0;
    bool valid=protocolValid && result.invariants && graphics_probe(&cfg);
    valid &= result.text==expected(Text,0);
    valid &= result.background==expected(Background,0xFFFFFF);
    valid &= result.brush==expected(Brush,0xFFFFFF);
    valid &= result.pen==expected(Pen,0);
    DWORD desired[]={cfg.colors.darkCaption,color(cfg.colors.border),color(cfg.colors.captionBackground),color(cfg.colors.captionText)};
    for (unsigned i=0;i<4;++i) if (supported&(1u<<i)) {
        DWORD value=0;
        valid &= SUCCEEDED(DwmGetWindowAttribute(window,attributes[i],&value,sizeof(value)));
        valid &= value==(themed?desired[i]:original[i]);
    }
    // A dialog created after initialization is picked up by the periodic window scan.
    HWND late=CreateWindowExW(0,L"STATIC",L"Late theme probe dialog",WS_OVERLAPPEDWINDOW,0,0,320,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if (!late) valid=false;
    if (themed && (supported&5)) {
        DWORD value=0; unsigned attempt=0;
        unsigned index=(supported&4)?2:0;
        for (;attempt<40;++attempt) {
            DwmGetWindowAttribute(late,attributes[index],&value,sizeof(value));
            if (value==desired[index]) break;
            Sleep(50);
        }
        valid &= attempt<40;
    }
    // Calls made outside frame.dll must not be intercepted.
    HDC dc=CreateCompatibleDC(nullptr); SetTextColor(dc,RGB(0,0,0));
    COLORREF unscoped=GetTextColor(dc); DeleteDC(dc);
    auto symbol=GetProcAddress(GetModuleHandleW(L"ThemeProbe.dll"),"ThemeSnapshot");
    DWORD(WINAPI* snapshot)(void*)=nullptr;
    static_assert(sizeof(snapshot)==sizeof(symbol));
    memcpy(&snapshot,&symbol,sizeof(snapshot));
    if (!snapshot || snapshot(nullptr)) return 5;
    symbol=GetProcAddress(GetModuleHandleW(L"ThemeProbe.dll"),"ThemeStop");
    DWORD(WINAPI* stop)(void*)=nullptr; memcpy(&stop,&symbol,sizeof(stop));
    if (!stop || stop(nullptr)) return 8;
    valid &= apply(&cfg)==ERROR_SERVICE_NOT_ACTIVE;
    for (unsigned i=0;i<4;++i) if (supported&(1u<<i)) {
        DWORD value=0;
        valid &= SUCCEEDED(DwmGetWindowAttribute(window,attributes[i],&value,sizeof(value))) && value==original[i];
        valid &= SUCCEEDED(DwmGetWindowAttribute(late,attributes[i],&value,sizeof(value))) && value==original[i];
    }
    FrameResult restored=frame_probe();
    cfg.mode=0;
    valid &= restored.text==0 && restored.background==0xFFFFFF && restored.invariants && graphics_probe(&cfg);
    DestroyWindow(late); DestroyWindow(window);
    FILE* file=_wfopen(argv[1],L"wb");
    if (!file) return 4;
    fprintf(file,"{\"text_rgb\":%lu,\"background_rgb\":%lu,\"brush_rgb\":%lu,\"pen_rgb\":%lu,\"unscoped_rgb\":%lu,\"dwm_attributes_tested\":%u,\"stop_restored\":%s,\"invariants_ok\":%s}",result.text,result.background,result.brush,result.pen,unscoped,supported,valid?"true":"false",valid?"true":"false");
    fclose(file);
    return valid?0:6;
}
