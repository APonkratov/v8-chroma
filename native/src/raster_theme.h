#pragma once
#include <windows.h>
bool initializeRasterTheme(HMODULE cairo, DWORD background, DWORD foreground, DWORD border);
void* prepareRasterSource(void* context);
void restoreRasterSource(void* context,void* original);
unsigned long long rasterChanges();
unsigned long long rasterHoverFrames();
bool rasterSupported();

void setRasterThemeColors(DWORD background,DWORD foreground,DWORD border);
