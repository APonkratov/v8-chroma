#pragma once
#include <cstdint>

// Protocol v3 is little-endian and uses UTF-16 even when wchar_t is 32 bits.
// Keep the Windows ABI unchanged; shared goldens verify both representations.
struct ColorMap { std::uint32_t kind, source, target; };
struct ProbeConfig {
    std::uint32_t size, version, mode, mapCount;
    char themeId[64];
    std::uint32_t colors[10];
    char16_t targetDir[260], logPath[520];
    ColorMap maps[256];
    std::uint32_t iconMode, iconPack;
};
static_assert(sizeof(ProbeConfig) == 4760, "Protocol v3 layout mismatch");
