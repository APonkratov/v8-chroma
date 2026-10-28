#pragma once
#include <windows.h>
// Only native top-level windows of this process; all supported attributes are restored.
void updateWindowTheme(bool enabled, COLORREF caption, COLORREF text, COLORREF border,bool dark=true);
unsigned themedWindowCount();
