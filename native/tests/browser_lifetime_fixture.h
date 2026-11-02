#pragma once
#include <windows.h>
#include <stdio.h>

namespace browser_fixture {
    using Destroy=BOOL(WINAPI*)(HWND);
    struct State { bool destroying=false; unsigned normal=0,unsafe=0; };
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
        auto state=(State*)GetWindowLongPtrW(window,GWLP_USERDATA);
        if (message==WM_NCCREATE) {
            state=(State*)((CREATESTRUCTW*)lParam)->lpCreateParams;
            SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)state);
        }
        if (state && message==WM_GETOBJECT && static_cast<DWORD>(lParam)==static_cast<DWORD>(OBJID_CLIENT)) {
            if (state->destroying) ++state->unsafe; else ++state->normal;
            return 73;
        }
        if (state && message==WM_DESTROY) {
            state->destroying=true;
            SendMessageW(window,WM_GETOBJECT,0,(LPARAM)OBJID_CLIENT);
            SendMessageW(window,WM_GETOBJECT,0,(LPARAM)(DWORD)OBJID_CLIENT);
        }
        return DefWindowProcW(window,message,wParam,lParam);
    }
    static HWND create(State& state,const wchar_t* name=L"WebViewWindowClass") {
        WNDCLASSW cls={}; cls.lpfnWndProc=procedure; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=name;
        if (!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        return CreateWindowW(name,L"Owned browser lifetime fixture",WS_OVERLAPPED,0,0,100,100,nullptr,nullptr,cls.hInstance,&state);
    }
    static bool probe(bool guarded,Destroy destroy,const wchar_t* name=L"WebViewWindowClass") {
        State state;
        HWND window=create(state,name); if (!window) return false;
        bool ok=SendMessageW(window,WM_GETOBJECT,0,(LPARAM)OBJID_CLIENT)==73;
        ok &= SendMessageW(window,WM_GETOBJECT,0,(LPARAM)(DWORD)OBJID_CLIENT)==73 && state.normal==2;
        ok &= destroy(window)!=FALSE;
        ok &= !IsWindow(window) && state.unsafe==(guarded?0u:2u);
        printf("Browser lifetime %s: %s; ordinary requests=%u, requests reaching detached document=%u\n",
            guarded?"guarded":"native",ok?"PASS":"FAIL",state.normal,state.unsafe);
        return ok;
    }
}
