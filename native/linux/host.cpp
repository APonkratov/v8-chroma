#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <string>
#include <unistd.h>
#include <sys/prctl.h>

static void* (*platformLookup)(void*, const char*);
static bool usePlatform;

template<class T> T api(void* library, const char* name) {
    auto result = reinterpret_cast<T>(usePlatform ? platformLookup(library, name) : dlsym(library, name));
    if (!result) { std::cerr << name << ": " << dlerror() << '\n'; std::exit(3); }
    return result;
}
struct Cairo {
    void* library;
    explicit Cairo(const char* path) : library(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {
        if (!library) { std::cerr << dlerror() << '\n'; std::exit(2); }
    }
    void sample(unsigned kind, double& r, double& g, double& b, double& a, bool opaque) {
        if (kind == 5 || kind == 6) {
            void* surface = api<void* (*)(int, int, int)>(library, "cairo_image_surface_create")(0, 1, 1);
            void* context = api<void* (*)(void*)>(library, "cairo_create")(surface);
            if (kind == 5) api<void (*)(void*, double, double, double)>(library, "cairo_set_source_rgb")(context, r, g, b);
            else api<void (*)(void*, double, double, double, double)>(library, "cairo_set_source_rgba")(context, r, g, b, a);
            void* pattern = api<void* (*)(void*)>(library, "cairo_get_source")(context);
            int status = api<int (*)(void*, double*, double*, double*, double*)>(library, "cairo_pattern_get_rgba")(pattern, &r, &g, &b, &a);
            if (status) std::exit(4);
            api<void (*)(void*)>(library, "cairo_destroy")(context);
            api<void (*)(void*)>(library, "cairo_surface_destroy")(surface);
        } else if (kind == 7) {
            void* pattern = api<void* (*)(double, double, double, double)>(library, "cairo_pattern_create_linear")(0, 0, 1, 1);
            if (opaque) api<void (*)(void*, double, double, double, double)>(library, "cairo_pattern_add_color_stop_rgb")(pattern, .5, r, g, b);
            else api<void (*)(void*, double, double, double, double, double)>(library, "cairo_pattern_add_color_stop_rgba")(pattern, .5, r, g, b, a);
            double offset;
            int status = api<int (*)(void*, int, double*, double*, double*, double*, double*)>(library, "cairo_pattern_get_color_stop_rgba")(pattern, 0, &offset, &r, &g, &b, &a);
            if (status || offset != .5) std::exit(5);
            api<void (*)(void*)>(library, "cairo_pattern_destroy")(pattern);
        } else {
            void* pattern = api<void* (*)()>(library, "cairo_pattern_create_mesh")();
            api<void (*)(void*)>(library, "cairo_mesh_pattern_begin_patch")(pattern);
            api<void (*)(void*, double, double)>(library, "cairo_mesh_pattern_move_to")(pattern, 0, 0);
            auto line = api<void (*)(void*, double, double)>(library, "cairo_mesh_pattern_line_to");
            line(pattern, 1, 0); line(pattern, 1, 1); line(pattern, 0, 1);
            if (opaque) api<void (*)(void*, unsigned, double, double, double)>(library, "cairo_mesh_pattern_set_corner_color_rgb")(pattern, 0, r, g, b);
            else api<void (*)(void*, unsigned, double, double, double, double)>(library, "cairo_mesh_pattern_set_corner_color_rgba")(pattern, 0, r, g, b, a);
            api<void (*)(void*)>(library, "cairo_mesh_pattern_end_patch")(pattern);
            int status = api<int (*)(void*, unsigned, unsigned, double*, double*, double*, double*)>(library, "cairo_mesh_pattern_get_corner_color_rgba")(pattern, 0, 0, &r, &g, &b, &a);
            if (status) std::exit(6);
            api<void (*)(void*)>(library, "cairo_pattern_destroy")(pattern);
        }
    }
};
int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 1;
    bool serve = argc == 4 && std::string(argv[3]) == "--serve";
    bool live = argc == 4 && std::string(argv[3]) == "--live";
    if (argc == 4 && !serve && !live) {
        void* binder = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
        if (!binder) return 7;
        platformLookup = api<void* (*)(void*, const char*)>(binder, "platform_lookup");
    }
    // Vendor Cairo also expects these X11 symbols in its host's global scope.
    for (auto name : {"libX11.so.6", "libXext.so.6", "libXrender.so.1"}) dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    Cairo vendor(argv[2]), system("libcairo.so.2");
    void (*graphicsSample)(unsigned, double*, double*, double*, double*) = nullptr;
    void (*graphicsFont)(unsigned, double*, double*, double*, double*) = nullptr;
    if (live) {
        // Test-only consent permits a sibling injector on CI hosts with Yama.
        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
        std::string path(argv[2]); path = path.substr(0, path.find_last_of('/')) + "/grphcs.so";
        void* graphics = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!graphics) return 8;
        api<void (*)()>(graphics, "graphics_init")();
        graphicsSample = api<decltype(graphicsSample)>(graphics, "graphics_sample");
        graphicsFont = api<decltype(graphicsFont)>(graphics, "graphics_font");
    }
    if (serve) {
        for (;;) {
            double r = 1, g = 1, b = 1, a = 1;
            vendor.sample(5, r, g, b, a, true);
            usleep(125000);
        }
    }
    std::string which;
    unsigned kind, opaque;
    double r, g, b, a;
    while (std::cin >> which >> kind >> opaque >> r >> g >> b >> a) {
        usePlatform = which == "platform" && platformLookup;
        if (live && which == "font") graphicsFont(kind, &r, &g, &b, &a);
        else if (live && which == "vendor") graphicsSample(kind, &r, &g, &b, &a);
        else (which == "vendor" ? vendor : system).sample(kind, r, g, b, a, opaque != 0);
        std::cout << std::setprecision(17) << r << ' ' << g << ' ' << b << ' ' << a << std::endl;
    }
}
