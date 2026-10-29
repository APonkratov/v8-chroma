#include "protocol.h"
#include "window_theme.h"
#include "raster_theme.h"
#include "editor_selection.h"
#include "icon_theme.h"
#include <vector>
#include <stdio.h>
#include <math.h>
#include <wchar.h>
#include <string.h>

static ProbeConfig cfg;
static SRWLOCK themeLock=SRWLOCK_INIT;
static thread_local unsigned readDepth;
struct ThemeRead {
    ThemeRead() { DWORD error=GetLastError(); if (!readDepth++) AcquireSRWLockShared(&themeLock); SetLastError(error); }
    ~ThemeRead() { DWORD error=GetLastError(); if (!--readDepth) ReleaseSRWLockShared(&themeLock); SetLastError(error); }
};
static DWORD generation=1;
static DWORD paleCaptionInk=0xFFFFFFFF;
static void refreshCaptionInk() {
    paleCaptionInk=0xFFFFFFFF;
    // In this platform profile FFFAD9 is shared by hover fills and the disabled
    // Apply caption. Only pale palettes need the caption corrected to text ink.
    for (DWORD i=0;i<cfg.mapCount;++i) if (cfg.maps[i].kind==CairoRgb && cfg.maps[i].source==0xFFFAD9) {
        DWORD color=cfg.maps[i].target;
        unsigned light=(((color>>16)&255)*54+((color>>8)&255)*183+(color&255)*19)/256;
        if (light>=160) paleCaptionInk=color;
        break;
    }
}
static volatile LONG active, started, stopping;
static SRWLOCK tableLock = SRWLOCK_INIT, snapshotLock = SRWLOCK_INIT;
static const unsigned capacity = 8192;
struct Row { DWORD kind, rgb, alpha, target, browser; unsigned long long count; };
static Row rows[capacity];
static unsigned long long dropped;
static unsigned patchCount;
static unsigned browserPatchCount, iconPatchCount;
static DWORD iconStatus=ERROR_NOT_SUPPORTED;
static void (__cdecl *realIconSource)(void*,void*),(*realIconMask)(void*,void*);
static void (__cdecl *realIconMaskSurface)(void*,void*,double,double);
static void __cdecl hookedIconSource(void* c,void* p) { ThemeRead guard; drawIconSource(c,p,realIconSource); }
static void __cdecl hookedIconMask(void* c,void* p) { ThemeRead guard; drawIconMask(c,p,realIconMask); }
static void __cdecl hookedIconMaskSurface(void* c,void* p,double x,double y) { ThemeRead guard; drawIconMaskSurface(c,p,x,y,realIconMaskSurface); }
static bool iconProfileSupported() {
    wchar_t exe[32768];
    if (!GetModuleFileNameW(nullptr,exe,32768)) return false;
    DWORD ignored=0, size=GetFileVersionInfoSizeW(exe,&ignored);
    if (!size || size>1024*1024) return false;
    std::vector<BYTE> bytes(size);
    if (!GetFileVersionInfoW(exe,0,size,bytes.data())) return false;
    VS_FIXEDFILEINFO* info=nullptr; UINT length=0;
    return VerQueryValueW(bytes.data(),L"\\",reinterpret_cast<void**>(&info),&length) &&
        length>=sizeof(*info) && info->dwSignature==0xFEEF04BD &&
        info->dwFileVersionMS==MAKELONG(3,8) && info->dwFileVersionLS==MAKELONG(2325,27);
}
static SRWLOCK lifecycleLock=SRWLOCK_INIT;
static const char* apiNames[] = {"", "SetTextColor", "SetBkColor", "CreateSolidBrush", "CreatePen",
    "cairo_set_source_rgb", "cairo_set_source_rgba", "cairo_pattern_add_color_stop_rgba", "cairo_mesh_pattern_set_corner_color_rgba"};
static DWORD fromColor(COLORREF color);

