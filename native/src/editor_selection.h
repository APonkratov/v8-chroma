#pragma once
#include "protocol.h"
bool initializeEditorSelection(HMODULE cairo);
void rememberEditorSource(void* context,DWORD original,bool alphaSource=true);
bool beginEditorSelectionFill(void* context,const RenderColors& colors);
void endEditorSelectionFill(void* context,bool changed);
bool editorSelectionGlyph(void* context);
class EditorSelectionPaint {
public:
    explicit EditorSelectionPaint(bool track);
    ~EditorSelectionPaint();
    EditorSelectionPaint(const EditorSelectionPaint&) = delete;
    EditorSelectionPaint& operator=(const EditorSelectionPaint&) = delete;

private:
    friend bool beginEditorSelectionFill(void*, const RenderColors&);
    friend bool drawSyntaxSelectionGlyphs(void*, const void*, int, const RenderColors&,
                                          void(__cdecl*)(void*, const void*, int));
    EditorSelectionPaint* previous;
    void* selectionContext = nullptr;
};
bool drawSyntaxSelectionGlyphs(void* context, const void* glyphs, int count, const RenderColors& colors,
                               void(__cdecl* draw)(void*, const void*, int));
bool drawComparisonGlyphs(void* context,const void* glyphs,int count,DWORD background,DWORD fallback,
                          void(__cdecl* draw)(void*,const void*,int));
bool drawInactiveCaptionGlyphs(void* context,const void* glyphs,int count,const RenderColors& colors,
                               void(__cdecl* draw)(void*,const void*,int));
bool editorSelectionSupported();
bool beginUiHighlightFill(void* context,const RenderColors& colors);
unsigned long long editorSelectionFills();
unsigned long long editorSelectionGlyphs();
