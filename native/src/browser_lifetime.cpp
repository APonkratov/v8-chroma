#include "browser_lifetime.h"
#include <commctrl.h>
#include <wchar.h>

namespace {
    volatile LONG64 calls,blocked,failures;
    struct Destroying { HWND window; Destroying* previous; };
    thread_local Destroying* destroying;
    constexpr UINT_PTR subclassId=0x56384348;

    LRESULT CALLBACK guard(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
        // WebKit's onGetObject asks a Document being detached for its AX cache.
        // That cache can already be null. UI Automation may synchronously ask
        // for it from EVENT_OBJECT_DESTROY inside DestroyWindow.
        // WM_GETOBJECT object IDs are 32-bit, even in a 64-bit LPARAM. Both
        // zero-extended and sign-extended OBJID_CLIENT occur in real callers.
        if (message==WM_GETOBJECT && static_cast<DWORD>(lParam)==static_cast<DWORD>(OBJID_CLIENT)) {
            InterlockedIncrement64(&blocked);
            return 0;
        }
        return DefSubclassProc(window,message,wParam,lParam);
    }
}

BOOL destroyBrowserWindow(HWND window,BrowserDestroyWindow original,bool enabled) {
    if (!enabled) return original(window);
    DWORD saved=GetLastError(),owner=0;
    DWORD thread=GetWindowThreadProcessId(window,&owner);
    wchar_t name[64]={};
    bool browser=thread==GetCurrentThreadId() && owner==GetCurrentProcessId() &&
        GetClassNameW(window,name,64) && !wcscmp(name,L"WebViewWindowClass");
    // DestroyWindow cannot destroy another thread's window. Leave that failure
    // and all non-browser windows entirely to the original API.
    if (!browser) { SetLastError(saved); return original(window); }
    for (auto frame=destroying;frame;frame=frame->previous)
        if (frame->window==window) { SetLastError(saved); return original(window); }
    InterlockedIncrement64(&calls);
    bool installed=SetWindowSubclass(window,guard,subclassId,0)!=FALSE;
    if (!installed) InterlockedIncrement64(&failures);
    Destroying frame={window,destroying}; destroying=&frame;
    SetLastError(saved);
    BOOL result=original(window);
    DWORD error=GetLastError();
    // The common-controls chain removes itself when the window dies. If the
    // API fails, remove just our temporary subclass, preserving other clients.
    if (installed && IsWindow(window)) RemoveWindowSubclass(window,guard,subclassId);
    destroying=frame.previous;
    SetLastError(error);
    return result;
}
unsigned long long browserDestroyCalls() { return InterlockedCompareExchange64(&calls,0,0); }
unsigned long long browserBlockedRequests() { return InterlockedCompareExchange64(&blocked,0,0); }
unsigned long long browserGuardFailures() { return InterlockedCompareExchange64(&failures,0,0); }
