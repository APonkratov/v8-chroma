// Compare the exact legacy Uniscribe entry point used by grphcs.dll with calt on/off.
#define UNISCRIBE_OPENTYPE 0x0100
#include <windows.h>
#include <usp10.h>
#include <cstdio>
#include <cwchar>
#include <vector>
#include <cstring>
#include "../src/font_ligatures.h"

struct Result {
    HRESULT status=E_FAIL;
    std::vector<WORD> glyphs;
    int width=0;
    int runs=0;
    std::vector<int> carets;
};
static Result shapeRun(HDC dc,const wchar_t* text,int count,int feature,SCRIPT_ANALYSIS analysis,OPENTYPE_TAG tag) {
    Result result;
    SCRIPT_CACHE cache=nullptr;
    WORD glyphs[256]{}, clusters[256]{};
    SCRIPT_VISATTR attributes[256]{};
    SCRIPT_CHARPROP charProps[256]{}; SCRIPT_GLYPHPROP glyphProps[256]{};
    int glyphCount=0, advances[256]{}; GOFFSET offsets[256]{}; ABC abc{};
    if (feature<0) {
        if (feature==-2) analysis.s.fInhibitLigate=1;
        result.status=ScriptShape(dc,&cache,text,count,256,&analysis,glyphs,clusters,attributes,&glyphCount);
        if (SUCCEEDED(result.status)) result.status=ScriptPlace(dc,&cache,glyphs,glyphCount,attributes,&analysis,advances,offsets,&abc);
    } else {
        OPENTYPE_FEATURE_RECORD record{static_cast<OPENTYPE_TAG>('c'|('a'<<8)|('l'<<16)|('t'<<24)),feature};
        TEXTRANGE_PROPERTIES properties{&record,1}; auto range=&properties;
        result.status=ScriptShapeOpenType(dc,&cache,&analysis,tag,0,&count,&range,1,text,count,256,
            clusters,charProps,glyphs,glyphProps,&glyphCount);
        if (SUCCEEDED(result.status)) result.status=ScriptPlaceOpenType(dc,&cache,&analysis,tag,0,&count,&range,1,
            text,clusters,charProps,count,glyphs,glyphProps,glyphCount,advances,offsets,&abc);
    }
    if (SUCCEEDED(result.status)) {
        result.glyphs.assign(glyphs,glyphs+glyphCount);
        for (int i=0;i<glyphCount;++i) result.width+=advances[i];
        if (feature<0) for (int i=0;i<=count;++i) {
            int x=0;
            if (FAILED(ScriptCPtoX(i,FALSE,count,glyphCount,clusters,attributes,advances,&analysis,&x))) {
                result.status=E_FAIL; break;
            }
            result.carets.push_back(x);
        }
    }
    ScriptFreeCache(&cache);
    return result;
}
static Result shape(HDC dc,const wchar_t* text,int feature,bool merge=false,bool operators=false) {
    int count=static_cast<int>(wcslen(text));
    SCRIPT_ITEM items[256]{}; OPENTYPE_TAG tags[256]{}; int itemCount=0;
    Result result;
    SCRIPT_CONTROL control{}; control.fMergeNeutralItems=merge?1:0;
    result.status=ScriptItemizeOpenType(text,count,255,merge?&control:nullptr,nullptr,items,tags,&itemCount);
    if (FAILED(result.status)) return result;
    if (!itemCount) { result.status=E_FAIL; return result; }
    if (operators) itemizeOperators(text,count,255,nullptr,nullptr,items,&itemCount,ScriptItemize);
    result.runs=itemCount;
    for (int i=0;i<itemCount;++i) {
        Result run=shapeRun(dc,text+items[i].iCharPos,items[i+1].iCharPos-items[i].iCharPos,feature,items[i].a,tags[i]);
        result.status=run.status;
        if (FAILED(run.status)) return result;
        result.glyphs.insert(result.glyphs.end(),run.glyphs.begin(),run.glyphs.end());
        if (!result.carets.empty() && !run.carets.empty()) result.carets.pop_back();
        for (int x:run.carets) result.carets.push_back(result.width+x);
        result.width+=run.width;
    }
    return result;
}
int wmain(int argc,wchar_t** argv) {
    const wchar_t* family=argc>1?argv[1]:L"Consolas";
    LOGFONTW font{}; font.lfHeight=-20; font.lfCharSet=DEFAULT_CHARSET;
    if (wcslen(family)>=LF_FACESIZE) return 2;
    wcscpy_s(font.lfFaceName,family);
    HDC dc=CreateCompatibleDC(nullptr); HFONT handle=CreateFontIndirectW(&font);
    if (!dc || !handle) return 2;
    auto previous=SelectObject(dc,handle);
    wchar_t actual[LF_FACESIZE]{}; GetTextFaceW(dc,LF_FACESIZE,actual);
    if (_wcsicmp(actual,family)) { fwprintf(stderr,L"Font fallback: %ls -> %ls\n",family,actual); return 2; }
    const wchar_t* samples[]={L"<=",L">=",L"<>",L"!=",L"==",L"=>",L"->",L":=",L"//",L"0123456789",L"\u0401\u0451\u0419\u0439\u0414\u0434\u0429\u0449"};
    const char* labels[]={"<=",">=","<>","!=","==","=>","->",":=","//","digits","Cyrillic"};
    bool ok=true; unsigned substitutions=0,operatorSubstitutions=0;
    unsigned index=0;
    for (auto sample:samples) {
        Result legacy=shape(dc,sample,-1), off=shape(dc,sample,0), on=shape(dc,sample,1), inhibited=shape(dc,sample,-2);
        Result merged=shape(dc,sample,-1,true), mergedOff=shape(dc,sample,0,true);
        Result isolated=shape(dc,sample,-1,false,true);
        bool success=SUCCEEDED(legacy.status) && SUCCEEDED(off.status) && SUCCEEDED(on.status) && !legacy.glyphs.empty();
        bool changed=on.glyphs!=off.glyphs;
        bool matches=legacy.glyphs==on.glyphs;
        bool sameWidth=legacy.width==off.width && on.width==off.width;
        if (changed) ++substitutions;
        if (SUCCEEDED(isolated.status) && isolated.glyphs!=off.glyphs) ++operatorSubstitutions;
        for (WORD glyph:legacy.glyphs) if (!glyph) success=false;
        ok &= success && sameWidth && SUCCEEDED(merged.status) && SUCCEEDED(mergedOff.status)
            && merged.width==legacy.width && SUCCEEDED(isolated.status) && isolated.width==legacy.width
            && isolated.carets==legacy.carets;
        // Print operator labels only; the Cyrillic coverage case is identified by length.
        printf("sample=%s chars=%zu runs=%d glyphs=%zu calt_changes=%u legacy_matches_on=%u inhibit_changes=%u width=%d stable_width=%u success=%u merged_runs=%d merged_changes=%u merged_width=%d\n",
            labels[index++],wcslen(sample),legacy.runs,legacy.glyphs.size(),changed?1:0,matches?1:0,
            inhibited.glyphs!=legacy.glyphs?1:0,legacy.width,sameWidth?1:0,success?1:0,
            merged.runs,merged.glyphs!=mergedOff.glyphs,merged.width);
    }
    const wchar_t* mixed[]={L"Если А <= Б И Б >= А И А <> Б Тогда",L"Сообщить(\"<= >= <> != == => -> := //\");",
        L"Latin <= Cyrillic Кириллица -> end",L"text 0123 = 4567",L"\u05d0\u05d1 <= \u05d2\u05d3"};
    for (auto sample:mixed) {
        auto original=shape(dc,sample,-1),isolated=shape(dc,sample,-1,false,true);
        bool sameFallback=FAILED(original.status) && original.status==isolated.status;
        bool stable=sameFallback || (SUCCEEDED(original.status) && SUCCEEDED(isolated.status) &&
            original.width==isolated.width && original.carets==isolated.carets);
        ok &= stable;
        printf("mixed chars=%zu original_runs=%d operator_runs=%d substituted=%u width=%d stable_geometry=%u same_fallback=%u\n",
            wcslen(sample),original.runs,isolated.runs,original.glyphs!=isolated.glyphs,isolated.width,stable,sameFallback);
    }
    printf("Uniscribe: %s; original calt substitutions=%u; operator-isolated substitutions=%u\n",
        ok?"PASS":"FAIL",substitutions,operatorSubstitutions);
    SelectObject(dc,previous); DeleteObject(handle); DeleteDC(dc);
    return ok?0:1;
}
