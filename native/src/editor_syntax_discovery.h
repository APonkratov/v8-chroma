#pragma once
#include <windows.h>

struct ResolvedSyntax {
    BYTE *paint, *close, *invalidate, *change, *save, *getLine;
    BYTE *clearString, *getRuns, *slot, *setRef;
    const BYTE *viewTable, *ownerTable, *sourceTable, *lexerTable;
    unsigned viewContextOffset, contextOwnerOffset;
    const BYTE* canvasFillReturn;
    const void* const* canvasFillImport;
    BYTE* textMarginFill;
};

// Uses loaded image structure and routine fingerprints. No build number or
// fixed module RVA is required. An incomplete or ambiguous match is rejected.
bool discoverSyntax(HMODULE frame, HMODULE ui, HMODULE text, HMODULE core,
                    HMODULE debug, ResolvedSyntax& result);
