#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

struct RogueEntity {
    uint16_t handle=0,flags=0,kind=0;
    uint32_t object=0,callback=0,payload=0,definition=0;
    std::array<float,3> position{};
    std::string name;
    std::optional<int32_t> health;
};
struct RogueEntities {
    bool registry_valid=false;
    uint32_t table=0,health_index=0;
    std::vector<RogueEntity> entities;
};

// Observational reader for the Revision 1 game-thread snapshot. Addresses are
// validated before reads; this helper never invokes game callbacks or writes RAM.
inline RogueEntities inspect_rogue_entities(const uint8_t* ram){
    auto valid=[](uint32_t a,uint32_t n){return a>=0x80000000u && a<=0x80800000u && n<=0x80800000u-a;};
    auto u32=[&](uint32_t a){uint32_t x=0;if(valid(a,4)&&!(a&3))std::memcpy(&x,ram+(a&0x1fffffff),4);return x;};
    auto u16=[&](uint32_t a){uint16_t x=0;if(valid(a,2)&&!(a&1))std::memcpy(&x,ram+((a&0x1fffffff)^2),2);return x;};
    auto u8=[&](uint32_t a){return valid(a,1)?ram[(a&0x1fffffff)^3]:uint8_t(0);};
    auto number=[&](uint32_t a){uint32_t bits=u32(a);float x;std::memcpy(&x,&bits,4);return x;};
    RogueEntities result;result.table=u32(0x80121780);result.health_index=u32(0x801288b4);
    // Original 80035930 allocates 0x4000 bytes and initializes 0x800 eight-byte
    // membership entries. +4/+6 are next/previous links, not generation fields.
    if(!valid(result.table,0x4000)||(result.table&3))return result;
    result.registry_valid=true;std::unordered_set<uint32_t> seen;
    for(unsigned i=0;i<0x800;++i){
        uint32_t object=u32(result.table+8*i);
        if(!valid(object,0x3c)||(object&3)||!seen.insert(object).second)continue;
        RogueEntity e;e.object=object;e.callback=u32(object);e.payload=u32(object+4);
        e.handle=u16(object+0x16);e.flags=u16(object+0x14);
        if(e.callback<0x80000400||e.callback>=0x800fd2c0||(e.flags&8))continue;
        uint32_t position=u32(object+8);
        if(!valid(position,12)||(position&3))continue;
        bool finite=true;
        for(unsigned j=0;j<3;++j){e.position[j]=number(position+4*j);finite&=std::isfinite(e.position[j])&&std::abs(e.position[j])<1e6f;}
        if(!finite||!valid(e.payload,0x3c)||(e.payload&3))continue;
        e.definition=u32(e.payload+0x34);
        if(!valid(e.definition,0x1c)||(e.definition&3))continue;
        e.kind=u16(e.definition);uint32_t size=u32(e.definition+8),name=u32(e.definition+12);
        if(!e.kind||e.kind>255||size<0x1c||size>4096||!valid(e.definition,size)||!valid(name,1))continue;
        bool terminated=false;
        for(unsigned j=0;j<128&&valid(name+j,1);++j){
            unsigned ch=u8(name+j);if(!ch){terminated=true;break;}
            if(ch<32||ch>126)break;e.name.push_back(char(ch));
        }
        if(!terminated||e.name.empty())continue;
        // These three callbacks call the original health getter 800e3bd8 on
        // payload+0x38. Keep health unknown for other classes until validated.
        if(e.callback==0x800ccb04||e.callback==0x800cad9c||e.callback==0x800ba850){
            uint32_t meta=u32(e.payload+0x38);
            if(valid(meta,0x1a8)&&!(meta&3)){
                uint32_t address=meta+0x190;
                if(!u8(meta+0x1a7)){
                    uint32_t base=u32(address);
                    if(result.health_index<256&&valid(base,4*(result.health_index+1)))address=base+4*result.health_index;
                    else address=0;
                }
                if(valid(address,4)&&!(address&3)){int32_t hp;uint32_t bits=u32(address);std::memcpy(&hp,&bits,4);e.health=hp;}
            }
        }
        result.entities.push_back(e);
    }
    return result;
}
