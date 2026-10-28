#include "raster_theme.h"
#include <stdint.h>
#include <string.h>
#include <algorithm>

namespace {
    struct Matrix { double xx,yx,xy,yy,x0,y0; };
    using Unary=void*(*)(void*);
    using Number=int(*)(void*);
    using Action=void(*)(void*);
    Unary source,target,groupTarget,reference,createPattern;
    Number surfaceType,format,width,height,stride,status,patternStatus,extend,filter,getOperator;
    Action flush,dirty,destroySurface,destroyPattern,saveContext,restoreContext;
    int(*patternSurface)(void*,void**);
    unsigned char*(*data)(void*);
    void*(*createSurface)(int,int,int);
    void(*setSource)(void*,void*);
    void(*getMatrix)(void*,Matrix*); void(*setMatrix)(void*,const Matrix*);
    void(*setExtend)(void*,int); void(*setFilter)(void*,int);
    void(*getOffset)(void*,double*,double*); void(*setOffset)(void*,double,double);
    void(*getScale)(void*,double*,double*); void(*setScale)(void*,double,double);
    bool supported;
    DWORD backgroundColor,foregroundColor,borderColor;
    volatile LONG64 changes,hoverFrames;
    template<class T> bool load(HMODULE m,const char* name,T &fn) {
        auto proc=GetProcAddress(m,name); static_assert(sizeof(fn)==sizeof(proc));
        memcpy(&fn,&proc,sizeof(fn)); return fn!=nullptr;
    }
    bool skin(unsigned r,unsigned g,unsigned b) {
        auto lo=std::min({r,g,b}),hi=std::max({r,g,b});
        return hi-lo<=20 || (r>=140 && r>=b && g>=b && r-b<=75 && (r>g?r-g:g-r)<=32)
            || (r>=100 && b>=r && g+4>=r && b-r<=75 && b>=g && b-g<=45);
    }
    int tabTone(unsigned r,unsigned g,unsigned b,unsigned alpha) {
        int position=std::clamp(((int)r+(int)g+(int)b-575)*255/94,0,255);
        int tolerance=2+255/(int)alpha;
        if (abs((int)r-(204+31*position/255))>tolerance ||
            abs((int)g-(198+32*position/255))>tolerance ||
            abs((int)b-(173+31*position/255))>tolerance) return -1;
        return position;
    }
    bool blueSkin(unsigned r,unsigned g,unsigned b) {
        return r>=100 && b>=r+8 && g>=r && b-r<=100 && b>=g && b-g<=65;
    }
    bool darkPalette() {
        return (((backgroundColor>>16)&255)*54+((backgroundColor>>8)&255)*183+(backgroundColor&255)*19)/256<160;
    }
    uint32_t transform(uint32_t pixel,bool alpha,bool chromeOnly) {
        unsigned a=alpha?pixel>>24:255;
        if (!a) return pixel;
        unsigned r=std::min(255u,(((pixel>>16)&255)*255+a/2)/a);
        unsigned g=std::min(255u,(((pixel>>8)&255)*255+a/2)/a);
        unsigned b=std::min(255u,((pixel&255)*255+a/2)/a);
        int tone=tabTone(r,g,b,a);
        bool blueHalo=blueSkin(r,g,b) && darkPalette();
        if (chromeOnly && !blueHalo) return pixel;
        if (!skin(r,g,b)) return pixel;
        unsigned intensity=(r*54+g*183+b*19)/256;
        auto blend=[&](unsigned shift)->unsigned {
            unsigned foreground=(foregroundColor>>shift)&255,background=(backgroundColor>>shift)&255;
            // Cached corner pixels must use the same border/panel colors as vector edges.
            if (tone>=0) return (((borderColor>>shift)&255)*(255-tone)+background*tone+127)/255;
            // Native blue hover glow is a background halo, not foreground ink.
            if (blueHalo) return background;
            return (foreground*(255-intensity)+background*intensity+127)/255;
        };
        unsigned red=blend(16),green=blend(8),blue=blend(0);
        // ARGB32 stays premultiplied and its exact alpha byte is retained.
        return (pixel&0xFF000000u)|((red*a/255)<<16)|((green*a/255)<<8)|(blue*a/255);
    }
}

void setRasterThemeColors(DWORD background,DWORD foreground,DWORD border) { backgroundColor=background; foregroundColor=foreground; borderColor=border; }

