#include <windows.h>
struct Color { double r,g,b,a,offset; unsigned corner; };
extern "C" __declspec(dllexport) void cairo_set_source_rgb(Color* c,double r,double g,double b) { *c={r,g,b,1.,0.,0}; }
// Import-library symbol for the optional prototype fixture; it loads real Cairo.
extern "C" __declspec(dllexport) void cairo_set_source(void*,void*) {}
extern "C" __declspec(dllexport) void cairo_mask(void*,void*) {}
extern "C" __declspec(dllexport) void cairo_mask_surface(void*,void*,double,double) {}
extern "C" __declspec(dllexport) void cairo_set_source_rgba(Color* c,double r,double g,double b,double a) { *c={r,g,b,a,0.,0}; }
extern "C" __declspec(dllexport) void cairo_pattern_add_color_stop_rgba(Color* c,double o,double r,double g,double b,double a) { *c={r,g,b,a,o,0}; }
extern "C" __declspec(dllexport) void cairo_mesh_pattern_set_corner_color_rgba(Color* c,unsigned corner,double r,double g,double b,double a) { *c={r,g,b,a,0.,corner}; }
extern "C" __declspec(dllexport) Color* cairo_get_source(Color* c) { return c; }
extern "C" __declspec(dllexport) int cairo_pattern_get_rgba(Color* c,double* r,double* g,double* b,double* a) { *r=c->r; *g=c->g; *b=c->b; *a=c->a; return 0; }
extern "C" __declspec(dllexport) void cairo_show_glyphs(Color* c,Color* result,int) { *result=*c; }
extern "C" __declspec(dllexport) void cairo_paint(Color*) {}
extern "C" __declspec(dllexport) void cairo_paint_with_alpha(Color*,double) {}
extern "C" __declspec(dllexport) void cairo_fill(Color*) {}
extern "C" __declspec(dllexport) void cairo_fill_preserve(Color*) {}
