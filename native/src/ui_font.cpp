// Interface font engine. No platform files or OS font preferences are modified.
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "font_discovery.h"
#include "ui_font_policy.h"

struct Config { DWORD size; wchar_t family[32],log[520]; };
static_assert(sizeof(Config)==1108);
static Config config;
static SRWLOCK lock=SRWLOCK_INIT;
static volatile LONG started,styleNotifications,controlBusy;
static bool enabled;
static DWORD generation,refreshStatus=ERROR_NOT_READY,reopenDialogs;
static unsigned patchCount;
using Convert=void(*)(LOGFONTW*,const void*,int);
using Stock=void(*)(LOGFONTW*,const void*,int,int);
static Convert realConvert;
static Stock realStock;
static decltype(&CreateFontIndirectW) realCreate;
struct Patch { void** slot; void* original; void* hook; };
static Patch patches[6];
#include "ui_font_refresh.h"

static bool designer() {
    int count=0; auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    bool result=args && count>1 && !_wcsicmp(args[1],L"DESIGNER");
    if (args) LocalFree(args);
    return result;
}
static bool sibling(HMODULE module) {
    wchar_t file[32768]{},exe[32768]{};
    if (!module || !GetModuleFileNameW(module,file,32768) || !GetModuleFileNameW(nullptr,exe,32768)) return false;
    auto a=wcsrchr(file,L'\\'),b=wcsrchr(exe,L'\\'); if (!a || !b) return false;
    *a=0; *b=0; return !_wcsicmp(file,exe);
}
static HMODULE coreModule(const char** provider=nullptr) {
    for (auto name:{"core83.dll","core85.dll"}) {
        auto core=GetModuleHandleA(name); if (!sibling(core)) continue;
        auto face=reinterpret_cast<BYTE*>(GetProcAddress(core,"?getFaceName@V8LogFont@core@@QEBAPEB_SXZ"));
        const BYTE layout[]={0x48,0x8d,0x41,0x1c,0xc3};
        if (face && !memcmp(face,layout,sizeof(layout))) { if (provider) *provider=name; return core; }
    }
    return nullptr;
}
static void map(LOGFONTW& font) {
    AcquireSRWLockShared(&lock);
    if (enabled) substituteInterfaceFont(font,config.family);
    ReleaseSRWLockShared(&lock);
}
static HFONT WINAPI create(const LOGFONTW* source) {
    if (!source) return realCreate(source);
    LOGFONTW font=*source; map(font); return realCreate(&font);
}
static void convert(LOGFONTW* result,const void* source,int dpi) {
    realConvert(result,source,dpi); DWORD error=GetLastError();
    // Font kind 1 is a platform stock font. Explicit names, editor fonts and
    // document style references retain their own typography even for UI aliases.
    if (readable(source,sizeof(DWORD)) && *static_cast<const DWORD*>(source)==1) map(*result);
    SetLastError(error);
}
static void stock(LOGFONTW* result,const void* source,int dpi,int mode) {
    realStock(result,source,dpi,mode); DWORD error=GetLastError();
    if (readable(source,sizeof(DWORD)) && *static_cast<const DWORD*>(source)==1) map(*result);
    SetLastError(error);
}
static bool patch(HMODULE module,const char* provider,const char* name,void* hook,void** original) {
    Image image(module); if (!image.nt) return false;
    auto rva=image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return false;
    auto imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image.base+rva);
    for (;image.contains(imports,sizeof(*imports)) && imports->Name;++imports) {
        if (!image.contains(image.base+imports->Name,1) || _stricmp(reinterpret_cast<char*>(image.base+imports->Name),provider) || !imports->OriginalFirstThunk) continue;
        auto names=reinterpret_cast<IMAGE_THUNK_DATA64*>(image.base+imports->OriginalFirstThunk);
        auto slots=reinterpret_cast<IMAGE_THUNK_DATA64*>(image.base+imports->FirstThunk);
        for (;image.contains(names,sizeof(*names)) && image.contains(slots,sizeof(*slots)) && names->u1.AddressOfData;++names,++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) || !image.contains(image.base+names->u1.AddressOfData,sizeof(IMAGE_IMPORT_BY_NAME))) continue;
            auto item=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image.base+names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(item->Name),name)) continue;
            auto expected=reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(provider),name));
            auto slot=reinterpret_cast<void**>(&slots->u1.Function);
            if (!expected || *slot!=expected || patchCount==6 || (*original && *original!=expected)) return false;
            DWORD old;
            if (!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old)) return false;
            if (!*original) *original=expected;
            InterlockedExchangePointer(slot,hook); patches[patchCount++]={slot,expected,hook};
            DWORD ignored; VirtualProtect(slot,sizeof(void*),old,&ignored); return true;
        }
    }
    return false;
}
static void rollback() {
    while (patchCount) {
        auto& p=patches[--patchCount]; DWORD old;
        if (VirtualProtect(p.slot,sizeof(void*),PAGE_READWRITE,&old)) {
            InterlockedCompareExchangePointer(p.slot,p.original,p.hook);
            DWORD ignored; VirtualProtect(p.slot,sizeof(void*),old,&ignored);
        }
    }
}
static BOOL CALLBACK root(HWND window,LPARAM value) {
    DWORD pid=0; GetWindowThreadProcessId(window,&pid);
    if (pid==GetCurrentProcessId() && IsWindowVisible(window) && !GetWindow(window,GW_OWNER) && (GetWindowLongPtrW(window,GWL_STYLE)&WS_CAPTION)) {
        *reinterpret_cast<HWND*>(value)=window; return FALSE;
    }
    return TRUE;
}
static HWND mainWindow() { HWND result=nullptr; EnumWindows(root,reinterpret_cast<LPARAM>(&result)); return result; }
static BOOL CALLBACK dialog(HWND window,LPARAM value) {
    DWORD pid=0; GetWindowThreadProcessId(window,&pid);
    if (pid==GetCurrentProcessId() && IsWindowVisible(window) && GetWindow(window,GW_OWNER) && (GetWindowLongPtrW(window,GWL_STYLE)&WS_CAPTION))
        *reinterpret_cast<DWORD*>(value)=1;
    return TRUE;
}
static DWORD preflight() {
    if (!designer() || !coreModule() || !sibling(GetModuleHandleW(L"frame.dll")) || !sibling(GetModuleHandleW(L"grphcs.dll"))) return ERROR_NOT_SUPPORTED;
    auto window=mainWindow(); return window?notifyStyle(window,false):ERROR_NOT_READY;
}
static DWORD validate(void* argument) {
    auto incoming=static_cast<const Config*>(argument);
    if (!incoming || incoming->size!=sizeof(Config) || incoming->family[31] || incoming->log[519] || !incoming->log[0]) return ERROR_INVALID_PARAMETER;
    if (incoming->family[0]) {
        LOGFONTW lf{}; lf.lfHeight=-16; lf.lfCharSet=DEFAULT_CHARSET; wcscpy_s(lf.lfFaceName,incoming->family);
        HDC dc=CreateCompatibleDC(nullptr); HFONT font=CreateFontIndirectW(&lf);
        if (!dc || !font) { if (dc) DeleteDC(dc); if (font) DeleteObject(font); return ERROR_NOT_ENOUGH_MEMORY; }
        auto previous=SelectObject(dc,font); wchar_t actual[32]{};
        bool ok=GetTextFaceW(dc,32,actual)>0 && !_wcsicmp(actual,incoming->family);
        SelectObject(dc,previous); DeleteObject(font); DeleteDC(dc);
        if (!ok) return ERROR_NOT_FOUND;
    }
    return 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI UiFontSnapshot(void*) {
    AcquireSRWLockShared(&lock);
    FILE* f=nullptr; _wfopen_s(&f,config.log,L"w, ccs=UTF-8");
    if (!f) { ReleaseSRWLockShared(&lock); return ERROR_OPEN_FAILED; }
    fwprintf(f,L"enabled=%u\nfamily=%ls\ngeneration=%lu\npatches=%u\nrefresh_status=%lu\nstyle_notifications=%ld\nreopen_dialogs=%lu\n",
        enabled,config.family,generation,patchCount,refreshStatus,InterlockedCompareExchange(&styleNotifications,0,0),reopenDialogs);
    bool bad=ferror(f)!=0; if (fclose(f)) bad=true;
    ReleaseSRWLockShared(&lock); return bad?ERROR_WRITE_FAULT:0;
}
extern "C" __declspec(dllexport) DWORD WINAPI UiFontCapabilities(void*) {
    return designer() && coreModule() && sibling(GetModuleHandleW(L"grphcs.dll")) && sibling(GetModuleHandleW(L"frame.dll")) && resolveStyle()?3:0;
}
extern "C" __declspec(dllexport) DWORD WINAPI UiFontApply(void* argument) {
    DWORD status=validate(argument); if (status) return status;
    if (!InterlockedCompareExchange(&started,0,0) || !patchCount) return ERROR_SERVICE_NOT_ACTIVE;
    if (InterlockedCompareExchange(&controlBusy,1,0)) return ERROR_BUSY;
    status=preflight();
    auto incoming=static_cast<const Config*>(argument);
    if (!status && _wcsicmp(incoming->log,config.log)) status=ERROR_INVALID_PARAMETER;
    if (!status) {
        AcquireSRWLockExclusive(&lock); config=*incoming; enabled=config.family[0]!=0; ++generation; refreshStatus=ERROR_NOT_READY; ReleaseSRWLockExclusive(&lock);
        status=UiFontSnapshot(nullptr);
    }
    InterlockedExchange(&controlBusy,0); return status;
}
extern "C" __declspec(dllexport) DWORD WINAPI UiFontStart(void* argument) {
    DWORD status=validate(argument); if (status) return status;
    if (InterlockedCompareExchange(&controlBusy,1,0)) return ERROR_BUSY;
    if (InterlockedCompareExchange(&started,0,0)) { InterlockedExchange(&controlBusy,0); return ERROR_ALREADY_INITIALIZED; }
    status=preflight();
    if (!status) {
        HMODULE pin=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&UiFontStart),&pin)) status=GetLastError();
    }
    if (!status) {
        config=*static_cast<const Config*>(argument); status=UiFontSnapshot(nullptr);
    }
    if (!status) {
        const char* provider=nullptr; coreModule(&provider);
        auto frame=GetModuleHandleW(L"frame.dll"),graphics=GetModuleHandleW(L"grphcs.dll");
        bool ok=patch(frame,"gdi32.dll","CreateFontIndirectW",reinterpret_cast<void*>(&create),reinterpret_cast<void**>(&realCreate));
        ok &= patch(graphics,provider,"?adjust_stock_font@core@@YAXAEAUV8LogFont@1@AEBUFont@1@HW4CompatibilityMode@1@@Z",reinterpret_cast<void*>(&stock),reinterpret_cast<void**>(&realStock));
        // Never own graphics' absolute-font import: the editor font engine may
        // be enabled later in this same process and needs that independent slot.
        patch(frame,provider,"?abs_font_to_logfont@core@@YAXAEAUV8LogFont@1@AEBUFont@1@H@Z",reinterpret_cast<void*>(&convert),reinterpret_cast<void**>(&realConvert));
        for (auto module:{frame,graphics}) {
            patch(module,provider,"?adjust_stock_font@core@@YAXAEAUV8LogFont@1@AEBUFont@1@HW4CompatibilityMode@1@@Z",reinterpret_cast<void*>(&stock),reinterpret_cast<void**>(&realStock));
        }
        if (!ok) { rollback(); status=ERROR_NOT_SUPPORTED; }
        else {
            AcquireSRWLockExclusive(&lock); enabled=config.family[0]!=0; ++generation; ReleaseSRWLockExclusive(&lock);
            InterlockedExchange(&started,1); status=UiFontSnapshot(nullptr);
            if (status) { AcquireSRWLockExclusive(&lock); enabled=false; ReleaseSRWLockExclusive(&lock); rollback(); InterlockedExchange(&started,0); }
        }
    }
    InterlockedExchange(&controlBusy,0); return status;
}
struct Refresh { DWORD size,kind; };
extern "C" __declspec(dllexport) DWORD WINAPI UiFontRefresh(void* argument) {
    auto request=static_cast<const Refresh*>(argument);
    if (!request || request->size!=sizeof(Refresh) || request->kind!=5) return ERROR_INVALID_PARAMETER;
    if (!InterlockedCompareExchange(&started,0,0) || !patchCount) return ERROR_SERVICE_NOT_ACTIVE;
    if (InterlockedCompareExchange(&controlBusy,1,0)) return ERROR_BUSY;
    auto window=mainWindow(); DWORD dialogs=0; EnumWindows(dialog,reinterpret_cast<LPARAM>(&dialogs));
    DWORD status=window?notifyStyle(window):ERROR_NOT_READY;
    if (window && !status) RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
    AcquireSRWLockExclusive(&lock); refreshStatus=status; reopenDialogs=dialogs; ReleaseSRWLockExclusive(&lock);
    DWORD written=UiFontSnapshot(nullptr); InterlockedExchange(&controlBusy,0); return status?status:written;
}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID) { return TRUE; }
