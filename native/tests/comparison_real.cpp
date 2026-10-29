#include "../src/protocol.h"
#include "../src/contrast.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

struct Glyph { unsigned long index; double x,y; };
extern "C" __declspec(dllimport) void cairo_set_source_rgb(void*,double,double,double);
extern "C" __declspec(dllimport) void cairo_set_source_rgba(void*,double,double,double,double);
extern "C" __declspec(dllimport) void cairo_show_glyphs(void*,const Glyph*,int);
extern "C" __declspec(dllimport) void cairo_paint(void*);
extern "C" __declspec(dllimport) void cairo_fill(void*);
template<class T> T load(HMODULE m,const char* name) { auto p=GetProcAddress(m,name); T f; memcpy(&f,&p,sizeof(f)); return f; }

extern "C" __declspec(dllexport) BOOL comparison_probe(const ProbeConfig* cfg) {
    auto m=GetModuleHandleW(L"cairo.dll");
    auto image=load<void*(*)(int,int,int)>(m,"cairo_image_surface_create");
    auto win32=load<decltype(image)>(m,"cairo_win32_surface_create_with_dib");
    auto getImage=load<void*(*)(void*)>(m,"cairo_win32_surface_get_image");
    auto context=load<void*(*)(void*)>(m,"cairo_create");
    auto source=load<void*(*)(void*)>(m,"cairo_get_source");
    auto getRgba=load<int(*)(void*,double*,double*,double*,double*)>(m,"cairo_pattern_get_rgba");
    auto rectangle=load<void(*)(void*,double,double,double,double)>(m,"cairo_rectangle");
    auto bytes=load<unsigned char*(*)(void*)>(m,"cairo_image_surface_get_data");
    auto stride=load<int(*)(void*)>(m,"cairo_image_surface_get_stride");
    auto flush=load<void(*)(void*)>(m,"cairo_surface_flush");
    auto family=load<void(*)(void*,const char*,int,int)>(m,"cairo_select_font_face");
    auto fontSize=load<void(*)(void*,double)>(m,"cairo_set_font_size");
    auto font=load<void*(*)(void*)>(m,"cairo_get_scaled_font");
    auto textGlyphs=load<int(*)(void*,double,double,const char*,int,Glyph**,int*,void**,int*,int*)>(m,"cairo_scaled_font_text_to_glyphs");
    auto freeGlyphs=load<void(*)(Glyph*)>(m,"cairo_glyph_free");
    auto offset=load<void(*)(void*,double,double)>(m,"cairo_surface_set_device_offset");
    auto deviceScale=load<void(*)(void*,double,double)>(m,"cairo_surface_set_device_scale");
    auto translate=load<void(*)(void*,double,double)>(m,"cairo_translate");
    auto scale=load<void(*)(void*,double,double)>(m,"cairo_scale");
    auto destroyContext=load<void(*)(void*)>(m,"cairo_destroy");
    auto destroySurface=load<void(*)(void*)>(m,"cairo_surface_destroy");
    bool ok=true; unsigned checked=0;
    // IMAGE ARGB/RGB and Win32 DIB targets, including DPI/CTM/device offsets.
    for (unsigned variant=0;variant<5;++variant) {
        bool dib=variant==2 || variant==4;
        void* output=dib?win32(1,240,100):image(variant==1?1:0,240,100);
        if (variant>=3) { deviceScale(output,1.5,1.5); offset(output,3,4); }
        void* ctx=context(output);
        if (variant>=3) { translate(ctx,2,1); scale(ctx,1.1,1.1); }
        family(ctx,"Courier New",0,0); fontSize(ctx,24);
        Glyph* glyphs=nullptr; int count=0;
        ok &= textGlyphs(font(ctx),12,36,"WWWW",4,&glyphs,&count,nullptr,nullptr,nullptr)==0;
        for (DWORD native:{0u,0x0000FFu,0x008000u,0xFF0000u,0x963200u}) for (bool changed:{false,true}) {
            if (changed) cairo_set_source_rgb(ctx,245./255.,245./255.,245./255.);
            else cairo_set_source_rgb(ctx,1.,1.,1.);
            cairo_paint(ctx); flush(output);
            void* pixels=dib?getImage(output):output;
            auto data=bytes(pixels); int pitch=stride(pixels);
            uint32_t bg; memcpy(&bg,data+pitch*10+10*4,4); bg &= 0xFFFFFF;
            cairo_set_source_rgba(ctx,((native>>16)&255)/255.,((native>>8)&255)/255.,(native&255)/255.,1.);
            void* original=source(ctx); double r,g,b,a; getRgba(original,&r,&g,&b,&a);
            DWORD originalInk=((DWORD)(r*255+.5)<<16)|((DWORD)(g*255+.5)<<8)|(DWORD)(b*255+.5);
            cairo_show_glyphs(ctx,glyphs,count); flush(output);
            bool inkSeen=false,readable=false;
            for (int y=0;y<100;++y) for (int x=0;x<240;++x) {
                uint32_t pixel; memcpy(&pixel,data+y*pitch+x*4,4);
                if (!dib && variant!=1 && (pixel>>24)!=255) continue;
                pixel &= 0xFFFFFF;
                inkSeen |= pixel==originalInk;
                readable |= ChromaContrast::ratio(pixel,bg)>=4.5;
            }
            ok &= source(ctx)==original;
            // Normal editor syntax and diagnostic/standard palettes are exact.
            if (!changed || !cfg->mode) ok &= inkSeen;
            else ok &= readable;
            ++checked;
        }
        if (variant==0) {
            // A single glyph batch crosses ordinary and changed backgrounds.
            // Correction must not recolor the comment on the ordinary half.
            cairo_set_source_rgb(ctx,1.,1.,1.); cairo_paint(ctx);
            cairo_set_source_rgb(ctx,245./255.,245./255.,245./255.);
            rectangle(ctx,60,0,180,100); cairo_fill(ctx); flush(output);
            auto data=bytes(output); int pitch=stride(output);
            uint32_t bg; memcpy(&bg,data+pitch*10+80*4,4); bg &= 0xFFFFFF;
            Glyph* mixed=nullptr; int mixedCount=0;
            ok &= textGlyphs(font(ctx),12,36,"WW  WW",6,&mixed,&mixedCount,nullptr,nullptr,nullptr)==0;
            cairo_set_source_rgba(ctx,0.,128./255.,0.,1.);
            void* original=source(ctx); double r,g,b,a; getRgba(original,&r,&g,&b,&a);
            DWORD ink=((DWORD)(r*255+.5)<<16)|((DWORD)(g*255+.5)<<8)|(DWORD)(b*255+.5);
            cairo_show_glyphs(ctx,mixed,mixedCount); flush(output);
            bool ordinary=false,readable=false;
            for (int y=10;y<40;++y) for (int x=8;x<110;++x) {
                uint32_t pixel; memcpy(&pixel,data+y*pitch+x*4,4); pixel &= 0xFFFFFF;
                if (x<50) ordinary |= pixel==ink;
                if (x>=65) readable |= ChromaContrast::ratio(pixel,bg)>=4.5;
            }
            ok &= ordinary && (!cfg->mode || readable) && source(ctx)==original;
            freeGlyphs(mixed);
            ++checked;
        }
        freeGlyphs(glyphs);
        destroyContext(ctx); destroySurface(output);
    }
    printf("%s comparison syntax: %s; %u painted cases, ordinary ink preserved, source restored\n",cfg->themeId,ok?"PASS":"FAIL",checked);
    return ok;
}
