// Named 1cv8.exe only inside a disposable test directory. Never shipped as an app.
#include <windows.h>
#include <wchar.h>
int wmain() {
    wchar_t name[100];
    swprintf(name,100,L"Local\\ThemeModeExit-%lu",GetCurrentProcessId());
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,name);
    if (!event) return 2;
    DWORD result=WaitForSingleObject(event,30000);
    CloseHandle(event);
    return result==WAIT_OBJECT_0?0:3;
}
