// The Windows asset manifest and exact pixel fingerprints are shared. Cairo
// objects and caches remain isolated for the vendor and system libraries.
#include "icons.h"
#include <icon_assets.h>
#include <dlfcn.h>
#include <pthread.h>
#include <cerrno>
#include <cstring>
#include <vector>

namespace linux_icons {
namespace {
struct Matrix { double xx,yx,xy,yy,x0,y0; };
struct Cache { uint64_t hash; int w,h,kind; double x,y,sx,sy; void* image; };
struct Renderer {
    void* (*target)(void*) = nullptr;
    int (*type)(void*),(*width)(void*),(*height)(void*),(*stride)(void*),(*format)(void*),(*status)(void*),(*patternStatus)(void*);
    unsigned char* (*data)(void*);
    void (*flush)(void*),(*dirty)(void*),(*destroy)(void*),(*destroyPattern)(void*);
    void* (*create)(int,int,int);
    void* (*createPattern)(void*);
    int (*getSurface)(void*,void**),(*extend)(void*),(*filter)(void*);
    void (*matrix)(void*,Matrix*),(*setMatrix)(void*,const Matrix*);
    void (*setExtend)(void*,int),(*setFilter)(void*,int);
    void (*offset)(void*,double*,double*),(*scale)(void*,double*,double*),(*setOffset)(void*,double,double),(*setScale)(void*,double,double);
    std::vector<void*> assets;
    std::vector<Cache> cache;
    unsigned mode=0;
    bool ready=false;
    size_t bytes=0;
    void clear() {
        for (auto& c:cache) if (c.image) destroy(c.image);
        for (auto a:assets) destroy(a);
        cache.clear(); assets.clear(); bytes=0; mode=0;
    }
    bool prepare(unsigned selected) {
        if (selected==mode) return true;
        clear();
        if (!selected) return true;
        auto source=selected==1?IconAssets::light:IconAssets::dark;
        constexpr size_t count=sizeof(IconAssets::light)/sizeof(*IconAssets::light);
        try {
            for (size_t i=0;i<count;++i) {
                const auto& a=source[i]; void* image=create(0,a.size,a.size);
                if (!image || status(image)) { if(image) destroy(image); clear(); return false; }
                assets.push_back(image);
                unsigned pos=0;
                for (unsigned run=0;run<a.count;++run) {
                    if (a.runs[run].count>a.size*a.size-pos) { clear(); return false; }
                    for (unsigned j=0;j<a.runs[run].count;++j,++pos)
                        reinterpret_cast<uint32_t*>(data(image)+(pos/a.size)*stride(image))[pos%a.size]=a.runs[run].pixel;
                }
                if (pos!=a.size*a.size) { clear(); return false; }
                dirty(image);
            }
            mode=selected; return true;
        } catch (...) { clear(); return false; }
    }
};
Renderer renderers[2];
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
unsigned selectedMode;
uint64_t calls,matches;
uint32_t pixel(const unsigned char* data,int x,int y,int stride,int kind) {
    if (kind==2) return data[y*stride+x]*0x01010101u;
    uint32_t value; memcpy(&value,data+y*stride+x*4,4);
    if (kind==1) value|=0xff000000u;
    return value>>24?value:0;
}
uint64_t fingerprint(const unsigned char* data,int w,int h,int stride,int kind,int ignore=0) {
    uint64_t hash=14695981039346656037ull;
    auto add=[&](uint32_t value) { for(unsigned i=0;i<32;i+=8) {hash^=(value>>i)&255;hash*=1099511628211ull;} };
    add(w);add(h);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) add(y>=h-ignore?0:pixel(data,x,y,stride,kind));
    return hash;
}
void* replacement(Renderer& r,void* context,void* image) {
    if (!r.ready || !r.prepare(__atomic_load_n(&selectedMode,__ATOMIC_ACQUIRE)) || !r.mode) return nullptr;
    int destination=r.type(r.target(context));
    // Linux Xlib (3) and image (0) surfaces only; never PDF/PS/SVG/printer output.
    if (destination!=0 && destination!=3) return nullptr;
    if (r.type(image)!=0) return nullptr;
    int w=r.width(image),h=r.height(image),kind=r.format(image),stride=r.stride(image);
    if ((kind!=0&&kind!=1&&kind!=2)||w<1||w>2048||h<1||h>64||stride<w*(kind==2?1:4)) return nullptr;
    r.flush(image); auto input=r.data(image); if(!input) return nullptr;
    auto hash=fingerprint(input,w,h,stride,kind);
    double x,y,sx,sy; r.offset(image,&x,&y);r.scale(image,&sx,&sy);
    for(const auto& c:r.cache) if(c.hash==hash&&c.w==w&&c.h==h&&c.kind==kind&&c.x==x&&c.y==y&&c.sx==sx&&c.sy==sy) return c.image;
    void* copy=nullptr;
    for(const auto& p:IconAssets::patches) {
        if (static_cast<int>(p.w)!=w||static_cast<int>(p.h)!=h) continue;
        if (p.preserveBottom && (p.preserveBottom!=4||w!=16||h!=16||p.x||p.y||p.size!=16)) continue;
        if (p.hash!=(p.preserveBottom?fingerprint(input,w,h,stride,kind,4):hash)) continue;
        if (p.asset>=r.assets.size()||p.x+p.size>p.w||p.y+p.size>p.h) continue;
        if (!copy) {
            copy=r.create(0,w,h);
            if(!copy||r.status(copy)) {if(copy)r.destroy(copy);return nullptr;}
            auto output=r.data(copy);
            for(int row=0;row<h;++row) for(int col=0;col<w;++col)
                reinterpret_cast<uint32_t*>(output+row*r.stride(copy))[col]=pixel(input,col,row,stride,kind);
        }
        auto asset=r.assets[p.asset];
        if(r.width(asset)!=static_cast<int>(p.size)||r.height(asset)!=static_cast<int>(p.size)) continue;
        for(unsigned row=0;row<p.size-p.preserveBottom;++row)
            memcpy(r.data(copy)+(row+p.y)*r.stride(copy)+p.x*4,r.data(asset)+row*r.stride(asset),p.size*4);
    }
    if(copy) {r.setOffset(copy,x,y);r.setScale(copy,sx,sy);r.dirty(copy);}
    if (r.cache.size()>=1024 || r.bytes+static_cast<size_t>(w)*h*4>16*1024*1024) {
        for(auto& c:r.cache) if(c.image)r.destroy(c.image);
        r.cache.clear();r.bytes=0;
    }
    try {r.cache.push_back({hash,w,h,kind,x,y,sx,sy,copy});} catch(...) {if(copy)r.destroy(copy);return nullptr;}
    if(copy) r.bytes+=static_cast<size_t>(w)*h*4;
    return copy;
}
}
bool initialize(unsigned bank,void* cairo) {
    if(bank>1||!cairo)return false;
    auto& r=renderers[bank]; bool ok=true;
#define LOAD(member,name) r.member=reinterpret_cast<decltype(r.member)>(dlsym(cairo,name));ok=ok&&r.member
    LOAD(target,"cairo_get_target");LOAD(type,"cairo_surface_get_type");LOAD(width,"cairo_image_surface_get_width");
    LOAD(height,"cairo_image_surface_get_height");LOAD(stride,"cairo_image_surface_get_stride");LOAD(format,"cairo_image_surface_get_format");
    LOAD(status,"cairo_surface_status");LOAD(data,"cairo_image_surface_get_data");LOAD(flush,"cairo_surface_flush");
    LOAD(dirty,"cairo_surface_mark_dirty");LOAD(destroy,"cairo_surface_destroy");LOAD(create,"cairo_image_surface_create");
    LOAD(createPattern,"cairo_pattern_create_for_surface");LOAD(destroyPattern,"cairo_pattern_destroy");
    LOAD(getSurface,"cairo_pattern_get_surface");LOAD(patternStatus,"cairo_pattern_status");
    LOAD(extend,"cairo_pattern_get_extend");LOAD(filter,"cairo_pattern_get_filter");
    LOAD(setExtend,"cairo_pattern_set_extend");LOAD(setFilter,"cairo_pattern_set_filter");
    LOAD(matrix,"cairo_pattern_get_matrix");LOAD(setMatrix,"cairo_pattern_set_matrix");
    LOAD(offset,"cairo_surface_get_device_offset");LOAD(setOffset,"cairo_surface_set_device_offset");
    LOAD(scale,"cairo_surface_get_device_scale");LOAD(setScale,"cairo_surface_set_device_scale");
#undef LOAD
    r.ready=ok;return ok;
}
void configure(unsigned mode) {__atomic_store_n(&selectedMode,mode,__ATOMIC_RELEASE);}
void source(unsigned bank,void* context,void* pattern,void(*original)(void*,void*)) {
    int saved=errno;pthread_mutex_lock(&mutex);__atomic_fetch_add(&calls,1,__ATOMIC_RELAXED);
    auto& r=renderers[bank];void* image=nullptr,*themed=nullptr;
    if(r.ready&&!r.getSurface(pattern,&image)) {
        void* copy=nullptr;try{copy=replacement(r,context,image);}catch(...){}
        if(copy) {
            themed=r.createPattern(copy);
            if(themed&&!r.patternStatus(themed)) {
                Matrix m;r.matrix(pattern,&m);r.setMatrix(themed,&m);
                r.setExtend(themed,r.extend(pattern));r.setFilter(themed,r.filter(pattern));__atomic_fetch_add(&matches,1,__ATOMIC_RELAXED);
            }else {if(themed)r.destroyPattern(themed);themed=nullptr;}
        }
    }
    errno=saved;original(context,themed?themed:pattern);saved=errno;
    if(themed)r.destroyPattern(themed);
    pthread_mutex_unlock(&mutex);errno=saved;
}
void surface(unsigned bank,void* context,void* image,double x,double y,void(*original)(void*,void*,double,double)) {
    int saved=errno;pthread_mutex_lock(&mutex);__atomic_fetch_add(&calls,1,__ATOMIC_RELAXED);void* copy=nullptr;
    try{copy=replacement(renderers[bank],context,image);}catch(...){}
    if(copy)__atomic_fetch_add(&matches,1,__ATOMIC_RELAXED);
    errno=saved;original(context,copy?copy:image,x,y);saved=errno;
    pthread_mutex_unlock(&mutex);errno=saved;
}
Stats stats(){return {__atomic_load_n(&selectedMode,__ATOMIC_ACQUIRE),__atomic_load_n(&calls,__ATOMIC_RELAXED),__atomic_load_n(&matches,__ATOMIC_RELAXED)};}
}
