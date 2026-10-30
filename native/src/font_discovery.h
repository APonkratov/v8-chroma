// Discover the editor's own font-only call chain. No version, image hash or RVA
// is used as a compatibility key. An unrecognized instruction layout simply
// leaves existing editors alone; the conversion hook still serves new editors.
#pragma once
#include <initializer_list>

struct Span { BYTE* data=nullptr; SIZE_T size=0; explicit operator bool() const { return data && size; } };
struct Pattern {
    int bytes[128]{}; unsigned size;
    Pattern(std::initializer_list<int> value):size(static_cast<unsigned>(value.size())) {
        unsigned i=0; for (auto byte:value) bytes[i++]=byte;
    }
    bool at(Span span,SIZE_T offset) const {
        if (offset>span.size || size>span.size-offset) return false;
        for (unsigned i=0;i<size;++i) if (bytes[i]>=0 && span.data[offset+i]!=bytes[i]) return false;
        return true;
    }
};
struct Image {
    BYTE* base=nullptr; DWORD size=0;
    IMAGE_NT_HEADERS64* nt=nullptr;
    explicit Image(HMODULE module) {
        if (!module) return;
        auto data=reinterpret_cast<BYTE*>(module);
        auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(data);
        if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 || dos->e_lfanew>0x100000) return;
        auto headers=reinterpret_cast<IMAGE_NT_HEADERS64*>(data+dos->e_lfanew);
        if (headers->Signature!=IMAGE_NT_SIGNATURE || headers->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
            headers->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
        base=data; size=headers->OptionalHeader.SizeOfImage; nt=headers;
    }
    bool contains(const void* pointer,SIZE_T length) const {
        auto address=reinterpret_cast<ULONG_PTR>(pointer),start=reinterpret_cast<ULONG_PTR>(base);
        return base && address>=start && address-start<size && length<=size-(address-start);
    }
    template<class F> void sections(bool executable,F visit) const {
        if (!nt) return;
        auto section=IMAGE_FIRST_SECTION(nt);
        for (unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
            auto& item=section[i];
            if (!(item.Characteristics&IMAGE_SCN_MEM_READ) ||
                bool(item.Characteristics&IMAGE_SCN_MEM_EXECUTE)!=executable ||
                (!executable && (item.Characteristics&IMAGE_SCN_MEM_WRITE))) continue;
            if (contains(base+item.VirtualAddress,item.Misc.VirtualSize)) visit(Span{base+item.VirtualAddress,item.Misc.VirtualSize});
        }
    }
    BYTE* unique(const Pattern& pattern,bool executable) const {
        BYTE* result=nullptr; unsigned count=0;
        sections(executable,[&](Span span) {
            for (SIZE_T i=0;i+pattern.size<=span.size;++i) if (pattern.at(span,i)) { result=span.data+i; ++count; }
        });
        return count==1?result:nullptr;
    }
    Span function(BYTE* instruction) const {
        if (!nt || !contains(instruction,1)) return {};
        auto& directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!contains(base+directory.VirtualAddress,directory.Size)) return {};
        auto entries=reinterpret_cast<RUNTIME_FUNCTION*>(base+directory.VirtualAddress);
        auto offset=static_cast<DWORD>(instruction-base);
        for (SIZE_T i=0;i<directory.Size/sizeof(*entries);++i) {
            auto& entry=entries[i];
            if (entry.BeginAddress<=offset && offset<entry.EndAddress && entry.EndAddress>entry.BeginAddress &&
                contains(base+entry.BeginAddress,entry.EndAddress-entry.BeginAddress))
                return {base+entry.BeginAddress,entry.EndAddress-entry.BeginAddress};
        }
        return {};
    }
    bool entry(BYTE* pointer) const { return function(pointer).data==pointer; }
    bool code(BYTE* pointer) const {
        bool found=false;
        sections(true,[&](Span span) {
            auto address=reinterpret_cast<ULONG_PTR>(pointer),start=reinterpret_cast<ULONG_PTR>(span.data);
            if (address>=start && address-start<span.size) found=true;
        });
        return found;
    }
};
static BYTE* relative(BYTE* instruction,unsigned displacement,unsigned length) {
    LONG value; memcpy(&value,instruction+displacement,sizeof(value));
    return instruction+length+value;
}
static BYTE* uniqueIn(Span span,const Pattern& pattern) {
    BYTE* result=nullptr; unsigned count=0;
    for (SIZE_T i=0;i+pattern.size<=span.size;++i) if (pattern.at(span,i)) { result=span.data+i; ++count; }
    return count==1?result:nullptr;
}
struct ResolvedEditor {
    DWORD getter,event,construct,loadFont,setFont,handler,loaderSlot;
    unsigned tableCount=0; DWORD tables[64]{};
};
static bool discoverEditor(const Image& text,const Image& core,ResolvedEditor& result) {
    auto event=text.unique({0xc2,0xc2,0x93,0x1b,0x8e,0x5a,0xd5,0x11,0xb0,0xba,0,0x80,0x48,0xda,7,0x65},false);
    if (!event) return false;
    // Settings dialog: get the thread's source and notify the text-settings GUID.
    auto notify=text.unique({0xe8,-1,-1,-1,-1,0x45,0x33,0xc9,0x48,0x8d,0x15,-1,-1,-1,-1,
        0x45,0x33,0xc0,0x48,0x8b,0xc8,0x4c,0x8b,0x10,0x41,0xff,0x52,0x18},true);
    if (!notify || relative(notify+8,3,7)!=event) return false;
    auto getter=relative(notify,1,5);
    Pattern getterCode{0x48,0x83,0xec,0x28,0xff,0x15,-1,-1,-1,-1,0x48,0x85,0xc0,0x74,0x0c,
        0x48,0x8b,0xc8,0x48,0x83,0xc4,0x28,0xe9,-1,-1,-1,-1,0x48,0x83,0xc4,0x28,0xc3};
    if (!text.entry(getter) || !getterCode.at(text.function(getter),0) || !text.entry(relative(getter+22,1,5))) return false;
    // The controller's callback compares exactly that GUID before reading settings.
    auto call=text.unique({0xe8,-1,-1,-1,-1,0xb2,1,0x48,0x8b,0xcb},true);
    if (!call || !text.contains(call-22,22) ||
        !Pattern{0x48,0x33,0x15,-1,-1,-1,-1}.at({call-22,7},0) ||
        !Pattern{0x48,0x33,0x05,-1,-1,-1,-1}.at({call-12,7},0) ||
        relative(call-22,3,7)!=event || relative(call-12,3,7)!=event+8) return false;
    auto handler=text.function(call);
    auto settings=relative(call,1,5);
    auto body=text.function(settings);
    if (!handler || !body || body.data!=settings) return false;
    auto global=uniqueIn(body,{0x48,0x8d,0x4d,-1,0xff,0x15,-1,-1,-1,-1,0x48,0x8d,0x55,-1,
        0xe8,-1,-1,-1,-1,0x48,0x8d,0x55,-1,0x49,0x8b,0xcc,0xe8,-1,-1,-1,-1});
    auto method=uniqueIn(body,{0x49,0x8b,0x04,0x24,0x48,0x8d,0x55,-1,0x49,0x8b,0xcc,
        0xff,0x90,-1,-1,-1,-1,0x48,0x8b,0xd0,0x49,0x8b,0xcc,0xe8,-1,-1,-1,-1});
    if (bool(global)==bool(method)) return false;
    if (global) {
        auto slot=relative(global+4,2,6);
        auto construct=GetProcAddress(reinterpret_cast<HMODULE>(core.base),"??0Font@core@@QEAA@XZ");
        if (!text.contains(slot,sizeof(void*)) || !construct ||
            *reinterpret_cast<void**>(slot)!=reinterpret_cast<void*>(construct)) return false;
        auto loader=relative(global+14,1,5);
        auto setter=relative(global+26,1,5);
        if (!text.entry(loader) || !text.entry(setter)) return false;
        result.construct=static_cast<DWORD>(slot-text.base);
        result.loadFont=static_cast<DWORD>(loader-text.base);
        result.setFont=static_cast<DWORD>(setter-text.base);
    } else {
        DWORD slot; memcpy(&slot,method+13,sizeof(slot));
        auto setter=relative(method+23,1,5);
        if (!slot || slot%sizeof(void*) || slot>0x800 || !text.entry(setter)) return false;
        result.loaderSlot=slot;
        result.setFont=static_cast<DWORD>(setter-text.base);
    }
    bool overflow=false;
    text.sections(false,[&](Span span) {
        for (SIZE_T i=24;i+sizeof(void*)<=span.size;i+=sizeof(void*)) {
            if (*reinterpret_cast<BYTE**>(span.data+i)!=handler.data) continue;
            auto table=span.data+i-24;
            if (!text.contains(table,result.loaderSlot+sizeof(void*))) continue;
            auto slots=reinterpret_cast<BYTE**>(table);
            // Leaf AddRef/Release thunks need not have x64 unwind entries.
            if (!text.code(slots[0]) || !text.code(slots[1]) || !text.code(slots[2]) ||
                (result.loaderSlot && !text.entry(slots[result.loaderSlot/sizeof(void*)]))) continue;
            if (result.tableCount==64) { overflow=true; continue; }
            result.tables[result.tableCount++]=static_cast<DWORD>(table-text.base);
        }
    });
    result.getter=static_cast<DWORD>(getter-text.base);
    result.event=static_cast<DWORD>(event-text.base);
    result.handler=static_cast<DWORD>(handler.data-text.base);
    return result.tableCount && !overflow;
}
static bool sourceLayout(const Image& core,BYTE* notify) {
    auto body=core.function(notify);
    if (!body || body.data!=notify) return false;
    // Lock at +0x20, hash buckets/mask/sentinel at +0x60/+0x78/+0x50.
    // Match the instructions that establish the layout before reading any nodes.
    return uniqueIn(body,{0x4c,0x8d,0x71,0x20,0x4c,0x89,0x75,0xb0,0x4d,0x85,0xf6,0x74,0x0a,
        0x49,0x8b,0x06,0x49,0x8b,0xce,0xff,0x50,0x18,0x90}) &&
        uniqueIn(body,{0x41,0x8b,0x45,0,0x48,0x8b,0x4b,0x78,0x48,0x23,0xc8,0x48,0xc1,0xe1,4,
        0x4c,0x8b,0x53,0x60,0x4d,0x8b,0x0c,0x0a,0x49,0x8b,0xc1,0x4c,0x8d,0x59,8,0x4c,0x8b,0x43,0x50}) &&
        uniqueIn(body,{0x48,0x8b,0x50,0x18,0x49,0x33,0x55,8,0x49,0x8b,0x4d,0,0x48,0x33,0x48,0x10,
        0x48,0x0b,0xd1,0x74,5,0x48,0x8b,0,0xeb,-1});
}
