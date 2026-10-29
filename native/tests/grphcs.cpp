#include "../src/protocol.h"
#include <math.h>
struct Color { double r,g,b,a,offset; unsigned corner; };
extern "C" __declspec(dllimport) void cairo_set_source_rgb(Color*,double,double,double);
extern "C" __declspec(dllimport) void cairo_set_source_rgba(Color*,double,double,double,double);
extern "C" __declspec(dllimport) void cairo_pattern_add_color_stop_rgba(Color*,double,double,double,double,double);
extern "C" __declspec(dllimport) void cairo_mesh_pattern_set_corner_color_rgba(Color*,unsigned,double,double,double,double);
extern "C" __declspec(dllimport) void cairo_show_glyphs(Color*,Color*,int);
extern "C" __declspec(dllimport) void cairo_paint(Color*);
extern "C" __declspec(dllimport) void cairo_paint_with_alpha(Color*,double);
extern "C" __declspec(dllimport) void cairo_fill(Color*);
extern "C" __declspec(dllimport) void cairo_fill_preserve(Color*);
extern "C" __declspec(dllimport) void cairo_set_source(void*,void*);
extern "C" __declspec(dllimport) void cairo_mask(void*,void*);
extern "C" __declspec(dllimport) void cairo_mask_surface(void*,void*,double,double);
extern "C" __declspec(dllexport) void graphics_icon_probe() {
    cairo_set_source(nullptr,nullptr);
    cairo_mask(nullptr,nullptr);
    cairo_mask_surface(nullptr,nullptr,0,0);
}
static DWORD expected(const ProbeConfig* cfg,DWORD kind,DWORD source) {
    if (cfg->mode) for (DWORD i=0;i<cfg->mapCount;++i)
        if (cfg->maps[i].kind==kind && cfg->maps[i].source==source) return cfg->maps[i].target;
    return source;
}
static bool colorIs(const Color &c,DWORD rgb) {
    return c.r==((rgb>>16)&255)/255. && c.g==((rgb>>8)&255)/255. && c.b==(rgb&255)/255.;
}
static bool same(const Color &a,const Color &b) { return a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a; }
static DWORD glyph(const ProbeConfig* cfg,DWORD current) {
    if (!cfg->mode) return current;
    if (current==cfg->colors.selectionBackground) return cfg->colors.selectionForeground;
    if (current==cfg->colors.glyphBackground) return cfg->colors.glyphForeground;
    return current;
}
extern "C" __declspec(dllexport) BOOL graphics_probe(const ProbeConfig* cfg) {
    Color c;
    cairo_set_source_rgb(&c,0.,0.,0.);
    BOOL valid=colorIs(c,expected(cfg,CairoRgb,0));
    cairo_set_source_rgba(&c,0.,0.,0.,.123456789);
    valid &= c.a==.123456789 && colorIs(c,expected(cfg,CairoRgba,0));
    cairo_pattern_add_color_stop_rgba(&c,.375,0.,0.,0.,.333333333);
    valid &= c.offset==.375 && c.a==.333333333 && colorIs(c,expected(cfg,ColorStop,0));
    cairo_mesh_pattern_set_corner_color_rgba(&c,3,0.,0.,0.,.987654321);
    valid &= c.corner==3 && c.a==.987654321 && colorIs(c,expected(cfg,MeshColor,0));
    cairo_set_source_rgba(&c,.123456789,.234567891,.345678912,.456789123);
    valid &= c.r==.123456789 && c.g==.234567891 && c.b==.345678912 && c.a==.456789123;
    cairo_set_source_rgba(&c,1.,1.,1.,.7654321);
    Color rendered,saved=c;
    cairo_show_glyphs(&c,&rendered,1);
    valid &= colorIs(rendered,glyph(cfg,expected(cfg,CairoRgba,0xFFFFFF))) && rendered.a==.7654321 && same(c,saved);
    cairo_set_source_rgba(&c,61./255.,78./255.,143./255.,.432109876);
    saved=c; cairo_show_glyphs(&c,&rendered,1);
    valid &= colorIs(rendered,glyph(cfg,expected(cfg,CairoRgba,0x3D4E8F))) && rendered.a==.432109876 && same(c,saved);
    cairo_set_source_rgb(&c,83./255.,106./255.,194./255.);
    saved=c; cairo_show_glyphs(&c,&rendered,1);
    valid &= colorIs(rendered,glyph(cfg,expected(cfg,CairoRgb,0x536AC2))) && rendered.a==1. && same(c,saved);
    // Pale ink used by the disabled Apply caption must remain legible in light themes.
    cairo_set_source_rgb(&c,255./255.,250./255.,217./255.);
    saved=c; cairo_show_glyphs(&c,&rendered,1);
    DWORD ink=expected(cfg,CairoRgb,0xFFFAD9);
    // The two bundled light themes must draw a dark caption, not their pale hover fill.
    if (cfg->mode && (strcmp(cfg->themeId,"visual-studio-light")==0 || strcmp(cfg->themeId,"github-light-default")==0))
        ink=cfg->colors.glyphForeground;
    valid &= colorIs(rendered,ink) && rendered.a==1. && same(c,saved);
    cairo_mesh_pattern_set_corner_color_rgba(&c,2,245./255.,242./255.,221./255.,.75);
    valid &= colorIs(c,expected(cfg,MeshColor,0xF5F2DD)) && c.a==.75 && c.corner==2;
    cairo_pattern_add_color_stop_rgba(&c,.625,255./255.,237./255.,102./255.,.25);
    valid &= colorIs(c,expected(cfg,ColorStop,0xFFED66)) && c.a==.25 && c.offset==.625;
    cairo_set_source_rgba(&c,230./255.,230./255.,230./255.,1.);
    valid &= colorIs(c,expected(cfg,CairoRgba,0xE6E6E6));
    cairo_set_source_rgb(&c,235./255.,231./255.,205./255.);
    valid &= colorIs(c,expected(cfg,CairoRgb,0xEBE7CD));
    cairo_set_source_rgb(&c,191./255.,205./255.,219./255.);
    valid &= colorIs(c,expected(cfg,CairoRgb,0xBFCDDB));
    cairo_set_source_rgba(&c,148./255.,148./255.,148./255.,1.);
    valid &= colorIs(c,expected(cfg,CairoRgba,0x949494));
    saved=c;
    cairo_paint(&c); cairo_paint_with_alpha(&c,.375); cairo_fill(&c); cairo_fill_preserve(&c);
    valid &= same(c,saved);
    return valid;
}
