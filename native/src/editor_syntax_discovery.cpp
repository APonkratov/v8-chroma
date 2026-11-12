#include "editor_syntax_discovery.h"
#include "pe_image.h"
#include <cstdint>

namespace {
struct Fingerprint {
    unsigned size;
    uint32_t prefix;
    uint64_t digest;
    uint16_t relocations[48];
};
#include "editor_syntax_signatures.h"

using namespace syntaxSignatures;

bool matches(Span span, const Fingerprint& fingerprint) {
    if (span.size != fingerprint.size) return false;
    uint32_t prefix;
    std::memcpy(&prefix, span.data, sizeof(prefix));
    if (prefix != fingerprint.prefix) return false;
    constexpr uint64_t basis = 14695981039346656037ull, prime = 1099511628211ull;
    uint64_t hash = basis;
    const uint16_t* relocation = fingerprint.relocations;
    for (unsigned i = 0; i < span.size;) {
        if (*relocation == i && i) {
            // Four zero bytes replace the relative address. Hash multiplication
            // still accounts for their position in the instruction stream.
            hash *= prime * prime * prime * prime;
            i += 4;
            ++relocation;
        } else {
            hash = (hash ^ span.data[i++]) * prime;
        }
    }
    return hash == fingerprint.digest;
}

struct Match { BYTE* entry = nullptr; unsigned variant = 0; };

template<size_t N> Match find(const Image& image, const Fingerprint (&fingerprints)[N]) {
    if (!image.nt) return {};
    const auto& directory = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!image.contains(image.base + directory.VirtualAddress, directory.Size)) return {};
    const auto* entries = reinterpret_cast<const RUNTIME_FUNCTION*>(image.base + directory.VirtualAddress);
    Match result;
    for (size_t i = 0; i < directory.Size / sizeof(*entries); ++i) {
        const auto& entry = entries[i];
        if (entry.EndAddress <= entry.BeginAddress || entry.EndAddress > image.size) continue;
        Span body{image.base + entry.BeginAddress, entry.EndAddress - entry.BeginAddress};
        for (unsigned variant = 0; variant < N; ++variant) {
            if (!matches(body, fingerprints[variant]) || !image.code(body.data)) continue;
            if (result.entry && result.entry != body.data) return {};
            result = {body.data, variant};
        }
    }
    return result;
}

BYTE* exportedEntry(const Image& image, const char* symbol) {
    auto* address = reinterpret_cast<BYTE*>(GetProcAddress(reinterpret_cast<HMODULE>(image.base), symbol));
    return image.entry(address) && image.code(address) ? address : nullptr;
}

struct Method { unsigned slot; const BYTE* entry; };

template<class Check> const BYTE* table(const Image& image, std::initializer_list<Method> methods, Check check) {
    const auto anchor = *methods.begin();
    if (!anchor.entry) return nullptr;
    unsigned slots = 3;
    for (const auto& method : methods) {
        if (!method.entry) return nullptr;
        if (method.slot >= slots) slots = method.slot + 1;
    }
    const BYTE* result = nullptr;
    bool ambiguous = false;
    image.sections(false, [&](Span section) {
        auto entries = reinterpret_cast<BYTE* const*>(section.data);
        for (size_t i = 0; i + slots <= section.size / sizeof(void*); ++i) {
            const auto* candidate = entries + i;
            if (candidate[anchor.slot] != anchor.entry) continue;
            bool compatible = true;
            for (const auto& method : methods) compatible &= candidate[method.slot] == method.entry;
            if (!compatible || !image.code(candidate[0]) || !image.code(candidate[1]) || !image.code(candidate[2])) continue;
            if (!check(candidate)) continue;
            if (result) ambiguous = true;
            result = reinterpret_cast<const BYTE*>(candidate);
        }
    });
    return ambiguous ? nullptr : result;
}

const BYTE* table(const Image& image, std::initializer_list<Method> methods) {
    return table(image, methods, [](auto) { return true; });
}

bool callAt(BYTE* function, unsigned offset, BYTE* target) {
    auto* call = function + offset;
    return *call == 0xe8 && relative(call, 1, 5) == target;
}

bool adjustsTo(const Image& image, BYTE* thunk, int adjustment, BYTE* target) {
    const Pattern entry{0x48, 0x83, 0xe9, adjustment, 0xe9, -1, -1, -1, -1};
    return target && image.contains(thunk, entry.size) && entry.at({thunk, entry.size}, 0) &&
           relative(thunk + 4, 1, 5) == target;
}
}

