#include "../src/browser_lifetime.h"
#include "browser_lifetime_fixture.h"

static BOOL WINAPI guardedDestroy(HWND window) { return destroyBrowserWindow(window,DestroyWindow,true); }
static BOOL WINAPI disabledDestroy(HWND window) { return destroyBrowserWindow(window,DestroyWindow,false); }
static BOOL WINAPI failedDestroy(HWND window) {
    SendMessageW(window,WM_GETOBJECT,0,(LPARAM)OBJID_CLIENT);
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}
int main() {
    bool ok=browser_fixture::probe(false,disabledDestroy);
    ok &= browser_fixture::probe(true,guardedDestroy);
    ok &= browser_fixture::probe(false,guardedDestroy,L"OwnedNonBrowserWindowClass");
    browser_fixture::State state;
    HWND window=browser_fixture::create(state); if (!window) return 2;
    ok &= !destroyBrowserWindow(window,failedDestroy,true) && GetLastError()==ERROR_ACCESS_DENIED;
    ok &= IsWindow(window) && state.normal==0;
    ok &= SendMessageW(window,WM_GETOBJECT,0,(LPARAM)OBJID_CLIENT)==73 && state.normal==1;
    DestroyWindow(window);
    ok &= state.unsafe==2 && browserGuardFailures()==0 && browserBlockedRequests()==3;
    printf("Browser lifetime failed destruction: %s; error preserved, subclass removed\n",ok?"PASS":"FAIL");
    return ok?0:1;
}
