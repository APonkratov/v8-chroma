#include "icon_theme.h"
#include <icon_assets.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <new>
namespace {
struct Matrix { double xx,yx,xy,yy,x0,y0; };
template<class T> bool load(HMODULE module,const char* name,T& fn) {
    auto address=GetProcAddress(module,name);
    static_assert(sizeof(fn)==sizeof(address)); memcpy(&fn,&address,sizeof(fn)); return fn!=nullptr;
}
using SetSource=void(__cdecl*)(void*,void*);



void* (*getTarget)(void*);
int (*patternSurface)(void*,void**),(*surfaceType)(void*),(*width)(void*),(*height)(void*),(*stride)(void*),(*format)(void*),(*status)(void*);
int (*getExtend)(void*),(*getFilter)(void*);
int (*statusPattern)(void*);
unsigned char* (*data)(void*);
void (*flush)(void*),(*dirty)(void*),(*destroySurface)(void*),(*destroyPattern)(void*);
void* (*createSurface)(int,int,int);

void* (*createPattern)(void*);

void (*getMatrix)(void*,Matrix*),(*setMatrix)(void*,const Matrix*);
void (*setExtend)(void*,int),(*setFilter)(void*,int);
void (*getOffset)(void*,double*,double*),(*setOffset)(void*,double,double);
void (*getScale)(void*,double*,double*),(*setScale)(void*,double,double);
struct Patch { uint64_t hash; int w,h,x,y,size; void* image; };
std::vector<Patch> patches;
struct Cache { uint64_t hash; int w,h,kind; double x,y,sx,sy; void* image; };
std::vector<Cache> cache;
SRWLOCK stateLock=SRWLOCK_INIT;
bool running;
unsigned currentMode;
size_t cacheBytes;
unsigned long long calls,matches,uniqueImages;
constexpr size_t maxImages=1024;

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
    cache.clear(); patches.clear(); cacheBytes=0;
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
    bool known=false;
    for (auto& patch:patches) if (patch.hash==hash && patch.w==w && patch.h==h) { known=true; break; }
    if (!known) return nullptr;
    if (cache.size()>=maxImages || cacheBytes+static_cast<size_t>(w)*h*4>16*1024*1024) {
        for (auto& entry:cache) if (entry.image) destroySurface(entry.image);
        cache.clear(); cacheBytes=0;
    }
    ++uniqueImages;
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
    if (copy) cacheBytes+=static_cast<size_t>(w)*h*4;
    return copy;
}
void* transformPattern(void* context,void* pattern) {
    void* source=nullptr,*themed=nullptr;
    int destination=running?surfaceType(getTarget(context)):-1;
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
}

