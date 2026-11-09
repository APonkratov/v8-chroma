#pragma once
#include <windows.h>
#include <cstring>
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