static DWORD record(DWORD kind, DWORD rgb, DWORD alpha,bool browser=false) {
    DWORD lastError = GetLastError();
    DWORD mapped = rgb;
    if (InterlockedCompareExchange(&active, 0, 0)) {
        if (cfg.mode == 1) {
            for (DWORD i=0; i<cfg.mapCount; ++i)
                if (cfg.maps[i].kind == kind && cfg.maps[i].source == rgb) { mapped = cfg.maps[i].target; break; }
            if (browser && rgb==fromColor(GetSysColor(COLOR_HIGHLIGHT))) mapped=cfg.colors.selectionBackground;
            if (browser && (rgb==0x0000EE || rgb==0x0000FF || rgb==0x000080 || rgb==0x00008B || rgb==0x551A8B || rgb==0x800080)) {
                // Reuse the existing profile's accent binding, keeping protocol v2.
                mapped=cfg.colors.glyphForeground;
                for (DWORD i=0;i<cfg.mapCount;++i)
                    if (cfg.maps[i].kind==ColorStop && cfg.maps[i].source==0xAA9600) { mapped=cfg.maps[i].target; break; }
            }
            if (browser && (rgb==0xF6F3DF || rgb==0xFAF9F3 || rgb==0xFAFAD2)) mapped=cfg.colors.rasterBackground;
            if (browser && rgb==0xB22222) {
                mapped=cfg.colors.glyphForeground;
                for (DWORD i=0;i<cfg.mapCount;++i)
                    if (cfg.maps[i].kind==CairoRgba && cfg.maps[i].source==0xFF0000) { mapped=cfg.maps[i].target; break; }
            }
            if (browser && (rgb==0xBDB79B || rgb==0xDCDCDC)) mapped=cfg.colors.border;
        }
        AcquireSRWLockExclusive(&tableLock);
        unsigned slot = (rgb * 2654435761u + kind * 2246822519u + alpha + (browser?7919u:0u)) % capacity;
        bool stored = false;
        for (unsigned i=0; i<64; ++i) {
            Row &r = rows[(slot+i)%capacity];
            if (!r.count || (r.kind==kind && r.rgb==rgb && r.alpha==alpha && r.browser==(DWORD)browser)) {
                r.kind=kind; r.rgb=rgb; r.alpha=alpha; r.target=mapped; r.browser=browser; ++r.count; stored=true; break;
            }
        }
        if (!stored) ++dropped;
        ReleaseSRWLockExclusive(&tableLock);
    }
    SetLastError(lastError);
    return mapped;
}
static DWORD fromColor(COLORREF c) { return (GetRValue(c)<<16) | (GetGValue(c)<<8) | GetBValue(c); }
static COLORREF toColor(DWORD c) { return RGB(c>>16, (c>>8)&255, c&255); }
static COLORREF mapColor(DWORD kind, COLORREF color) {
    if (color & 0xFF000000u) return color; // Palette-index and special COLORREF values are not plain RGB.
    DWORD rgb=fromColor(color), mapped=record(kind,rgb,255);
    return mapped==rgb ? color : toColor(mapped);
}
static unsigned channel(double n) { return (unsigned)(fmax(0., fmin(1., n))*255.+0.5); }
static void mapRgb(DWORD kind, double &r, double &g, double &b, double a) {
    DWORD rgb=(channel(r)<<16)|(channel(g)<<8)|channel(b);
    DWORD mapped=record(kind, rgb, channel(a));
    // Diagnostic mode and unknown colors keep the original floating point values exactly.
    if (mapped != rgb) { r=((mapped>>16)&255)/255.; g=((mapped>>8)&255)/255.; b=(mapped&255)/255.; }
}
static decltype(&SetTextColor) realText;
static decltype(&SetBkColor) realBackground;
static decltype(&CreateSolidBrush) realBrush;
static decltype(&CreatePen) realPen;
static decltype(&GetSysColor) realSystemColor;
using RgbFn = void (__cdecl *)(void*, double,double,double);
using RgbaFn = void (__cdecl *)(void*, double,double,double,double);
using StopFn = void (__cdecl *)(void*, double,double,double,double,double);
using MeshFn = void (__cdecl *)(void*, unsigned,double,double,double,double);
using GlyphFn = void (__cdecl *)(void*, const void*,int);
using SourceFn = void* (__cdecl *)(void*);
using PatternFn = int (__cdecl *)(void*,double*,double*,double*,double*);
static RgbFn realRgb; static RgbaFn realRgba; static StopFn realStop; static MeshFn realMesh;
static GlyphFn realGlyphs; static SourceFn getSource; static PatternFn getRgba;
static void(*saveContext)(void*); static void(*restoreContext)(void*);
using DrawFn=void(__cdecl*)(void*);
using PaintAlphaFn=void(__cdecl*)(void*,double);
static DrawFn realPaint,realFill,realFillPreserve;
static PaintAlphaFn realPaintAlpha;
static void drawThemed(void* context,DrawFn function) {
    ThemeRead guard;
    DWORD last=GetLastError();
    bool selection=(cfg.mode && active && (function==realFill || function==realFillPreserve))?
        beginEditorSelectionFill(context,cfg.colors):false;
    bool uiBand=(!selection && cfg.mode && active && (function==realFill || function==realFillPreserve))?
        beginUiHighlightFill(context,cfg.colors):false;
    void* previous=(cfg.mode && active)?prepareRasterSource(context):nullptr;
    SetLastError(last); function(context); last=GetLastError();
    restoreRasterSource(context,previous); endEditorSelectionFill(context,selection || uiBand); SetLastError(last);
}
static void __cdecl hookedPaint(void* context) { drawThemed(context,realPaint); }
static void __cdecl hookedFill(void* context) { drawThemed(context,realFill); }
static void __cdecl hookedFillPreserve(void* context) { drawThemed(context,realFillPreserve); }
static void __cdecl hookedPaintAlpha(void* context,double alpha) {
    ThemeRead guard;
    DWORD last=GetLastError();
    void* previous=(cfg.mode && active)?prepareRasterSource(context):nullptr;
    SetLastError(last); realPaintAlpha(context,alpha); last=GetLastError();
    restoreRasterSource(context,previous); SetLastError(last);
}
template<typename T> static T symbol(HMODULE module,const char* name) {
    auto address=GetProcAddress(module,name);
    T function=nullptr;
    static_assert(sizeof(function)==sizeof(address));
    memcpy(&function,&address,sizeof(function));
    return function;
}
static COLORREF WINAPI hookedText(HDC h,COLORREF c) { ThemeRead guard; return realText(h,mapColor(Text,c)); }
static COLORREF WINAPI hookedBackground(HDC h,COLORREF c) { ThemeRead guard; return realBackground(h,mapColor(Background,c)); }
static HBRUSH WINAPI hookedBrush(COLORREF c) { ThemeRead guard; return realBrush(mapColor(Brush,c)); }
static HPEN WINAPI hookedPen(int style,int width,COLORREF c) { ThemeRead guard; return realPen(style,width,mapColor(Pen,c)); }
static DWORD WINAPI hookedSystemColor(int index) {
    ThemeRead guard;
    if (cfg.mode && active) {
        switch (index) {
        case COLOR_ACTIVECAPTION: case COLOR_INACTIVECAPTION:
        case COLOR_GRADIENTACTIVECAPTION: case COLOR_GRADIENTINACTIVECAPTION:
            return toColor(cfg.colors.captionBackground);
        case COLOR_CAPTIONTEXT: case COLOR_INACTIVECAPTIONTEXT:
            return toColor(cfg.colors.captionText);
        case COLOR_SCROLLBAR: return toColor(cfg.colors.rasterBackground);
        default: break;
        }
    }
    return realSystemColor(index);
}
static void __cdecl hookedRgb(void* h,double r,double g,double b) {
    ThemeRead guard;
    DWORD original=(channel(r)<<16)|(channel(g)<<8)|channel(b);
    mapRgb(CairoRgb,r,g,b,1.); realRgb(h,r,g,b);
    if (cfg.mode && active) { DWORD last=GetLastError(); rememberEditorSource(h,original,false); SetLastError(last); }
}
static void __cdecl hookedRgba(void* h,double r,double g,double b,double a) {
    ThemeRead guard;
    DWORD original=(channel(r)<<16)|(channel(g)<<8)|channel(b);
    mapRgb(CairoRgba,r,g,b,a); realRgba(h,r,g,b,a);
    if (cfg.mode && active) { DWORD last=GetLastError(); rememberEditorSource(h,original); SetLastError(last); }
}
static void __cdecl hookedStop(void* h,double o,double r,double g,double b,double a) { ThemeRead guard; mapRgb(ColorStop,r,g,b,a); realStop(h,o,r,g,b,a); }
static void __cdecl hookedMesh(void* h,unsigned c,double r,double g,double b,double a) { ThemeRead guard; mapRgb(MeshColor,r,g,b,a); realMesh(h,c,r,g,b,a); }
static void __cdecl hookedGlyphs(void* context,const void* glyphs,int count) {
    ThemeRead guard;
    DWORD last=GetLastError();
    double r,g,b,a;
    bool change=false;
    DWORD foreground=cfg.colors.glyphForeground;
    if (cfg.mode && InterlockedCompareExchange(&active,0,0) &&
        getRgba(getSource(context),&r,&g,&b,&a)==0) {
        DWORD current=(channel(r)<<16)|(channel(g)<<8)|channel(b);
        // Selection blue is also used for hot/pressed captions by this platform.
        // Mapping it as a fill makes glyphs disappear on the same selection fill.
        change=current==cfg.colors.glyphBackground || current==cfg.colors.rasterBackground ||
            current==cfg.colors.selectionBackground || current==paleCaptionInk;
        if (current==cfg.colors.selectionBackground) foreground=cfg.colors.selectionForeground;
        if (editorSelectionGlyph(context)) { change=true; foreground=cfg.colors.selectionForeground; }
    }
    if (change) {
        if (saveContext && restoreContext) saveContext(context);
        realRgba(context,((foreground>>16)&255)/255.,((foreground>>8)&255)/255.,(foreground&255)/255.,a);
    }
    SetLastError(last); realGlyphs(context,glyphs,count); last=GetLastError();
    if (change) {
        if (saveContext && restoreContext) restoreContext(context);
        else realRgba(context,r,g,b,a); // Minimal mock Cairo lacks state stack functions.
    }
    SetLastError(last);
}

