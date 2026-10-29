#include "editor_selection.h"
#include "contrast.h"
#include <math.h>
#include <cmath>
#include <stdint.h>
#include <string.h>

namespace {
    struct Extents { double xBearing,yBearing,width,height,xAdvance,yAdvance; };
    union PathData { struct { int type,length; } header; struct { double x,y; } point; };
    struct Path { int status; PathData* data; int count; };
    using Unary=void*(*)(void*);
    Unary source,target,groupTarget,scaledFont,fontFace;
    int(*surfaceType)(void*); int(*operation)(void*); int(*fontStatus)(void*); int(*faceType)(void*);
    void(*textExtents)(void*,const char*,Extents*);
    Path*(*copyPath)(void*); void(*destroyPath)(Path*);
    void(*save)(void*); void(*restore)(void*); void(*rgba)(void*,double,double,double,double);
    int(*getRgba)(void*,double*,double*,double*,double*);
    int(*setData)(void*,const void*,void*,void(*)(void*));
    void*(*getData)(void*,const void*);
    unsigned(*referenceCount)(void*); int(*patternStatus)(void*);
    void*(*createRgba)(double,double,double,double); void(*setSource)(void*,void*); void(*destroyPattern)(void*);
    static int originalColorKey;
    bool supported;
    bool comparisonSupported;
    Unary win32Image;
    int(*imageWidth)(void*),(*imageHeight)(void*),(*imageStride)(void*),(*imageFormat)(void*);
    unsigned char*(*imageData)(void*);
    void(*flushSurface)(void*),(*userToDevice)(void*,double*,double*);
    void(*deviceOffset)(void*,double*,double*),(*deviceScale)(void*,double*,double*);
    volatile LONG64 fills,glyphs;
    template<class T> bool load(HMODULE m,const char* name,T &fn) {
        auto proc=GetProcAddress(m,name); static_assert(sizeof(proc)==sizeof(fn));
        memcpy(&fn,&proc,sizeof(fn)); return fn!=nullptr;
    }
    bool screen(void* context) {
        int op=operation(context),kind=surfaceType(target(context));
        return (op==1 || op==2) && (kind==0 || kind==7) && groupTarget(context)==target(context);
    }
    bool monospace(void* context) {
        // WebKit USER fonts own shaping callbacks. Asking them to measure probe
        // strings from a rendering hook can re-enter browser code without its
        // shaping state. The editor uses native TOY/FreeType/Win32 faces.
        void* face=fontFace(context);
        if (!face) return false;
        int kind=faceType(face);
        if (kind!=0 && kind!=1 && kind!=2) return false; // TOY, FT, WIN32; never USER.
        void* font=scaledFont(context);
        if (!font || fontStatus(font)) return false;
        Extents thin={},wide={}; textExtents(font,"i",&thin); textExtents(font,"W",&wide);
        return thin.xAdvance>0. && fabs(thin.xAdvance-wide.xAdvance)<.01;
    }
    DWORD original(void* context) {
        auto value=(uintptr_t)getData(source(context),&originalColorKey);
        return value?(DWORD)((value-1)&0xFFFFFF):0xFFFFFFFF;
    }
    bool rectangle(void* context) {
        Path* path=copyPath(context); if (!path) return false;
        bool valid=path->status==0 && path->data && (path->count==9 || path->count==11);
        if (valid) {
            auto d=path->data;
            valid=d[0].header.type==0 && d[0].header.length==2 &&
                d[2].header.type==1 && d[2].header.length==2 &&
                d[4].header.type==1 && d[4].header.length==2 &&
                d[6].header.type==1 && d[6].header.length==2 &&
                d[8].header.type==3 && d[8].header.length==1;
            if (valid) {
                double x0=d[1].point.x,y0=d[1].point.y,x1=d[3].point.x,y1=d[3].point.y;
                double x2=d[5].point.x,y2=d[5].point.y,x3=d[7].point.x,y3=d[7].point.y;
                valid=fabs(y0-y1)<.001 && fabs(x1-x2)<.001 && fabs(y2-y3)<.001 && fabs(x3-x0)<.001 &&
                    fabs(x1-x0)>1.5 && fabs(y2-y1)>=4.;
            }
        }
        destroyPath(path); return valid;
    }
}

