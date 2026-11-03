#include "fonts.h"
#include "elf.h"
#include "editor_refresh.h"
#include <fontconfig/fontconfig.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cctype>
#include <string>

namespace linux_fonts {
namespace {
// Linux LP64 LOGFONT uses five 64-bit longs, eight byte flags and UTF-16.
struct LogFont { int64_t height,width,escape,orientation,weight; unsigned char italic,underline,strikeout,charset,out,clip,quality,pitch; char16_t family[32]; };
static_assert(sizeof(LogFont)==112 && offsetof(LogFont,family)==48);
using Convert=void(*)(LogFont*,const void*,int);
using Stock=void(*)(LogFont*,const void*,int,int);
Convert originalConvert;
Stock originalStock;
void (*originalMapped)(LogFont*,const void*);
thread_local LogFont* pendingStock;
bool stockMetadata;
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
Config config{};
Stats snapshot{};
char path[PATH_MAX];
std::string platformPath;
struct stat previous{};
void* (*eventSource)();
const void* styleEvent;
void* (*editorSource)();
const void* editorEvent;
bool editorRefreshRequired;
uintptr_t notifyBegin,notifyEnd;
struct Installed { uintptr_t* slot;uintptr_t original,hook; };
std::vector<Installed> installed;

void utf8(const char16_t* value,char (&out)[128]) {
    unsigned pos=0;
    for(unsigned i=0;i<31&&value[i];++i) {
        uint32_t c=value[i];
        if(c>=0xd800&&c<=0xdbff&&i+1<31&&value[i+1]>=0xdc00&&value[i+1]<=0xdfff) c=0x10000+((c-0xd800)<<10)+(value[++i]-0xdc00);
        if(c<128)out[pos++]=static_cast<char>(c);
        else if(c<2048){out[pos++]=static_cast<char>(192|(c>>6));out[pos++]=static_cast<char>(128|(c&63));}
        else if(c<65536){out[pos++]=static_cast<char>(224|(c>>12));out[pos++]=static_cast<char>(128|((c>>6)&63));out[pos++]=static_cast<char>(128|(c&63));}
        else {out[pos++]=static_cast<char>(240|(c>>18));out[pos++]=static_cast<char>(128|((c>>12)&63));out[pos++]=static_cast<char>(128|((c>>6)&63));out[pos++]=static_cast<char>(128|(c&63));}
    }
    out[pos]=0;
}
std::string utf8(const char16_t* value) {char out[128];utf8(value,out);return out;}
bool equal(const char16_t* a,const char16_t* b) {
    for(unsigned i=0;i<32;++i) {
        unsigned x=a[i],y=b[i];if(x<128)x=std::tolower(x);if(y<128)y=std::tolower(y);
        if(x!=y)return false;
        if(!x)return true;
    }
    return false;
}
bool interfaceFont(const LogFont& f) {
    return f.charset!=2&&!f.family[31]&&(equal(f.family,u"MS Shell Dlg")||equal(f.family,u"MS Shell Dlg 2")||equal(f.family,u"MS Sans Serif")||equal(f.family,u"Microsoft Sans Serif"));
}
bool available(const char16_t* name) {
    auto family=utf8(name); if(family.empty())return false;
    if(!FcInitBringUptoDate())return false;
    auto pattern=FcPatternCreate(); auto objects=FcObjectSetBuild(FC_FAMILY,nullptr);
    if(!pattern||!objects){if(pattern)FcPatternDestroy(pattern);if(objects)FcObjectSetDestroy(objects);return false;}
    auto fonts=FcFontList(nullptr,pattern,objects);bool found=false;
    if(fonts) for(int i=0;i<fonts->nfont&&!found;++i) for(int j=0;;++j) {
        FcChar8* candidate=nullptr;if(FcPatternGetString(fonts->fonts[i],FC_FAMILY,j,&candidate)!=FcResultMatch)break;
        if(!FcStrCmpIgnoreCase(candidate,reinterpret_cast<const FcChar8*>(family.c_str()))) {found=true;break;}
    }
    if(fonts)FcFontSetDestroy(fonts);
    FcObjectSetDestroy(objects);FcPatternDestroy(pattern);return found;
}
bool systemFamily(char16_t (&name)[32]) {
    auto pattern=FcPatternCreate();if(!pattern)return false;
    FcPatternAddString(pattern,FC_FAMILY,reinterpret_cast<const FcChar8*>("sans"));
    FcConfigSubstitute(nullptr,pattern,FcMatchPattern);FcDefaultSubstitute(pattern);
    FcResult result;auto match=FcFontMatch(nullptr,pattern,&result);FcPatternDestroy(pattern);
    if(!match)return false;
    FcChar8* text=nullptr;bool ok=FcPatternGetString(match,FC_FAMILY,0,&text)==FcResultMatch;
    char16_t decoded[32]{};unsigned count=0;
    if(ok)for(int remaining=static_cast<int>(strlen(reinterpret_cast<char*>(text)));remaining>0;) {
        FcChar32 value=0;int bytes=FcUtf8ToUcs4(text,&value,remaining);
        if(bytes<=0||value>0x10ffff||count+(value>0xffff?2:1)>31){ok=false;break;}
        if(value>0xffff){value-=0x10000;decoded[count++]=0xd800+(value>>10);decoded[count++]=0xdc00+(value&1023);}
        else decoded[count++]=static_cast<char16_t>(value);
        text+=bytes;remaining-=bytes;
    }
    if(ok&&count)memcpy(name,decoded,sizeof(name));else ok=false;
    FcPatternDestroy(match);return ok;
}
void substitute(LogFont& f,bool editor,int dpi,bool allowInterface=true) {
    if(editor&&config.editorEnabled&&(equal(f.family,u"Courier New")||equal(f.family,u"Courier")||equal(f.family,u"DejaVu Sans Mono"))) {
        memcpy(f.family,config.editor,sizeof(f.family));f.width=0;
        if(dpi>0&&dpi<=2400)f.height=-((static_cast<int64_t>(config.points10)*dpi+360)/720);
        ++snapshot.editorCalls;
    }else if(allowInterface&&config.uiEnabled&&interfaceFont(f)) {
        memcpy(f.family,config.ui,sizeof(f.family));f.width=0;++snapshot.uiCalls;
    }
}
void convert(LogFont* result,const void* source,int dpi) {
    pendingStock=nullptr;originalConvert(result,source,dpi);int saved=errno;
    pthread_mutex_lock(&mutex);
    ++snapshot.conversions;utf8(result->family,snapshot.source);
    substitute(*result,true,dpi,*static_cast<const uint32_t*>(source)==1);pthread_mutex_unlock(&mutex);errno=saved;
}
void interfaceConvert(LogFont* result,const void* source,int dpi) {
    pendingStock=nullptr;originalConvert(result,source,dpi);int saved=errno;
    pthread_mutex_lock(&mutex);
    utf8(result->family,snapshot.uiSource);
    substitute(*result,false,dpi,*static_cast<const uint32_t*>(source)==1);
    pthread_mutex_unlock(&mutex);errno=saved;
}
void stock(LogFont* result,const void* source,int dpi,int compatibility) {
    pendingStock=nullptr;originalStock(result,source,dpi,compatibility);int saved=errno;
    pthread_mutex_lock(&mutex);
    utf8(result->family,snapshot.uiSource);
    if(*static_cast<const uint32_t*>(source)==1) {
        if(stockMetadata) {
            uint64_t id,attributes;
            memcpy(&id,static_cast<const unsigned char*>(source)+128,8);
            memcpy(&attributes,static_cast<const unsigned char*>(source)+160,8);
            // Stock default GUI / ANSI variable / system fonts, without an
            // explicit face override. Linux resolves these to Arial, not the
            // Windows MS Shell Dlg aliases. Fixed and symbol fonts stay intact.
            if(config.uiEnabled&&!(attributes&1)&&(id==0||id==3||id==4)&&result->charset!=2) {
                memcpy(result->family,config.ui,sizeof(result->family));result->width=0;++snapshot.uiCalls;
                pendingStock=result;
            }
        }else substitute(*result,false,dpi);
    }
    pthread_mutex_unlock(&mutex);errno=saved;
}
void mapped(LogFont* result,const void* value) {
    originalMapped(result,value);int saved=errno;
    if(pendingStock==result) {
        pendingStock=nullptr;pthread_mutex_lock(&mutex);
        if(config.uiEnabled){memcpy(result->family,config.ui,sizeof(result->family));result->width=0;++snapshot.uiCalls;}
        pthread_mutex_unlock(&mutex);
    }
    errno=saved;
}
uintptr_t relative(const unsigned char* code,unsigned offset,unsigned length) {
    int32_t displacement;memcpy(&displacement,code+offset,4);return reinterpret_cast<uintptr_t>(code)+length+displacement;
}
bool discoverStyle(const linux_elf::Image& core,void* handle) {
    styleEvent=dlsym(handle,"_ZN4core25kDefaultStyleChangedEventE");if(!styleEvent)return false;
    uintptr_t found=0;unsigned matches=0;
    for(const auto& r:core.ranges) if(r.flags&PF_X) for(auto address=r.begin;address+32<r.end;++address) {
        auto p=reinterpret_cast<const unsigned char*>(address);
        // The platform calls its process event source, then notifies this exact
        // exported GUID through vtable slot 3 with null arguments.
        const unsigned char head[]={0xe8,0,0,0,0,0x48,0x8b,0x28,0x48,0x8b,0x35};
        const unsigned char tail[]={0x48,0x89,0xc7,0x31,0xd2,0x31,0xc9,0xff,0x55,0x18};
        if(p[0]!=head[0]||memcmp(p+5,head+5,6)||memcmp(p+15,tail,sizeof(tail)))continue;
        auto slot=relative(p+8,3,7);
        if(!core.contains(slot,8)||*reinterpret_cast<const void* const*>(slot)!=styleEvent)continue;
        auto getter=relative(p,1,5);
        const unsigned char body[]={0x48,0x85,0xc0,0x74,0x09,0x48,0x89,0xc7,0x58,0xe9};
        if(!core.contains(getter,24,PF_R|PF_X))continue;
        auto code=reinterpret_cast<const unsigned char*>(getter);
        if(code[0]!=0x50||code[1]!=0xe8||memcmp(code+6,body,sizeof(body))||memcmp(code+20,"\x31\xc0\x59\xc3",4))continue;
        found=getter;++matches;
    }
    if(matches!=1)return false;
    eventSource=reinterpret_cast<decltype(eventSource)>(found);
    for(const auto& r:core.ranges) if(r.flags&PF_X){notifyBegin=r.begin;notifyEnd=r.end;break;}
    return true;
}
bool discoverEditor() {
    if(editorSource)return true;
    linux_elf::Image text((platformPath+"/txtedui.so").c_str());
    const unsigned char guid[]={0xc2,0xc2,0x93,0x1b,0x8e,0x5a,0xd5,0x11,0xb0,0xba,0,0x80,0x48,0xda,7,0x65};
    uintptr_t getter=0,event=0;bool ambiguous=false;
    for(const auto& r:text.ranges) if(r.flags&PF_X) for(auto address=r.begin;address+29<r.end;++address) {
        auto p=reinterpret_cast<const unsigned char*>(address);
        const unsigned char head[]={0x4c,0x8b,0x00,0x48,0x8d,0x35};
        const unsigned char tail[]={0x48,0x89,0xc7,0x31,0xd2,0x31,0xc9,0x41,0xff,0x50,0x18};
        if(p[0]!=0xe8||memcmp(p+5,head,sizeof(head))||memcmp(p+15,tail,sizeof(tail)))continue;
        auto id=relative(p+8,3,7);
        if(!text.contains(id,16)||memcmp(reinterpret_cast<void*>(id),guid,16))continue;
        auto entry=relative(p,1,5);if(!text.contains(entry,24,PF_R|PF_X))continue;
        auto code=reinterpret_cast<const unsigned char*>(entry);
        const unsigned char body[]={0x48,0x85,0xc0,0x74,0x09,0x48,0x89,0xc7,0x58,0xe9};
        if(code[0]!=0x50||code[1]!=0xe8||memcmp(code+6,body,sizeof(body))||memcmp(code+20,"\x31\xc0\x59\xc3",4))continue;
        if(getter&&getter!=entry)ambiguous=true;
        getter=entry;event=id;
    }
    if(!getter||ambiguous)return false;
    editorSource=reinterpret_cast<decltype(editorSource)>(getter);editorEvent=reinterpret_cast<void*>(event);
    return true;
}
bool notify(void* source,const void* event,const std::vector<linux_elf::Range>& code) {
    if(!source||!(linux_elf::protection(reinterpret_cast<uintptr_t>(source))&PROT_READ))return false;
    auto table=*static_cast<uintptr_t**>(source);
    if(!(linux_elf::protection(reinterpret_cast<uintptr_t>(table))&PROT_READ))return false;
    auto method=table[3];bool known=false;
    for(const auto& r:code)if((r.flags&PF_X)&&method>=r.begin&&method<r.end)known=true;
    if(!known)return false;
    reinterpret_cast<void(*)(void*,const void*,void*,void*)>(method)(source,event,nullptr,nullptr);return true;
}
void gtkFont() {
    using Get=void(*)(void*,const char*,...);using Set=void(*)(void*,const char*,...);
    static std::string original;
    void* gtk=dlopen("libgtk-3.so.0",RTLD_NOW|RTLD_NOLOAD);if(!gtk)return;
    auto settings=reinterpret_cast<void*(*)()>(dlsym(gtk,"gtk_settings_get_default"));
    auto get=reinterpret_cast<Get>(dlsym(gtk,"g_object_get"));auto set=reinterpret_cast<Set>(dlsym(gtk,"g_object_set"));
    auto free=reinterpret_cast<void(*)(void*)>(dlsym(gtk,"g_free"));
    auto parse=reinterpret_cast<void*(*)(const char*)>(dlsym(gtk,"pango_font_description_from_string"));
    auto size=reinterpret_cast<int(*)(void*)>(dlsym(gtk,"pango_font_description_get_size"));
    auto release=reinterpret_cast<void(*)(void*)>(dlsym(gtk,"pango_font_description_free"));
    auto object=settings?settings():nullptr;
    if(object&&get&&set&&free&&parse&&size&&release) {
        if(original.empty()){char* value=nullptr;get(object,"gtk-font-name",&value,nullptr);if(value){original=value;free(value);}}
        if(!original.empty()) {
            auto description=parse(original.c_str());double points=description?size(description)/1024.:11.;if(description)release(description);
            if(points<=0||points>72)points=11;
            pthread_mutex_lock(&mutex);auto font=config;pthread_mutex_unlock(&mutex);
            std::string selected=original;
            if(font.uiEnabled){char number[24];const int hundredths=static_cast<int>(points*100+0.5);snprintf(number,sizeof(number)," %d.%02d",hundredths/100,hundredths%100);selected=utf8(font.ui)+number;}
            set(object,"gtk-font-name",selected.c_str(),nullptr);
        }
    }
    dlclose(gtk);
}

}
bool initialize(const char* platform,const char* configPath) {
    platformPath=platform;
    if(strlen(configPath)+12>=sizeof(path))return false;
    snprintf(path,sizeof(path),"%s.fonts",configPath);
    std::string corePath=std::string(platform)+"/core83.so",graphicsPath=std::string(platform)+"/grphcs.so";
    void* coreHandle=dlopen(corePath.c_str(),RTLD_NOW|RTLD_NOLOAD);if(!coreHandle)return false;
    linux_elf::Image core(corePath.c_str()),graphics(graphicsPath.c_str());
    originalConvert=reinterpret_cast<Convert>(dlsym(coreHandle,"_ZN4core19abs_font_to_logfontERNS_9V8LogFontERKNS_4FontEi"));
    originalMapped=reinterpret_cast<decltype(originalMapped)>(dlsym(coreHandle,"_ZN4core9V8LogFont11setFaceNameERKN3ale11BasicStringIDsLm0ELm0ESaEE"));
    originalStock=reinterpret_cast<Stock>(dlsym(coreHandle,"_ZN4core17adjust_stock_fontERNS_9V8LogFontERKNS_4FontEiNS_17CompatibilityModeE"));
    // Verify the LP64 layout from the exported constructor before interpreting
    // any LOGFONT objects. Other layouts are rejected without installing hooks.
    auto ctor=static_cast<unsigned char*>(dlsym(coreHandle,"_ZN4core9V8LogFontC1ERK11tagLOGFONTW"));
    const unsigned char prefix[]={0x0f,0x10,0x46,0x60,0x0f,0x11,0x47,0x60,0x0f,0x10,0x46,0x50};
    if(!originalConvert||!originalStock||!ctor||!core.contains(reinterpret_cast<uintptr_t>(ctor),sizeof(prefix),PF_R|PF_X)||memcmp(ctor,prefix,sizeof(prefix))) {dlclose(coreHandle);return false;}
    auto stockCtor=static_cast<unsigned char*>(dlsym(coreHandle,"_ZN4core4FontC1Eil"));
    if(stockCtor&&core.contains(reinterpret_cast<uintptr_t>(stockCtor),128,PF_R|PF_X)) {
        auto contains=[&](const unsigned char* pattern,size_t size){return std::search(stockCtor,stockCtor+128,pattern,pattern+size)!=stockCtor+128;};
        const unsigned char idStore[]={0x4c,0x89,0xb3,0x80,0,0,0};
        const unsigned char attributeStore[]={0x48,0xc7,0x83,0xa0,0,0,0,0,0,0,0};
        stockMetadata=contains(idStore,sizeof(idStore))&&contains(attributeStore,sizeof(attributeStore));
    }
    bool convertInstalled=false,stockInstalled=false,mappedInstalled=false;
    bool frameActive=false,frameStock=false,frameConvert=false;
    for(const auto& module:{graphicsPath,platformPath+"/frame.so"}) {
    linux_elf::Image image(module.c_str());bool frame=module!=graphicsPath;
    if(frame&&image.base)frameActive=true;
    image.relocations([&](const char* name,uintptr_t* slot) {
        uintptr_t original=0,hook=0;
        if(!strcmp(name,"_ZN4core19abs_font_to_logfontERNS_9V8LogFontERKNS_4FontEi")){original=reinterpret_cast<uintptr_t>(originalConvert);hook=frame?reinterpret_cast<uintptr_t>(&interfaceConvert):reinterpret_cast<uintptr_t>(&convert);}
        if(!strcmp(name,"_ZN4core17adjust_stock_fontERNS_9V8LogFontERKNS_4FontEiNS_17CompatibilityModeE")){original=reinterpret_cast<uintptr_t>(originalStock);hook=reinterpret_cast<uintptr_t>(&stock);}
        if(originalMapped&&!strcmp(name,"_ZN4core9V8LogFont11setFaceNameERKN3ale11BasicStringIDsLm0ELm0ESaEE")){original=reinterpret_cast<uintptr_t>(originalMapped);hook=reinterpret_cast<uintptr_t>(&mapped);}
        if(hook) {
            auto current=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
            bool lazy=false;
            if(current>=6 && image.contains(current-6,16,PF_R|PF_X)) {
                auto entry=reinterpret_cast<const unsigned char*>(current-6);
                lazy=entry[0]==0xff&&entry[1]==0x25&&entry[6]==0x68&&entry[11]==0xe9&&relative(entry,2,6)==reinterpret_cast<uintptr_t>(slot);
            }
            if((current==original||lazy)&&linux_elf::replace(slot,current,hook)) {
                installed.push_back({slot,current,hook});
                convertInstalled|=hook==reinterpret_cast<uintptr_t>(&convert);
                if(!frame)stockInstalled|=hook==reinterpret_cast<uintptr_t>(&stock);
                else {frameStock|=hook==reinterpret_cast<uintptr_t>(&stock);frameConvert|=hook==reinterpret_cast<uintptr_t>(&interfaceConvert);}
                mappedInstalled|=hook==reinterpret_cast<uintptr_t>(&mapped);
            }
        }
    });
    }
    if(!convertInstalled||!stockInstalled){for(const auto& p:installed)linux_elf::replace(p.slot,p.hook,p.original);installed.clear();dlclose(coreHandle);return false;}
    discoverStyle(core,coreHandle);
    snapshot.available=1|((stockMetadata&&(!originalMapped||mappedInstalled)&&(!frameActive||(frameStock&&frameConvert)))?2:0);dlclose(coreHandle);return true;
}
bool reload() {
    if(!snapshot.available)return false;
    Config next{};int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat meta{};
    bool ok=fd>=0&&!fstat(fd,&meta)&&S_ISREG(meta.st_mode)&&meta.st_uid==getuid()&&!(meta.st_mode&0022)&&meta.st_size==sizeof(next);
    if(ok&&meta.st_ino==previous.st_ino&&meta.st_dev==previous.st_dev&&meta.st_mtim.tv_sec==previous.st_mtim.tv_sec&&meta.st_mtim.tv_nsec==previous.st_mtim.tv_nsec){close(fd);return false;}
    if(ok)ok=read(fd,&next,sizeof(next))==sizeof(next);
    int error=errno;if(fd>=0)close(fd);errno=error;
    if(!ok)next={};
    if(ok&&(next.size!=sizeof(next)||next.version!=1||next.editorEnabled>1||next.uiEnabled>1||next.points10<80||next.points10>320||next.points10%5||next.editor[31]||next.ui[31])) {ok=false;next={};}
    if(ok)previous=meta;else previous={};
    // A Windows optical-face name maps to the same variable font file on Linux.
    bool fallback=false;
    if(ok&&next.uiEnabled&&equal(next.ui,u"Segoe UI Variable Small")&&!available(next.ui)) {
        if(available(u"Segoe UI Variable")) {
            memset(next.ui,0,sizeof(next.ui));memcpy(next.ui,u"Segoe UI Variable",sizeof(u"Segoe UI Variable"));fallback=true;
        }else fallback=systemFamily(next.ui);
    }
    const char* problem="";
    if(next.editorEnabled&&!available(next.editor)){problem="editor-font-unavailable";next.editorEnabled=0;}
    if(next.uiEnabled&&!(snapshot.available&2)){problem="interface-font-unsupported";next.uiEnabled=0;}
    else if(next.uiEnabled&&!available(next.ui)){problem="interface-font-unavailable";next.uiEnabled=0;}
    pthread_mutex_lock(&mutex);
    bool change=memcmp(&next,&config,sizeof(next));
    if(change){editorRefreshRequired|=config.editorEnabled||next.editorEnabled;config=next;++snapshot.generation;snapshot.refresh=eventSource?1:95;}
    snprintf(snapshot.error,sizeof(snapshot.error),"%s",problem);
    if(next.editorEnabled)utf8(next.editor,snapshot.editor);else snapshot.editor[0]=0;
    if(next.uiEnabled)utf8(next.ui,snapshot.ui);else snapshot.ui[0]=0;
    snapshot.editorEnabled=next.editorEnabled;snapshot.uiEnabled=next.uiEnabled;snapshot.points10=next.points10;
    snapshot.uiFallback=fallback;
    pthread_mutex_unlock(&mutex);return change;
}
void refresh() {
    pthread_mutex_lock(&mutex);
    bool required=editorRefreshRequired;editorRefreshRequired=false;
    pthread_mutex_unlock(&mutex);
    gtkFont();
    bool style=eventSource&&notify(eventSource(),styleEvent,{{notifyBegin,notifyEnd,PF_R|PF_X}});
    unsigned updated=0;
    bool editor=false;
    try {if(required&&discoverEditor()) {
        linux_elf::Image text((platformPath+"/txtedui.so").c_str()),core((platformPath+"/core83.so").c_str());
        editor=linux_editor::refresh(text,core,editorSource(),editorEvent,updated);
    }}catch(...){editor=false;}
    pthread_mutex_lock(&mutex);snapshot.refresh=style&&(!required||editor)?0:95;pthread_mutex_unlock(&mutex);
}
Stats stats(){pthread_mutex_lock(&mutex);auto copy=snapshot;pthread_mutex_unlock(&mutex);return copy;}
}
