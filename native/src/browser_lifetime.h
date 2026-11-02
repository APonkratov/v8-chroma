#pragma once
#include <windows.h>

using BrowserDestroyWindow = BOOL (WINAPI*)(HWND);
BOOL destroyBrowserWindow(HWND window, BrowserDestroyWindow original, bool enabled);
unsigned long long browserDestroyCalls();
unsigned long long browserBlockedRequests();
unsigned long long browserGuardFailures();
