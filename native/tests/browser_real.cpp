#include "../src/protocol.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Glyph { unsigned long index; double x,y; };
struct Extents { double xb,yb,width,height,xa,ya; };
extern "C" __declspec(dllimport) void cairo_set_source_rgb(void*,double,double,double);
extern "C" __declspec(dllimport) void cairo_set_source_rgba(void*,double,double,double,double);
extern "C" __declspec(dllimport) void cairo_show_glyphs(void*,const Glyph*,int);
extern "C" __declspec(dllimport) void cairo_pattern_add_color_stop_rgba(void*,double,double,double,double,double);
extern "C" __declspec(dllimport) void cairo_paint(void*);
extern "C" __declspec(dllimport) void cairo_paint_with_alpha(void*,double);
extern "C" __declspec(dllimport) void cairo_fill(void*);
extern "C" __declspec(dllimport) void cairo_fill_preserve(void*);
template<class T> T load(HMODULE m,const char* name) { auto p=GetProcAddress(m,name); T f; memcpy(&f,&p,sizeof(f)); return f; }
static unsigned unicodeCalls;
static void(*rectangle)(void*,double,double,double,double);
static void(*fill)(void*);
static int unicodeProbe(void*,unsigned long codepoint,unsigned long* glyph) {
    ++unicodeCalls; *glyph=codepoint; return 0;
}
static int drawUserGlyph(void*,unsigned long,void* ctx,Extents* extents) {
    rectangle(ctx,0.,-1.,1.,1.); fill(ctx); *extents={0.,-1.,1.,1.,1.,0.}; return 0;
}
extern "C" __declspec(dllexport) BOOL browser_probe(const ProbeConfig* cfg) {
    HMODULE m=GetModuleHandleW(L"cairo.dll");
    auto image=load<void*(*)(int,int,int)>(m,"cairo_image_surface_create");
    auto bytes=load<unsigned char*(*)(void*)>(m,"cairo_image_surface_get_data");
    auto context=load<void*(*)(void*)>(m,"cairo_create");
    rectangle=load<decltype(rectangle)>(m,"cairo_rectangle");
    fill=&cairo_fill;
    auto paint=&cairo_paint;
    auto source=load<void*(*)(void*)>(m,"cairo_get_source");
    auto flush=load<void(*)(void*)>(m,"cairo_surface_flush");
    auto status=load<int(*)(void*)>(m,"cairo_status");
    auto fontSize=load<void(*)(void*,double)>(m,"cairo_set_font_size");
    auto userFace=load<void*(*)()>(m,"cairo_user_font_face_create");
    auto setUnicode=load<void(*)(void*,decltype(&unicodeProbe))>(m,"cairo_user_font_face_set_unicode_to_glyph_func");
    auto setRender=load<void(*)(void*,decltype(&drawUserGlyph))>(m,"cairo_user_font_face_set_render_glyph_func");
    auto setFace=load<void(*)(void*,void*)>(m,"cairo_set_font_face");
    auto destroyFace=load<void(*)(void*)>(m,"cairo_font_face_destroy");
    auto destroyContext=load<void(*)(void*)>(m,"cairo_destroy");
    auto destroySurface=load<void(*)(void*)>(m,"cairo_surface_destroy");
    void* output=image(0,120,90),*ctx=context(output);
    cairo_set_source_rgb(ctx,1.,1.,1.); paint(ctx); flush(output);
    bool ok=((uint32_t*)bytes(output))[119]==(0xFF000000|(cfg->mode?cfg->colors.glyphBackground:0xFFFFFF));
    void* face=userFace(); setUnicode(face,unicodeProbe); setRender(face,drawUserGlyph);
    setFace(ctx,face); fontSize(ctx,18.); unicodeCalls=0;
    cairo_set_source_rgb(ctx,0.,0.,0.); void* original=source(ctx);
    Glyph glyph={42,12.,25.}; cairo_show_glyphs(ctx,&glyph,1); flush(output);
    DWORD text=cfg->mode?cfg->colors.glyphForeground:0;
    ok &= ((uint32_t*)bytes(output))[120*12+16]==(0xFF000000|text) && source(ctx)==original;
    COLORREF native=GetSysColor(COLOR_HIGHLIGHT);
    DWORD nativeRgb=(GetRValue(native)<<16)|(GetGValue(native)<<8)|GetBValue(native);
    cairo_set_source_rgba(ctx,GetRValue(native)/255.,GetGValue(native)/255.,GetBValue(native)/255.,1.);
    rectangle(ctx,0.,45.,120.,30.); fill(ctx); flush(output);
    ok &= ((uint32_t*)bytes(output))[120*46+119]==(0xFF000000|(cfg->mode?cfg->colors.selectionBackground:nativeRgb));
    cairo_set_source_rgba(ctx,1.,1.,1.,1.); original=source(ctx);
    glyph.y=70.; cairo_show_glyphs(ctx,&glyph,1); flush(output);
    DWORD selected=cfg->mode?cfg->colors.selectionForeground:0xFFFFFF;
    ok &= ((uint32_t*)bytes(output))[120*57+16]==(0xFF000000|selected) && source(ctx)==original;
    ok &= unicodeCalls==0 && status(ctx)==0;
    DWORD accent=cfg->colors.glyphForeground;
    for (DWORD i=0;i<cfg->mapCount;++i)
        if (cfg->maps[i].kind==ColorStop && cfg->maps[i].source==0xAA9600) { accent=cfg->maps[i].target; break; }
    const DWORD links[]={0x0000EE,0x0000FF,0x000080,0x00008B,0x551A8B,0x800080};
    for (unsigned i=0;i<6;++i) {
        DWORD link=links[i];
        cairo_set_source_rgba(ctx,((link>>16)&255)/255.,((link>>8)&255)/255.,(link&255)/255.,1.);
        rectangle(ctx,i*12.,80.,10.,8.); fill(ctx); flush(output);
        ok &= ((uint32_t*)bytes(output))[120*82+i*12+2]==(0xFF000000|(cfg->mode?accent:link));
    }
    cairo_set_source_rgba(ctx,250./255.,250./255.,210./255.,1.);
    rectangle(ctx,0.,80.,10.,8.); fill(ctx); flush(output);
    ok &= ((uint32_t*)bytes(output))[120*82+2]==(0xFF000000|(cfg->mode?cfg->colors.rasterBackground:0xFAFAD2));
    // Scrollbar gradients and cached button/arrow skins use separate imports.
    auto linear=load<void*(*)(double,double,double,double)>(m,"cairo_pattern_create_linear");
    auto stopColor=load<int(*)(void*,int,double*,double*,double*,double*,double*)>(m,"cairo_pattern_get_color_stop_rgba");
    auto patternDestroy=load<void(*)(void*)>(m,"cairo_pattern_destroy");
    void* gradient=linear(0.,0.,0.,10.);
    cairo_pattern_add_color_stop_rgba(gradient,0.,246./255.,243./255.,223./255.,1.);
    double off,r,g,b,a; ok &= stopColor(gradient,0,&off,&r,&g,&b,&a)==0;
    DWORD gradientColor=((DWORD)(r*255+.5)<<16)|((DWORD)(g*255+.5)<<8)|(DWORD)(b*255+.5);
    ok &= gradientColor==(cfg->mode?cfg->colors.rasterBackground:0xF6F3DF);
    patternDestroy(gradient);
    auto setSurface=load<void(*)(void*,void*,double,double)>(m,"cairo_set_source_surface");
    auto dirty=load<void(*)(void*)>(m,"cairo_surface_mark_dirty");
    void* skin=image(0,16,16); auto skinPixels=(uint32_t*)bytes(skin);
    for (unsigned i=0;i<256;++i) skinPixels[i]=0xFFFFFFFF;
    skinPixels[8*16+8]=0xFF000000; dirty(skin);
    setSurface(ctx,skin,0.,0.); original=source(ctx);
    cairo_paint(ctx); cairo_paint_with_alpha(ctx,1.);
    rectangle(ctx,0.,0.,16.,16.); cairo_fill_preserve(ctx); cairo_fill(ctx); flush(output);
    ok &= ((uint32_t*)bytes(output))[0]==(0xFF000000|(cfg->mode?cfg->colors.rasterBackground:0xFFFFFF));
    ok &= ((uint32_t*)bytes(output))[8*120+8]==(0xFF000000|(cfg->mode?cfg->colors.rasterForeground:0));
    ok &= source(ctx)==original && skinPixels[0]==0xFFFFFFFF && skinPixels[8*16+8]==0xFF000000;
    destroySurface(skin);
    printf("%s browser: %s; background/text/selection, USER-font callbacks=%u, Cairo state preserved\n",cfg->themeId,ok?"PASS":"FAIL",unicodeCalls);
    destroyFace(face); destroyContext(ctx); destroySurface(output);
    return ok;
}
