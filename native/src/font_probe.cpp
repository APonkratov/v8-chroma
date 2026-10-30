// Editor font substitution. Loaded only when a font is explicitly selected.
#include <windows.h>
#include <shellapi.h>
#include <usp10.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "font_ligatures.h"

struct Config {
    DWORD size, points10;
    wchar_t source[LF_FACESIZE], family[LF_FACESIZE], logPath[520];
};
static_assert(sizeof(Config)==1176);
static_assert(sizeof(LOGFONTW)==92);
static Config config;
static SRWLOCK lock=SRWLOCK_INIT;
static bool enabled;
static volatile LONG started;
static unsigned long long replaced;
static DWORD generation=1, refreshKind, refreshWindows, refreshFailures;
static DWORD editorStatus=ERROR_NOT_READY, editorRefreshes, preservedEditors, lastEditorCount;
using Convert=void(*)(LOGFONTW*,const void*,int);
static Convert realConvert;
static decltype(&ScriptItemize) realItemize;
static unsigned long long ligatureItemizations;
struct Patch { void** slot; void* original; void* hook; };
static Patch patches[2];
static unsigned patchCount;

static void convert(LOGFONTW* result,const void* font,int dpi) {
    realConvert(result,font,dpi);
    DWORD error=GetLastError();
    AcquireSRWLockExclusive(&lock);
    if (enabled && !_wcsicmp(result->lfFaceName,config.source)) {
        wcscpy_s(result->lfFaceName,config.family);
        // abs_font_to_logfont has already converted points to device units.
        // Absolute size in points; Designer zoom remains independent.
        if (config.points10) result->lfHeight=-MulDiv(config.points10,dpi,720);
        result->lfWidth=0;
        ++replaced;
    }
    ReleaseSRWLockExclusive(&lock);
    SetLastError(error);
}
static HRESULT WINAPI itemize(const WCHAR* text,int count,int capacity,const SCRIPT_CONTROL* control,
    const SCRIPT_STATE* state,SCRIPT_ITEM* items,int* itemCount) {
    bool active;
    AcquireSRWLockShared(&lock); active=enabled; ReleaseSRWLockShared(&lock);
    HRESULT status=realItemize(text,count,capacity,control,state,items,itemCount);
    DWORD error=GetLastError();
    if (active && SUCCEEDED(status) && itemizeOperators(text,count,capacity,control,state,items,itemCount,realItemize)) {
        AcquireSRWLockExclusive(&lock); ++ligatureItemizations; ReleaseSRWLockExclusive(&lock);
    }
    SetLastError(error); return status;
}
static bool patch(HMODULE module,const char* provider,const char* name,void* hook,void** original) {
    auto base=reinterpret_cast<BYTE*>(module);
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64) return false;
    auto rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return false;
    auto imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+rva);
    for (;imports->Name;++imports) {
        if (_stricmp(reinterpret_cast<char*>(base+imports->Name),provider)) continue;
        if (!imports->OriginalFirstThunk) return false;
        auto names=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+imports->OriginalFirstThunk);
        auto slots=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+imports->FirstThunk);
        for (;names->u1.AddressOfData;++names,++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto imported=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(imported->Name),name)) continue;
            auto expected=reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(provider),name));
            auto slot=reinterpret_cast<void**>(&slots->u1.Function);
            if (!expected || *slot!=expected || patchCount==2) return false;
            *original=expected;
            DWORD old;
            if (!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old)) return false;
            InterlockedExchangePointer(slot,hook);
            patches[patchCount++]={slot,expected,hook};
            DWORD ignored;
            return VirtualProtect(slot,sizeof(void*),old,&ignored)!=FALSE;
        }
    }
    return false;
}
static void rollback() {
    while (patchCount) {
        auto& item=patches[--patchCount];
        DWORD old;
        if (VirtualProtect(item.slot,sizeof(void*),PAGE_READWRITE,&old)) {
            InterlockedCompareExchangePointer(item.slot,item.original,item.hook);
            DWORD ignored; VirtualProtect(item.slot,sizeof(void*),old,&ignored);
        }
    }
}
static bool sibling(HMODULE module) {
    wchar_t path[32768]{},exe[32768]{};
    if (!module || !GetModuleFileNameW(module,path,32768) || !GetModuleFileNameW(nullptr,exe,32768)) return false;
    auto a=wcsrchr(path,L'\\'),b=wcsrchr(exe,L'\\');
    if (!a || !b) return false;
    *a=0; *b=0;
    return !_wcsicmp(path,exe);
}
static HMODULE fontCore(const char** provider=nullptr) {
    auto graphics=GetModuleHandleW(L"grphcs.dll");
    if (!sibling(graphics)) return nullptr;
    auto base=reinterpret_cast<BYTE*>(graphics);
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64) return nullptr;
    auto rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return nullptr;
    auto imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+rva);
    for (;imports->Name;++imports) {
        auto name=reinterpret_cast<const char*>(base+imports->Name);
        if (_strnicmp(name,"core",4)) continue;
        auto core=GetModuleHandleA(name);
        if (!sibling(core)) continue;
        auto face=reinterpret_cast<BYTE*>(GetProcAddress(core,"?getFaceName@V8LogFont@core@@QEBAPEB_SXZ"));
        // LOGFONT-compatible prefix, including the face name at offset 28.
        const BYTE faceLayout[]={0x48,0x8d,0x41,0x1c,0xc3};
        if (!face || memcmp(face,faceLayout,sizeof(faceLayout)) ||
            !GetProcAddress(core,"?abs_font_to_logfont@core@@YAXAEAUV8LogFont@1@AEBUFont@1@H@Z")) continue;
        if (provider) *provider=name;
        return core;
    }
    return nullptr;
}
static bool supported() {
    int argc=0;
    auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    bool designer=argv && argc>1 && !_wcsicmp(argv[1],L"DESIGNER");
    if (argv) LocalFree(argv);
    return designer && fontCore();
}
static bool available(const wchar_t* family) {
    LOGFONTW lf{}; lf.lfHeight=-20; lf.lfCharSet=DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName,family);
    HDC dc=CreateCompatibleDC(nullptr);
    HFONT font=CreateFontIndirectW(&lf);
    if (!dc || !font) {
        if (dc) DeleteDC(dc);
        if (font) DeleteObject(font);
        return false;
    }
    auto previous=SelectObject(dc,font);
    wchar_t actual[LF_FACESIZE]{};
    bool ok=GetTextFaceW(dc,LF_FACESIZE,actual)>0 && !_wcsicmp(actual,family);
    SelectObject(dc,previous); DeleteObject(font); DeleteDC(dc);
    return ok;
}
static DWORD snapshot() {
    AcquireSRWLockExclusive(&lock);
    FILE* f=nullptr;
    _wfopen_s(&f,config.logPath,L"w");
    if (!f) { ReleaseSRWLockExclusive(&lock); return ERROR_OPEN_FAILED; }
    fwprintf(f,L"enabled=%u\nreplaced=%llu\nfamily=%ls\npoints10=%lu\ngeneration=%lu\nrefresh_kind=%lu\nrefresh_windows=%lu\nrefresh_failures=%lu\n",
        enabled?1:0,replaced,config.family,config.points10,generation,refreshKind,refreshWindows,refreshFailures);
    fwprintf(f,L"editor_status=%lu\neditor_refreshes=%lu\npreserved_editors=%lu\neditor_last_count=%lu\nligature_itemizations=%llu\n",
        editorStatus,editorRefreshes,preservedEditors,lastEditorCount,ligatureItemizations);
    bool failed=ferror(f)!=0;
    if (fclose(f)) failed=true;
    ReleaseSRWLockExclusive(&lock);
    return failed?ERROR_WRITE_FAULT:ERROR_SUCCESS;
}
static DWORD validate(void* argument) {
    auto incoming=static_cast<const Config*>(argument);
    if (!incoming || incoming->size!=sizeof(Config) || incoming->source[31] || incoming->family[31] ||
        incoming->logPath[519] || !incoming->source[0] || !incoming->family[0] || !incoming->logPath[0] ||
        (incoming->points10<80 || incoming->points10>320 || incoming->points10%5)) return ERROR_INVALID_PARAMETER;
    if (!supported()) return ERROR_NOT_SUPPORTED;
    if (!available(incoming->family)) return ERROR_NOT_FOUND;
    return ERROR_SUCCESS;
}
extern "C" __declspec(dllexport) DWORD WINAPI FontProbeApply(void* argument) {
    DWORD status=validate(argument); if (status) return status;
    if (!started || !patchCount) return ERROR_SERVICE_NOT_ACTIVE;
    AcquireSRWLockExclusive(&lock);
    config=*static_cast<const Config*>(argument); enabled=true; ++generation;
    ReleaseSRWLockExclusive(&lock);
    return snapshot();
}
struct Refresh { DWORD size,kind; };
#include "font_discovery.h"
static ResolvedEditor resolvedEditor;
static const ResolvedEditor* activeProfile;
static BYTE* activeEditor;
static BYTE* activeCore;
static BYTE* editorProfile() {
    auto module=GetModuleHandleW(L"txtedui.dll");
    auto core=fontCore();
    if (!sibling(module) || !core) return nullptr;
    auto base=reinterpret_cast<BYTE*>(module),coreBase=reinterpret_cast<BYTE*>(core);
    if (activeProfile && activeEditor==base && activeCore==coreBase) return base;
    ResolvedEditor found{};
    if (!discoverEditor(Image(module),Image(core),found)) return nullptr;
    resolvedEditor=found; activeEditor=base; activeCore=coreBase; activeProfile=&resolvedEditor;
    return base;
}
extern "C" __declspec(dllexport) DWORD WINAPI FontProbeCapabilities(void*) {
    if (!supported()) return 0;
    return 1|(editorProfile()?2:0);
}
static bool readable(const void* pointer,SIZE_T size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!pointer || !VirtualQuery(pointer,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
        (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    auto address=reinterpret_cast<ULONG_PTR>(pointer),start=reinterpret_cast<ULONG_PTR>(info.BaseAddress);
    return address>=start && address-start<info.RegionSize && size<=info.RegionSize-(address-start);
}
struct EditorRefs {
    void* items[256]{};
    unsigned count=0;
    ~EditorRefs() {
        for (unsigned i=0;i<count;++i)
            reinterpret_cast<ULONG(*)(void*)>((*reinterpret_cast<void***>(items[i]))[2])(items[i]);
    }
};
// Read the event source's recipient list under its own lock, just as its
// native notify implementation does. Capture owning references before unlock.
// Only known text-editor controllers are updated; other settings subscribers
// include the BSL document adapter, which resets selection and folds.
static DWORD collectEditors(BYTE* base,void* source,EditorRefs& refs) {
    auto core=activeCore;
    if (!core || !readable(source,0x80)) return ERROR_NOT_SUPPORTED;
    auto sourceTable=*reinterpret_cast<void***>(source);
    if (!readable(sourceTable,4*sizeof(void*)) ||
        !sourceLayout(Image(reinterpret_cast<HMODULE>(core)),static_cast<BYTE*>(sourceTable[3]))) return ERROR_NOT_SUPPORTED;
    auto sourceBytes=static_cast<BYTE*>(source);
    auto mutex=sourceBytes+0x20;
    auto mutexTable=*reinterpret_cast<void***>(mutex);
    if (!readable(mutexTable,5*sizeof(void*))) return ERROR_INVALID_DATA;
    using LockMethod=void(*)(void*);
    reinterpret_cast<LockMethod>(mutexTable[3])(mutex);
    struct Unlock { void* mutex; LockMethod method; ~Unlock() { method(mutex); } } unlock{mutex,reinterpret_cast<LockMethod>(mutexTable[4])};
    auto sentinel=*reinterpret_cast<BYTE**>(sourceBytes+0x50);
    auto buckets=*reinterpret_cast<BYTE**>(sourceBytes+0x60);
    auto mask=*reinterpret_cast<ULONG_PTR*>(sourceBytes+0x78);
    if (mask>0xfffff) return ERROR_INVALID_DATA;
    if (!buckets) return ERROR_INVALID_DATA;
    auto bucket=buckets+16*(*reinterpret_cast<const DWORD*>(base+activeProfile->event)&mask);
    if (!readable(bucket,16)) return ERROR_INVALID_DATA;
    auto node=*reinterpret_cast<BYTE**>(bucket);
    auto last=*reinterpret_cast<BYTE**>(bucket+8);
    if (node==sentinel) return ERROR_SUCCESS;
    if (!readable(last,8)) return ERROR_INVALID_DATA;
    auto end=*reinterpret_cast<BYTE**>(last);
    unsigned visited=0;
    for (;node!=end;node=*reinterpret_cast<BYTE**>(node)) {
        if (++visited>4096 || !readable(node,0x30)) return ERROR_INVALID_DATA;
        if (memcmp(node+0x10,base+activeProfile->event,sizeof(GUID))) continue;
        auto head=*reinterpret_cast<BYTE**>(node+0x20);
        auto expected=*reinterpret_cast<ULONG_PTR*>(node+0x28);
        if (expected>4096 || !readable(head,8)) return ERROR_INVALID_DATA;
        unsigned recipients=0;
        for (auto item=*reinterpret_cast<BYTE**>(head);item!=head;item=*reinterpret_cast<BYTE**>(item)) {
            if (++recipients>expected || !readable(item,0x28)) return ERROR_INVALID_DATA;
            auto recipient=*reinterpret_cast<void**>(item+0x20);
            if (!readable(recipient,sizeof(void*))) return ERROR_INVALID_DATA;
            auto table=*reinterpret_cast<void***>(recipient);
            bool known=false;
            for (unsigned i=0;i<activeProfile->tableCount;++i)
                if (table==reinterpret_cast<void**>(base+activeProfile->tables[i])) known=true;
            if (!known) continue;
            bool duplicate=false;
            for (unsigned i=0;i<refs.count;++i) if (refs.items[i]==recipient) duplicate=true;
            if (!duplicate) {
                if (refs.count==256) return ERROR_BUFFER_OVERFLOW;
                reinterpret_cast<ULONG(*)(void*)>(table[1])(recipient);
                refs.items[refs.count++]=recipient;
            }
        }
        return recipients==expected?ERROR_SUCCESS:ERROR_INVALID_DATA;
    }
    return ERROR_SUCCESS;
}
static DWORD updateEditorFonts(BYTE* base,void* source) {
    EditorRefs refs;
    DWORD status=collectEditors(base,source,refs); if (status) return status;
    for (unsigned i=0;i<refs.count;++i) {
        // The verified profiles use inline Font values (132/136 bytes),
        // with no destructor. The loaders use aligned stores.
        alignas(16) BYTE font[1024]{};
        if (activeProfile->loaderSlot) {
            auto table=*reinterpret_cast<void***>(refs.items[i]);
            reinterpret_cast<void*(*)(void*,void*)>(table[activeProfile->loaderSlot/sizeof(void*)])(refs.items[i],font);
        } else {
            auto construct=*reinterpret_cast<void(**)(void*)>(base+activeProfile->construct);
            construct(font);
            reinterpret_cast<void(*)(void*,void*)>(base+activeProfile->loadFont)(nullptr,font);
        }
        reinterpret_cast<void(*)(void*,const void*)>(base+activeProfile->setFont)(refs.items[i],font);
        AcquireSRWLockExclusive(&lock); ++preservedEditors; ++lastEditorCount; ReleaseSRWLockExclusive(&lock);
    }
    return ERROR_SUCCESS;
}
static volatile LONG dispatchBusy,dispatchPending;
static HWND dispatchWindow;
static UINT dispatchMessage;
static WPARAM dispatchToken;
static DWORD dispatchStatus;
static LRESULT CALLBACK editorHook(int code,WPARAM wp,LPARAM lp) {
    if (code==HC_ACTION) {
        auto message=reinterpret_cast<const CWPSTRUCT*>(lp);
        if (message->hwnd==dispatchWindow && message->message==dispatchMessage &&
            message->wParam==dispatchToken && InterlockedCompareExchange(&dispatchPending,2,1)==1) {
            DWORD status=ERROR_NOT_SUPPORTED;
            if (auto base=editorProfile()) {
                using Getter=void*(*)();
                auto source=reinterpret_cast<Getter>(base+activeProfile->getter)();
                status=source?updateEditorFonts(base,source):ERROR_NOT_READY;
            }
            dispatchStatus=status;
            InterlockedExchange(&dispatchPending,0);
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
static BOOL CALLBACK findEditorRoot(HWND window,LPARAM data) {
    DWORD pid=0; GetWindowThreadProcessId(window,&pid);
    if (pid==GetCurrentProcessId() && IsWindowVisible(window) && !GetWindow(window,GW_OWNER)) {
        *reinterpret_cast<HWND*>(data)=window; return FALSE;
    }
    return TRUE;
}
static DWORD refreshEditor() {
    if (InterlockedCompareExchange(&dispatchBusy,1,0)) return ERROR_BUSY;
    // A timed-out callback might still be executing. Never reuse its state.
    if (InterlockedCompareExchange(&dispatchPending,0,0)) {
        InterlockedExchange(&dispatchBusy,0); return ERROR_BUSY;
    }
    AcquireSRWLockExclusive(&lock); lastEditorCount=0; ReleaseSRWLockExclusive(&lock);
    DWORD status=ERROR_NOT_SUPPORTED;
    if (editorProfile()) {
        dispatchWindow=nullptr;
        EnumWindows(findEditorRoot,reinterpret_cast<LPARAM>(&dispatchWindow));
        dispatchMessage=RegisterWindowMessageW(L"V8Chroma.FontProbe.RefreshEditor.v1");
        DWORD thread=dispatchWindow?GetWindowThreadProcessId(dispatchWindow,nullptr):0;
        if (thread && dispatchMessage) {
            HHOOK hook=SetWindowsHookExW(WH_CALLWNDPROC,editorHook,nullptr,thread);
            if (hook) {
                ++dispatchToken; dispatchStatus=ERROR_NOT_READY;
                InterlockedExchange(&dispatchPending,1);
                DWORD_PTR result=0;
                if (SendMessageTimeoutW(dispatchWindow,dispatchMessage,dispatchToken,0,
                    SMTO_ABORTIFHUNG|SMTO_BLOCK,5000,&result)) status=dispatchStatus;
                else status=ERROR_TIMEOUT;
                UnhookWindowsHookEx(hook);
                InterlockedCompareExchange(&dispatchPending,0,1);
            } else status=GetLastError();
        } else status=ERROR_NOT_READY;
    }
    AcquireSRWLockExclusive(&lock);
    editorStatus=status; if (!status) ++editorRefreshes;
    ReleaseSRWLockExclusive(&lock);
    InterlockedExchange(&dispatchBusy,0);
    return status;
}
extern "C" __declspec(dllexport) DWORD WINAPI FontProbeRefresh(void* argument) {
    auto request=static_cast<const Refresh*>(argument);
    if (!request || request->size!=sizeof(Refresh) || request->kind!=4) return ERROR_INVALID_PARAMETER;
    if (!started || !patchCount) return ERROR_SERVICE_NOT_ACTIVE;
    DWORD status=refreshEditor();
    AcquireSRWLockExclusive(&lock);
    refreshKind=4; refreshWindows=status?0:1; refreshFailures=status?1:0;
    ReleaseSRWLockExclusive(&lock);
    DWORD written=snapshot();
    return status?status:written;
}
extern "C" __declspec(dllexport) DWORD WINAPI FontProbeStart(void* argument) {
    DWORD validation=validate(argument); if (validation) return validation;
    auto incoming=static_cast<const Config*>(argument);
    if (InterlockedCompareExchange(&started,1,0)) return ERROR_ALREADY_INITIALIZED;
    config=*incoming;
    DWORD logStatus=snapshot();
    if (logStatus) return logStatus;
    HMODULE graphics=GetModuleHandleW(L"grphcs.dll"), pin=nullptr;
    if (!graphics) return ERROR_MOD_NOT_FOUND;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&FontProbeStart),&pin)) return GetLastError();
    // Only the verified LOGFONT prefix is modified; newer V8LogFont may append fields.
    const char* provider=nullptr;
    if (!fontCore(&provider)) return ERROR_NOT_SUPPORTED;
    bool ok=patch(graphics,provider,"?abs_font_to_logfont@core@@YAXAEAUV8LogFont@1@AEBUFont@1@H@Z",
        reinterpret_cast<void*>(&convert),reinterpret_cast<void**>(&realConvert));
    // Ligatures are optional when the platform uses another shaping API.
    if (ok) patch(graphics,"USP10.dll","ScriptItemize",reinterpret_cast<void*>(&itemize),reinterpret_cast<void**>(&realItemize));
    AcquireSRWLockExclusive(&lock); enabled=ok; ReleaseSRWLockExclusive(&lock);
    if (!ok) { rollback(); return ERROR_PROC_NOT_FOUND; }
    DWORD status=snapshot();
    if (status) {
        AcquireSRWLockExclusive(&lock); enabled=false; ReleaseSRWLockExclusive(&lock);
        rollback();
    }
    return status;
}
extern "C" __declspec(dllexport) DWORD WINAPI FontProbeStop(void*) {
    AcquireSRWLockExclusive(&lock); enabled=false; ReleaseSRWLockExclusive(&lock);
    // Keep DLL and pass-through hooks resident: existing cached fonts remain valid.
    // Follow with FontProbeRefresh(kind=4) to recreate fonts in supported editors.
    return snapshot();
}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID) { return TRUE; }