bool discoverSyntax(HMODULE frameModule, HMODULE uiModule, HMODULE textModule, HMODULE coreModule,
                    HMODULE debugModule, ResolvedSyntax& result) {
    const Image frame(frameModule), ui(uiModule), text(textModule), core(coreModule), debug(debugModule);
    if (!frame.nt || !ui.nt || !text.nt || !core.nt || !debug.nt) return false;
    const auto draw = find(frame, paint), cleanup = find(frame, close);
    const auto invalidateRows = find(frame, invalidate).entry;
    // The two view layouts are distinguished by QueryInterface's receiver
    // adjustment and the matching paint and cleanup bodies.
    if (!draw.entry || !cleanup.entry || draw.variant != cleanup.variant) return false;
    const bool extendedView = draw.variant == 1;
    const auto viewTable = table(frame, {{extendedView ? 34u : 33u, invalidateRows}}, [&](auto candidate) {
        const auto query = frame.function(candidate[0]);
        return query.data == candidate[0] && matches(query, viewQuery[draw.variant]);
    });
    const auto ownerTable = table(ui, {{0, find(ui, ownerQuery).entry}, {1, find(ui, ownerRetain).entry},
                                      {2, find(ui, ownerRelease).entry}});
    const auto release = find(debug, lexerRelease).entry;
    const auto lexerTable = release ? table(debug, {{3, find(debug, lexerQuery).entry}, {5, find(debug, lexerStyle).entry}}, [&](auto candidate) {
        // BSL and another lexer share the parsing methods. The release thunk
        // identifies which implementation owns this interface.
        return adjustsTo(debug, candidate[2], 8, release);
    }) : nullptr;
    const auto runs = find(text, getRuns).entry;
    const auto lookup = find(text, slot).entry;
    const auto assign = find(text, setRef).entry;
    const auto clear = find(text, clearString).entry;
    const auto destroy = find(text, vectorDestroy).entry;
    const auto sourceTable = table(text, {{11, runs}, {2, find(text, sourceRelease).entry}});
    // Verify the formatter and the ownership chain before copying a run list.
    if (!viewTable || !ownerTable || !lexerTable || !sourceTable || !find(ui, prepare).entry ||
        !lookup || !assign || !clear || !destroy || !callAt(runs, 0x14c, assign) || !callAt(runs, 0x187, lookup) ||
        !callAt(runs, 0x25a, clear) || !callAt(assign, 0x36, destroy)) return false;

    auto* saveText = exportedEntry(core, "?save@TextManager@core@@QEAA_NAEAV?$BasicString@_S$0A@$0A@Vallocator@std@@@ale@@@Z");
    auto* readLine = exportedEntry(core, "?getLine@TextManager@core@@QEAA_NHPEAV?$BasicString@_S$0A@$0A@Vallocator@std@@@ale@@@Z");
    auto* onChange = exportedEntry(core, "?onRichDocumentEvent@TextManager@core@@AEAAXW4Event@IRichDocumentSink@@PEBUChange@4@_K@Z");
    const Pattern changeEntry{0x48,0x8b,0xc4,0x55,0x41,0x56,0x41,0x57};
    if (!matches(core.function(saveText), save[0]) || !matches(core.function(readLine), getLine[0]) ||
        !changeEntry.at(core.function(onChange), 0)) return false;

    result = {draw.entry, cleanup.entry, invalidateRows, onChange, saveText, readLine, clear, runs, lookup, assign,
              viewTable, ownerTable, sourceTable, lexerTable, extendedView ? 0x1e8u : 0x1b8u,
              extendedView ? 0xe0u : 0xd8u};
    // The background is drawn once by the view and again in its text margin.
    // Recognize both paths before enabling either override.
    const unsigned canvasFillOffset = extendedView ? 0xcd : 0xdb;
    auto* call = draw.entry + canvasFillOffset;
    const auto margin = find(ui, textMarginFill);
    if (margin.entry && margin.variant == draw.variant && call[0] == 0xff && call[1] == 0x15) {
        auto* importSlot = relative(call, 2, 6);
        if (frame.contains(importSlot, sizeof(void*))) {
            result.canvasFillReturn = call + 6;
            result.canvasFillImport = reinterpret_cast<const void* const*>(importSlot);
            result.textMarginFill = margin.entry;
        }
    }
    return true;
}

bool discoverComparisonSyntax(HMODULE frameModule, HMODULE uiModule, HMODULE frontendModule,
                              const ResolvedSyntax& editor, ResolvedComparisonSyntax& result) {
    const Image frame(frameModule), ui(uiModule), frontend(frontendModule);
    if (!frame.nt || !ui.nt || !frontend.nt) return false;
    const unsigned variant = editor.viewContextOffset == 0x1e8 ? 1 : 0;
    const auto format = find(ui, formatLine);
    const auto ink = find(frame, setForeground);
    const auto split = find(frame, splitLayoutRange);
    const auto decorator = find(ui, difference);
    if (!format.entry || !ink.entry || !split.entry || !decorator.entry ||
        format.variant != variant || ink.variant != variant || split.variant != variant ||
        decorator.variant != variant || !callAt(ink.entry, 0x14, split.entry)) return false;

    const auto layout = table(frame, {{5, ink.entry}}, [&](auto candidate) {
        const Pattern row{0x48, 0x8b, 0x41, 0x30, 0xc3}, text{0x48, 0x8b, 0x41, 0x38, 0xc3};
        return frame.code(candidate[3]) && frame.contains(candidate[3], row.size) &&
               row.at({candidate[3], row.size}, 0) &&
               frame.code(candidate[4]) && frame.contains(candidate[4], text.size) &&
               text.at({candidate[4], text.size}, 0);
    });
    const auto diff = table(ui, {{4, decorator.entry}}, [&](auto candidate) {
        const auto release = ui.function(candidate[2]);
        return release.data == candidate[2] && matches(release, differenceRelease[0]);
    });
    const auto release = find(frontend, frontendLexerRelease).entry;
    const auto lexer = table(frontend, {{3, find(frontend, frontendLexerQuery).entry},
                                       {5, find(frontend, frontendLexerStyle).entry}}, [&](auto candidate) {
        return adjustsTo(frontend, candidate[2], 0x28, release);
    });
    const auto structured = find(frontend, refreshStructuredComparison).entry;
    const auto text = find(frontend, openTextComparison).entry;
    if (!layout || !diff || !lexer || !structured || !text) return false;

    result = {format.entry, ink.entry, structured, text, layout, diff, lexer,
              variant ? 0x180u : 0x178u, editor.contextOwnerOffset + 0x10u};
    return true;
}
