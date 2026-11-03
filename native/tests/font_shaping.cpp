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
    result.status=feature<0
        ?ScriptItemize(text,count,255,merge?&control:nullptr,nullptr,items,&itemCount)
        :ScriptItemizeOpenType(text,count,255,merge?&control:nullptr,nullptr,items,tags,&itemCount);
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
static bool controlItemsPreserved(const wchar_t* text,bool bidi,bool expectedItemization) {
    int count=static_cast<int>(wcslen(text)),originalCount=0;
    SCRIPT_ITEM original[256]{},revised[256]{};
    SCRIPT_CONTROL control{}; SCRIPT_STATE state{};
    const SCRIPT_CONTROL* inputControl=bidi?&control:nullptr;
    const SCRIPT_STATE* inputState=bidi?&state:nullptr;
    if (FAILED(ScriptItemize(text,count,255,inputControl,inputState,original,&originalCount))) return false;
    int revisedCount=0; bool itemized=false;
    if (FAILED(itemizeEditorText(text,count,255,inputControl,inputState,revised,&revisedCount,
        ScriptItemize,true,itemized)) || itemized!=expectedItemization) return false;
    if (!expectedItemization && (originalCount!=revisedCount ||
        memcmp(original,revised,(originalCount+1)*sizeof(SCRIPT_ITEM)))) return false;
    for (int begin=0;begin<count;) {
        if (!programmingOperator(text[begin])) { ++begin; continue; }
        int end=begin+1;
        while (end<count && programmingOperator(text[end])) ++end;
        if (end-begin>=2) {
            SCRIPT_CONTROL merged=control; merged.fMergeNeutralItems=1;
            SCRIPT_ITEM isolated[9]{}; int isolatedCount=0;
            if (FAILED(ScriptItemize(text+begin,end-begin,8,&merged,inputState,isolated,&isolatedCount)) ||
                isolatedCount!=1) return false;
            bool found=false;
            for (int j=0;j<revisedCount;++j)
                if (revised[j].iCharPos<=begin && revised[j+1].iCharPos>=end &&
                    !memcmp(&revised[j].a,&isolated[0].a,sizeof(SCRIPT_ANALYSIS))) found=true;
            if (!found) return false;
        }
        begin=end;
    }
    const SCRIPT_PROPERTIES** properties=nullptr; int propertyCount=0;
    if (FAILED(ScriptGetProperties(&properties,&propertyCount))) return false;
    unsigned controls=0;
    for (int i=0;i<originalCount;++i) {
        if (original[i].a.eScript>=propertyCount) return false;
        if (!properties[original[i].a.eScript]->fControl) continue;
        ++controls;
        bool preserved=false;
        for (int j=0;j<revisedCount;++j)
            if (original[i].iCharPos==revised[j].iCharPos && original[i+1].iCharPos==revised[j+1].iCharPos &&
                !memcmp(&original[i].a,&revised[j].a,sizeof(SCRIPT_ANALYSIS))) preserved=true;
        if (!preserved) return false;
    }
    return controls>0 && revised[revisedCount].iCharPos==count;
}
static bool itemCapacityPreserved() {
    const wchar_t* text=L"\t\t<>";
    SCRIPT_ITEM original[256]{}; int originalCount=0;
    if (FAILED(ScriptItemize(text,4,255,nullptr,nullptr,original,&originalCount)) || originalCount<2) return false;
    std::vector<SCRIPT_ITEM> revised(original,original+originalCount+1);
    revised.push_back({-1,{}});
    int revisedCount=originalCount;
    bool full=itemizeOperators(text,4,originalCount,nullptr,nullptr,revised.data(),&revisedCount,ScriptItemize);
    if (!full || revisedCount!=originalCount || revised[revisedCount].iCharPos!=4 ||
        revised.back().iCharPos!=-1) return false;
    for (int i=0;i<originalCount-1;++i)
        if (memcmp(&original[i],&revised[i],sizeof(SCRIPT_ITEM))) return false;
    revised.assign(original,original+originalCount+1);
    revisedCount=originalCount;
    bool tooSmall=itemizeOperators(text,4,originalCount-1,nullptr,nullptr,revised.data(),&revisedCount,ScriptItemize);
    return !tooSmall && revisedCount==originalCount &&
        !memcmp(original,revised.data(),(originalCount+1)*sizeof(SCRIPT_ITEM));
}
static HRESULT WINAPI itemizeWithError(const WCHAR* text,int count,int capacity,const SCRIPT_CONTROL* control,
    const SCRIPT_STATE* state,SCRIPT_ITEM* items,int* itemCount) {
    HRESULT status=ScriptItemize(text,count,capacity,control,state,items,itemCount);
    SetLastError(ERROR_NOT_READY);
    return status;
}
static bool nativeResultPreserved() {
    const wchar_t* text=L"\t\t<=";
    SCRIPT_ITEM native[256]{},revised[256]{}; int nativeCount=0,revisedCount=0;
    HRESULT expected=itemizeWithError(text,4,255,nullptr,nullptr,native,&nativeCount);
    bool changed=true;
    HRESULT disabled=itemizeEditorText(text,4,255,nullptr,nullptr,revised,&revisedCount,itemizeWithError,false,changed);
    if (disabled!=expected || GetLastError()!=ERROR_NOT_READY || changed || nativeCount!=revisedCount ||
        memcmp(native,revised,(nativeCount+1)*sizeof(SCRIPT_ITEM))) return false;
    HRESULT enabled=itemizeEditorText(text,4,255,nullptr,nullptr,revised,&revisedCount,itemizeWithError,true,changed);
    if (enabled!=expected || GetLastError()!=ERROR_NOT_READY || !changed) return false;
    HRESULT failed=itemizeEditorText(nullptr,0,255,nullptr,nullptr,revised,&revisedCount,itemizeWithError,true,changed);
    return failed==E_INVALIDARG && GetLastError()==ERROR_NOT_READY && !changed;
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
            && isolated.carets==legacy.carets && isolated.glyphs==merged.glyphs;
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
    const wchar_t* tabbed[]={L"\t\tЕсли А <> Б Тогда",L"Если А <> Б Тогда\t\t",
        L"Если А\t\t<=\t\tБ Тогда",L"\t\t<=\t\t",L"<=\t\t>="};
    for (auto sample:tabbed) for (bool bidi:{false,true}) {
        bool preserved=controlItemsPreserved(sample,bidi,true);
        ok &= preserved;
        printf("control items chars=%zu bidi=%u preserved=%u\n",wcslen(sample),bidi,preserved);
    }
    for (bool bidi:{false,true}) ok &= controlItemsPreserved(L"\t\tТекст",bidi,false);
    bool capacity=itemCapacityPreserved(),nativeResult=nativeResultPreserved();
    ok &= capacity && nativeResult;
    printf("item capacity=%u native result/error/pass-through=%u\n",capacity,nativeResult);
    printf("Uniscribe: %s; original calt substitutions=%u; operator-isolated substitutions=%u\n",
        ok?"PASS":"FAIL",substitutions,operatorSubstitutions);
    SelectObject(dc,previous); DeleteObject(handle); DeleteDC(dc);
    return ok?0:1;
}