struct IconState { unsigned mode; std::vector<Patch> patches; };
bool initializeIconTheme(HMODULE cairo) {
    bool ok=true;
#define ICON_LOAD(fn,name) ok = load(cairo,name,fn) && ok
    ICON_LOAD(getTarget,"cairo_get_target"); ICON_LOAD(patternSurface,"cairo_pattern_get_surface");
    ICON_LOAD(surfaceType,"cairo_surface_get_type"); ICON_LOAD(width,"cairo_image_surface_get_width");
    ICON_LOAD(height,"cairo_image_surface_get_height"); ICON_LOAD(stride,"cairo_image_surface_get_stride");
    ICON_LOAD(format,"cairo_image_surface_get_format"); ICON_LOAD(status,"cairo_surface_status");
    ICON_LOAD(data,"cairo_image_surface_get_data"); ICON_LOAD(flush,"cairo_surface_flush");
    ICON_LOAD(dirty,"cairo_surface_mark_dirty"); ICON_LOAD(createSurface,"cairo_image_surface_create");
    ICON_LOAD(destroySurface,"cairo_surface_destroy"); ICON_LOAD(createPattern,"cairo_pattern_create_for_surface");
    ICON_LOAD(destroyPattern,"cairo_pattern_destroy"); ICON_LOAD(getMatrix,"cairo_pattern_get_matrix");
    ICON_LOAD(setMatrix,"cairo_pattern_set_matrix"); ICON_LOAD(getExtend,"cairo_pattern_get_extend");
    ICON_LOAD(setExtend,"cairo_pattern_set_extend"); ICON_LOAD(getFilter,"cairo_pattern_get_filter");
    ICON_LOAD(setFilter,"cairo_pattern_set_filter"); ICON_LOAD(statusPattern,"cairo_pattern_status");
    ICON_LOAD(getOffset,"cairo_surface_get_device_offset"); ICON_LOAD(setOffset,"cairo_surface_set_device_offset");
    ICON_LOAD(getScale,"cairo_surface_get_device_scale"); ICON_LOAD(setScale,"cairo_surface_set_device_scale");
#undef ICON_LOAD
    return ok;
}
void discardIconTheme(IconState* state) {
    if (!state) return;
    for (auto& p:state->patches) destroySurface(p.image);
    delete state;
}
IconState* prepareIconTheme(unsigned mode,DWORD& error) {
    error=ERROR_SUCCESS;
    auto state=new(std::nothrow) IconState{mode,{}};
    if (!state) { error=ERROR_NOT_ENOUGH_MEMORY; return nullptr; }
    if (!mode) return state;
    const auto assets=mode==1 ? IconAssets::light : IconAssets::dark;
    // Each patch owns its surface. Decode before publishing; never read files in a hook.
    try {
        for (auto& p:IconAssets::patches) {
            const auto& a=assets[p.asset];
            void* surface=createSurface(0,a.size,a.size);
            if (!surface || status(surface)) {
                if (surface) destroySurface(surface);
                error=ERROR_NOT_ENOUGH_MEMORY; break;
            }
            unsigned pos=0;
            for (unsigned i=0;i<a.count;++i) {
                if (a.runs[i].count>a.size*a.size-pos) { error=ERROR_INVALID_DATA; break; }
                for (unsigned n=0;n<a.runs[i].count;++n,++pos)
                    reinterpret_cast<uint32_t*>(data(surface)+(pos/a.size)*stride(surface))[pos%a.size]=a.runs[i].pixel;
            }
            if (error || pos!=a.size*a.size || a.size!=p.size) {
                destroySurface(surface); error=ERROR_INVALID_DATA; break;
            }
            dirty(surface);
            try { state->patches.push_back({p.hash,static_cast<int>(p.w),static_cast<int>(p.h),
                      static_cast<int>(p.x),static_cast<int>(p.y),static_cast<int>(p.size),surface}); }
            catch (...) { destroySurface(surface); throw; }
        }
    } catch (...) { error=ERROR_NOT_ENOUGH_MEMORY; }
    if (error) { discardIconTheme(state); return nullptr; }
    return state;
}
void activateIconTheme(IconState* state) {
    AcquireSRWLockExclusive(&stateLock);
    releaseImages(); currentMode=state?state->mode:0; running=currentMode!=0;
    if (state) patches.swap(state->patches);
    ReleaseSRWLockExclusive(&stateLock);
    discardIconTheme(state);
}
void drawIconSource(void* context,void* pattern,void(__cdecl* original)(void*,void*)) {
    DWORD last=GetLastError(); AcquireSRWLockExclusive(&stateLock); ++calls;
    void* themed=transformPattern(context,pattern);
    SetLastError(last); original(context,themed?themed:pattern); last=GetLastError();
    if (themed) destroyPattern(themed);
    ReleaseSRWLockExclusive(&stateLock); SetLastError(last);
}
void drawIconMask(void* context,void* pattern,void(__cdecl* original)(void*,void*)) {
    drawIconSource(context,pattern,original);
}
void drawIconMaskSurface(void* context,void* surface,double x,double y,
                         void(__cdecl* original)(void*,void*,double,double)) {
    DWORD last=GetLastError(); AcquireSRWLockExclusive(&stateLock); ++calls;
    void* copy=nullptr;
    if (running) {
        int destination=surfaceType(getTarget(context));
        if (destination==0 || destination==7) { try { copy=replacement(surface); } catch (...) {} }
    }
    if (copy) ++matches;
    SetLastError(last); original(context,copy?copy:surface,x,y); last=GetLastError();
    ReleaseSRWLockExclusive(&stateLock); SetLastError(last);
}
IconStats iconStats() {
    AcquireSRWLockShared(&stateLock);
    IconStats stats={currentMode,static_cast<unsigned>(cache.size()),cacheBytes,calls,matches};
    ReleaseSRWLockShared(&stateLock); return stats;
}
