#pragma once

struct ResolvedStyle {
    BYTE* getter=nullptr;
    BYTE* call=nullptr;
    unsigned length=0;
    bool tail=false;
};
// Both ordinary calls and compiler-generated tail calls describe the same
// event-source ABI. Match the GUID reference and complete dispatch, never RVAs.
static bool discoverStyle(const Image& core,const BYTE* event,ResolvedStyle& result) {
    if (!core.contains(event,sizeof(GUID))) return false;
    Pattern direct{0xe8,-1,-1,-1,-1,0x4c,0x8b,0x10,0x45,0x33,0xc9,0x45,0x33,0xc0,
        0x48,0x8d,0x15,-1,-1,-1,-1,0x48,0x8b,0xc8,0x41,0xff,0x52,0x18};
    Pattern tail{0xe8,-1,-1,-1,-1,0x4c,0x8b,0x10,0x45,0x33,0xc9,0x45,0x33,0xc0,
        0x48,0x8d,0x15,-1,-1,-1,-1,0x48,0x8b,0xc8,
        0x48,0x8b,0x5c,0x24,-1,0x48,0x8b,0x74,0x24,-1,0x48,0x83,0xc4,-1,0x5f,0x49,0xff,0x62,0x18};
    bool ambiguous=false;
    result={};
    core.sections(true,[&](Span span) {
        for (SIZE_T i=0;i+direct.size<=span.size;++i) {
            auto p=span.data+i;
            bool isTail=tail.at(span,i);
            if (!isTail && !direct.at(span,i)) continue;
            unsigned length=isTail?tail.size:direct.size;
            auto body=core.function(p);
            if (!body || !core.contains(p,length) || p+length>body.data+body.size ||
                (isTail && p+length!=body.data+body.size) || relative(p+14,3,7)!=event) continue;
            auto getter=relative(p,1,5);
            if (!core.entry(getter)) continue;
            if (result.getter && result.getter!=getter) ambiguous=true;
            result={getter,p,length,isTail};
        }
    });
    if (ambiguous) result={};
    return result.getter!=nullptr;
}
