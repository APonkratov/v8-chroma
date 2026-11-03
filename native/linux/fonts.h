#pragma once
#include <cstdint>
namespace linux_fonts {
struct Config {
    std::uint32_t size,version,editorEnabled,points10,uiEnabled;
    char16_t editor[32],ui[32];
};
static_assert(sizeof(Config)==148);
struct Stats {
    unsigned generation,available,refresh,editorEnabled,uiEnabled,points10,uiFallback;
    std::uint64_t editorCalls,uiCalls,conversions;
    char editor[128],ui[128],error[64],source[128],uiSource[128];
};
bool initialize(const char* platform,const char* config);
bool reload();
void refresh(); // GTK main thread only.
Stats stats();
}
