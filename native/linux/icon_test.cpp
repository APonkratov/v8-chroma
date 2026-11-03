#include "icons.h"
#include <cairo.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
static void check(bool condition,const char* message){if(!condition){fprintf(stderr,"%s\n",message);std::exit(1);}}
static uint32_t pixel(cairo_surface_t* image,int x,int y){cairo_surface_flush(image);uint32_t v;memcpy(&v,cairo_image_surface_get_data(image)+y*cairo_image_surface_get_stride(image)+4*x,4);return v;}
int main(){
    void* cairo=dlopen("libcairo.so.2",RTLD_NOW|RTLD_NOLOAD);
    check(linux_icons::initialize(0,cairo)&&linux_icons::initialize(1,cairo),"Cairo API");
    for(unsigned bank=0;bank<2;++bank)for(int size:{14,16,20,24,28,32,48,64}){
        auto image=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,size*3,size);
        for(int y=0;y<size;++y)for(int x=0;x<size*3;++x){uint32_t value=(x==size*3-1&&y==size-1)?0:0xffffffffu;memcpy(cairo_image_surface_get_data(image)+y*cairo_image_surface_get_stride(image)+x*4,&value,4);}
        cairo_surface_mark_dirty(image);cairo_surface_set_device_offset(image,2,3);cairo_surface_set_device_scale(image,1.5,1.5);
        auto pattern=cairo_pattern_create_for_surface(image);cairo_matrix_t transform;cairo_matrix_init_translate(&transform,4,5);
        cairo_pattern_set_matrix(pattern,&transform);cairo_pattern_set_extend(pattern,CAIRO_EXTEND_PAD);cairo_pattern_set_filter(pattern,CAIRO_FILTER_NEAREST);
        auto target=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,100,100);auto context=cairo_create(target);
        for(unsigned mode:{2u,1u,0u}){
            linux_icons::configure(mode);linux_icons::source(bank,context,pattern,reinterpret_cast<void(*)(void*,void*)>(&cairo_set_source));
            auto selected=cairo_get_source(context);cairo_surface_t* result=nullptr;check(!cairo_pattern_get_surface(selected,&result),"surface pattern");
            check(pixel(result,size+1,1)==(mode==2?0xff4488ccu:mode==1?0xff44cc88u:0xffffffffu),"known atlas replacement/stock restoration");
            check(pixel(result,0,0)==0xffffffffu&&pixel(result,size*3-1,size-1)==0,"atlas outside patch and transparent pixels preserved");
            cairo_matrix_t actual;cairo_pattern_get_matrix(selected,&actual);check(!memcmp(&actual,&transform,sizeof(actual)),"pattern matrix preserved");
            check(cairo_pattern_get_extend(selected)==CAIRO_EXTEND_PAD&&cairo_pattern_get_filter(selected)==CAIRO_FILTER_NEAREST,"extend/filter preserved");
            double x,y;cairo_surface_get_device_offset(result,&x,&y);check(x==2&&y==3,"device offset preserved");
            cairo_surface_get_device_scale(result,&x,&y);check(x==1.5&&y==1.5,"device scale preserved");
            check(pixel(image,size+1,1)==0xffffffffu,"original atlas remains immutable");
        }
        linux_icons::configure(2);
        auto recording=cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA,nullptr);auto exportContext=cairo_create(recording);
        linux_icons::source(bank,exportContext,pattern,reinterpret_cast<void(*)(void*,void*)>(&cairo_set_source));
        cairo_surface_t* unchanged=nullptr;cairo_pattern_get_surface(cairo_get_source(exportContext),&unchanged);check(unchanged==image,"non-screen export surfaces bypassed");
        cairo_destroy(exportContext);cairo_surface_destroy(recording);
        // Same geometry, changed bytes: no match and no stale cached replacement.
        uint32_t unknown=0xfffefdfcu;memcpy(cairo_image_surface_get_data(image),&unknown,4);cairo_surface_mark_dirty(image);
        linux_icons::surface(bank,context,image,0,0,reinterpret_cast<void(*)(void*,void*,double,double)>(&cairo_set_source_surface));
        cairo_pattern_get_surface(cairo_get_source(context),&unchanged);check(unchanged==image,"unknown image passes unchanged");
        cairo_destroy(context);cairo_surface_destroy(target);cairo_pattern_destroy(pattern);cairo_surface_destroy(image);
    }
    auto swatch=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,16,16);
    for(int y=0;y<16;++y)for(int x=0;x<16;++x){uint32_t p=y<12?0xffcc2244u:0xff12ab34u;memcpy(cairo_image_surface_get_data(swatch)+y*cairo_image_surface_get_stride(swatch)+x*4,&p,4);}
    cairo_surface_mark_dirty(swatch);auto target=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,16,16);auto context=cairo_create(target);
    linux_icons::surface(1,context,swatch,0,0,reinterpret_cast<void(*)(void*,void*,double,double)>(&cairo_set_source_surface));
    cairo_surface_t* result=nullptr;cairo_pattern_get_surface(cairo_get_source(context),&result);
    check(pixel(result,2,2)==0xff4488ccu&&pixel(result,2,14)==0xff12ab34u,"dynamic color swatch preserved");
    cairo_destroy(context);cairo_surface_destroy(target);cairo_surface_destroy(swatch);
    check(linux_icons::stats().matches>0,"real replacements observed");
    puts("Linux icons: known atlases, light/dark/stock, unknown passthrough, alpha, device geometry, pattern properties, export isolation, swatches and separate banks: PASS");
}
