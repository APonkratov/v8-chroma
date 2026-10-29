// Owned Cairo-shaped surfaces: no installed platform or graphics runtime needed.
#include "../src/icon_theme.h"
#include <stdint.h>
#include <vector>
#include <stdio.h>
#include <thread>
#include <atomic>
struct Matrix { double xx=1,yx=0,xy=0,yy=1,x0=0,y0=0; };
struct Surface { int kind,w,h; std::vector<unsigned char> pixels; double x=0,y=0,sx=1,sy=1; };
struct Pattern { Surface* surface; Matrix matrix; int extend=0,filter=0; };
struct Context { Surface* target; };
#define API extern "C" __declspec(dllexport)
API void* cairo_get_target(Context* c) { return c->target; }
API int cairo_pattern_get_surface(Pattern* p,void** s) { *s=p->surface; return 0; }
API int cairo_surface_get_type(Surface*) { return 0; }
API int cairo_image_surface_get_width(Surface* s) { return s->w; }
API int cairo_image_surface_get_height(Surface* s) { return s->h; }
API int cairo_image_surface_get_stride(Surface* s) { return s->w*(s->kind==2?1:4); }
API int cairo_image_surface_get_format(Surface* s) { return s->kind; }
static bool rejectSurface;
API int cairo_surface_status(Surface*) { return rejectSurface?1:0; }
API int cairo_pattern_status(Pattern*) { return 0; }
API unsigned char* cairo_image_surface_get_data(Surface* s) { return s->pixels.data(); }
API void cairo_surface_flush(Surface*) {}
API void cairo_surface_mark_dirty(Surface*) {}
API Surface* cairo_image_surface_create(int k,int w,int h) { return new Surface{k,w,h,std::vector<unsigned char>(w*h*(k==2?1:4))}; }
API void cairo_surface_destroy(Surface* s) { delete s; }
API Pattern* cairo_pattern_create_for_surface(Surface* s) { return new Pattern{s,{}}; }
API void cairo_pattern_destroy(Pattern* p) { delete p; }
API void cairo_pattern_get_matrix(Pattern* p,Matrix* m) { *m=p->matrix; }
API void cairo_pattern_set_matrix(Pattern* p,const Matrix* m) { p->matrix=*m; }
API int cairo_pattern_get_extend(Pattern* p) { return p->extend; }
API int cairo_pattern_get_filter(Pattern* p) { return p->filter; }
API void cairo_pattern_set_extend(Pattern* p,int n) { p->extend=n; }
API void cairo_pattern_set_filter(Pattern* p,int n) { p->filter=n; }
API void cairo_surface_get_device_offset(Surface* s,double* x,double* y) { *x=s->x; *y=s->y; }
API void cairo_surface_set_device_offset(Surface* s,double x,double y) { s->x=x;s->y=y; }
API void cairo_surface_get_device_scale(Surface* s,double* x,double* y) { *x=s->sx; *y=s->sy; }
API void cairo_surface_set_device_scale(Surface* s,double x,double y) { s->sx=x;s->sy=y; }
static unsigned expectedMode;
static bool valid=true;
static Surface* source;
static void inspectSurface(void* surface) {
    auto s=static_cast<Surface*>(surface);
    if (!expectedMode) { valid &= s==source; return; }
    auto pixels=reinterpret_cast<uint32_t*>(s->pixels.data());
    int size=s->h;
    valid &= s!=source && s->kind==0 && s->w==size*3;
    valid &= pixels[size]==(expectedMode==1?0xff44cc88u:0xff4488ccu);
    valid &= pixels[0]==(source->kind==2?0xffffffffu:0xffcc2244u);
    valid &= pixels[s->w*s->h-1]==0;
    valid &= s->x==source->x && s->sy==source->sy;
}
static void __cdecl inspect(void*,void* pattern) {
    auto p=static_cast<Pattern*>(pattern);
    inspectSurface(p->surface);
    valid &= p->matrix.x0==3.5 && p->matrix.yy==.75 && p->filter==2 && p->extend==1;
}
static void __cdecl inspectMask(void*,void* surface,double x,double y) {
    inspectSurface(surface); valid &= x==2.5 && y==4.5;
}
int main() {
    if (!initializeIconTheme(GetModuleHandleW(nullptr))) return 2;
    Surface target={0,1,1,{0,0,0,0}}; Context context={&target};
    for (int size:{14,16,20,24,28,32,48,64}) for (int kind:{0,2}) {
        source=cairo_image_surface_create(kind,size*3,size);
        if (kind==2) { for (auto& n:source->pixels) n=255; source->pixels.back()=0; }
        else { auto p=reinterpret_cast<uint32_t*>(source->pixels.data());
            for (int i=0;i<size*size*3-1;++i) p[i]=0xffcc2244u; }
        source->x=1.25; source->sy=1.5;
        const auto original=source->pixels;
        Pattern pattern={source,{1,0,0,.75,3.5,0},1,2};
        for (unsigned mode:{0u,1u,2u,0u,2u,1u}) {
            DWORD error=0; auto state=prepareIconTheme(mode,error);
            if (error || !state) return 3;
            activateIconTheme(state); expectedMode=mode;
            drawIconSource(&context,&pattern,inspect);
            drawIconMask(&context,&pattern,inspect);
            drawIconMaskSurface(&context,source,2.5,4.5,inspectMask);
            valid &= source->pixels==original && iconStats().mode==mode;
        }
        source->pixels[0]^=1; expectedMode=0;
        drawIconSource(&context,&pattern,inspect); // Mutated/unknown surface is untouched.
        cairo_surface_destroy(source);
    }
    activateIconTheme(nullptr);
    valid &= iconStats().bytes==0 && iconStats().cached==0 && iconStats().mode==0;
    // Formatting color controls: replace the known glyph, retain any selected
    // swatch, and invalidate the cached image when only that swatch changes.
    source=cairo_image_surface_create(0,16,16);
    Pattern colorPattern={source,{}};
    for (unsigned mode:{1u,2u}) {
        DWORD colorError=0; activateIconTheme(prepareIconTheme(mode,colorError));
        expectedMode=mode;
        valid &= colorError==0;
        for (uint32_t swatch:{0xff112233u,0xffffcc00u,0x80402010u,0u}) {
            auto values=reinterpret_cast<uint32_t*>(source->pixels.data());
            for (int i=0;i<16*16;++i) values[i]=i<16*12?0xffcc2244u:swatch;
            auto original=source->pixels;
            drawIconSource(&context,&colorPattern,[](void*,void* value) {
                auto result=static_cast<Pattern*>(value)->surface;
                auto pixels=reinterpret_cast<uint32_t*>(result->pixels.data());
                auto native=reinterpret_cast<uint32_t*>(source->pixels.data());
                valid &= result!=source;
                uint32_t expected=expectedMode==1?0xff44cc88u:0xff4488ccu;
                for (int i=0;i<16*16;++i) valid &= pixels[i]==(i<16*12?expected:native[i]);
            });
            valid &= source->pixels==original;
            source->pixels[0]^=1; // A changed glyph is unknown even with a known swatch.
            drawIconSource(&context,&colorPattern,[](void*,void* value) {
                valid &= static_cast<Pattern*>(value)->surface==source;
            });
        }
        valid &= iconStats().cached==4;
    }
    activateIconTheme(nullptr); cairo_surface_destroy(source);
    source=cairo_image_surface_create(0,48,16);
    auto p=reinterpret_cast<uint32_t*>(source->pixels.data());
    for (int i=0;i<48*16-1;++i) p[i]=0xffcc2244u;
    Pattern pattern={source,{1,0,0,.75,3.5,0},1,2};
    DWORD error=0; activateIconTheme(prepareIconTheme(1,error));
    rejectSurface=true;
    auto failed=prepareIconTheme(2,error);
    valid &= failed==nullptr && error!=0 && iconStats().mode==1;
    rejectSurface=false;
    std::atomic<bool> done{false}, stressOk{true};
    std::atomic<unsigned> stressCalls{0};
    std::thread render([&] {
        while (!done) { drawIconSource(&context,&pattern,[](void*,void* value) {
            auto surface=static_cast<Pattern*>(value)->surface;
            auto pixel=reinterpret_cast<uint32_t*>(surface->pixels.data())[16];
            if (pixel!=0xffcc2244u && pixel!=0xff44cc88u && pixel!=0xff4488ccu) valid=false;
        }); ++stressCalls; Sleep(1); }
    });
    while (!stressCalls) Sleep(1);
    for (unsigned i=0;i<80;++i) {
        auto state=prepareIconTheme(i%3,error);
        if (error) stressOk=false;
        activateIconTheme(state);
        Sleep(1);
    }
    done=true; render.join(); valid &= stressOk && stressCalls>=10;
    activateIconTheme(nullptr); cairo_surface_destroy(source);
    printf("Integrated icon module: sizes, masks, state, dynamic swatches, mutation, toggles, cache release: %s\n",valid?"PASS":"FAIL");
    return valid?0:1;
}