// WebKit keeps its own Cairo imports. Its HTML is outside the editor's raster
// and font heuristics: only solid colors and contrasting glyph ink are changed.
static void browserColor(DWORD kind,double& r,double& g,double& b,double alpha) {
    DWORD original=(channel(r)<<16)|(channel(g)<<8)|channel(b);
    DWORD mapped=record(kind,original,channel(alpha),true);
    if (mapped!=original) { r=((mapped>>16)&255)/255.; g=((mapped>>8)&255)/255.; b=(mapped&255)/255.; }
}
static void __cdecl browserRgb(void* context,double r,double g,double b) {
    ThemeRead guard; browserColor(CairoRgb,r,g,b,1.); realRgb(context,r,g,b);
}
static void __cdecl browserRgba(void* context,double r,double g,double b,double a) {
    ThemeRead guard; browserColor(CairoRgba,r,g,b,a); realRgba(context,r,g,b,a);
}
static void __cdecl browserStop(void* pattern,double offset,double r,double g,double b,double a) {
    ThemeRead guard; browserColor(ColorStop,r,g,b,a); realStop(pattern,offset,r,g,b,a);
}
static void browserDraw(void* context,DrawFn function) {
    ThemeRead guard;
    DWORD last=GetLastError();
    void* previous=(cfg.mode && active)?prepareRasterSource(context):nullptr;
    SetLastError(last); function(context); last=GetLastError();
    restoreRasterSource(context,previous); SetLastError(last);
}
static void __cdecl browserPaint(void* context) { browserDraw(context,realPaint); }
static void __cdecl browserFill(void* context) { browserDraw(context,realFill); }
static void __cdecl browserFillPreserve(void* context) { browserDraw(context,realFillPreserve); }
static void __cdecl browserPaintAlpha(void* context,double alpha) {
    ThemeRead guard;
    DWORD last=GetLastError();
    void* previous=(cfg.mode && active)?prepareRasterSource(context):nullptr;
    SetLastError(last); realPaintAlpha(context,alpha); last=GetLastError();
    restoreRasterSource(context,previous); SetLastError(last);
}
static void __cdecl browserGlyphs(void* context,const void* glyphs,int count) {
    ThemeRead guard;
    DWORD last=GetLastError();
    double r=0.,g=0.,b=0.,a=0.;
    bool changed=cfg.mode && active && saveContext && restoreContext &&
        getRgba(getSource(context),&r,&g,&b,&a)==0 &&
        ((channel(r)<<16)|(channel(g)<<8)|channel(b))==cfg.colors.glyphBackground;
    if (changed) {
        saveContext(context);
        DWORD ink=cfg.colors.selectionForeground;
        realRgba(context,((ink>>16)&255)/255.,((ink>>8)&255)/255.,(ink&255)/255.,a);
    }
    SetLastError(last); realGlyphs(context,glyphs,count); last=GetLastError();
    if (changed) restoreContext(context);
    SetLastError(last);
}

