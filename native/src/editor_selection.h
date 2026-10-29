#pragma once
#include "protocol.h"
bool initializeEditorSelection(HMODULE cairo);
void rememberEditorSource(void* context,DWORD original,bool alphaSource=true);
bool beginEditorSelectionFill(void* context,const RenderColors& colors);
void endEditorSelectionFill(void* context,bool changed);
bool editorSelectionGlyph(void* context);
bool drawComparisonGlyphs(void* context,const void* glyphs,int count,DWORD background,DWORD fallback,
                          void(__cdecl* draw)(void*,const void*,int));
bool editorSelectionSupported();
bool beginUiHighlightFill(void* context,const RenderColors& colors);
unsigned long long editorSelectionFills();
unsigned long long editorSelectionGlyphs();
