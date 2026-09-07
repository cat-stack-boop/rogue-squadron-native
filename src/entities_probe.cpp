#include "native_entities.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <set>
int main(int argc,char** argv){
    if(argc!=2){std::fprintf(stderr,"Usage: rogue_entities_probe <diagnostic-rdram.bin>\n");return 2;}
    std::ifstream in(argv[1],std::ios::binary);
    std::vector<uint8_t> ram((std::istreambuf_iterator<char>(in)),{});
    if(ram.size()!=0x800000)return 2;
    unsigned checks=0;auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("entity probe failed");};
    auto entities=inspect_rogue_entities(ram.data());check(entities.registry_valid);
    std::set<uint32_t> unique;bool turret=false,tie=false,wingman=false;
    for(auto& e:entities.entities){
        check(unique.insert(e.object).second);
        if(e.name=="Gun_Turret_05"){check(e.kind==13&&e.health==60);check(std::abs(e.position[2]-145.901428f)<1e-4f);turret=true;}
        if(e.name=="Backward_TIE_01"){check(e.kind==12&&e.health==10);tie=true;}
        if(e.name=="Wingmen_02"){check(e.kind==39&&e.health==350);wingman=true;}
    }
    check(turret&&tie&&wingman);
    uint32_t invalid=0xfffffffcu;std::memcpy(ram.data()+0x121780,&invalid,4);
    check(!inspect_rogue_entities(ram.data()).registry_valid);
    std::memcpy(ram.data()+0x121780,&entities.table,4);
    for(unsigned i=0;i<2048;++i)std::memcpy(ram.data()+(entities.table&0x1fffffff)+8*i,&invalid,4);
    auto broken=inspect_rogue_entities(ram.data());check(broken.registry_valid&&broken.entities.empty());
    std::printf("{\"passed\":true,\"checks\":%u,\"named_entities\":%zu}\n",checks,entities.entities.size());
}
