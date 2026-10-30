#include "../src/ui_font_policy.h"
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <initializer_list>

int wmain() {
    for (auto alias : {L"MS Shell Dlg", L"ms shell dlg 2", L"MS Sans Serif", L"Microsoft Sans Serif"}) {
        LOGFONTW font{};
        wcscpy_s(font.lfFaceName, alias);
        font.lfHeight=-18; font.lfWidth=7; font.lfWeight=FW_BOLD;
        font.lfItalic=1; font.lfUnderline=1; font.lfCharSet=RUSSIAN_CHARSET;
        font.lfQuality=CLEARTYPE_QUALITY; font.lfEscapement=900;
        auto expected=font;
        wcscpy_s(expected.lfFaceName,L"Segoe UI Variable Small"); expected.lfWidth=0;
        // Ignore bytes after the NUL: LOGFONT strings have no padding contract.
        if (!substituteInterfaceFont(font,L"Segoe UI Variable Small") ||
            wcscmp(font.lfFaceName,expected.lfFaceName) || memcmp(&font,&expected,offsetof(LOGFONTW,lfFaceName))) return 1;
    }
    for (auto face : {L"Courier New", L"Consolas", L"Arial", L"Tahoma", L"Wingdings", L"Segoe UI"}) {
        LOGFONTW font{}; wcscpy_s(font.lfFaceName,face); auto before=font;
        if (substituteInterfaceFont(font,L"Segoe UI Variable Small") || memcmp(&font,&before,sizeof(font))) return 2;
    }
    LOGFONTW font{}; wcscpy_s(font.lfFaceName,L"MS Sans Serif");
    auto before=font;
    if (substituteInterfaceFont(font,L"") || memcmp(&font,&before,sizeof(font))) return 3;
    font.lfCharSet=SYMBOL_CHARSET; before=font;
    if (substituteInterfaceFont(font,L"Segoe UI") || memcmp(&font,&before,sizeof(font))) return 4;
    std::puts("Interface aliases: size/style preserved, explicit faces/symbols/native mode untouched");
    return 0;
}
