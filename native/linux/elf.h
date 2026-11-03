#pragma once
#include <link.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

namespace linux_elf {
struct Range { uintptr_t begin, end; unsigned flags; };
struct Image {
    uintptr_t base = 0;
    std::vector<Range> ranges;
    const Elf64_Dyn* dynamic = nullptr;
    uintptr_t unwind = 0;
    explicit Image(const char* path) {
        struct Context { Image* image; struct stat identity; } context{this,{}};
        if (stat(path, &context.identity)) return;
        dl_iterate_phdr([](dl_phdr_info* module, size_t, void* value) {
            auto& c = *static_cast<Context*>(value);
            struct stat actual{};
            if (stat(module->dlpi_name, &actual) || actual.st_dev != c.identity.st_dev || actual.st_ino != c.identity.st_ino) return 0;
            c.image->base = module->dlpi_addr;
            for (unsigned i = 0; i < module->dlpi_phnum; ++i) {
                const auto& p = module->dlpi_phdr[i];
                if (p.p_type == PT_LOAD) c.image->ranges.push_back({module->dlpi_addr+p.p_vaddr,module->dlpi_addr+p.p_vaddr+p.p_memsz,p.p_flags});
                if (p.p_type == PT_DYNAMIC) c.image->dynamic = reinterpret_cast<const Elf64_Dyn*>(module->dlpi_addr+p.p_vaddr);
                if (p.p_type == PT_GNU_EH_FRAME) c.image->unwind = module->dlpi_addr+p.p_vaddr;
            }
            return 1;
        }, &context);
    }
    bool contains(uintptr_t address, size_t bytes, unsigned flags = PF_R) const {
        for (const auto& r : ranges) if ((r.flags & flags) == flags && address >= r.begin && address < r.end && bytes <= r.end-address) return true;
        return false;
    }
    uintptr_t pointer(uintptr_t value) const { return contains(value,1) ? value : base+value; }
    Range function(uintptr_t address) const {
        // GNU .eh_frame_hdr: version 1, pcrel sdata4 frame pointer,
        // udata4 count, datarel sdata4 binary-search table.
        if(!contains(unwind,12))return {};
        auto p=reinterpret_cast<const unsigned char*>(unwind);
        if(memcmp(p,"\x01\x1b\x03\x3b",4))return {};
        uint32_t count;memcpy(&count,p+8,4);
        if(!count||count>1048576||!contains(unwind+12,size_t(count)*8))return {};
        auto entry=[&](unsigned i){int32_t offset;memcpy(&offset,p+12+size_t(i)*8,4);return unwind+offset;};
        unsigned lo=0,hi=count;
        while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(entry(mid)<=address)lo=mid+1;else hi=mid;}
        if(!lo)return {};
        uintptr_t begin=entry(lo-1),end=lo<count?entry(lo):0;
        if(!end)for(const auto& r:ranges)if((r.flags&PF_X)&&begin>=r.begin&&begin<r.end)end=r.end;
        if(end<=begin||!contains(begin,end-begin,PF_R|PF_X)||address>=end)return {};
        return {begin,end,PF_R|PF_X};
    }
    template<class Visit> void relocations(Visit visit) const {
        if (!dynamic) return;
        uintptr_t symbols=0, strings=0, relocations=0; size_t size=0;
        for (unsigned i=0;i<4096 && contains(reinterpret_cast<uintptr_t>(dynamic+i),sizeof(*dynamic));++i) {
            const auto& d=dynamic[i]; if (d.d_tag==DT_NULL) break;
            if (d.d_tag==DT_SYMTAB) symbols=pointer(d.d_un.d_ptr);
            if (d.d_tag==DT_STRTAB) strings=pointer(d.d_un.d_ptr);
            if (d.d_tag==DT_JMPREL) relocations=pointer(d.d_un.d_ptr);
            if (d.d_tag==DT_PLTRELSZ) size=d.d_un.d_val;
        }
        if (!symbols || !strings || size>8*1024*1024 || !contains(relocations,size)) return;
        auto entries=reinterpret_cast<const Elf64_Rela*>(relocations);
        for (size_t i=0;i<size/sizeof(*entries);++i) {
            const auto& r=entries[i]; if (ELF64_R_TYPE(r.r_info)!=R_X86_64_JUMP_SLOT) continue;
            auto symbol=symbols+ELF64_R_SYM(r.r_info)*sizeof(Elf64_Sym);
            if (!contains(symbol,sizeof(Elf64_Sym))) continue;
            auto name=strings+reinterpret_cast<const Elf64_Sym*>(symbol)->st_name;
            auto slot=base+r.r_offset;
            if (contains(name,256) && memchr(reinterpret_cast<const void*>(name),0,256) && contains(slot,8))
                visit(reinterpret_cast<const char*>(name),reinterpret_cast<uintptr_t*>(slot));
        }
    }
};
inline int protection(uintptr_t address) {
    FILE* maps=fopen("/proc/self/maps","r"); if (!maps) return 0;
    char line[1024],access[8]; unsigned long begin,end; int result=0;
    while (fgets(line,sizeof(line),maps)) if (sscanf(line,"%lx-%lx %7s",&begin,&end,access)==3 && address>=begin && address<end) {
        result=(access[0]=='r'?PROT_READ:0)|(access[1]=='w'?PROT_WRITE:0)|(access[2]=='x'?PROT_EXEC:0); break;
    }
    fclose(maps); return result;
}
inline bool replace(uintptr_t* slot, uintptr_t original, uintptr_t hook) {
    const auto size=static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    auto page=reinterpret_cast<uintptr_t>(slot)&~(size-1);
    int previous=protection(reinterpret_cast<uintptr_t>(slot));
    if (!(previous&PROT_READ) || (previous&PROT_EXEC) || mprotect(reinterpret_cast<void*>(page),size,previous|PROT_WRITE)) return false;
    bool changed=__atomic_compare_exchange_n(slot,&original,hook,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    bool restored=!mprotect(reinterpret_cast<void*>(page),size,previous);
    if (!restored && changed) __atomic_compare_exchange_n(slot,&hook,original,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    return changed && restored;
}
}
