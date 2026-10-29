// Opt-in experiment: replace exact known Cairo images, without changing 1C files.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace {
struct Matrix { double xx,yx,xy,yy,x0,y0; };
template<class T> bool load(HMODULE module,const char* name,T& fn) {
    auto address=GetProcAddress(module,name);
    static_assert(sizeof(fn)==sizeof(address)); memcpy(&fn,&address,sizeof(fn)); return fn!=nullptr;
}
using SetSource=void(__cdecl*)(void*,void*);
SetSource realSetSource;
void (*realMask)(void*,void*);
void (*realMaskSurface)(void*,void*,double,double);
void* (*getTarget)(void*);
int (*patternSurface)(void*,void**),(*surfaceType)(void*),(*width)(void*),(*height)(void*),(*stride)(void*),(*format)(void*),(*status)(void*);
int (*getExtend)(void*),(*getFilter)(void*);
int (*statusPattern)(void*);
unsigned char* (*data)(void*);
void (*flush)(void*),(*dirty)(void*),(*destroySurface)(void*),(*destroyPattern)(void*);
void* (*createSurface)(int,int,int);
void* (*readPng)(const char*);
void* (*createPattern)(void*);
int (*writePng)(void*,const char*);
void (*getMatrix)(void*,Matrix*),(*setMatrix)(void*,const Matrix*);
void (*setExtend)(void*,int),(*setFilter)(void*,int);
void (*getOffset)(void*,double*,double*),(*setOffset)(void*,double,double);
void (*getScale)(void*,double*,double*),(*setScale)(void*,double,double);
struct Patch { uint64_t hash; int w,h,x,y,size; void* image; };
std::vector<Patch> patches;
struct Cache { uint64_t hash; int w,h,kind; double x,y,sx,sy; void* image; };
std::vector<Cache> cache;
SRWLOCK stateLock=SRWLOCK_INIT;
void** importSlot;
void** maskSlot;
void** maskSurfaceSlot;
std::wstring root;
bool running;
unsigned long long calls,matches,uniqueImages;
constexpr size_t maxImages=1024;

