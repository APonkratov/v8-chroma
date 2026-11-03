#include <cairo.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
struct LogFont { int64_t height,width,escape,orientation,weight; unsigned char flags[8]; char16_t family[32]; };
struct Font { unsigned kind,padding; LogFont value; uint64_t reserved0,stockId,reserved[3],attributes; int scale,padding2; };
extern "C" void fixture_convert(LogFont*,const Font*,int) asm("_ZN4core19abs_font_to_logfontERNS_9V8LogFontERKNS_4FontEi");
extern "C" void fixture_stock(LogFont*,const Font*,int,int) asm("_ZN4core17adjust_stock_fontERNS_9V8LogFontERKNS_4FontEiNS_17CompatibilityModeE");
extern "C" void fixture_mapped(LogFont*,const char16_t* const*) asm("_ZN4core9V8LogFont11setFaceNameERKN3ale11BasicStringIDsLm0ELm0ESaEE");

struct Dispatch {
    decltype(&cairo_set_source_rgb) rgb;
    decltype(&cairo_set_source_rgba) rgba;
    decltype(&cairo_pattern_add_color_stop_rgba) stop;
    decltype(&cairo_set_source) source;
} dispatch{};
extern "C" void graphics_init() {
    void* cairo = dlopen("libcairo.so.2", RTLD_NOW | RTLD_NOLOAD);
    if (!cairo) std::abort();
    dispatch.rgb = reinterpret_cast<decltype(dispatch.rgb)>(dlsym(cairo, "cairo_set_source_rgb"));
    dispatch.rgba = reinterpret_cast<decltype(dispatch.rgba)>(dlsym(cairo, "cairo_set_source_rgba"));
    dispatch.stop = reinterpret_cast<decltype(dispatch.stop)>(dlsym(cairo, "cairo_pattern_add_color_stop_rgba"));
    dispatch.source = reinterpret_cast<decltype(dispatch.source)>(dlsym(cairo, "cairo_set_source"));
}
extern "C" void graphics_font(unsigned kind,double* family,double* height,double* width,double* flags) {
    Font input{};input.kind=(kind==7||kind==9||kind==10)?1:0;input.value.height=-17;input.value.width=7;input.value.weight=400;
    if(kind==9)input.attributes=1;
    if(kind==10)input.stockId=1;
    const char16_t* name=(kind==5||kind==10)?u"Courier New":(kind==6||kind==7)?u"Arial":u"MS Sans Serif";
    memcpy(input.value.family,name,(std::char_traits<char16_t>::length(name)+1)*2);
    LogFont result{};
    if(input.kind==1)fixture_stock(&result,&input,96,0);else fixture_convert(&result,&input,96);
    if(input.kind==1){const char16_t* mapped=kind==7?u"Arial":result.family;fixture_mapped(&result,&mapped);}
    uint32_t hash=2166136261u;for(unsigned i=0;i<32&&result.family[i];++i){hash^=result.family[i];hash*=16777619;}
    *family=hash;*height=result.height;*width=result.width;*flags=result.weight;
}
extern "C" void graphics_sample(unsigned kind, double* r, double* g, double* b, double* a) {
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    auto* context = cairo_create(surface);
    if (kind == 5) dispatch.rgb(context, *r, *g, *b);
    else dispatch.rgba(context, *r, *g, *b, *a);
    if (cairo_pattern_get_rgba(cairo_get_source(context), r, g, b, a)) std::abort();
    cairo_destroy(context); cairo_surface_destroy(surface);
}
