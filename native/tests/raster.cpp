#include "../src/raster_theme.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
template<class T> T load(HMODULE module,const char* name) {
    auto symbol=GetProcAddress(module,name); T value=nullptr; memcpy(&value,&symbol,sizeof(value)); return value;
}
int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    HMODULE cairo=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!cairo || !initializeRasterTheme(cairo,0x343746,0xF8F8F2,0x191A21)) return 3;
    auto image=load<void*(*)(int,int,int)>(cairo,"cairo_image_surface_create");
    auto bytes=load<unsigned char*(*)(void*)>(cairo,"cairo_image_surface_get_data");
    auto dirty=load<void(*)(void*)>(cairo,"cairo_surface_mark_dirty");
    auto destroySurface=load<void(*)(void*)>(cairo,"cairo_surface_destroy");
    auto context=load<void*(*)(void*)>(cairo,"cairo_create");
    auto destroyContext=load<void(*)(void*)>(cairo,"cairo_destroy");
    auto setSource=load<void(*)(void*,void*,double,double)>(cairo,"cairo_set_source_surface");
    auto getSource=load<void*(*)(void*)>(cairo,"cairo_get_source");
    auto surface=load<int(*)(void*,void**)>(cairo,"cairo_pattern_get_surface");
    auto paint=load<void(*)(void*)>(cairo,"cairo_paint");
    auto flush=load<void(*)(void*)>(cairo,"cairo_surface_flush");
    auto setOperator=load<void(*)(void*,int)>(cairo,"cairo_set_operator");
    auto translate=load<void(*)(void*,double,double)>(cairo,"cairo_translate");
    auto pushGroup=load<void(*)(void*)>(cairo,"cairo_push_group");
    auto popGroup=load<void*(*)(void*)>(cairo,"cairo_pop_group");
    auto patternDestroy=load<void(*)(void*)>(cairo,"cairo_pattern_destroy");
    void* input=image(0,8,8), *output=image(0,8,8), *ctx=context(output);
    auto pixels=(uint32_t*)bytes(input);
    for (unsigned i=0;i<64;++i) pixels[i]=0xFFFFFFFF;
    pixels[1]=0xFF000000; pixels[2]=0x80808080; pixels[3]=0;
    uint32_t original[64]; memcpy(original,pixels,sizeof(original)); dirty(input);
    setSource(ctx,input,0,0); void* source=getSource(ctx);
    void* saved=prepareRasterSource(ctx), *copy=nullptr;
    bool ok=saved==source && getSource(ctx)!=source && !surface(getSource(ctx),&copy);
    auto transformed=(uint32_t*)bytes(copy);
    ok &= transformed[0]==0xFF343746 && transformed[1]==0xFFF8F8F2 &&
          transformed[2]==0x801A1B23 && transformed[3]==0;
    paint(ctx); restoreRasterSource(ctx,saved);
    ok &= getSource(ctx)==source && !memcmp(original,pixels,sizeof(original));
    flush(output); ok &= ((uint32_t*)bytes(output))[0]==0xFF343746;
    // A hover layer that is already dark must not be transformed a second time.
    setSource(ctx,output,0,0); ok &= prepareRasterSource(ctx)==nullptr;
    for (unsigned i=0;i<64;++i) pixels[i]=0;
    pixels[0]=0xFFFFFFFF; dirty(input); setSource(ctx,input,0,0);
    ok &= prepareRasterSource(ctx)==nullptr; // Sparse glyph/coverage texture.
    for (unsigned i=0;i<64;++i) pixels[i]=0xFFFFFFFF;
    dirty(input); setSource(ctx,input,0,0);
    setOperator(ctx,6); ok &= prepareRasterSource(ctx)==nullptr; setOperator(ctx,2);
    pushGroup(ctx); ok &= prepareRasterSource(ctx)==nullptr; patternDestroy(popGroup(ctx));
    // Saving/restoring the state keeps the original source-space anchor intact.
    setSource(ctx,input,0,0); translate(ctx,4,0); saved=prepareRasterSource(ctx);
    ok &= saved!=nullptr; restoreRasterSource(ctx,saved);
    translate(ctx,-4,0); paint(ctx); flush(output);
    ok &= ((uint32_t*)bytes(output))[0]==0xFFFFFFFF;

    // The second row of Designer's Parameters tabs uses a full translucent strip.
    void* tab=image(0,44,8);
    for (unsigned i=0;i<44*8;++i) ((uint32_t*)bytes(tab))[i]=0xC8B8B4A0;
    dirty(tab); setSource(ctx,tab,0,0); saved=prepareRasterSource(ctx); copy=nullptr;
    ok &= saved!=nullptr && !surface(getSource(ctx),&copy);
    if (copy) ok &= (((uint32_t*)bytes(copy))[0]>>24)==200 && (((uint32_t*)bytes(copy))[0]&0xFFFFFF)<0x606060;
    restoreRasterSource(ctx,saved);
    // Actual Designer controls are nine-slice skins. The blue rim is outside
    // the old neutral-color range and each edge/corner is drawn separately.
    void* slice=image(0,5,10); auto slicePixels=(uint32_t*)bytes(slice);
    for (int y=0;y<10;++y) for (int x=0;x<5;++x)
        slicePixels[y*5+x]=x==4?0xFF9EB6E9:0xFFCFE5FF;
    dirty(slice); setSource(ctx,slice,0,0); saved=prepareRasterSource(ctx); copy=nullptr;
    ok &= saved!=nullptr && !surface(getSource(ctx),&copy);
    if (copy) {
        auto colored=(uint32_t*)bytes(copy);
        ok &= colored[0]==0xFF343746 && colored[4]==0xFF191A21;
    }
    restoreRasterSource(ctx,saved);
    // A sparse rounded tab corner must use the vector border ink, keeping alpha.
    void* arc=image(0,7,7); auto arcPixels=(uint32_t*)bytes(arc);
    for (int i=0;i<49;++i) arcPixels[i]=0;
    arcPixels[0]=0xFFCCC6AD; arcPixels[1]=0xFFDDD7BD; arcPixels[8]=0x90747263;
    dirty(arc); setSource(ctx,arc,0,0); saved=prepareRasterSource(ctx); copy=nullptr;
    ok &= saved!=nullptr && !surface(getSource(ctx),&copy);
    if (copy) {
        auto colored=(uint32_t*)bytes(copy);
        ok &= colored[0]==0xFF191A21 && colored[1]==0xFF191A21 && colored[8]==0x900E0E12 && colored[48]==0;
    }
    restoreRasterSource(ctx,saved);
    // A colored icon with a coincidental native-border pixel is left alone.
    arcPixels[2]=0xFFFF0000; dirty(arc); setSource(ctx,arc,0,0);
    ok &= prepareRasterSource(ctx)==nullptr;
    void* corner=image(0,8,8);
    for (unsigned i=0;i<64;++i) ((uint32_t*)bytes(corner))[i]=0x8D817F70;
    dirty(corner); setSource(ctx,corner,0,0); saved=prepareRasterSource(ctx);
    ok &= saved!=nullptr; restoreRasterSource(ctx,saved);
    // Border pixels in rounded corners agree exactly with the vector border.
    for (unsigned i=0;i<64;++i) ((uint32_t*)bytes(corner))[i]=0xFFCCC6AD;
    dirty(corner); setSource(ctx,corner,0,0); saved=prepareRasterSource(ctx); copy=nullptr;
    ok &= saved!=nullptr && !surface(getSource(ctx),&copy);
    if (copy) ok &= ((uint32_t*)bytes(copy))[0]==0xFF191A21;
    restoreRasterSource(ctx,saved);
    // A cached blue hover halo can surround an already dark label/background.
    // Recolor only that halo; the white glyph coverage must remain white.
    void* hover=image(0,64,20);
    auto hoverPixels=(uint32_t*)bytes(hover);
    for (unsigned i=0;i<64*20;++i) hoverPixels[i]=i<64*3?0xFFC7DCF5:0xFF343746;
    hoverPixels[64*10+30]=0xFFF8F8F2;
    dirty(hover); setSource(ctx,hover,0,0); saved=prepareRasterSource(ctx); copy=nullptr;
    ok &= saved!=nullptr && !surface(getSource(ctx),&copy);
    if (copy) {
        auto colored=(uint32_t*)bytes(copy);
        ok &= colored[0]==0xFF343746 && colored[64*10+30]==0xFFF8F8F2 && colored[64*8]==0xFF343746;
    }
    restoreRasterSource(ctx,saved);
    // Dense square coverage and a strip containing holes are still masks.
    // Hollow native hover rims get one outer contour, with no opaque interior.
    void* ring=image(0,64,24); auto ringPixels=(uint32_t*)bytes(ring);
    for (int y=0;y<24;++y) for (int x=0;x<64;++x)
        ringPixels[y*64+x]=(x<3 || x>=61 || y<3 || y>=21)?0xFFC7DCF5:0;
    dirty(ring); setSource(ctx,ring,0,0); saved=prepareRasterSource(ctx); copy=nullptr;
    ok &= saved!=nullptr && !surface(getSource(ctx),&copy);
    if (copy) {
        auto colored=(uint32_t*)bytes(copy);
        ok &= colored[0]==0xFF191A21 && colored[64+1]==0 && colored[64*12+32]==0;
        ok &= ringPixels[64+1]==0xFFC7DCF5 && rasterHoverFrames()>0;
    }
    restoreRasterSource(ctx,saved);
    // A blue glyph-like mark in the interior disqualifies the ring classifier.
    ringPixels[64*12+32]=0xFFC7DCF5; dirty(ring); setSource(ctx,ring,0,0);
    ok &= prepareRasterSource(ctx)==nullptr;
    // Already-themed opaque skins retain the entire label inside the thin rim.
    for (int y=0;y<24;++y) for (int x=0;x<64;++x)
        ringPixels[y*64+x]=(x<3 || x>=61 || y<3 || y>=21)?0xFFF8F8F2:0xFF343746;
    ringPixels[64*12+30]=0xFFF8F8F2;
    dirty(ring); setSource(ctx,ring,0,0); saved=prepareRasterSource(ctx); copy=nullptr;
    ok &= saved!=nullptr && !surface(getSource(ctx),&copy);
    if (copy) {
        auto colored=(uint32_t*)bytes(copy);
        ok &= colored[0]==0xFF191A21 && colored[64+1]==0xFF343746 && colored[64*12+30]==0xFFF8F8F2;
    }
    restoreRasterSource(ctx,saved);
    for (unsigned i=0;i<64;++i) pixels[i]=0x80808080;
    dirty(input); setSource(ctx,input,0,0); ok &= prepareRasterSource(ctx)==nullptr;
    for (unsigned i=0;i<44*8;++i) ((uint32_t*)bytes(tab))[i]=0x80808080;
    dirty(tab); setSource(ctx,tab,0,0); ok &= prepareRasterSource(ctx)==nullptr;
    for (unsigned i=0;i<44*8;++i) ((uint32_t*)bytes(tab))[i]=(i%2)?0x80808080:0;
    dirty(tab); setSource(ctx,tab,0,0); ok &= prepareRasterSource(ctx)==nullptr;
    // Colorful icons, large images, and A8 masks are deliberately unchanged.
    for (unsigned i=0;i<64;++i) pixels[i]=0xFFFF0000;
    dirty(input); setSource(ctx,input,0,0); ok &= prepareRasterSource(ctx)==nullptr;
    void* large=image(0,200,200); setSource(ctx,large,0,0); ok &= prepareRasterSource(ctx)==nullptr;
    void* mask=image(2,8,8); setSource(ctx,mask,0,0); ok &= prepareRasterSource(ctx)==nullptr;
    void* strip=image(0,12,1024);
    for (unsigned i=0;i<12*1024;++i) ((uint32_t*)bytes(strip))[i]=0xFFF5F2DD;
    dirty(strip); setSource(ctx,strip,0,0); saved=prepareRasterSource(ctx);
    ok &= saved!=nullptr; restoreRasterSource(ctx,saved);

    // A different palette changes the actual output without rebuilding the renderer.
    ok &= initializeRasterTheme(cairo,0x102030,0xABCDEF,0x010203);
    setSource(ctx,slice,0,0); saved=prepareRasterSource(ctx); copy=nullptr;
    ok &= saved!=nullptr && !surface(getSource(ctx),&copy);
    if (copy) ok &= ((uint32_t*)bytes(copy))[0]==0xFF102030 && ((uint32_t*)bytes(copy))[4]==0xFF010203;
    restoreRasterSource(ctx,saved);
    for (unsigned i=0;i<64;++i) pixels[i]=0xFFFFFFFF;
    pixels[1]=0xFF000000; pixels[2]=0x80808080; dirty(input);
    setSource(ctx,input,0,0); saved=prepareRasterSource(ctx); copy=nullptr;
    ok &= saved!=nullptr && !surface(getSource(ctx),&copy);
    transformed=(uint32_t*)bytes(copy);
    ok &= transformed[0]==0xFF102030 && transformed[1]==0xFFABCDEF && transformed[2]==0x80081018;
    restoreRasterSource(ctx,saved);
    destroyContext(ctx); destroySurface(input); destroySurface(output); destroySurface(large); destroySurface(mask); destroySurface(strip); destroySurface(tab); destroySurface(corner); destroySurface(hover); destroySurface(ring); destroySurface(slice); destroySurface(arc);
    printf("Real Cairo raster tests: %s; alpha, source state, hover idempotence, compositing/groups, exclusions, vertical strip\n",ok?"PASS":"FAIL");
    FreeLibrary(cairo); return ok?0:1;
}