std::string utf8(const std::wstring& input) {
    int n=WideCharToMultiByte(CP_UTF8,0,input.c_str(),-1,nullptr,0,nullptr,nullptr);
    if (n<=0) return {};
    std::string output(static_cast<size_t>(n),'\0');
    WideCharToMultiByte(CP_UTF8,0,input.c_str(),-1,output.data(),n,nullptr,nullptr);
    output.pop_back(); return output;
}
void hashByte(uint64_t& value,unsigned byte) { value^=byte; value*=1099511628211ull; }
uint32_t sourcePixel(unsigned char* input,int x,int y,int pitch,int kind) {
    if (kind==2) { unsigned a=input[y*pitch+x]; return a*0x01010101u; }
    uint32_t pixel=reinterpret_cast<uint32_t*>(input+y*pitch)[x];
    if (kind==1) pixel|=0xFF000000u;
    return (pixel>>24)?pixel:0;
}
uint64_t fingerprint(unsigned char* input,int w,int h,int pitch,int kind) {
    uint64_t value=14695981039346656037ull;
    for (int n:{w,h}) for (int shift=0;shift<32;shift+=8) hashByte(value,(n>>shift)&255);
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        uint32_t pixel=sourcePixel(input,x,y,pitch,kind);
        for (int shift=0;shift<32;shift+=8) hashByte(value,(pixel>>shift)&255);
    }
    return value;
}
void releaseImages() {
    for (auto& entry:cache) if (entry.image) destroySurface(entry.image);
    for (auto& patch:patches) destroySurface(patch.image);
    cache.clear(); patches.clear();
}
void* replacement(void* source) {
    if (surfaceType(source)!=0) return nullptr;
    int w=width(source),h=height(source),kind=format(source),pitch=stride(source);
    // Only small UI resources. No documents, pictures, printer or PDF surfaces.
    if ((kind!=0 && kind!=1 && kind!=2) || w<1 || h<1 || h>64 || w>2048 || pitch<w*(kind==2?1:4)) return nullptr;
    flush(source); auto input=data(source); if (!input) return nullptr;
    uint64_t hash=fingerprint(input,w,h,pitch,kind);
    double offsetX,offsetY,scaleX,scaleY;
    getOffset(source,&offsetX,&offsetY); getScale(source,&scaleX,&scaleY);
    for (auto& entry:cache) if (entry.hash==hash && entry.w==w && entry.h==h && entry.kind==kind &&
        entry.x==offsetX && entry.y==offsetY && entry.sx==scaleX && entry.sy==scaleY) return entry.image;
    if (cache.size()>=maxImages) return nullptr;
    ++uniqueImages;
    wchar_t name[128]; swprintf(name,128,L"\\%016llx-%dx%d.png",static_cast<unsigned long long>(hash),w,h);
    std::string filename=utf8(root+L"\\observed"+name);
    if (kind!=2) writePng(source,filename.c_str());
    else {
        void* diagnostic=createSurface(0,w,h);
        if (diagnostic && !status(diagnostic)) {
            auto pixels=data(diagnostic); int outPitch=stride(diagnostic);
            for (int y=0;y<h;++y) for (int x=0;x<w;++x)
                reinterpret_cast<uint32_t*>(pixels+y*outPitch)[x]=sourcePixel(input,x,y,pitch,kind);
            dirty(diagnostic); writePng(diagnostic,filename.c_str());
        }
        if (diagnostic) destroySurface(diagnostic);
    }
    void* copy=nullptr;
    for (auto& patch:patches) if (patch.hash==hash && patch.w==w && patch.h==h) {
        if (!copy) {
            copy=createSurface(0,w,h);
            if (!copy || status(copy)) { if (copy) destroySurface(copy); copy=nullptr; break; }
            auto output=data(copy); int outPitch=stride(copy);
            for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
                reinterpret_cast<uint32_t*>(output+y*outPitch)[x]=sourcePixel(input,x,y,pitch,kind);
            }
        }
        auto output=data(copy),pixels=data(patch.image);
        int outPitch=stride(copy),patchPitch=stride(patch.image);
        for (int y=0;y<patch.size;++y)
            memcpy(output+(y+patch.y)*outPitch+patch.x*4,pixels+y*patchPitch,static_cast<size_t>(patch.size)*4);
    }
    if (copy) {
        double x,y; getOffset(source,&x,&y); setOffset(copy,x,y);
        getScale(source,&x,&y); setScale(copy,x,y); dirty(copy);
    }
    cache.push_back({hash,w,h,kind,offsetX,offsetY,scaleX,scaleY,copy});
    return copy;
}
void* transformPattern(void* context,void* pattern) {
    void* source=nullptr,*themed=nullptr;
    int destination=surfaceType(getTarget(context));
    if (running && (destination==0 || destination==7) && !patternSurface(pattern,&source)) {
        void* copy=nullptr;
        try { copy=replacement(source); } catch (...) { copy=nullptr; }
        if (copy) {
            themed=createPattern(copy);
            if (themed && !statusPattern(themed)) {
                Matrix matrix; getMatrix(pattern,&matrix); setMatrix(themed,&matrix);
                setExtend(themed,getExtend(pattern)); setFilter(themed,getFilter(pattern));
                ++matches;
            } else { if (themed) destroyPattern(themed); themed=nullptr; }
        }
    }
    return themed;
}
void __cdecl hookedSetSource(void* context,void* pattern) {
    DWORD last=GetLastError(); AcquireSRWLockExclusive(&stateLock); ++calls;
    void* themed=transformPattern(context,pattern);
    SetLastError(last); realSetSource(context,themed?themed:pattern); last=GetLastError();
    if (themed) destroyPattern(themed);
    ReleaseSRWLockExclusive(&stateLock); SetLastError(last);
}
void __cdecl hookedMask(void* context,void* pattern) {
    DWORD last=GetLastError(); AcquireSRWLockExclusive(&stateLock); ++calls;
    void* themed=transformPattern(context,pattern);
    SetLastError(last); realMask(context,themed?themed:pattern); last=GetLastError();
    if (themed) destroyPattern(themed);
    ReleaseSRWLockExclusive(&stateLock); SetLastError(last);
}
void __cdecl hookedMaskSurface(void* context,void* surface,double x,double y) {
    DWORD last=GetLastError(); AcquireSRWLockExclusive(&stateLock); ++calls;
    void* copy=nullptr; int destination=surfaceType(getTarget(context));
    if (running && (destination==0 || destination==7)) {
        try { copy=replacement(surface); } catch (...) { copy=nullptr; }
    }
    if (copy) ++matches;
    SetLastError(last); realMaskSurface(context,copy?copy:surface,x,y); last=GetLastError();
    ReleaseSRWLockExclusive(&stateLock); SetLastError(last);
}
void** findImport(HMODULE module,const char* requested) {
    auto base=reinterpret_cast<unsigned char*>(module);
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return nullptr;
    auto descriptor=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+directory.VirtualAddress);
    for (;descriptor->Name;++descriptor) {
        if (_stricmp(reinterpret_cast<char*>(base+descriptor->Name),"cairo.dll") || !descriptor->OriginalFirstThunk) continue;
        auto names=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+descriptor->OriginalFirstThunk);
        auto slots=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+descriptor->FirstThunk);
        for (;names->u1.AddressOfData;++names,++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto name=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            if (!strcmp(reinterpret_cast<char*>(name->Name),requested)) return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}
