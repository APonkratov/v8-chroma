#include "window_theme.h"
#include <dwmapi.h>

namespace {
    constexpr DWORD attributes[]={20,34,35,36}; // immersive dark, border, caption, caption text
    constexpr wchar_t marker[]=L"ThemeProbe.DwmState.v1";
    struct WindowState { HWND window; DWORD original[4],last[4]; unsigned supported; };
    WindowState states[128];
    SRWLOCK lock=SRWLOCK_INIT;
    bool enable;
    COLORREF colors[4];
    unsigned count;

    bool owned(HWND window) {
        DWORD pid=0; GetWindowThreadProcessId(window,&pid);
        return pid==GetCurrentProcessId();
    }
    bool marked(WindowState &state) {
        return state.window && owned(state.window) && GetPropW(state.window,marker)==&state;
    }
    BOOL CALLBACK visit(HWND window,LPARAM) {
        if (!owned(window) || !(GetWindowLongPtrW(window,GWL_STYLE)&WS_CAPTION)) return TRUE;
        WindowState* state=nullptr;
        WindowState* available=nullptr;
        for (auto &entry:states) {
            if (entry.window==window && marked(entry)) { state=&entry; break; }
            if (!marked(entry) && !available) available=&entry;
        }
        if (!state) {
            if (!available) return TRUE;
            state=available; *state={}; state->window=window;
            for (unsigned i=0;i<4;++i)
                if (SUCCEEDED(DwmGetWindowAttribute(window,attributes[i],&state->original[i],sizeof(DWORD)))) state->supported|=1u<<i;
            if (!state->supported || !SetPropW(window,marker,state)) { *state={}; return TRUE; }
            for (unsigned i=0;i<4;++i) state->last[i]=state->original[i];
        }
        bool changed=false;
        for (unsigned i=0;i<4;++i) if ((state->supported&(1u<<i)) && state->last[i]!=colors[i]) {
            if (SUCCEEDED(DwmSetWindowAttribute(window,attributes[i],&colors[i],sizeof(DWORD)))) {
                state->last[i]=colors[i]; changed=true;
            }
        }
        if (changed) RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME);
        ++count;
        return TRUE;
    }
}

void updateWindowTheme(bool enabled,COLORREF caption,COLORREF text,COLORREF border,bool dark) {
    AcquireSRWLockExclusive(&lock);
    enable=enabled;
    count=0;
    if (enable) {
        colors[0]=dark?TRUE:FALSE; colors[1]=border; colors[2]=caption; colors[3]=text;
        EnumWindows(visit,0);
    } else {
        for (auto &state:states) {
            if (marked(state)) {
                for (unsigned i=0;i<4;++i) if (state.supported&(1u<<i))
                    DwmSetWindowAttribute(state.window,attributes[i],&state.original[i],sizeof(DWORD));
                RemovePropW(state.window,marker);
                RedrawWindow(state.window,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME);
            }
            state={};
        }
    }
    ReleaseSRWLockExclusive(&lock);
}
unsigned themedWindowCount() {
    AcquireSRWLockShared(&lock); unsigned result=count; ReleaseSRWLockShared(&lock); return result;
}