bool initializeEditorSelection(HMODULE m) {
    supported=true;
#define LOAD(fn,name) supported &= load(m,name,fn)
    LOAD(source,"cairo_get_source"); LOAD(target,"cairo_get_target"); LOAD(groupTarget,"cairo_get_group_target");
    LOAD(surfaceType,"cairo_surface_get_type"); LOAD(operation,"cairo_get_operator");
    LOAD(scaledFont,"cairo_get_scaled_font"); LOAD(fontStatus,"cairo_scaled_font_status");
    LOAD(fontFace,"cairo_get_font_face"); LOAD(faceType,"cairo_font_face_get_type");
    LOAD(textExtents,"cairo_scaled_font_text_extents"); LOAD(copyPath,"cairo_copy_path"); LOAD(destroyPath,"cairo_path_destroy");
    LOAD(save,"cairo_save"); LOAD(restore,"cairo_restore"); LOAD(rgba,"cairo_set_source_rgba");
    LOAD(getRgba,"cairo_pattern_get_rgba"); LOAD(setData,"cairo_pattern_set_user_data"); LOAD(getData,"cairo_pattern_get_user_data");
    LOAD(referenceCount,"cairo_pattern_get_reference_count"); LOAD(patternStatus,"cairo_pattern_status");
    LOAD(createRgba,"cairo_pattern_create_rgba"); LOAD(setSource,"cairo_set_source"); LOAD(destroyPattern,"cairo_pattern_destroy");
#undef LOAD
    comparisonSupported=supported;
#define LOAD_COMPARISON(fn,name) comparisonSupported &= load(m,name,fn)
    LOAD_COMPARISON(win32Image,"cairo_win32_surface_get_image");
    LOAD_COMPARISON(imageWidth,"cairo_image_surface_get_width");
    LOAD_COMPARISON(imageHeight,"cairo_image_surface_get_height");
    LOAD_COMPARISON(imageStride,"cairo_image_surface_get_stride");
    LOAD_COMPARISON(imageFormat,"cairo_image_surface_get_format");
    LOAD_COMPARISON(imageData,"cairo_image_surface_get_data");
    LOAD_COMPARISON(flushSurface,"cairo_surface_flush");
    LOAD_COMPARISON(userToDevice,"cairo_user_to_device");
    LOAD_COMPARISON(deviceOffset,"cairo_surface_get_device_offset");
    LOAD_COMPARISON(deviceScale,"cairo_surface_get_device_scale");
#undef LOAD_COMPARISON
    return supported;
}
bool editorSelectionSupported() { return supported; }
bool beginUiHighlightFill(void* context,const RenderColors& colors) {
    if (!supported || original(context)!=0xFFFFFF || !screen(context)) return false;
    Path* path=copyPath(context); if (!path) return false;
    double minX=1e20,minY=1e20,maxX=-1e20,maxY=-1e20;
    int points=0; bool linear=path->status==0 && path->data;
    for(int i=0;linear && i<path->count;) {
        auto d=path->data+i; int type=d->header.type,len=d->header.length;
        if (len<1 || i+len>path->count || type==2) { linear=false; break; }
        if ((type==0 || type==1) && len==2) {
            minX=fmin(minX,d[1].point.x); maxX=fmax(maxX,d[1].point.x);
            minY=fmin(minY,d[1].point.y); maxY=fmax(maxY,d[1].point.y); ++points;
        } else if (type!=3) linear=false;
        i+=len;
    }
    double w=maxX-minX,h=maxY-minY;
    // Exclude ordinary editor rectangles, large compound canvases, curves and icons.
    bool band=linear && points>=4 && fmax(w,h)>=64. &&
        (fmin(w,h)<=8. || (points>=8 && fmin(w,h)<=256.));
    destroyPath(path); if (!band) return false;
    double r,g,b,a; if (getRgba(source(context),&r,&g,&b,&a)) return false;
    save(context); DWORD color=colors.rasterBackground;
    rgba(context,((color>>16)&255)/255.,((color>>8)&255)/255.,(color&255)/255.,a);
    return true;
}
void rememberEditorSource(void* context,DWORD rgb,bool alphaSource) {
    if (!supported) return;
    // Metadata follows the pattern through cairo_save/restore and cached sources.
    // Encode the RGB without an allocation; nullptr is reserved for unknown origin.
    void* pattern=source(context);
    void* encoded=(void*)(uintptr_t)(((alphaSource?1u:0u)<<25)|(rgb+1));
    if (getData(pattern,&originalColorKey)==encoded) return;
    // Cairo can reuse an unchanged solid source across save/restore. Do not
    // mutate provenance on a shared pattern belonging to a saved graphics state.
    if (referenceCount(pattern)>1) {
        double r,g,b,a; if (getRgba(pattern,&r,&g,&b,&a)) return;
        void* copy=createRgba(r,g,b,a);
        if (!copy || patternStatus(copy)) { if (copy) destroyPattern(copy); return; }
        setSource(context,copy); destroyPattern(copy); pattern=source(context);
    }
    setData(pattern,&originalColorKey,encoded,nullptr);
}
bool beginEditorSelectionFill(void* context,const RenderColors& colors) {
    if (!supported || original(context)!=0 || !screen(context) || !rectangle(context)) return false;
    // The very first selected line is filled before the editor sets its font.
    // Its RGBA brush distinguishes it from ordinary RGB UI rectangle brushes.
    auto origin=(uintptr_t)getData(source(context),&originalColorKey);
    if (!(origin&(1u<<25))) return false;
    double r,g,b,a;
    if (getRgba(source(context),&r,&g,&b,&a)) return false;
    save(context);
    DWORD color=colors.selectionBackground;
    rgba(context,((color>>16)&255)/255.,((color>>8)&255)/255.,(color&255)/255.,a);
    InterlockedIncrement64(&fills); return true;
}
void endEditorSelectionFill(void* context,bool changed) { if (changed) restore(context); }
bool editorSelectionGlyph(void* context) {
    if (!supported) return false;
    DWORD rgb=original(context);
    bool inverted=rgb==0xFFFFFF || rgb==0x00FFFF || rgb==0xFFFF00 || rgb==0xFF7FFF || rgb==0x69CDFF;
    if (!inverted || !screen(context) || !monospace(context)) return false;
    InterlockedIncrement64(&glyphs); return true;
}
bool drawComparisonGlyphs(void* context,const void* glyphs,int count,DWORD background,DWORD fallback,
                          void(__cdecl* draw)(void*,const void*,int)) {
    DWORD lastError=GetLastError();
    if (!comparisonSupported || !glyphs || count<=0 || background>0xFFFFFF) return false;
    double r,g,b,a;
    if (getRgba(source(context),&r,&g,&b,&a) || a!=1.) return false;
    auto channel=[](double v)->DWORD { return (DWORD)(fmax(0.,fmin(1.,v))*255.+.5); };
    DWORD current=(channel(r)<<16)|(channel(g)<<8)|channel(b);
    if (current==fallback) return false;
    if (ChromaContrast::ratio(current,background)>=4.5 || !screen(context) || !monospace(context)) return false;
    void* destination=target(context);
    void* image=surfaceType(destination)==7?win32Image(destination):destination;
    if (!image || surfaceType(image)!=0) return false;
    int format=imageFormat(image),width=imageWidth(image),height=imageHeight(image),stride=imageStride(image);
    if ((format!=0 && format!=1) || width<=0 || height<=0 || stride<=0 ||
        static_cast<size_t>(stride)<static_cast<size_t>(width)*4) return false;
    // Check the painted fill per glyph; a batch can cross changed/unchanged
    // blocks. Never sample a printer/PDF/group or measure a WebKit USER font.
    flushSurface(destination);
    auto bytes=imageData(image); if (!bytes) return false;
    double offsetX,offsetY,scaleX,scaleY;
    deviceOffset(destination,&offsetX,&offsetY); deviceScale(destination,&scaleX,&scaleY);
    struct Glyph { unsigned long index; double x,y; };
    const auto values=static_cast<const Glyph*>(glyphs);
    auto changed=[&](int index) {
        double x=values[index].x,y=values[index].y+1.;
        userToDevice(context,&x,&y);
        x=x*scaleX+offsetX; y=y*scaleY+offsetY;
        if (!std::isfinite(x) || !std::isfinite(y) || x<0 || y<0 || x>=width || y>=height) return false;
        uint32_t pixel; memcpy(&pixel,bytes+static_cast<size_t>(y)*stride+static_cast<size_t>(x)*4,sizeof(pixel));
        return (pixel&0xFFFFFF)==background && (format==1 || (pixel>>24)==255);
    };
    int first=0;
    while (first<count && !changed(first)) ++first;
    if (first==count) return false;
    DWORD ink=ChromaContrast::readableInk(current,background,fallback);
    if (ink==current) return false;
    // Draw adjacent glyphs with the same background together, preserving their
    // absolute positions and the source pattern/last-error state of the caller.
    if (first) { SetLastError(lastError); draw(context,values,first); lastError=GetLastError(); }
    save(context);
    for (int begin=first;begin<count;) {
        bool corrected=changed(begin); int end=begin+1;
        while (end<count && changed(end)==corrected) ++end;
        if (corrected) rgba(context,((ink>>16)&255)/255.,((ink>>8)&255)/255.,(ink&255)/255.,a);
        else rgba(context,r,g,b,a);
        SetLastError(lastError); draw(context,values+begin,end-begin); lastError=GetLastError();
        begin=end;
    }
    restore(context); SetLastError(lastError);
    return true;
}
unsigned long long editorSelectionFills() { return InterlockedCompareExchange64(&fills,0,0); }
unsigned long long editorSelectionGlyphs() { return InterlockedCompareExchange64(&glyphs,0,0); }