bool exchangeImport(void** slot,void* expected,void* replacementValue) {
    DWORD old=0;
    if (!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old)) return false;
    bool ok=InterlockedCompareExchangePointer(slot,replacementValue,expected)==expected;
    DWORD ignored; VirtualProtect(slot,sizeof(void*),old,&ignored); return ok;
}
DWORD initialize(const wchar_t* directory) {
    if (!directory || wcslen(directory)>220 || !((directory[0] && directory[1]==L':') || (directory[0]==L'\\' && directory[1]==L'\\'))) return ERROR_INVALID_PARAMETER;
    HMODULE cairo=GetModuleHandleW(L"cairo.dll"),graphics=GetModuleHandleW(L"grphcs.dll");
    if (!cairo || !graphics) return ERROR_MOD_NOT_FOUND;
    bool ok=true;
#define LOAD(fn,name) ok &= load(cairo,name,fn)
    LOAD(realSetSource,"cairo_set_source"); LOAD(patternSurface,"cairo_pattern_get_surface");
    LOAD(realMask,"cairo_mask"); LOAD(realMaskSurface,"cairo_mask_surface");
    LOAD(getTarget,"cairo_get_target");
    LOAD(surfaceType,"cairo_surface_get_type"); LOAD(width,"cairo_image_surface_get_width"); LOAD(height,"cairo_image_surface_get_height");
    LOAD(stride,"cairo_image_surface_get_stride"); LOAD(format,"cairo_image_surface_get_format"); LOAD(data,"cairo_image_surface_get_data");
    LOAD(status,"cairo_surface_status"); LOAD(statusPattern,"cairo_pattern_status"); LOAD(flush,"cairo_surface_flush"); LOAD(dirty,"cairo_surface_mark_dirty");
    LOAD(createSurface,"cairo_image_surface_create"); LOAD(readPng,"cairo_image_surface_create_from_png"); LOAD(writePng,"cairo_surface_write_to_png");
    LOAD(destroySurface,"cairo_surface_destroy"); LOAD(createPattern,"cairo_pattern_create_for_surface"); LOAD(destroyPattern,"cairo_pattern_destroy");
    LOAD(getMatrix,"cairo_pattern_get_matrix"); LOAD(setMatrix,"cairo_pattern_set_matrix");
    LOAD(getExtend,"cairo_pattern_get_extend"); LOAD(setExtend,"cairo_pattern_set_extend"); LOAD(getFilter,"cairo_pattern_get_filter"); LOAD(setFilter,"cairo_pattern_set_filter");
    LOAD(getOffset,"cairo_surface_get_device_offset"); LOAD(setOffset,"cairo_surface_set_device_offset"); LOAD(getScale,"cairo_surface_get_device_scale"); LOAD(setScale,"cairo_surface_set_device_scale");
#undef LOAD
    if (!ok) return ERROR_PROC_NOT_FOUND;
    root=directory;
    FILE* file=_wfopen((root+L"\\pack.tsv").c_str(),L"rb");
    if (!file) return ERROR_FILE_NOT_FOUND;
    char line[512]; bool valid=true;
    while (fgets(line,sizeof(line),file)) {
        if (line[0]=='#' || line[0]=='\r' || line[0]=='\n') continue;
        unsigned long long hash; int w,h,x,y,size; char name[96],trailing;
        if (sscanf(line,"%llx %d %d %d %d %d %95s %c",&hash,&w,&h,&x,&y,&size,name,&trailing)!=7 ||
            w<8 || w>2048 || h<8 || h>64 || size<8 || size>64 || x<0 || y<0 || x+size>w || y+size>h || patches.size()>=4096) { valid=false; break; }
        for (const char* p=name;*p;++p) if (!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='-' || *p=='.')) valid=false;
        if (strstr(name,"..") || !valid) { valid=false; break; }
        std::wstring wide; for (const char* p=name;*p;++p) wide.push_back(*p);
        auto path=utf8(root+L"\\"+wide); void* image=readPng(path.c_str());
        if (!image || status(image) || width(image)!=size || height(image)!=size || format(image)!=0) {
            if (image) destroySurface(image);
            valid=false; break;
        }
        flush(image); patches.push_back({hash,w,h,x,y,size,image});
    }
    if (ferror(file)) valid=false;
    fclose(file);
    if (!valid) { releaseImages(); return ERROR_INVALID_DATA; }
    CreateDirectoryW((root+L"\\observed").c_str(),nullptr);
    importSlot=findImport(graphics,"cairo_set_source");
    maskSlot=findImport(graphics,"cairo_mask"); maskSurfaceSlot=findImport(graphics,"cairo_mask_surface");
    if (!importSlot || *importSlot!=reinterpret_cast<void*>(realSetSource)) { releaseImages(); return ERROR_INVALID_FUNCTION; }
    if ((maskSlot && *maskSlot!=reinterpret_cast<void*>(realMask)) ||
        (maskSurfaceSlot && *maskSurfaceSlot!=reinterpret_cast<void*>(realMaskSurface))) { releaseImages(); return ERROR_INVALID_FUNCTION; }
    if (!exchangeImport(importSlot,reinterpret_cast<void*>(realSetSource),reinterpret_cast<void*>(hookedSetSource))) { releaseImages(); return ERROR_WRITE_FAULT; }
    if (maskSlot && !exchangeImport(maskSlot,reinterpret_cast<void*>(realMask),reinterpret_cast<void*>(hookedMask))) {
        exchangeImport(importSlot,reinterpret_cast<void*>(hookedSetSource),reinterpret_cast<void*>(realSetSource));
        releaseImages(); return ERROR_WRITE_FAULT;
    }
    if (maskSurfaceSlot && !exchangeImport(maskSurfaceSlot,reinterpret_cast<void*>(realMaskSurface),reinterpret_cast<void*>(hookedMaskSurface))) {
        if (maskSlot) exchangeImport(maskSlot,reinterpret_cast<void*>(hookedMask),reinterpret_cast<void*>(realMask));
        exchangeImport(importSlot,reinterpret_cast<void*>(hookedSetSource),reinterpret_cast<void*>(realSetSource));
        releaseImages(); return ERROR_WRITE_FAULT;
    }
    running=true; return ERROR_SUCCESS;
}
}

