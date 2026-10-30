// Native style event discovery and GUI-thread dispatch; shared PE validation.
#pragma once
#include "ui_font_discovery.h"
static BYTE* styleGetter;
static const GUID* styleEvent;
static HMODULE styleCore;
static volatile LONG styleBusy,stylePending;
static HWND styleWindow;
static UINT styleMessage;
static WPARAM styleToken;
static DWORD styleStatus;
static bool styleNotify;
static bool resolveStyle() {
    (void)&discoverEditor; // Reuse the established PE/pattern helpers only.
    for (auto name:{L"core83.dll",L"core85.dll"}) {
        auto module=GetModuleHandleW(name); Image core(module);
        auto event=reinterpret_cast<BYTE*>(GetProcAddress(module,"?kDefaultStyleChangedEvent@core@@3U_GUID@@B"));
        if (!core.contains(event,sizeof(GUID))) continue;
        ResolvedStyle resolved{};
        if (!discoverStyle(core,event,resolved)) continue;
        styleGetter=resolved.getter; styleEvent=reinterpret_cast<const GUID*>(event); styleCore=module; return true;
    }
    return false;
}
static bool readable(const void* p,SIZE_T length) {
    MEMORY_BASIC_INFORMATION info{};
    if (!p || !VirtualQuery(p,&info,sizeof(info)) || info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    auto offset=reinterpret_cast<ULONG_PTR>(p)-reinterpret_cast<ULONG_PTR>(info.BaseAddress);
    return offset<info.RegionSize && length<=info.RegionSize-offset;
}
static LRESULT CALLBACK styleHook(int code,WPARAM wp,LPARAM lp) {
    if (code==HC_ACTION) {
        auto msg=reinterpret_cast<CWPSTRUCT*>(lp);
        if (msg->hwnd==styleWindow && msg->message==styleMessage && msg->wParam==styleToken && InterlockedCompareExchange(&stylePending,2,1)==1) {
            auto source=reinterpret_cast<void*(*)()>(styleGetter)();
            styleStatus=ERROR_NOT_READY;
            if (readable(source,sizeof(void*))) {
                auto table=*reinterpret_cast<BYTE***>(source);
                if (readable(table,4*sizeof(void*)) && sourceLayout(Image(styleCore),table[3])) {
                    if (styleNotify) {
                        reinterpret_cast<void(*)(void*,const GUID*,void*,void*)>(table[3])(source,styleEvent,nullptr,nullptr);
                        InterlockedIncrement(&styleNotifications);
                    }
                    styleStatus=0;
                } else styleStatus=ERROR_NOT_SUPPORTED;
            }
            InterlockedExchange(&stylePending,0);
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
static DWORD notifyStyle(HWND window,bool notify=true) {
    if (InterlockedCompareExchange(&styleBusy,1,0)) return ERROR_BUSY;
    if (InterlockedCompareExchange(&stylePending,0,0)) { InterlockedExchange(&styleBusy,0); return ERROR_BUSY; }
    DWORD status=ERROR_NOT_SUPPORTED;
    if (resolveStyle()) {
        styleWindow=window; styleMessage=RegisterWindowMessageW(L"V8Chroma.UiFont.StyleRefresh.v1");
        DWORD thread=GetWindowThreadProcessId(window,nullptr);
        HHOOK hook=SetWindowsHookExW(WH_CALLWNDPROC,styleHook,nullptr,thread);
        if (hook) {
            styleNotify=notify; ++styleToken; styleStatus=ERROR_NOT_READY; InterlockedExchange(&stylePending,1);
            DWORD_PTR result=0;
            status=SendMessageTimeoutW(window,styleMessage,styleToken,0,SMTO_BLOCK|SMTO_ABORTIFHUNG,5000,&result)?styleStatus:ERROR_TIMEOUT;
            UnhookWindowsHookEx(hook); InterlockedCompareExchange(&stylePending,0,1);
        } else status=GetLastError();
    }
    InterlockedExchange(&styleBusy,0); return status;
}