bool initializeRasterTheme(HMODULE m,DWORD background,DWORD foreground,DWORD border) {
    setRasterThemeColors(background,foreground,border);
    supported=true;
#define LOAD(fn,name) supported &= load(m,name,fn)
    LOAD(source,"cairo_get_source"); LOAD(target,"cairo_get_target");
    LOAD(groupTarget,"cairo_get_group_target"); LOAD(getOperator,"cairo_get_operator");
    LOAD(saveContext,"cairo_save"); LOAD(restoreContext,"cairo_restore");
    LOAD(reference,"cairo_pattern_reference"); LOAD(createPattern,"cairo_pattern_create_for_surface");
    LOAD(surfaceType,"cairo_surface_get_type"); LOAD(format,"cairo_image_surface_get_format");
    LOAD(width,"cairo_image_surface_get_width"); LOAD(height,"cairo_image_surface_get_height");
    LOAD(stride,"cairo_image_surface_get_stride"); LOAD(status,"cairo_surface_status");
    LOAD(patternStatus,"cairo_pattern_status");
    LOAD(extend,"cairo_pattern_get_extend"); LOAD(filter,"cairo_pattern_get_filter");
    LOAD(flush,"cairo_surface_flush"); LOAD(dirty,"cairo_surface_mark_dirty");
    LOAD(destroySurface,"cairo_surface_destroy"); LOAD(destroyPattern,"cairo_pattern_destroy");
    LOAD(patternSurface,"cairo_pattern_get_surface"); LOAD(data,"cairo_image_surface_get_data");
    LOAD(createSurface,"cairo_image_surface_create"); LOAD(setSource,"cairo_set_source");
    LOAD(getMatrix,"cairo_pattern_get_matrix"); LOAD(setMatrix,"cairo_pattern_set_matrix");
    LOAD(setExtend,"cairo_pattern_set_extend"); LOAD(setFilter,"cairo_pattern_set_filter");
    LOAD(getOffset,"cairo_surface_get_device_offset"); LOAD(setOffset,"cairo_surface_set_device_offset");
    LOAD(getScale,"cairo_surface_get_device_scale"); LOAD(setScale,"cairo_surface_set_device_scale");
#undef LOAD
    return supported;
}
bool rasterSupported() { return supported; }
unsigned long long rasterChanges() { return InterlockedCompareExchange64(&changes,0,0); }
unsigned long long rasterHoverFrames() { return InterlockedCompareExchange64(&hoverFrames,0,0); }
void* prepareRasterSource(void* context) {
    if (!supported) return nullptr;
    int operation=getOperator(context);
    if ((operation!=1 && operation!=2) || groupTarget(context)!=target(context)) return nullptr;
    int destination=surfaceType(target(context));
    if (destination!=0 && destination!=7) return nullptr; // Image or Win32 display; never a printer/PDF/SVG.
    void* pattern=source(context), *surface=nullptr;
    if (patternSurface(pattern,&surface) || surfaceType(surface)!=0) return nullptr;
    int kind=format(surface),w=width(surface),h=height(surface),pitch=stride(surface);
    if ((kind!=0 && kind!=1) || w<=0 || h<=0 || w>4096 || h>4096 || w*h>131072 ||
        (std::min(w,h)>32 && std::max(w,h)>64) || pitch<w*4) return nullptr;
    flush(surface); auto input=data(surface);
    if (!input) return nullptr;
    unsigned visible=0,neutral=0,light=0,opaque=0,tabSkin=0,blue=0,blueBorder=0,tabBorder=0,chrome=0;
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        uint32_t p=((uint32_t*)(input+y*pitch))[x]; unsigned a=kind==0?p>>24:255;
        if (!a) continue;
        ++visible;
        if (a>=250) ++opaque;
        unsigned r=std::min(255u,(((p>>16)&255)*255+a/2)/a);
        unsigned g=std::min(255u,(((p>>8)&255)*255+a/2)/a);
        unsigned b=std::min(255u,((p&255)*255+a/2)/a);
        if (skin(r,g,b)) ++neutral;
        if ((r*54+g*183+b*19)/256>=160) ++light;
        // The platform's cached tab skin uses EBE6CC and CCC6AD, including
        // premultiplication rounding at the faint end of its alpha gradient.
        if (tabTone(r,g,b,a)>=0) ++tabSkin;
        if (blueSkin(r,g,b)) ++blue;
        if (skin(r,g,b) || blueSkin(r,g,b)) ++chrome;
        if (abs((int)r-158)<=2 && abs((int)g-182)<=2 && abs((int)b-233)<=2) ++blueBorder;
        if ((r==204 && g==198 && b==173) || (r==179 && g==172 && b==134)) ++tabBorder;
    }
    // Designer uses nine-slice skins, not complete control bitmaps. In particular
    // their 9EB6E9 border falls outside the generic neutral-image classifier.
    // Match the original palette AND slice geometry, never arbitrary small icons.
    bool blueSlice=kind==0 && blueBorder && blueBorder*3<=visible && chrome==visible &&
        visible*100>=(unsigned)(w*h)*60 &&
        ((w==5 && (h==5 || h==10)) || (h==5 && w>=8) || (w==7 && h>=12 && h<=32));
    bool tabCorner=kind==0 && tabBorder && chrome==visible && visible*100<=(unsigned)(w*h)*60 &&
        ((w==7 && h==7) || (w==10 && h==26));
    // Multi-row tabs also use dense translucent horizontal background strips.
    // Their rectangular coverage distinguishes them from sparse glyph masks.
    bool translucentStrip=w>=8 && h<=32 && (w<=8 || w>=h*4) &&
        visible*100>=(unsigned)(w*h)*98 && tabSkin*100>=visible*98;
    // Never re-invert an already themed hover layer or a glyph/coverage mask:
    // that can erase its overlaid label. Square dense masks remain excluded.
    bool chromeOnly=light*2<visible && darkPalette() && w>=32 && h<=32 && blue>0;
    // Aero hot frames are hollow blue-tinted ARGB rings. They were previously
    // excluded together with sparse glyph masks, leaving the native thick rim.
    // Require four straight sides and an empty interior; never transform glyphs.
    int left[64],right[64],top[1024],bottom[1024];
    bool ring=false,ringOpaque=false,ringAlreadyThemed=false;
    uint32_t ringBackground=0;
    if (kind==0 && w>=16 && w<=1024 && h>=12 && h<=64 && visible) {
        // A cached control may already contain themed pixels. Sample its inner
        // background rather than requiring the original blue rim to survive.
        uint32_t samples[9]; int at=0;
        for (int y:{4,h/2,h-5}) for (int x:{4,w/2,w-5})
            samples[at++]=((uint32_t*)(input+y*pitch))[x];
        int best=0;
        for (int i=0;i<9;++i) {
            int votes=0; for (int j=0;j<9;++j) if (samples[i]==samples[j]) ++votes;
            if (votes>best) { best=votes; ringBackground=samples[i]; }
        }
        unsigned bgAlpha=ringBackground>>24;
        ringOpaque=bgAlpha>=250;
        ringAlreadyThemed=ringOpaque && (ringBackground&0xFFFFFF)==backgroundColor;
        auto rimPixel=[&](uint32_t pixel) {
            if ((pixel>>24)<64) return false;
            if (!ringOpaque) return bgAlpha==0;
            int distance=abs((int)((pixel>>16)&255)-(int)((ringBackground>>16)&255))+
                abs((int)((pixel>>8)&255)-(int)((ringBackground>>8)&255))+
                abs((int)(pixel&255)-(int)(ringBackground&255));
            return distance>=48;
        };
        std::fill(left,left+h,w); std::fill(right,right+h,-1);
        std::fill(top,top+w,h); std::fill(bottom,bottom+w,-1);
        unsigned inner=0,rim=0,rimSkin=0; int margin=std::min(4,std::min(w,h)/3);
        int north=0,south=0,west=0,east=0;
        for (int y=0;y<h;++y) {
            int covered=0;
            for (int x=0;x<w;++x) if (rimPixel(((uint32_t*)(input+y*pitch))[x])) {
                uint32_t p=((uint32_t*)(input+y*pitch))[x]; unsigned a=p>>24;
                unsigned r=std::min(255u,(((p>>16)&255)*255+a/2)/a);
                unsigned g=std::min(255u,(((p>>8)&255)*255+a/2)/a);
                unsigned b=std::min(255u,((p&255)*255+a/2)/a);
                ++rim; if (skin(r,g,b) || ((r<<16)|(g<<8)|b)==foregroundColor) ++rimSkin;
                left[y]=std::min(left[y],x); right[y]=std::max(right[y],x);
                top[x]=std::min(top[x],y); bottom[x]=std::max(bottom[x],y); ++covered;
                if (x>=margin && x<w-margin && y>=margin && y<h-margin) ++inner;
            }
            if (y<margin) north=std::max(north,covered);
            if (y>=h-margin) south=std::max(south,covered);
        }
        for (int x=0;x<w;++x) {
            int covered=0;
            for (int y=0;y<h;++y) if (rimPixel(((uint32_t*)(input+y*pitch))[x])) ++covered;
            if (x<margin) west=std::max(west,covered);
            if (x>=w-margin) east=std::max(east,covered);
        }
        unsigned innerArea=(w-2*margin)*(h-2*margin);
        ring=(bgAlpha==0 || ringOpaque) && rim && rimSkin*100>=rim*85 &&
            inner*100<=innerArea*(ringOpaque?25:0) &&
            north*10>=w*6 && south*10>=w*6 && west*10>=h*6 && east*10>=h*6;
    }
    if (!ring && !blueSlice && !tabCorner && (!visible || neutral*100<visible*95 || (light*2<visible && !chromeOnly) ||
        (opaque*100<(unsigned)(w*h)*60 && !translucentStrip))) return nullptr;
    void* copy=createSurface(kind,w,h);
    if (!copy || status(copy)) { if (copy) destroySurface(copy); return nullptr; }
    auto output=data(copy); int outPitch=stride(copy);
    if (!output || outPitch<w*4) { destroySurface(copy); return nullptr; }
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        uint32_t pixel=((uint32_t*)(input+y*pitch))[x];
        if (blueSlice && (pixel>>24)) {
            unsigned a=pixel>>24;
            unsigned r=std::min(255u,(((pixel>>16)&255)*255+a/2)/a);
            unsigned g=std::min(255u,(((pixel>>8)&255)*255+a/2)/a);
            unsigned b=std::min(255u,((pixel&255)*255+a/2)/a);
            DWORD color=(abs((int)r-158)<=2 && abs((int)g-182)<=2 && abs((int)b-233)<=2)?borderColor:backgroundColor;
            pixel=(a<<24)|((((color>>16)&255)*a/255)<<16)|((((color>>8)&255)*a/255)<<8)|((color&255)*a/255);
        } else if (tabCorner && (pixel>>24)) {
            unsigned a=pixel>>24;
            // These textures contain only the antialiased border curve. Preserve
            // coverage; every visible pixel uses the same ink as vector edges.
            pixel=(a<<24)|((((borderColor>>16)&255)*a/255)<<16)|((((borderColor>>8)&255)*a/255)<<8)|((borderColor&255)*a/255);
        } else if (ring) {
            unsigned a=pixel>>24;
            bool edge=a>=64 && (x==left[y] || x==right[y] || y==top[x] || y==bottom[x]);
            bool band=x<4 || x>=w-4 || y<4 || y>=h-4;
            if (edge) pixel=(a<<24)|((((borderColor>>16)&255)*a/255)<<16)|
                ((((borderColor>>8)&255)*a/255)<<8)|((borderColor&255)*a/255);
            else if (band) pixel=ringOpaque?(0xFF000000|backgroundColor):0;
            else if (!ringAlreadyThemed) pixel=transform(pixel,true,false);
        } else pixel=transform(pixel,kind==0,chromeOnly);
        ((uint32_t*)(output+y*outPitch))[x]=pixel;
    }
    double x,y; getOffset(surface,&x,&y); setOffset(copy,x,y);
    getScale(surface,&x,&y); setScale(copy,x,y); dirty(copy);
    void* themed=createPattern(copy); destroySurface(copy);
    if (!themed || patternStatus(themed)) { if (themed) destroyPattern(themed); return nullptr; }
    Matrix matrix; getMatrix(pattern,&matrix); setMatrix(themed,&matrix);
    setExtend(themed,extend(pattern)); setFilter(themed,filter(pattern));
    reference(pattern); saveContext(context); setSource(context,themed); destroyPattern(themed);
    InterlockedIncrement64(&changes);
    if (ring || blueSlice) InterlockedIncrement64(&hoverFrames);
    return pattern;
}
void restoreRasterSource(void* context,void* original) {
    if (original) { restoreContext(context); destroyPattern(original); }
}
