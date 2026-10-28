#include "../src/protocol.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>
struct Glyph { unsigned long index; double x,y; };
extern "C" __declspec(dllimport) void cairo_set_source_rgb(void*,double,double,double);
extern "C" __declspec(dllimport) void cairo_set_source_rgba(void*,double,double,double,double);
extern "C" __declspec(dllimport) void cairo_show_glyphs(void*,const Glyph*,int);
extern "C" __declspec(dllimport) void cairo_fill(void*);
extern "C" __declspec(dllimport) void cairo_fill_preserve(void*);
extern "C" __declspec(dllimport) void cairo_paint(void*);
extern "C" __declspec(dllimport) void cairo_paint_with_alpha(void*,double);
extern "C" __declspec(dllimport) void cairo_pattern_add_color_stop_rgba(void*,double,double,double,double,double);
extern "C" __declspec(dllimport) void cairo_mesh_pattern_set_corner_color_rgba(void*,unsigned,double,double,double,double);
template<class T> T load(HMODULE m,const char* name) { auto p=GetProcAddress(m,name); T f; memcpy(&f,&p,sizeof(f)); return f; }
// Keep the two pattern imports present for the production scoped IAT contract.
extern "C" __declspec(dllexport) void* required_import(unsigned index) {
    return index?(void*)&cairo_pattern_add_color_stop_rgba:(void*)&cairo_mesh_pattern_set_corner_color_rgba;
}
extern "C" __declspec(dllexport) BOOL real_selection_probe(const ProbeConfig* cfg) {
    HMODULE m=GetModuleHandleW(L"cairo.dll");
    auto image=load<void*(*)(int,int,int)>(m,"cairo_image_surface_create");
    auto bytes=load<unsigned char*(*)(void*)>(m,"cairo_image_surface_get_data");
    auto context=load<void*(*)(void*)>(m,"cairo_create");
    auto rectangle=load<void(*)(void*,double,double,double,double)>(m,"cairo_rectangle");
    auto source=load<void*(*)(void*)>(m,"cairo_get_source");
    auto family=load<void(*)(void*,const char*,int,int)>(m,"cairo_select_font_face");
    auto fontSize=load<void(*)(void*,double)>(m,"cairo_set_font_size");
    auto getFont=load<void*(*)(void*)>(m,"cairo_get_scaled_font");
    auto textGlyphs=load<int(*)(void*,double,double,const char*,int,Glyph**,int*,void**,int*,int*)>(m,"cairo_scaled_font_text_to_glyphs");
    auto freeGlyphs=load<void(*)(Glyph*)>(m,"cairo_glyph_free");
    auto flush=load<void(*)(void*)>(m,"cairo_surface_flush");
    auto dirty=load<void(*)(void*)>(m,"cairo_surface_mark_dirty");
    auto setSurface=load<void(*)(void*,void*,double,double)>(m,"cairo_set_source_surface");
    auto destroyContext=load<void(*)(void*)>(m,"cairo_destroy");
    auto destroySurface=load<void(*)(void*)>(m,"cairo_surface_destroy");
    void* output=image(0,240,200),*ctx=context(output);
    cairo_set_source_rgba(ctx,1.,1.,1.,1.); cairo_paint(ctx);
    DWORD bg=cfg->mode?cfg->colors.selectionBackground:0;
    // First selected line: the platform has not set the editor's font yet.
    cairo_set_source_rgba(ctx,0.,0.,0.,1.); void* original=source(ctx);
    rectangle(ctx,2,2,230,28); cairo_fill(ctx); flush(output);
    bool ok=source(ctx)==original && ((uint32_t*)bytes(output))[240*5+5]==(0xFF000000|bg);
    family(ctx,"Courier New",0,0); fontSize(ctx,18.);
    const DWORD inversions[]={0xFFFFFF,0x00FFFF,0xFFFF00,0xFF7FFF,0x69CDFF};
    for (unsigned row=0;row<5;++row) {
        int y=2+row*28;
        cairo_set_source_rgba(ctx,0.,0.,0.,1.); rectangle(ctx,2,y,230,28); cairo_fill_preserve(ctx); cairo_fill(ctx);
        DWORD native=inversions[row];
        cairo_set_source_rgba(ctx,((native>>16)&255)/255.,((native>>8)&255)/255.,(native&255)/255.,1.);
        original=source(ctx); Glyph* glyphs=nullptr; int count=0;
        ok &= textGlyphs(getFont(ctx),10,y+21,"WWWW",4,&glyphs,&count,nullptr,nullptr,nullptr)==0;
        cairo_show_glyphs(ctx,glyphs,count); freeGlyphs(glyphs); flush(output);
        DWORD ink=cfg->mode?cfg->colors.selectionForeground:native;
        unsigned found=0;
        for (int py=y;py<y+28;++py) for (int x=8;x<80;++x)
            if (((uint32_t*)bytes(output))[240*py+x]==(0xFF000000|ink)) ++found;
        ok &= found>4 && source(ctx)==original;
    }
    // An ordinary RGB UI brush stays foreground-colored, not selection-colored.
    cairo_set_source_rgb(ctx,0.,0.,0.); rectangle(ctx,2,150,80,12); cairo_fill(ctx); flush(output);
    ok &= ((uint32_t*)bytes(output))[240*154+5]==(0xFF000000|(cfg->mode?cfg->colors.glyphForeground:0));
    // A large RGB editor rectangle keeps the editor background. A narrow group
    // highlight uses panel, preserving its source and its alpha coverage.
    cairo_set_source_rgb(ctx,1.,1.,1.); rectangle(ctx,0,150,240,50);
    cairo_fill(ctx); flush(output);
    ok &= ((uint32_t*)bytes(output))[240*155+5]==(0xFF000000|(cfg->mode?cfg->colors.glyphBackground:0xFFFFFF));
    original=source(ctx); rectangle(ctx,2,150,230,6); cairo_fill(ctx); flush(output);
    ok &= ((uint32_t*)bytes(output))[240*152+5]==(0xFF000000|(cfg->mode?cfg->colors.rasterBackground:0xFFFFFF));
    ok &= source(ctx)==original;
    auto fillRule=load<void(*)(void*,int)>(m,"cairo_set_fill_rule");
    // Native group skin: same-winding outer and inner contours, even-odd hole.
    cairo_set_source_rgb(ctx,1.,1.,1.); fillRule(ctx,1);
    rectangle(ctx,2,148,230,48); rectangle(ctx,6,152,222,40);
    original=source(ctx); cairo_fill(ctx); flush(output); fillRule(ctx,0);
    ok &= ((uint32_t*)bytes(output))[240*149+3]==(0xFF000000|(cfg->mode?cfg->colors.rasterBackground:0xFFFFFF));
    ok &= source(ctx)==original;
    // White RGB UI captions must remain readable after their fill maps to panel.
    family(ctx,"Arial",0,0);
    cairo_set_source_rgb(ctx,1.,1.,1.);
    void* whiteSource=source(ctx); Glyph* uiGlyphs=nullptr; int uiCount=0;
    ok &= textGlyphs(getFont(ctx),95,164,"WWWW",4,&uiGlyphs,&uiCount,nullptr,nullptr,nullptr)==0;
    cairo_show_glyphs(ctx,uiGlyphs,uiCount); freeGlyphs(uiGlyphs); flush(output);
    unsigned whiteInk=0; DWORD uiInk=cfg->mode?cfg->colors.glyphForeground:0xFFFFFF;
    for(int y=148;y<166;++y) for(int x=95;x<175;++x)
        if (((uint32_t*)bytes(output))[240*y+x]==(0xFF000000|uiInk)) ++whiteInk;
    ok &= whiteInk>4 && source(ctx)==whiteSource;
    // Compare/merge cells: both drawing APIs and their legend swatches use the
    // exact profile targets, while diagnostic leaves native pastel colors intact.
    const DWORD compareColors[]={0xE0FFFF,0xFFE4C4,0xC0DCC0,0xBFF7B8,0xDCDCDC,0x0000A0};
    for (unsigned i=0;i<6;++i) for (DWORD kind:{DWORD(CairoRgb),DWORD(CairoRgba)}) {
        DWORD native=compareColors[i],expected=native;
        if(cfg->mode) for(DWORD j=0;j<cfg->mapCount;++j)
            if(cfg->maps[j].kind==kind && cfg->maps[j].source==native) expected=cfg->maps[j].target;
        double r=((native>>16)&255)/255.,g=((native>>8)&255)/255.,b=(native&255)/255.;
        if(kind==CairoRgb) cairo_set_source_rgb(ctx,r,g,b); else cairo_set_source_rgba(ctx,r,g,b,1.);
        void* before=source(ctx); rectangle(ctx,180+i*9,130,7,7); cairo_fill(ctx); flush(output);
        ok &= ((uint32_t*)bytes(output))[240*133+182+i*9]==(0xFF000000|expected) && source(ctx)==before;
    }
    if (cfg->mode) {
        // Reproduce a cached opaque hot skin, including text-like pixels inside.
        void* skin=image(0,80,24); auto pixels=(uint32_t*)bytes(skin);
        for (int y=0;y<24;++y) for (int x=0;x<80;++x)
            pixels[y*80+x]=0xFF000000|((x<3 || x>=77 || y<3 || y>=21)?cfg->colors.rasterForeground:cfg->colors.rasterBackground);
        pixels[80*12+30]=0xFF000000|cfg->colors.rasterForeground;
        dirty(skin); setSurface(ctx,skin,0,170); original=source(ctx); cairo_paint_with_alpha(ctx,1.); flush(output);
        auto out=(uint32_t*)bytes(output);
        ok &= out[240*170]==(0xFF000000|cfg->colors.border);
        ok &= out[240*171+1]==(0xFF000000|cfg->colors.rasterBackground);
        ok &= out[240*182+30]==(0xFF000000|cfg->colors.rasterForeground);
        ok &= pixels[81]==(0xFF000000|cfg->colors.rasterForeground) && source(ctx)==original;
        destroySurface(skin);
    }
    destroyContext(ctx); destroySurface(output);
    printf("%s: %s (real Cairo + production IAT; first line, all inverse glyph colors, RGB brush, thin hover/label)\n",cfg->themeId,ok?"PASS":"FAIL");
    return ok;
}

