#pragma once
#include <cstdint>
namespace linux_icons {
bool initialize(unsigned bank, void* cairo);
void configure(unsigned mode);
void source(unsigned bank, void* context, void* pattern, void(*original)(void*,void*));
void surface(unsigned bank, void* context, void* image, double x, double y, void(*original)(void*,void*,double,double));
struct Stats { unsigned mode; std::uint64_t calls, matches; };
Stats stats();
}
