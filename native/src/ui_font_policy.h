#pragma once
#include <windows.h>
#include <cwchar>

// Interface aliases only. Explicit document/editor fonts are not candidates.
inline bool isInterfaceFont(const LOGFONTW& font) {
    if (font.lfCharSet==SYMBOL_CHARSET || font.lfFaceName[LF_FACESIZE-1]) return false;
    return !_wcsicmp(font.lfFaceName,L"MS Shell Dlg") || !_wcsicmp(font.lfFaceName,L"MS Shell Dlg 2") ||
        !_wcsicmp(font.lfFaceName,L"MS Sans Serif") || !_wcsicmp(font.lfFaceName,L"Microsoft Sans Serif");
}
inline bool substituteInterfaceFont(LOGFONTW& font,const wchar_t* family) {
    if (!family || !family[0] || !isInterfaceFont(font)) return false;
    wcscpy_s(font.lfFaceName,family); font.lfWidth=0;
    return true;
}
