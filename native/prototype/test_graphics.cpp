#include <windows.h>
extern "C" __declspec(dllimport) void cairo_set_source(void*,void*);
extern "C" __declspec(dllimport) void cairo_mask(void*,void*);
extern "C" __declspec(dllimport) void cairo_mask_surface(void*,void*,double,double);
extern "C" __declspec(dllexport) void prototype_set_source(void* context,void* pattern) {
    cairo_set_source(context,pattern);
}
extern "C" __declspec(dllexport) void prototype_mask(void* context,void* pattern) {
    cairo_mask(context,pattern);
}
extern "C" __declspec(dllexport) void prototype_mask_surface(void* context,void* surface,double x,double y) {
    cairo_mask_surface(context,surface,x,y);
}