// Regression for 8.5.1's RGBA captions and gradient tabs, distinct from RGB 8.3.
extern "C" __declspec(dllexport) BOOL chrome_probe(const ProbeConfig* cfg) {
    auto m=GetModuleHandleW(L"cairo.dll");
    auto image=load<void*(*)(int,int,int)>(m,"cairo_image_surface_create");
    auto context=load<void*(*)(void*)>(m,"cairo_create");
    auto bytes=load<unsigned char*(*)(void*)>(m,"cairo_image_surface_get_data");
    auto flush=load<void(*)(void*)>(m,"cairo_surface_flush");
    auto linear=load<void*(*)(double,double,double,double)>(m,"cairo_pattern_create_linear");
    auto stops=load<int(*)(void*,int,double*,double*,double*,double*,double*)>(m,"cairo_pattern_get_color_stop_rgba");
    auto destroyPattern=load<void(*)(void*)>(m,"cairo_pattern_destroy");
    auto destroyContext=load<void(*)(void*)>(m,"cairo_destroy");
    auto destroySurface=load<void(*)(void*)>(m,"cairo_surface_destroy");
    void* surface=image(0,80,24),*ctx=context(surface);
    cairo_set_source_rgba(ctx,204./255.,204./255.,204./255.,1.); cairo_paint(ctx); flush(surface);
    bool ok=((uint32_t*)bytes(surface))[0]==(0xFF000000|(cfg->mode?cfg->colors.selectionBackground:0xCCCCCC));
    // Full editor backgrounds remain distinct from narrow white UI bands.
    cairo_set_source_rgb(ctx,1.,1.,1.); cairo_paint(ctx); flush(surface);
    ok &= ((uint32_t*)bytes(surface))[0]==(0xFF000000|(cfg->mode?cfg->colors.glyphBackground:0xFFFFFF));
    cairo_set_source_rgba(ctx,1.,1.,1.,1.); cairo_paint(ctx); flush(surface);
    ok &= ((uint32_t*)bytes(surface))[0]==(0xFF000000|(cfg->mode?cfg->colors.glyphBackground:0xFFFFFF));
    cairo_set_source_rgba(ctx,241./255.,241./255.,241./255.,1.); cairo_paint(ctx); flush(surface);
    ok &= ((uint32_t*)bytes(surface))[0]==(0xFF000000|(cfg->mode?cfg->colors.rasterBackground:0xF1F1F1));
    // Modern logon uses solid enabled/disabled yellow, independently of the old gradients.
    const DWORD logon[]={0xFFDD00,0x403700,0xFFFCE5,0xCFB300,0x4D4D54,0x404040,0xF2F2F2};
    DWORD disabled=0;
    for (DWORD i=0;i<cfg->mapCount;++i)
        if (cfg->maps[i].kind==CairoRgba && cfg->maps[i].source==0x9B8549) disabled=cfg->maps[i].target;
    const DWORD expected[]={cfg->colors.selectionBackground,cfg->colors.glyphForeground,
        cfg->colors.rasterBackground,disabled,cfg->colors.glyphForeground,
        cfg->colors.glyphForeground,cfg->colors.rasterBackground};
    for (unsigned i=0;i<7;++i) {
        DWORD c=logon[i];
        cairo_set_source_rgba(ctx,((c>>16)&255)/255.,((c>>8)&255)/255.,(c&255)/255.,1.);
        cairo_paint(ctx); flush(surface);
        ok &= ((uint32_t*)bytes(surface))[0]==(0xFF000000|(cfg->mode?expected[i]:c));
    }
    // Classic logon emits F0..FE as separate solid bands, not a Cairo gradient.
    for (unsigned c=240;c<255;++c) {
        cairo_set_source_rgba(ctx,c/255.,c/255.,c/255.,1.); cairo_paint(ctx); flush(surface);
        DWORD original=c*0x010101;
        ok &= ((uint32_t*)bytes(surface))[0]==(0xFF000000|(cfg->mode?cfg->colors.rasterBackground:original));
    }
    // MDI windows cache caption ink obtained from GetSysColor before the switch.
    // Include distinct foregrounds from every bundled palette, RGB and RGBA.
    const DWORD cachedInk[]={0xBFBDB6,0xA9B7C6,0xF8F8F2,0xE6EDF3,0x1F2328,
        0xD6DEEB,0xC5CDD3,0xABB2BF,0xDADADA,0x3B3B3B,0xA7DBF7};
    for (DWORD c:cachedInk) for (unsigned alpha=0;alpha<2;++alpha) {
        double r=((c>>16)&255)/255.,g=((c>>8)&255)/255.,b=(c&255)/255.;
        if (alpha) cairo_set_source_rgba(ctx,r,g,b,1.);
        else cairo_set_source_rgb(ctx,r,g,b);
        cairo_paint(ctx); flush(surface);
        ok &= ((uint32_t*)bytes(surface))[0]==(0xFF000000|(cfg->mode?cfg->colors.glyphForeground:c));
    }
    void* gradient=linear(0.,0.,0.,24.);
    const DWORD native[]={0xF5F2DD,0xFCFAEB,0xEEEAD3,0xEBE6CC,0xD6D1BA,0xCCC6AD};
    for (unsigned i=0;i<6;++i) {
        DWORD c=native[i];
        cairo_pattern_add_color_stop_rgba(gradient,i/5.,((c>>16)&255)/255.,((c>>8)&255)/255.,(c&255)/255.,1.);
        double off,r,g,b,a;
        ok &= stops(gradient,i,&off,&r,&g,&b,&a)==0;
        DWORD actual=((DWORD)(r*255+.5)<<16)|((DWORD)(g*255+.5)<<8)|(DWORD)(b*255+.5);
        ok &= actual==(cfg->mode?(i<4?cfg->colors.rasterBackground:cfg->colors.border):c);
    }
    printf("%s chrome: %s; RGBA caption, cached caption ink, logon states, tab gradients\n",cfg->themeId,ok?"PASS":"FAIL");
    destroyPattern(gradient); destroyContext(ctx); destroySurface(surface);
    return ok;
}
