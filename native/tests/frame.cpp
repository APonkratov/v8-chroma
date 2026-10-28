#include <windows.h>
struct FrameResult { DWORD text, background, brush, pen; BOOL invariants; };
static DWORD rgb(COLORREF c) { return (GetRValue(c)<<16)|(GetGValue(c)<<8)|GetBValue(c); }
extern "C" __declspec(dllexport) DWORD system_color_probe(int index) { return rgb(GetSysColor(index)); }
extern "C" __declspec(dllexport) FrameResult frame_probe() {
    HDC dc=CreateCompatibleDC(nullptr);
    COLORREF initial=GetTextColor(dc);
    SetLastError(1234);
    COLORREF returned=SetTextColor(dc,RGB(0,0,0));
    BOOL valid=returned==initial && GetLastError()==1234;
    DWORD text=rgb(GetTextColor(dc));
    COLORREF initialBg=GetBkColor(dc);
    returned=SetBkColor(dc,RGB(255,255,255)); valid &= returned==initialBg;
    DWORD bg=rgb(GetBkColor(dc));
    HBRUSH brush=CreateSolidBrush(RGB(255,255,255));
    LOGBRUSH lb={}; valid &= GetObjectW(brush,sizeof(lb),&lb)==sizeof(lb);
    HPEN pen=CreatePen(PS_SOLID,2,RGB(0,0,0));
    LOGPEN lp={}; valid &= GetObjectW(pen,sizeof(lp),&lp)==sizeof(lp) && lp.lopnWidth.x==2;
    DeleteObject(brush); DeleteObject(pen); DeleteDC(dc);
    return {text,bg,rgb(lb.lbColor),rgb(lp.lopnColor),valid};
}
