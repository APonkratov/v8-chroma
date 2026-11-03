// Owned source/list fixtures: check bounded traversal and reference ownership.
#include "editor_refresh.h"
#include <cstdlib>
#include <iostream>
#include <functional>

static void require(bool value){if(!value){std::cerr<<"Editor refresh fixture failed\n";std::abort();}}
static unsigned locks,unlocks,retained,released,otherCalls;
static void lockSource(void*){++locks;}
static void unlockSource(void*){++unlocks;}
static void addRef(void*){++retained;}
static void release(void*){++released;}
static void unrelated(void*){++otherCalls;}
template<class T> static void store(unsigned char* p,unsigned offset,T value){memcpy(p+offset,&value,sizeof(value));}

int main(){
    alignas(16) std::array<unsigned char,1024> image{};
    auto start=reinterpret_cast<uintptr_t>(image.data());
    linux_elf::Image core("/nonexistent-v8-chroma-fixture");
    core.base=start;core.unwind=start;
    core.ranges.push_back({start,start+image.size(),PF_R|PF_X});
    for(auto fn:{&lockSource,&unlockSource}){auto p=reinterpret_cast<uintptr_t>(fn);core.ranges.push_back({p,p+1,PF_R|PF_X});}
    memcpy(image.data(),"\x01\x1b\x03\x3b",4);store<uint32_t>(image.data(),8,1);store<int32_t>(image.data(),12,128);
    // Independent instruction fragments establishing the supported list ABI.
    const std::vector<std::vector<unsigned char>> layout={
        {0x4c,0x8d,0x77,0x18,0x48,0x8b,0x47,0x18,0x4c,0x89,0xf7,0xff,0x50,0x18},
        {0x45,0x8b,0x4d,0,0x48,0x8b,0x6b,0x48},
        {0x48,0x8b,0x43,0x40,0x4a,0x8b,0x04,0xc0},
        {0xf3,0x41,0x0f,0x6f,0x45,0,0xf3,0x0f,0x6f,0x49,8},
        {0x48,0x8b,0x7e,0x68},{0x48,0x8b,0x5d,0x28},
        {0x48,0x83,0xc5,0x18,0x48,0x89,0xe8,0x48,0x8b,0x6d,0},
        {0x48,0x8b,0x7d,0x20,0x48,0x89,0x7c,0x24,0x18},
        {0x48,0x8b,0x7c,0x24,0x40,0x48,0x8b,0x07,0xff,0x50,0x20,0x90}};
    std::vector<unsigned> offsets;
    unsigned at=128;
    for(const auto& bytes:layout){offsets.push_back(at);memcpy(image.data()+at,bytes.data(),bytes.size());at+=bytes.size()+8;}
    require(core.function(start+128).begin==start+128);
    require(linux_editor::sourceLayout(core,start+128));
    for(auto offset:offsets){image[offset]^=1;require(!linux_editor::sourceLayout(core,start+128));image[offset]^=1;}
    image[3]=0;require(!core.function(start+128).begin);image[3]=0x3b;
    store<uint32_t>(image.data(),8,0xffffffff);require(!core.function(start+128).begin);store<uint32_t>(image.data(),8,1);

    std::array<uintptr_t,5> mutexTable{0,0,0,reinterpret_cast<uintptr_t>(&lockSource),reinterpret_cast<uintptr_t>(&unlockSource)};
    std::array<uintptr_t,4> sourceTable{0,0,0,start+128};
    std::array<uintptr_t,4> editorTable{0,reinterpret_cast<uintptr_t>(&addRef),reinterpret_cast<uintptr_t>(&release),0};
    std::array<uintptr_t,4> adapterTable{0,reinterpret_cast<uintptr_t>(&unrelated),reinterpret_cast<uintptr_t>(&unrelated),0};
    std::array<unsigned char,512> editor{};store(editor.data(),0,editorTable.data());
    std::array<unsigned char,32> adapter{};store(adapter.data(),0,adapterTable.data());
    std::array<unsigned char,0x70> node{};
    std::array<unsigned char,0x28> first{},second{};
    std::array<unsigned char,0x50> source{};
    std::array<uint32_t,4> guid{0x12345678,2,3,4};
    std::array<uintptr_t,1> before{reinterpret_cast<uintptr_t>(node.data())},buckets{reinterpret_cast<uintptr_t>(before.data())};
    memcpy(node.data()+8,guid.data(),16);
    store(node.data(),0x18,first.data());store<uintptr_t>(node.data(),0x28,2);store<uintptr_t>(node.data(),0x68,guid[0]);
    store(first.data(),0,second.data());store(first.data(),0x20,editor.data());
    store(second.data(),0,node.data()+0x18);store(second.data(),0x20,adapter.data());
    store(source.data(),0,sourceTable.data());store(source.data(),0x18,mutexTable.data());
    store(source.data(),0x40,buckets.data());store<uintptr_t>(source.data(),0x48,1);
    linux_editor::Profile profile;profile.cache=128;profile.tables.push_back(reinterpret_cast<uintptr_t>(editorTable.data()));
    auto check=[&](bool expected,unsigned references){
        locks=unlocks=retained=released=otherCalls=0;
        {linux_editor::Memory memory;linux_editor::Refs refs;
         require(linux_editor::collect(core,profile,reinterpret_cast<uintptr_t>(source.data()),reinterpret_cast<uintptr_t>(guid.data()),refs,memory)==expected);
         require(refs.count==references&&retained==references&&released==0&&locks==1&&unlocks==1&&otherCalls==0);}
        require(retained==released);
    };
    check(true,1); // Only the editor is held; the BSL adapter is untouched.
    store<uintptr_t>(node.data(),0x28,4097);check(false,0);
    store<uintptr_t>(node.data(),0x28,3);check(false,1); // Release references on partial failure.
    store<uintptr_t>(node.data(),0x28,2);
    store(second.data(),0,second.data());check(false,1); // Cyclic recipient list is bounded.
    store(second.data(),0,node.data()+0x18);
    store<uintptr_t>(source.data(),0x48,0);check(false,0);store<uintptr_t>(source.data(),0x48,1);
    store<uintptr_t>(first.data(),0x20,1);check(false,0);store(first.data(),0x20,editor.data());
    guid[1]^=1;check(true,0);guid[1]^=1;
    // An unmatched cyclic hash chain cannot loop forever.
    store(node.data(),0,node.data());guid[1]^=1;check(false,0);guid[1]^=1;store<uintptr_t>(node.data(),0,0);
    std::cout<<"Linux editor refresh: source ABI rejection, malformed unwind/list bounds, retained references and adapter isolation: PASS\n";
}
