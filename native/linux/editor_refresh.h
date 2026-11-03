// Discover and call the editor's font-only path. A text-settings broadcast also
// reaches the BSL document adapter and resets folds/selection/Undo. Never use it.
#pragma once
#include "elf.h"
#include <array>
#include <algorithm>
#include <dlfcn.h>

namespace linux_editor {
struct Pattern {
    std::vector<int> bytes;
    Pattern(std::initializer_list<int> value):bytes(value){}
    bool at(const linux_elf::Image& image,uintptr_t address) const {
        if(!image.contains(address,bytes.size(),PF_R|PF_X))return false;
        auto p=reinterpret_cast<const unsigned char*>(address);
        for(size_t i=0;i<bytes.size();++i)if(bytes[i]>=0&&p[i]!=bytes[i])return false;
        return true;
    }
    uintptr_t unique(const linux_elf::Image& image,linux_elf::Range range) const {
        uintptr_t result=0;
        if(range.end<=range.begin||range.end-range.begin>65536)return 0;
        for(auto p=range.begin;p+bytes.size()<=range.end;++p)if(at(image,p)){if(result)return 0;result=p;}
        return result;
    }
};
inline uintptr_t relative(uintptr_t address,unsigned offset,unsigned size){int32_t displacement;memcpy(&displacement,reinterpret_cast<void*>(address+offset),4);return address+size+displacement;}
inline uint32_t number(uintptr_t address){uint32_t value;memcpy(&value,reinterpret_cast<void*>(address),4);return value;}
struct Profile {
    uintptr_t construct=0,load=0,assign=0,set=0;
    unsigned transform=0,cache=0;
    std::vector<uintptr_t> tables;
};
inline bool entry(const linux_elf::Image& image,uintptr_t address){return image.function(address).begin==address;}
inline bool importedCall(const linux_elf::Image& image,uintptr_t call,uintptr_t expected) {
    if(!expected||!image.contains(call,5,PF_R|PF_X)||*reinterpret_cast<unsigned char*>(call)!=0xe8)return false;
    auto target=relative(call,1,5);
    if(target==expected)return true;
    if(!image.contains(target,6,PF_R|PF_X)||memcmp(reinterpret_cast<void*>(target),"\xff\x25",2))return false;
    auto slot=relative(target,2,6);
    return image.contains(slot,8)&&__atomic_load_n(reinterpret_cast<uintptr_t*>(slot),__ATOMIC_ACQUIRE)==expected;
}
inline bool discover(const linux_elf::Image& text,const linux_elf::Image& core,uintptr_t event,Profile& result) {
    void* handle=dlopen(([] (uintptr_t address){Dl_info info{};return dladdr(reinterpret_cast<void*>(address),&info)?info.dli_fname:"";})(core.base+1),RTLD_NOW|RTLD_NOLOAD);
    if(!handle)return false;
    Profile next;
    next.construct=reinterpret_cast<uintptr_t>(dlsym(handle,"_ZN4core4FontC1Ev"));
    next.assign=reinterpret_cast<uintptr_t>(dlsym(handle,"_ZN4core4FontaSERKS0_"));
    dlclose(handle);
    if(!next.construct||!next.assign||!entry(core,next.construct)||!entry(core,next.assign))return false;
    Pattern head{0x55,0x41,0x56,0x53,0x48,0x83,0xec,0x10,0x48,0x89,0xfb,0xf3,0x0f,0x6f,0x06,0x48,0x8d,0x05,-1,-1,-1,-1,
        0xf3,0x0f,0x6f,0x08,0x66,0x0f,0x74,0xc8,0x66,0x0f,0xd7,0xc1,0x3d,0xff,0xff,0,0,0x0f,0x84,-1,-1,-1,-1};
    uintptr_t handler=0;
    for(const auto& range:text.ranges)if(range.flags&PF_X)for(auto p=range.begin;p+head.bytes.size()<=range.end;++p) {
        if(!head.at(text,p)||relative(p+15,3,7)!=event||!entry(text,p))continue;
        auto branch=relative(p+39,2,6);
        auto body=text.function(p);
        if(branch<body.begin||branch+12>body.end||!Pattern{0x48,0x89,0xdf,0xe8,-1,-1,-1,-1,0x48,0x89,0xdf,0xbe,1,0,0,0}.at(text,branch))continue;
        auto settings=relative(branch+3,1,5);
        if(!entry(text,settings))continue;
        Pattern chain{0x48,0x89,0xdf,0xe8,-1,-1,-1,-1,0x48,0x89,0xde,0xe8,-1,-1,-1,-1,
            0x48,0x8d,0x5c,0x24,-1,0x48,0x89,0xdf,0xe8,-1,-1,-1,-1,
            0x49,0x8b,0x04,0x24,0x48,0x8d,0xb4,0x24,-1,-1,-1,-1,0x4c,0x89,0xe7,0x48,0x89,0xda,
            0xff,0x90,-1,-1,-1,-1,0x49,0x8d,0x9c,0x24,-1,-1,-1,-1,
            0x48,0x8d,0x74,0x24,-1,0x48,0x89,0xdf,0xe8,-1,-1,-1,-1,
            0x4c,0x89,0xe7,0x48,0x89,0xde,0xe8,-1,-1,-1,-1};
        auto font=chain.unique(text,text.function(settings));
        if(!font||!importedCall(text,font+3,next.construct)||!importedCall(text,font+24,next.construct)||!importedCall(text,font+69,next.assign))continue;
        next.load=relative(font+11,1,5);next.set=relative(font+80,1,5);
        next.transform=number(font+49);next.cache=number(font+57);
        if(!entry(text,next.load)||!entry(text,next.set)||!next.transform||next.transform%8||next.transform>2048||next.cache<8||next.cache>65536)continue;
        if(!Pattern{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,-1,-1,-1,-1,0x49,0x89,0xf6,0xe8}.at(text,next.load))continue;
        if(!Pattern{0x53,0x48,0x83,0xec,0x20,0x48,0x89,0xfb,0x83,0x3e,3}.at(text,next.set))continue;
        if(handler)return false;
        handler=p;
    }
    if(!handler)return false;
    for(const auto& range:text.ranges)if((range.flags&PF_R)&&!(range.flags&PF_X)) {
        for(auto p=(range.begin+7)&~uintptr_t(7);p+32<=range.end;p+=8) {
            if(*reinterpret_cast<const uintptr_t*>(p+24)!=handler)continue;
            auto table=reinterpret_cast<const uintptr_t*>(p);
            if(!text.contains(p,next.transform+8))continue;
            if(!text.contains(table[0],1,PF_R|PF_X)||!text.contains(table[1],1,PF_R|PF_X)||!text.contains(table[2],1,PF_R|PF_X)||!entry(text,table[next.transform/8]))continue;
            if(next.tables.size()==64)return false;
            next.tables.push_back(p);
        }
    }
    if(next.tables.empty())return false;
    result=std::move(next);return true;
}
struct Memory {
    struct Span{uintptr_t begin,end;};std::vector<Span> spans;
    Memory(){
        FILE* maps=fopen("/proc/self/maps","r");if(!maps)return;
        char line[1024],access[8];unsigned long begin,end;
        while(fgets(line,sizeof(line),maps))if(sscanf(line,"%lx-%lx %7s",&begin,&end,access)==3&&access[0]=='r')spans.push_back({begin,end});
        fclose(maps);
    }
    bool readable(uintptr_t p,size_t size)const{for(const auto& s:spans)if(p>=s.begin&&p<s.end&&size<=s.end-p)return true;return false;}
};
inline bool sourceLayout(const linux_elf::Image& core,uintptr_t method) {
    auto body=core.function(method);if(body.begin!=method)return false;
    return Pattern{0x4c,0x8d,0x77,0x18,0x48,0x8b,0x47,0x18,0x4c,0x89,0xf7,0xff,0x50,0x18}.unique(core,body)&&
        Pattern{0x45,0x8b,0x4d,0,0x48,0x8b,0x6b,0x48}.unique(core,body)&&
        Pattern{0x48,0x8b,0x43,0x40,0x4a,0x8b,0x04,0xc0}.unique(core,body)&&
        Pattern{0xf3,0x41,0x0f,0x6f,0x45,0,0xf3,0x0f,0x6f,0x49,8}.unique(core,body)&&
        Pattern{0x48,0x8b,0x7e,0x68}.unique(core,body)&&
        Pattern{0x48,0x8b,0x5d,0x28}.unique(core,body)&&
        Pattern{0x48,0x83,0xc5,0x18,0x48,0x89,0xe8,0x48,0x8b,0x6d,0}.unique(core,body)&&
        Pattern{0x48,0x8b,0x7d,0x20,0x48,0x89,0x7c,0x24,0x18}.unique(core,body)&&
        Pattern{0x48,0x8b,0x7c,0x24,0x40,0x48,0x8b,0x07,0xff,0x50,0x20,0x90}.unique(core,body);
}
struct Refs {
    std::array<void*,256> items{};unsigned count=0;
    ~Refs(){for(unsigned i=0;i<count;++i){auto table=*static_cast<uintptr_t**>(items[i]);reinterpret_cast<void(*)(void*)>(table[2])(items[i]);}}
};
inline bool collect(const linux_elf::Image& core,const Profile& profile,uintptr_t source,uintptr_t event,Refs& refs,const Memory& memory) {
    auto read=[](uintptr_t p){return *reinterpret_cast<const uintptr_t*>(p);};
    if(!memory.readable(source,0x50)||!memory.readable(read(source),32)||!sourceLayout(core,read(read(source)+24)))return false;
    auto mutex=source+0x18,table=read(mutex);
    if(!memory.readable(table,40)||!core.contains(read(table+24),1,PF_R|PF_X)||!core.contains(read(table+32),1,PF_R|PF_X))return false;
    reinterpret_cast<void(*)(void*)>(read(table+24))(reinterpret_cast<void*>(mutex));
    struct Unlock{uintptr_t mutex,method;~Unlock(){reinterpret_cast<void(*)(void*)>(method)(reinterpret_cast<void*>(mutex));}}unlock{mutex,read(table+32)};
    auto buckets=read(source+0x40),count=read(source+0x48);
    if(!count||count>1048576||!memory.readable(buckets,count*8))return false;
    auto bucket=number(event)%count,previous=read(buckets+bucket*8);
    if(!previous)return true;
    if(!memory.readable(previous,8))return false;
    unsigned visited=0;
    for(auto node=read(previous);node;node=read(node)) {
        if(++visited>4096||!memory.readable(node,0x70))return false;
        if(read(node+0x68)%count!=bucket)return true;
        if(memcmp(reinterpret_cast<void*>(node+8),reinterpret_cast<void*>(event),16))continue;
        auto head=node+0x18,expected=read(node+0x28);unsigned recipients=0;
        if(expected>4096)return false;
        for(auto item=read(head);item!=head;item=read(item)) {
            if(++recipients>expected||!memory.readable(item,0x28))return false;
            auto recipient=read(item+0x20);
            if(!memory.readable(recipient,8))return false;
            auto vt=read(recipient);
            if(std::find(profile.tables.begin(),profile.tables.end(),vt)==profile.tables.end())continue;
            auto value=reinterpret_cast<void*>(recipient);
            if(std::find(refs.items.begin(),refs.items.begin()+refs.count,value)!=refs.items.begin()+refs.count)continue;
            if(refs.count==refs.items.size()||!memory.readable(recipient+profile.cache,176))return false;
            reinterpret_cast<void(*)(void*)>(read(vt+8))(value);refs.items[refs.count++]=value;
        }
        return recipients==expected;
    }
    return true;
}
inline bool refresh(const linux_elf::Image& text,const linux_elf::Image& core,void* source,const void* event,unsigned& updated) {
    Profile profile;
    if(!discover(text,core,reinterpret_cast<uintptr_t>(event),profile))return false;
    Memory memory;Refs refs;
    if(!collect(core,profile,reinterpret_cast<uintptr_t>(source),reinterpret_cast<uintptr_t>(event),refs,memory))return false;
    for(unsigned i=0;i<refs.count;++i) {
        alignas(16) unsigned char native[1024]{},scaled[1024]{};
        auto object=refs.items[i];
        auto table=*static_cast<uintptr_t**>(object);
        reinterpret_cast<void(*)(void*)>(profile.construct)(native);
        reinterpret_cast<void(*)(void*,void*)>(profile.load)(nullptr,native);
        reinterpret_cast<void(*)(void*)>(profile.construct)(scaled);
        reinterpret_cast<void(*)(void*,const void*,void*)>(table[profile.transform/8])(object,native,scaled);
        auto cache=static_cast<unsigned char*>(object)+profile.cache;
        reinterpret_cast<void(*)(void*,const void*)>(profile.assign)(cache,scaled);
        reinterpret_cast<void(*)(void*,const void*)>(profile.set)(object,cache);
        ++updated;
    }
    return true;
}
}