extern "C" __declspec(dllexport) DWORD WINAPI ToolbarIconsStart(void* directory) {
    AcquireSRWLockExclusive(&stateLock);
    DWORD result;
    try { result=running?ERROR_ALREADY_INITIALIZED:initialize(static_cast<const wchar_t*>(directory)); }
    catch (...) { releaseImages(); result=ERROR_NOT_ENOUGH_MEMORY; }
    ReleaseSRWLockExclusive(&stateLock); return result;
}
extern "C" __declspec(dllexport) DWORD WINAPI ToolbarIconsStop(void*) {
    AcquireSRWLockExclusive(&stateLock);
    DWORD result=ERROR_SUCCESS;
    auto restore=[&](void** slot,void* hook,void* original) {
        if (slot && *slot!=original && !exchangeImport(slot,hook,original)) result=ERROR_WRITE_FAULT;
    };
    if (running) {
        restore(importSlot,reinterpret_cast<void*>(hookedSetSource),reinterpret_cast<void*>(realSetSource));
        restore(maskSlot,reinterpret_cast<void*>(hookedMask),reinterpret_cast<void*>(realMask));
        restore(maskSurfaceSlot,reinterpret_cast<void*>(hookedMaskSurface),reinterpret_cast<void*>(realMaskSurface));
    }
    if (!result) { running=false; releaseImages(); }
    ReleaseSRWLockExclusive(&stateLock); return result;
}
extern "C" __declspec(dllexport) DWORD WINAPI ToolbarIconsSnapshot(void*) {
    AcquireSRWLockExclusive(&stateLock);
    FILE* file=_wfopen((root+L"\\stats.txt").c_str(),L"wb");
    if (file) { fprintf(file,"running=%d\npatches=%zu\ncalls=%llu\nmatches=%llu\nunique_images=%llu\n",running,patches.size(),calls,matches,uniqueImages); fclose(file); }
    ReleaseSRWLockExclusive(&stateLock); return file?ERROR_SUCCESS:ERROR_WRITE_FAULT;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
