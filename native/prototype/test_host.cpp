#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

template<class T> T load(HMODULE m,const char* name) {
    auto address=GetProcAddress(m,name); T fn=nullptr; memcpy(&fn,&address,sizeof(fn));
    if (!fn) { printf("Missing %s\n",name); ExitProcess(3); } return fn;
}
struct Matrix { double xx,yx,xy,yy,x0,y0; };
int wmain(int argc,wchar_t** argv) {
    if (argc!=4) return 2;
    HMODULE cairo=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    HMODULE graphics=LoadLibraryW(L"grphcs.dll"),icons=LoadLibraryW(argv[2]);
    if (!cairo || !graphics || !icons) { printf("Load error %lu\n",GetLastError()); return 3; }
    auto start=load<DWORD(WINAPI*)(void*)>(icons,"ToolbarIconsStart");
    auto stop=load<DWORD(WINAPI*)(void*)>(icons,"ToolbarIconsStop");
    auto setSource=load<void(*)(void*,void*)>(graphics,"prototype_set_source");
    auto mask=load<void(*)(void*,void*)>(graphics,"prototype_mask");
    auto maskSurface=load<void(*)(void*,void*,double,double)>(graphics,"prototype_mask_surface");
    auto read=load<void*(*)(const char*)>(cairo,"cairo_image_surface_create_from_png");
    auto create=load<void*(*)(int,int,int)>(cairo,"cairo_image_surface_create");
    auto bytes=load<unsigned char*(*)(void*)>(cairo,"cairo_image_surface_get_data");
    auto stride=load<int(*)(void*)>(cairo,"cairo_image_surface_get_stride");
    auto setRgba=load<void(*)(void*,double,double,double,double)>(cairo,"cairo_set_source_rgba");
    auto flush=load<void(*)(void*)>(cairo,"cairo_surface_flush");
    auto dirty=load<void(*)(void*)>(cairo,"cairo_surface_mark_dirty");
    auto pattern=load<void*(*)(void*)>(cairo,"cairo_pattern_create_for_surface");
    auto context=load<void*(*)(void*)>(cairo,"cairo_create");
    auto getSource=load<void*(*)(void*)>(cairo,"cairo_get_source");
    auto getSurface=load<int(*)(void*,void**)>(cairo,"cairo_pattern_get_surface");
    auto getMatrix=load<void(*)(void*,Matrix*)>(cairo,"cairo_pattern_get_matrix");
    auto setMatrix=load<void(*)(void*,const Matrix*)>(cairo,"cairo_pattern_set_matrix");
    auto setExtend=load<void(*)(void*,int)>(cairo,"cairo_pattern_set_extend");
    auto getExtend=load<int(*)(void*)>(cairo,"cairo_pattern_get_extend");
    auto setFilter=load<void(*)(void*,int)>(cairo,"cairo_pattern_set_filter");
    auto getFilter=load<int(*)(void*)>(cairo,"cairo_pattern_get_filter");
    auto paintAlpha=load<void(*)(void*,double)>(cairo,"cairo_paint_with_alpha");
    auto destroySurface=load<void(*)(void*)>(cairo,"cairo_surface_destroy");
    auto destroyPattern=load<void(*)(void*)>(cairo,"cairo_pattern_destroy");
    auto destroyContext=load<void(*)(void*)>(cairo,"cairo_destroy");
    std::wstring wide=argv[3]; std::string pack(wide.begin(),wide.end());
    std::wstring invalid=wide+L"/invalid";
    if (start(const_cast<wchar_t*>(invalid.c_str()))!=ERROR_INVALID_DATA) {
        printf("Invalid pack was not rejected\n"); return 4;
    }
    DWORD result=start(argv[3]);
    if (result) { printf("Start error %lu\n",result); return 4; }
    bool ok=start(argv[3])==ERROR_ALREADY_INITIALIZED;
    for (int size:{14,16,20,24,28,32,48,64}) {
        std::string sourcePath=pack+"/source-"+std::to_string(size)+".png";
        void* source=read(sourcePath.c_str()),*output=create(0,size*3,size),*ctx=context(output),*original=pattern(source);
        Matrix expected{1,0,0,1,.5,.25}; setMatrix(original,&expected); setExtend(original,2); setFilter(original,3);
        setSource(ctx,original); void* custom=getSource(ctx),*replacement=nullptr;
        ok &= custom!=original && !getSurface(custom,&replacement) && replacement!=source;
        if (!replacement) { printf("No replacement at size %d\n",size); return 5; }
        Matrix actual; getMatrix(custom,&actual); ok &= !memcmp(&actual,&expected,sizeof(actual)) && getExtend(custom)==2 && getFilter(custom)==3;
        // Replace just the middle cell of a three-cell image: untouched pixels survive.
        auto pixels=bytes(replacement);
        ok &= reinterpret_cast<uint32_t*>(pixels)[0]==0xFFCC2244u;
        ok &= reinterpret_cast<uint32_t*>(pixels)[size+size/2]==0xFF44CC88u;
        ok &= reinterpret_cast<uint32_t*>(pixels)[size*2+size/2]==0xFFCC2244u;
        ok &= reinterpret_cast<uint32_t*>(bytes(source))[size+size/2]==0xFFCC2244u;
        // Native disabled opacity remains in Cairo's paint operation.
        paintAlpha(ctx,.4); flush(output);
        unsigned alpha=reinterpret_cast<uint32_t*>(bytes(output))[size+size/2]>>24;
        ok &= alpha>=101 && alpha<=103;
        // Exercise both mask imports with A8 sources: preserve other cells and
        // native source ink, while replacing the middle cell's alpha geometry.
        void* a8=create(2,size*3,size); int a8Pitch=stride(a8);
        memset(bytes(a8),255,static_cast<size_t>(a8Pitch)*size);
        bytes(a8)[(size-1)*a8Pitch+size*3-1]=0; dirty(a8);
        for (int method=0;method<2;++method) {
            void* maskedOutput=create(0,size*3,size),*maskedContext=context(maskedOutput);
            setRgba(maskedContext,.2,.4,.8,1.);
            if (method==0) maskSurface(maskedContext,a8,0,0);
            else { void* a8Pattern=pattern(a8); mask(maskedContext,a8Pattern); destroyPattern(a8Pattern); }
            flush(maskedOutput); auto row=reinterpret_cast<uint32_t*>(bytes(maskedOutput)+(size-1)*stride(maskedOutput));
            ok &= row[size-1]==0xFF3366CCu && row[size*2-1]==0 && row[size*3-2]==0xFF3366CCu;
            ok &= bytes(a8)[(size-1)*a8Pitch+size*2-1]==255;
            destroyContext(maskedContext); destroySurface(maskedOutput);
        }
        destroySurface(a8);
        // Unknown source content must pass through, even after a positive cache hit.
        reinterpret_cast<uint32_t*>(bytes(source))[0]^=1; dirty(source);
        setSource(ctx,original); ok &= getSource(ctx)==original;
        destroyPattern(original); destroyContext(ctx); destroySurface(source); destroySurface(output);
    }
    ok &= stop(nullptr)==0 && stop(nullptr)==0;
    void* source=create(0,16,16),*ctx=context(source),*original=pattern(source);
    setSource(ctx,original); ok &= getSource(ctx)==original;
    destroyPattern(original); destroyContext(ctx); destroySurface(source);
    printf("Toolbar prototype: exact source, eight sizes, A8 masks, native opacity, pattern state, unknown source, stop: %s\n",ok?"PASS":"FAIL");
    return ok?0:1;
}