struct Patch { void** slot; void* original; void* replacement; };
static Patch patches[32];
static bool patchImport(HMODULE module,const char* provider,const char* name,void* hook,void** original) {
    auto base=(BYTE*)module;
    auto dos=(IMAGE_DOS_HEADER*)base;
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
    auto nt=(IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);
    if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64) return false;
    DWORD rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return false;
    auto descriptor=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);
    for (;descriptor->Name;++descriptor) {
        if (_stricmp((char*)(base+descriptor->Name),provider)) continue;
        if (!descriptor->OriginalFirstThunk) return false;
        auto names=(IMAGE_THUNK_DATA64*)(base+descriptor->OriginalFirstThunk);
        auto iat=(IMAGE_THUNK_DATA64*)(base+descriptor->FirstThunk);
        for (;names->u1.AddressOfData;++names,++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto imported=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if (strcmp((char*)imported->Name,name)) continue;
            auto slot=(void**)&iat->u1.Function;
            void* expected=(void*)GetProcAddress(GetModuleHandleA(provider),name);
            if (!expected || *slot!=expected || patchCount>=32) return false;
            *original=expected;
            DWORD protection;
            if (!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection)) return false;
            InterlockedExchangePointer(slot,hook);
            DWORD ignored;
            bool restored=VirtualProtect(slot,sizeof(void*),protection,&ignored)!=FALSE;
            patches[patchCount++]={slot,expected,hook};
            return restored;
        }
    }
    return false;
}
static void rollback(unsigned first=0) {
    for (unsigned i=first;i<patchCount;++i) {
        DWORD old;
        if (VirtualProtect(patches[i].slot,sizeof(void*),PAGE_READWRITE,&old)) {
            InterlockedCompareExchangePointer(patches[i].slot,patches[i].original,patches[i].replacement);
            DWORD ignored; VirtualProtect(patches[i].slot,sizeof(void*),old,&ignored);
        }
    }
    patchCount=first;
}
static DWORD snapshotUnlocked() {
    AcquireSRWLockExclusive(&snapshotLock);
    Row* copy=(Row*)HeapAlloc(GetProcessHeap(),0,sizeof(rows));
    if (!copy) { ReleaseSRWLockExclusive(&snapshotLock); return ERROR_NOT_ENOUGH_MEMORY; }
    AcquireSRWLockShared(&tableLock);
    memcpy(copy,rows,sizeof(rows));
    auto lost=dropped;
    ReleaseSRWLockShared(&tableLock);
    wchar_t temp[530]; swprintf(temp,530,L"%ls.tmp",cfg.logPath);
    FILE* file=_wfopen(temp,L"wb");
    DWORD result=ERROR_SUCCESS;
    if (file) {
        fprintf(file,"# theme-probe-v3\n# mode=%s\n# theme=%s\n# protocol=3\n# generation=%lu\n# patched_imports=%u\n# browser_patched_imports=%u\n# themed_windows=%u\n# raster_supported=%u\n# raster_changes=%llu\n# hover_frames=%llu\n# selection_supported=%u\n# selection_fills=%llu\n# selection_glyphs=%llu\n# dropped=%llu\nmodule,api,source_rgb,alpha,calls,target_rgb\n",cfg.mode?"theme":"diagnostic",cfg.themeId,generation,patchCount,browserPatchCount,themedWindowCount(),rasterSupported()?1:0,rasterChanges(),rasterHoverFrames(),editorSelectionSupported()?1:0,editorSelectionFills(),editorSelectionGlyphs(),lost);
        auto icons=iconStats();
        fprintf(file,"# base_patched_imports=%u\n# icon_patched_imports=%u\n# icon_status=%lu\n# icon_mode=%u\n# icon_pack=%u\n# icon_cache_bytes=%llu\n# icon_cache_entries=%u\n# icon_calls=%llu\n# icon_matches=%llu\n",patchCount-iconPatchCount,iconPatchCount,iconStatus,icons.mode,icons.mode?1:0,icons.bytes,icons.cached,icons.calls,icons.matches);
        for (unsigned i=0;i<capacity;++i) if (copy[i].count) {
            auto &r=copy[i]; fprintf(file,"%s,%s,%06lX,%lu,%llu,%06lX\n",r.browser?"WebKit.dll":r.kind<=Pen?"frame.dll":"grphcs.dll",apiNames[r.kind],r.rgb,r.alpha,r.count,r.target);
        }
        if (ferror(file)) result=ERROR_WRITE_FAULT;
        if (fclose(file)) result=ERROR_WRITE_FAULT;
        if (!result && !MoveFileExW(temp,cfg.logPath,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) result=GetLastError();
    } else result=ERROR_OPEN_FAILED;
    HeapFree(GetProcessHeap(),0,copy);
    ReleaseSRWLockExclusive(&snapshotLock);
    return result;
}
extern "C" __declspec(dllexport) DWORD WINAPI ThemeSnapshot(void*) {
    ThemeRead guard; return snapshotUnlocked();
}
static DWORD WINAPI writer(void*) {
    unsigned tick=0;
    while (!InterlockedCompareExchange(&stopping,0,0)) {
        AcquireSRWLockExclusive(&lifecycleLock);
        RenderColors c; bool enabled;
        { ThemeRead guard; c=cfg.colors; enabled=cfg.mode && active; }
        updateWindowTheme(enabled,toColor(c.captionBackground),toColor(c.captionText),toColor(c.border),c.darkCaption!=0);
        ReleaseSRWLockExclusive(&lifecycleLock);
        if (!(tick++%4)) ThemeSnapshot(nullptr);
        Sleep(250);
    }
    return 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI ThemeStop(void*) {
    AcquireSRWLockExclusive(&lifecycleLock);
    AcquireSRWLockExclusive(&themeLock);
    InterlockedExchange(&active,0); InterlockedExchange(&stopping,1);
    activateIconTheme(nullptr);
    ReleaseSRWLockExclusive(&themeLock);
    updateWindowTheme(false,0,0,0);
    ReleaseSRWLockExclusive(&lifecycleLock);
    return ThemeSnapshot(nullptr);
}
static HMODULE verifiedModule(const wchar_t* name) {
    HMODULE m=GetModuleHandleW(name);
    if (!m) return nullptr;
    wchar_t actual[260],expected[260];
    if (!GetModuleFileNameW(m,actual,260)) return nullptr;
    if (swprintf(expected,260,L"%ls\\%ls",cfg.targetDir,name)<0) return nullptr;
    return _wcsicmp(actual,expected)==0 ? m : nullptr;
}
static DWORD validateConfig(void* argument) {
    if (!argument) return ERROR_INVALID_PARAMETER;
    const auto incoming=(const ProbeConfig*)argument;
    if (incoming->size!=sizeof(cfg)||incoming->version!=3||incoming->mode>1||incoming->mapCount>256||
        incoming->targetDir[259]||incoming->logPath[519]||incoming->themeId[63]||!incoming->themeId[0]) return ERROR_INVALID_PARAMETER;
    if (incoming->iconMode>2 || incoming->iconPack!=(incoming->iconMode?1u:0u) ||
        (!strcmp(incoming->themeId,"diagnostic") && incoming->iconMode)) return ERROR_INVALID_PARAMETER;
    for (const char* p=incoming->themeId;*p;++p)
        if (!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='-')) return ERROR_INVALID_PARAMETER;
    const auto &c=incoming->colors;
    const DWORD colors[]={c.glyphBackground,c.glyphForeground,c.selectionBackground,c.selectionForeground,
        c.rasterBackground,c.rasterForeground,c.captionBackground,c.captionText,c.border};
    for (DWORD color:colors) if (color>0xFFFFFF) return ERROR_INVALID_PARAMETER;
    if (c.darkCaption>1 || (incoming->mode && !incoming->mapCount) ||
        (incoming->mode && c.glyphBackground==c.selectionBackground && c.glyphForeground!=c.selectionForeground)) return ERROR_INVALID_PARAMETER;
    for (DWORD i=0;i<incoming->mapCount;++i) {
        const auto &map=incoming->maps[i];
        if (map.kind<Text || map.kind>MeshColor || map.source>0xFFFFFF || map.target>0xFFFFFF) return ERROR_INVALID_PARAMETER;
        for (DWORD j=0;j<i;++j)
            if (incoming->maps[j].kind==map.kind && incoming->maps[j].source==map.source) return ERROR_INVALID_PARAMETER;
    }
    return ERROR_SUCCESS;
}
extern "C" __declspec(dllexport) DWORD WINAPI ThemeApply(void* argument) {
    DWORD validation=validateConfig(argument); if (validation) return validation;
    const auto incoming=(const ProbeConfig*)argument;
    AcquireSRWLockExclusive(&lifecycleLock);
    AcquireSRWLockExclusive(&themeLock);
    DWORD result=ERROR_SUCCESS;
    if (!active || stopping) result=ERROR_SERVICE_NOT_ACTIVE;
    else if (_wcsicmp(incoming->targetDir,cfg.targetDir) || _wcsicmp(incoming->logPath,cfg.logPath)) result=ERROR_INVALID_PARAMETER;
    IconState* prepared=nullptr;
    if (!result && !iconStatus) prepared=prepareIconTheme(incoming->iconMode,result);
    if (!result) {
        result=snapshotUnlocked();
        if (!result) {
            wchar_t history[650];
            swprintf(history,650,L"%ls.g%04lu-%hs.csv",cfg.logPath,generation,cfg.themeId);
            if (!CopyFileW(cfg.logPath,history,FALSE)) result=GetLastError();
        }
    }
    if (!result) {
        cfg=*incoming; ++generation;
        activateIconTheme(prepared); prepared=nullptr;
        refreshCaptionInk();
        AcquireSRWLockExclusive(&tableLock); memset(rows,0,sizeof(rows)); dropped=0; ReleaseSRWLockExclusive(&tableLock);
        setRasterThemeColors(cfg.colors.rasterBackground,cfg.colors.rasterForeground,cfg.colors.border);
    }
    discardIconTheme(prepared);
    RenderColors c=cfg.colors; bool enabled=cfg.mode && active;
    ReleaseSRWLockExclusive(&themeLock);
    if (!result) updateWindowTheme(enabled,toColor(c.captionBackground),toColor(c.captionText),toColor(c.border),c.darkCaption!=0);
    ReleaseSRWLockExclusive(&lifecycleLock);
    return result;
}
extern "C" __declspec(dllexport) DWORD WINAPI ThemeInitialize(void* argument) {
    DWORD validation=validateConfig(argument); if (validation) return validation;
    const auto incoming=(const ProbeConfig*)argument;
    if (InterlockedCompareExchange(&started,1,0)) return ERROR_ALREADY_INITIALIZED;
    cfg=*incoming;
    refreshCaptionInk();
    const auto &c=cfg.colors;
    if (ThemeSnapshot(nullptr)) return ERROR_OPEN_FAILED;
    HMODULE pin;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)&ThemeInitialize,&pin)) return GetLastError();
    HMODULE frame=verifiedModule(L"frame.dll"), graphics=verifiedModule(L"grphcs.dll");
    if (!frame||!graphics) return ERROR_MOD_NOT_FOUND;
    HMODULE browser=verifiedModule(L"WebKit.dll");
    getSource=symbol<SourceFn>(GetModuleHandleW(L"cairo.dll"),"cairo_get_source");
    getRgba=symbol<PatternFn>(GetModuleHandleW(L"cairo.dll"),"cairo_pattern_get_rgba");
    if (!getSource||!getRgba) return ERROR_PROC_NOT_FOUND;
    saveContext=symbol<DrawFn>(GetModuleHandleW(L"cairo.dll"),"cairo_save");
    restoreContext=symbol<DrawFn>(GetModuleHandleW(L"cairo.dll"),"cairo_restore");
    initializeRasterTheme(GetModuleHandleW(L"cairo.dll"),c.rasterBackground,c.rasterForeground,c.border);
    initializeEditorSelection(GetModuleHandleW(L"cairo.dll"));
    bool ok=true;
    ok &= patchImport(frame,"gdi32.dll","SetTextColor",(void*)hookedText,(void**)&realText);
    ok &= patchImport(frame,"gdi32.dll","SetBkColor",(void*)hookedBackground,(void**)&realBackground);
    ok &= patchImport(frame,"gdi32.dll","CreateSolidBrush",(void*)hookedBrush,(void**)&realBrush);
    ok &= patchImport(frame,"gdi32.dll","CreatePen",(void*)hookedPen,(void**)&realPen);
    ok &= patchImport(frame,"user32.dll","GetSysColor",(void*)hookedSystemColor,(void**)&realSystemColor);
    ok &= patchImport(graphics,"cairo.dll","cairo_set_source_rgb",(void*)hookedRgb,(void**)&realRgb);
    ok &= patchImport(graphics,"cairo.dll","cairo_set_source_rgba",(void*)hookedRgba,(void**)&realRgba);
    // Both are imported by the selected 8.3.27 build; treat omissions as an incompatible profile.
    ok &= patchImport(graphics,"cairo.dll","cairo_pattern_add_color_stop_rgba",(void*)hookedStop,(void**)&realStop);
    ok &= patchImport(graphics,"cairo.dll","cairo_mesh_pattern_set_corner_color_rgba",(void*)hookedMesh,(void**)&realMesh);
    ok &= patchImport(graphics,"cairo.dll","cairo_show_glyphs",(void*)hookedGlyphs,(void**)&realGlyphs);
    ok &= patchImport(graphics,"cairo.dll","cairo_paint",(void*)hookedPaint,(void**)&realPaint);
    ok &= patchImport(graphics,"cairo.dll","cairo_paint_with_alpha",(void*)hookedPaintAlpha,(void**)&realPaintAlpha);
    ok &= patchImport(graphics,"cairo.dll","cairo_fill",(void*)hookedFill,(void**)&realFill);
    ok &= patchImport(graphics,"cairo.dll","cairo_fill_preserve",(void*)hookedFillPreserve,(void**)&realFillPreserve);
    // This supported Designer build loads WebKit before the theme is attached.
    // Mock/non-browser hosts remain supported with the original fourteen hooks.
    if (browser) {
        unsigned first=patchCount;
        ok &= patchImport(browser,"cairo.dll","cairo_set_source_rgb",(void*)browserRgb,(void**)&realRgb);
        ok &= patchImport(browser,"cairo.dll","cairo_set_source_rgba",(void*)browserRgba,(void**)&realRgba);
        ok &= patchImport(browser,"cairo.dll","cairo_show_glyphs",(void*)browserGlyphs,(void**)&realGlyphs);
        ok &= patchImport(browser,"cairo.dll","cairo_pattern_add_color_stop_rgba",(void*)browserStop,(void**)&realStop);
        ok &= patchImport(browser,"cairo.dll","cairo_paint",(void*)browserPaint,(void**)&realPaint);
        ok &= patchImport(browser,"cairo.dll","cairo_paint_with_alpha",(void*)browserPaintAlpha,(void**)&realPaintAlpha);
        ok &= patchImport(browser,"cairo.dll","cairo_fill",(void*)browserFill,(void**)&realFill);
        ok &= patchImport(browser,"cairo.dll","cairo_fill_preserve",(void*)browserFillPreserve,(void**)&realFillPreserve);
        browserPatchCount=patchCount-first;
    }
    if (!ok) { rollback(); ThemeSnapshot(nullptr); return ERROR_PROC_NOT_FOUND; }
    // Optional icon imports never make the color engine unavailable.
    if (iconProfileSupported() && initializeIconTheme(GetModuleHandleW(L"cairo.dll"))) {
        unsigned first=patchCount;
        bool iconsOk=patchImport(graphics,"cairo.dll","cairo_set_source",(void*)hookedIconSource,(void**)&realIconSource);
        iconsOk &= patchImport(graphics,"cairo.dll","cairo_mask",(void*)hookedIconMask,(void**)&realIconMask);
        iconsOk &= patchImport(graphics,"cairo.dll","cairo_mask_surface",(void*)hookedIconMaskSurface,(void**)&realIconMaskSurface);
        if (iconsOk) { iconPatchCount=patchCount-first; iconStatus=ERROR_SUCCESS; }
        else { rollback(first); iconStatus=ERROR_PROC_NOT_FOUND; }
    }
    if (!iconStatus) {
        DWORD error=0; auto prepared=prepareIconTheme(cfg.iconMode,error);
        if (error) { rollback(); return error; }
        activateIconTheme(prepared);
    }
    InterlockedExchange(&active,1);
    updateWindowTheme(cfg.mode!=0,toColor(cfg.colors.captionBackground),toColor(cfg.colors.captionText),toColor(cfg.colors.border),cfg.colors.darkCaption!=0);
    DWORD snapshotResult=ThemeSnapshot(nullptr);
    if (snapshotResult) { activateIconTheme(nullptr); InterlockedExchange(&active,0); updateWindowTheme(false,0,0,0); rollback(); return snapshotResult; }
    HANDLE thread=CreateThread(nullptr,0,writer,nullptr,0,nullptr);
    if (!thread) { activateIconTheme(nullptr); DWORD error=GetLastError(); InterlockedExchange(&active,0); updateWindowTheme(false,0,0,0); rollback(); return error; }
    CloseHandle(thread);
    return ERROR_SUCCESS;
}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID) { return TRUE; }
