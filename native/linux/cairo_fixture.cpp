#include <cairo.h>
#include <dlfcn.h>
#include <cstdlib>

// An owned vendor-shaped library delegates to real system Cairo. This tests
// loader auditing and actual graphics state without redistributing 1C code.
template<class T> T next(const char* name) {
    auto result = reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
    if (!result) std::abort();
    return result;
}
extern "C" void cairo_set_source_rgb(cairo_t* c, double r, double g, double b) {
    static auto real = next<decltype(&cairo_set_source_rgb)>("cairo_set_source_rgb"); real(c, r, g, b);
}
extern "C" void cairo_set_source_rgba(cairo_t* c, double r, double g, double b, double a) {
    static auto real = next<decltype(&cairo_set_source_rgba)>("cairo_set_source_rgba"); real(c, r, g, b, a);
}
extern "C" void cairo_pattern_add_color_stop_rgb(cairo_pattern_t* p, double o, double r, double g, double b) {
    static auto real = next<decltype(&cairo_pattern_add_color_stop_rgb)>("cairo_pattern_add_color_stop_rgb"); real(p, o, r, g, b);
}
extern "C" void cairo_pattern_add_color_stop_rgba(cairo_pattern_t* p, double o, double r, double g, double b, double a) {
    static auto real = next<decltype(&cairo_pattern_add_color_stop_rgba)>("cairo_pattern_add_color_stop_rgba"); real(p, o, r, g, b, a);
}
extern "C" void cairo_mesh_pattern_set_corner_color_rgb(cairo_pattern_t* p, unsigned n, double r, double g, double b) {
    static auto real = next<decltype(&cairo_mesh_pattern_set_corner_color_rgb)>("cairo_mesh_pattern_set_corner_color_rgb"); real(p, n, r, g, b);
}
extern "C" void cairo_mesh_pattern_set_corner_color_rgba(cairo_pattern_t* p, unsigned n, double r, double g, double b, double a) {
    static auto real = next<decltype(&cairo_mesh_pattern_set_corner_color_rgba)>("cairo_mesh_pattern_set_corner_color_rgba"); real(p, n, r, g, b, a);
}
