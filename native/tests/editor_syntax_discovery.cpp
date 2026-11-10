#include "../src/editor_syntax_discovery.h"
#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::wstring directory = argv[1];
    const auto load = [&](const wchar_t* name) {
        return LoadLibraryExW((directory + L"\\" + name).c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    };
    const auto frame = load(L"frame.dll"), ui = load(L"txtedui.dll"), text = load(L"txtedt.dll"), debug = load(L"debug.dll");
    auto core = load(L"core83.dll");
    if (!core) core = load(L"core85.dll");
    if (!frame || !ui || !text || !core || !debug) return 3;
    const auto discover = [&](ResolvedSyntax& out) { return discoverSyntax(frame, ui, text, core, debug, out); };
    ResolvedSyntax found{};
    if (!discover(found)) { std::puts("Editor ABI not recognized"); return 4; }
    std::printf("view context=%#x owner=%#x, paint=%#llx, cache=%#llx\n",
                found.viewContextOffset, found.contextOwnerOffset,
                static_cast<unsigned long long>(found.paint - reinterpret_cast<BYTE*>(frame)),
                static_cast<unsigned long long>(found.getRuns - reinterpret_cast<BYTE*>(text)));

    const auto rejectChange = [&](BYTE* byte, bool backgroundOnly = false) {
        DWORD old;
        if (!VirtualProtect(byte, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
        *byte ^= 1;
        ResolvedSyntax altered{};
        const bool recognized = discover(altered);
        const bool rejected = backgroundOnly ? recognized && !altered.textMarginFill && !altered.canvasFillReturn : !recognized;
        *byte ^= 1;
        DWORD ignored;
        return VirtualProtect(byte, 1, old, &ignored) && rejected;
    };
    // One ordinary instruction and one masked displacement in a verified call.
    if (!rejectChange(found.getRuns + 0x20) || !rejectChange(found.getRuns + 0x14d)) return 5;
    ResolvedSyntax restored{};
    if (!discover(restored) || restored.viewTable != found.viewTable || restored.getRuns != found.getRuns) return 6;
    if (restored.textMarginFill) {
        if (!restored.canvasFillReturn || !restored.canvasFillImport || !rejectChange(restored.textMarginFill, true)) return 7;
        std::puts("Editor background paths discovered and changed margin rejected");
    }
    std::puts("Editor ABI discovered and unsafe changes rejected");
    return 0;
}
