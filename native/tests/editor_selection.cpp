#include "../src/editor_selection.h"
#include "../src/contrast.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

struct UserExtents { double xBearing,yBearing,width,height,xAdvance,yAdvance; };
static unsigned userConversions;
static void*(*testSource)(void*);
static int(*testRgba)(void*,double*,double*,double*,double*);
static DWORD glyphInks[3];
static unsigned glyphBatches;
static void recordGlyphInk(void* context,const void*,int) {
    double r,g,b,a;
    if (glyphBatches >= 3 || testRgba(testSource(context),&r,&g,&b,&a)) return;
    glyphInks[glyphBatches++]=RGB(unsigned(r*255+.5),unsigned(g*255+.5),unsigned(b*255+.5));
}
static int userUnicode(void*,unsigned long codepoint,unsigned long* glyph) {
    ++userConversions; *glyph=codepoint; return 0;
}
static int userRender(void*,unsigned long,void*,UserExtents* extents) {
    *extents={0.,0.,1.,1.,1.,0.}; return 0;
}
template<class T> T load(HMODULE module,const char* name) {
    auto symbol=GetProcAddress(module,name); T value=nullptr; memcpy(&value,&symbol,sizeof(value)); return value;
}
int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    HMODULE cairo=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!cairo || !initializeEditorSelection(cairo)) return 3;
    auto image=load<void*(*)(int,int,int)>(cairo,"cairo_image_surface_create");
    auto bytes=load<unsigned char*(*)(void*)>(cairo,"cairo_image_surface_get_data");
    auto context=load<void*(*)(void*)>(cairo,"cairo_create");
    auto rgba=load<void(*)(void*,double,double,double,double)>(cairo,"cairo_set_source_rgba");
    auto rectangle=load<void(*)(void*,double,double,double,double)>(cairo,"cairo_rectangle");
    auto getSource=load<void*(*)(void*)>(cairo,"cairo_get_source");
    auto family=load<void(*)(void*,const char*,int,int)>(cairo,"cairo_select_font_face");
    auto fontSize=load<void(*)(void*,double)>(cairo,"cairo_set_font_size");
    auto fill=load<void(*)(void*)>(cairo,"cairo_fill");
    auto flush=load<void(*)(void*)>(cairo,"cairo_surface_flush");
    auto save=load<void(*)(void*)>(cairo,"cairo_save"); auto restore=load<void(*)(void*)>(cairo,"cairo_restore");
    auto destroyContext=load<void(*)(void*)>(cairo,"cairo_destroy");
    auto destroySurface=load<void(*)(void*)>(cairo,"cairo_surface_destroy");
    auto newPath=load<void(*)(void*)>(cairo,"cairo_new_path");
    auto setOperator=load<void(*)(void*,int)>(cairo,"cairo_set_operator");
    void* output=image(0,80,40),*ctx=context(output);
    family(ctx,"Courier New",0,0); fontSize(ctx,12.);
    RenderColors colors={}; colors.selectionBackground=0x7448B0; colors.selectionForeground=0xFFFFFF;
    auto black=[&] { rgba(ctx,1.,1.,1.,1.); rememberEditorSource(ctx,0); };
    black(); void* original=getSource(ctx); rectangle(ctx,2,2,60,16);
    bool changed=beginEditorSelectionFill(ctx,colors); bool ok=changed;
    fill(ctx); endEditorSelectionFill(ctx,changed); flush(output);
    ok &= getSource(ctx)==original && ((uint32_t*)bytes(output))[80*5+5]==0xFF7448B0;
    // Distinct selection foreground, all default inversions and source stack.
    for (DWORD rgb:{0xFFFFFFu,0x00FFFFu,0xFFFF00u,0xFF7FFFu,0x69CDFFu}) {
        rgba(ctx,0.,1.,1.,.4); rememberEditorSource(ctx,rgb);
        ok &= editorSelectionGlyph(ctx);
        save(ctx); rgba(ctx,1.,0.,0.,1.); rememberEditorSource(ctx,0xFF0000); restore(ctx);
        ok &= editorSelectionGlyph(ctx);
    }
    rgba(ctx,1.,1.,1.,1.); rememberEditorSource(ctx,0x0000FF); ok &= !editorSelectionGlyph(ctx);
    // Two native colors can map to exactly the same target. Saving a pattern
    // must preserve its origin even if Cairo reuses the unchanged solid source.
    rgba(ctx,1.,1.,1.,1.); rememberEditorSource(ctx,0xFFFFFF); save(ctx);
    rgba(ctx,1.,1.,1.,1.); rememberEditorSource(ctx,0x0000FF); restore(ctx);
    ok &= editorSelectionGlyph(ctx);
    // A caret, an ordinary RGB UI brush, a non-black brush and unsupported compositing
    // must never be mistaken for an editor selection background.
    black(); newPath(ctx); rectangle(ctx,2,2,1,16); ok &= !beginEditorSelectionFill(ctx,colors);
    newPath(ctx); rectangle(ctx,2,2,60,16); family(ctx,"Arial",0,0); rememberEditorSource(ctx,0,false); ok &= !beginEditorSelectionFill(ctx,colors);
    // First-line selection precedes setting the monospaced editor font.
    rememberEditorSource(ctx,0,true); changed=beginEditorSelectionFill(ctx,colors); ok &= changed; endEditorSelectionFill(ctx,changed);
    family(ctx,"Courier New",0,0); rememberEditorSource(ctx,0x333333); ok &= !beginEditorSelectionFill(ctx,colors);
    black(); setOperator(ctx,6); ok &= !beginEditorSelectionFill(ctx,colors); setOperator(ctx,2);
    // Live palette updates change only the intended rectangle.
    colors.selectionBackground=0xADD6FF; black(); newPath(ctx); rectangle(ctx,2,2,60,16);
    changed=beginEditorSelectionFill(ctx,colors); ok &= changed; fill(ctx); endEditorSelectionFill(ctx,changed); flush(output);
    ok &= ((uint32_t*)bytes(output))[80*5+5]==0xFFADD6FF;
    // A TextMate glyph batch can include ordinary ink on both sides of a selection.
    struct Glyph { unsigned long index; double x,y; };
    Glyph mixed[]={{42,6,10},{42,40,10},{42,72,10}};
    testSource=getSource;
    testRgba=load<int(*)(void*,double*,double*,double*,double*)>(cairo,"cairo_pattern_get_rgba");
    colors.glyphBackground=0x011627;
    colors.selectionBackground=0x103362;
    colors.selectionForeground=0xD8EAFA;
    {
        EditorSelectionPaint paint(true);
        black(); newPath(ctx); rectangle(ctx,20,2,35,16);
        changed=beginEditorSelectionFill(ctx,colors); ok &= changed; fill(ctx); endEditorSelectionFill(ctx,changed);
        rgba(ctx,16./255.,52./255.,151./255.,1.); rememberEditorSource(ctx,0x103497);
        original=getSource(ctx);
        ok &= !editorSelectionGlyph(ctx);
        ok &= drawSyntaxSelectionGlyphs(ctx,mixed,3,colors,recordGlyphInk);
        ok &= glyphBatches==3 && glyphInks[0]==RGB(16,52,151) &&
            glyphInks[1]==RGB(0xD8,0xEA,0xFA) && glyphInks[2]==glyphInks[0] && getSource(ctx)==original;
    }
    ok &= !drawSyntaxSelectionGlyphs(ctx,mixed,3,colors,recordGlyphInk);
    // Native gray caption ink can share the inactive frame's border fill.
    family(ctx,"Arial",0,0);
    colors.border=0x5F7E97; colors.captionText=0xD6DEEB;
    rgba(ctx,95./255.,126./255.,151./255.,1.);
    newPath(ctx); rectangle(ctx,20,2,35,16); fill(ctx);
    rememberEditorSource(ctx,0x666666);
    original=getSource(ctx); glyphBatches=0;
    ok &= drawInactiveCaptionGlyphs(ctx,mixed,3,colors,recordGlyphInk);
    DWORD corrected=(GetRValue(glyphInks[1])<<16)|(GetGValue(glyphInks[1])<<8)|GetBValue(glyphInks[1]);
    ok &= glyphBatches==3 && glyphInks[0]==RGB(95,126,151) && glyphInks[2]==glyphInks[0] &&
        ChromaContrast::ratio(corrected,colors.border)>=4.5 && getSource(ctx)==original;
    rememberEditorSource(ctx,0x777777);
    ok &= !drawInactiveCaptionGlyphs(ctx,mixed,3,colors,recordGlyphInk);
    // Browser fonts supply callbacks owned by WebKit. Merely recognizing an
    // editor glyph must not invoke their Unicode/shaping/rendering callbacks.
    auto userFace=load<void*(*)()>(cairo,"cairo_user_font_face_create");
    auto setUnicode=load<void(*)(void*,decltype(&userUnicode))>(cairo,"cairo_user_font_face_set_unicode_to_glyph_func");
    auto setRender=load<void(*)(void*,decltype(&userRender))>(cairo,"cairo_user_font_face_set_render_glyph_func");
    auto setFace=load<void(*)(void*,void*)>(cairo,"cairo_set_font_face");
    auto destroyFace=load<void(*)(void*)>(cairo,"cairo_font_face_destroy");
    void* face=userFace(); setUnicode(face,userUnicode); setRender(face,userRender); setFace(ctx,face);
    rgba(ctx,1.,1.,1.,1.); rememberEditorSource(ctx,0xFFFFFF);
    bool browserExcluded=!editorSelectionGlyph(ctx) && userConversions==0;
    // Contrast correction must also reject USER fonts before querying metrics.
    rgba(ctx,98./255.,114./255.,164./255.,1.);
    Glyph glyph={42,12,25};
    browserExcluded &= !drawComparisonGlyphs(ctx,&glyph,1,0x1E4E5A,0xF8F8F2,
        [](void*,const void*,int) { ++userConversions; }) && userConversions==0;
    rememberEditorSource(ctx,0x808080);
    browserExcluded &= !drawInactiveCaptionGlyphs(ctx,&glyph,1,colors,recordGlyphInk) && userConversions==0;
    printf("Browser font exclusion: %s; Unicode callbacks=%u\n",browserExcluded?"PASS":"FAIL",userConversions);
    ok &= browserExcluded;
    destroyFace(face);
    printf("Real Cairo editor selection: %s; background, inverse glyphs, source stack, caret/UI exclusions, live colors\n",ok?"PASS":"FAIL");
    destroyContext(ctx); destroySurface(output); FreeLibrary(cairo); return ok?0:1;
}
