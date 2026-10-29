#pragma once
#include <windows.h>
enum ApiKind { Text=1, Background, Brush, Pen, CairoRgb, CairoRgba, ColorStop, MeshColor };
struct ColorMap { DWORD kind, source, target; };
struct RenderColors {
    DWORD glyphBackground, glyphForeground, selectionBackground, selectionForeground;
    DWORD rasterBackground, rasterForeground;
    DWORD captionBackground, captionText, border, darkCaption;
};
struct ProbeConfig {
    DWORD size, version, mode, mapCount;
    char themeId[64];
    RenderColors colors;
    wchar_t targetDir[260];
    wchar_t logPath[520];
    ColorMap maps[256];
    DWORD iconMode, iconPack; // 0=stock, 1=light, 2=dark; pack 1=8.3.27.2325
};
static_assert(sizeof(RenderColors)==40 && sizeof(ProbeConfig)==4760, "Protocol v3 layout mismatch");
