#include <cstdint>
#include <cstring>
struct LogFont { int64_t height,width,escape,orientation,weight; unsigned char flags[8]; char16_t family[32]; };
struct Font { unsigned kind,padding; LogFont value; uint64_t reserved0,stockId,reserved[3],attributes; int scale,padding2; };
static_assert(sizeof(Font)==176);
extern "C" void fixture_convert(LogFont*,const Font*,int) asm("_ZN4core19abs_font_to_logfontERNS_9V8LogFontERKNS_4FontEi");
extern "C" void fixture_stock(LogFont*,const Font*,int,int) asm("_ZN4core17adjust_stock_fontERNS_9V8LogFontERKNS_4FontEiNS_17CompatibilityModeE");
extern "C" void fixture_mapped(LogFont*,const char16_t* const*) asm("_ZN4core9V8LogFont11setFaceNameERKN3ale11BasicStringIDsLm0ELm0ESaEE");
void fixture_convert(LogFont* result,const Font* font,int){*result=font->value;}
void fixture_stock(LogFont* result,const Font* font,int,int){*result=font->value;}
void fixture_mapped(LogFont* result,const char16_t* const* value){char16_t copy[32]{};for(unsigned i=0;i<31&&(*value)[i];++i)copy[i]=(*value)[i];memcpy(result->family,copy,sizeof(copy));}
