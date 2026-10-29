#pragma once
#include <windows.h>
// Caller order: probe themeLock -> internal icon lock. No callback acquires themeLock.
struct IconState;
bool initializeIconTheme(HMODULE cairo);
IconState* prepareIconTheme(unsigned mode, DWORD& error);
void activateIconTheme(IconState* state);
void discardIconTheme(IconState* state);
void drawIconSource(void* context, void* pattern, void(__cdecl* original)(void*,void*));
void drawIconMask(void* context, void* pattern, void(__cdecl* original)(void*,void*));
void drawIconMaskSurface(void* context,void* surface,double x,double y,
                         void(__cdecl* original)(void*,void*,double,double));
struct IconStats { unsigned mode, cached; unsigned long long bytes, calls, matches; };
IconStats iconStats();
