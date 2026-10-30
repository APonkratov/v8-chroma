#pragma once
#include <vector>
#include <cstring>

static bool programmingOperator(WCHAR ch) {
    return ch && wcschr(L"<>=!:+-/*~&|^%?",ch);
}

// Keep the original analysis outside ASCII operator sequences. In particular,
// merging all neutrals into Cyrillic items selects a different shaping engine
// and still loses programming ligatures. Let Uniscribe analyse each operator
// sequence in its own context; all glyphs, clusters and advances remain native.
static bool itemizeOperators(const WCHAR* text,int count,int capacity,const SCRIPT_CONTROL* control,
    const SCRIPT_STATE* state,SCRIPT_ITEM* items,int* itemCount,decltype(&ScriptItemize) native) {
    if (!text || !items || !itemCount || count<2 || *itemCount<1 || *itemCount>=capacity ||
        (state && (state->uBidiLevel&1))) return false;
    try {
        std::vector<SCRIPT_ITEM> revised;
        revised.reserve(static_cast<size_t>(capacity<count+1?capacity:count+1));
        int source=0,cursor=0; bool changed=false;
        auto append=[&](int position,SCRIPT_ANALYSIS analysis) {
            if (!revised.empty() && !memcmp(&revised.back().a,&analysis,sizeof(analysis))) return true;
            if (static_cast<int>(revised.size())>=capacity-1) return false;
            revised.push_back({position,analysis}); return true;
        };
        auto copy=[&](int end) {
            while (cursor<end) {
                while (source+1<*itemCount && items[source+1].iCharPos<=cursor) ++source;
                if (!append(cursor,items[source].a)) return false;
                int next=items[source+1].iCharPos;
                if (next<=cursor) return false;
                cursor=next<end?next:end;
            }
            return true;
        };
        for (int begin=0;begin<count;) {
            if (!programmingOperator(text[begin])) { ++begin; continue; }
            int end=begin+1;
            while (end<count && programmingOperator(text[end])) ++end;
            if (end-begin<2) { begin=end; continue; }
            if (!copy(begin)) return false;
            int last=source;
            while (last+1<*itemCount && items[last+1].iCharPos<end) ++last;
            bool leftToRight=true;
            for (int i=source;i<=last;++i)
                if (items[i].a.fRTL || items[i].a.fLayoutRTL || items[i].a.s.uBidiLevel) leftToRight=false;
            SCRIPT_CONTROL merged=control?*control:SCRIPT_CONTROL{};
            merged.fMergeNeutralItems=1;
            SCRIPT_ITEM isolated[8]{}; int runs=0;
            if (leftToRight && SUCCEEDED(native(text+begin,end-begin,8,&merged,state,isolated,&runs)) &&
                runs==1 && !isolated[0].a.fRTL && !isolated[0].a.s.uBidiLevel) {
                if (!append(begin,isolated[0].a)) return false;
                cursor=end;
                changed=true;
            } else if (!copy(end)) return false;
            begin=end;
        }
        if (!changed || !copy(count)) return false;
        revised.push_back({count,items[*itemCount].a});
        memcpy(items,revised.data(),revised.size()*sizeof(SCRIPT_ITEM));
        *itemCount=static_cast<int>(revised.size())-1;
        return true;
    } catch (...) { return false; } // Keep the complete original result on allocation failure.
}
