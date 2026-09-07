// Run the original Rev 1 save serializer/decoder against the production host
// store. Only SI locking and the EEPROM hardware boundary are fixture adapters.
#include "native_eeprom.hpp"
#include "recomp.h"
#include "funcs.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <zlib.h>

namespace {
using Image=NativeEeprom::Image;
unsigned checks=0,writes=0,reads=0;
bool fail_next_write=false;
NativeEeprom* device=nullptr;
std::vector<uint8_t> ram(0x800000);
gpr addr(uint32_t a){return (gpr)(int64_t)(int32_t)a;}
void require(bool good,const char* reason){if(!good){std::fprintf(stderr,"FAIL: %s\n",reason);std::exit(1);}++checks;}
void put_bytes(uint32_t address,std::span<const uint8_t> data){for(size_t i=0;i<data.size();++i)ram[((address&0x7fffff)+i)^3]=data[i];}
std::vector<uint8_t> bytes(uint32_t address,size_t count){std::vector<uint8_t> out(count);for(size_t i=0;i<count;++i)out[i]=ram[((address&0x7fffff)+i)^3];return out;}
void init(){
    std::fill(ram.begin(),ram.end(),0);
    *reinterpret_cast<uint32_t*>(ram.data()+0x104310)=1; // EEPROM mode
    *reinterpret_cast<uint32_t*>(ram.data()+0x104314)=0x80200000; // original scratch buffer
    *reinterpret_cast<uint32_t*>(ram.data()+0x104318)=200; // record bytes
    *reinterpret_cast<uint32_t*>(ram.data()+0x1042b8)=176; // save body bytes
}
recomp_context context(){recomp_context c{};c.r29=addr(0x807fff00);c.r17=0x12345678;return c;}
void lock_si(uint8_t*,recomp_context*){} // No concurrent SI calls in this probe.
void eeprom_write(uint8_t* rdram,recomp_context* c){
    if(fail_next_write){fail_next_write=false;c->r2=(gpr)-1;return;}
    auto data=bytes(uint32_t(c->r6),uint32_t(c->r7));
    require(device->write((uint32_t(c->r5)&255)*8,data),"production host store commits original record");++writes;c->r2=0;
}
void eeprom_read(uint8_t*,recomp_context* c){
    std::vector<uint8_t> data(uint32_t(c->r7));
    require(device->read((uint32_t(c->r5)&255)*8,data),"production host store reads original record");
    put_bytes(uint32_t(c->r6),data);++reads;c->r2=0;
}
int serialize(unsigned slot,std::span<const uint8_t> metadata,std::span<const uint8_t> body){
    init();put_bytes(0x80210000,metadata);put_bytes(0x80211000,body);auto c=context();
    c.r4=0;c.r5=0;c.r6=slot;c.r7=addr(0x80210000);
    *reinterpret_cast<uint32_t*>(ram.data()+0x7fff10)=0x80211000;
    *reinterpret_cast<uint32_t*>(ram.data()+0x7fff14)=body.size();
    main_80006EC8(ram.data(),&c);require(c.r29==addr(0x807fff00),"serializer restores stack");return int32_t(c.r2);
}
int deserialize(unsigned slot){
    init();std::vector<uint8_t> sentinel(176,0xcd);put_bytes(0x80220000,sentinel);auto c=context();
    c.r4=0;c.r5=0;c.r6=slot;c.r7=addr(0x80220000);
    *reinterpret_cast<uint32_t*>(ram.data()+0x7fff10)=176;
    main_80007064(ram.data(),&c);require(c.r29==addr(0x807fff00),"decoder restores stack");return int32_t(c.r2);
}
uint32_t word(const Image& data,size_t offset){return uint32_t(data[offset])<<24|uint32_t(data[offset+1])<<16|uint32_t(data[offset+2])<<8|data[offset+3];}
void write_file(const std::filesystem::path& path,const Image& data){std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(data.data()),data.size());require(bool(out),"codec fixture writes");}
}
extern "C" void* rs_checked_memory(uint8_t* data,uint32_t address,unsigned size,unsigned swap){
    uint32_t offset=(address&0x1fffffff)^swap;
    if((address&0xff800000)!=0x80000000 || offset>0x800000-size || (offset&(size-1)))std::abort();
    return data+offset;
}
extern "C" recomp_func_t* get_function(int32_t address){switch(uint32_t(address)){
    case 0x800039b4:case 0x80003a10:return lock_si;
    case 0x80007ff0:return main_80007FF0;
    case 0x8002bcfc:return main_8002BCFC;
    case 0x80020b80:return main_80020B80;
    case 0x80024320:return eeprom_write;
    case 0x800242b0:return eeprom_read;
    default:std::fprintf(stderr,"Unexpected save-codec dependency %08x\n",uint32_t(address));std::abort();
}}
extern "C" void switch_error(const char*,uint32_t,uint32_t){std::abort();}
extern "C" void do_break(uint32_t){std::abort();}

int main(int argc,char** argv){
    require(argc==3,"codec fixture and disposable directory supplied");
    Image fixture;std::ifstream in(argv[1],std::ios::binary);in.read(reinterpret_cast<char*>(fixture.data()),fixture.size());
    require(bool(in)&&NativeEeprom::complete_checksums(fixture),"codec uses a valid original save fixture");
    auto folder=std::filesystem::path(argv[2]);std::filesystem::create_directories(folder);
    auto primary=folder/"rogue-squadron.eep";write_file(primary,fixture);
    NativeEeprom store;require(store.load(folder),"codec host store loads");device=&store;
    std::vector<uint8_t> metadata(fixture.begin()+0x28,fixture.begin()+0x38);
    std::vector<uint8_t> body(fixture.begin()+0x38,fixture.begin()+0xe8);
    // Synthetic body variants exercise every saved byte; this does not complete
    // a mission or award progress to the user's actual pilot.
    for(unsigned slot=0;slot<2;++slot){
        auto variant=body;for(size_t i=0;i<variant.size();++i)variant[i]^=uint8_t(i+slot*71);
        metadata[15]=uint8_t(slot+1);
        device=&store;require(serialize(slot,metadata,variant)==0,"original serializer reports success after a committed write");
        auto image=store.snapshot();size_t offset=32+slot*200;
        require(word(image,offset)==adler32(1,variant.data(),variant.size()),"original body checksum matches independent zlib");
        require(word(image,offset+4)==adler32(1,metadata.data(),metadata.size()),"original metadata checksum matches independent zlib");
        require(std::equal(variant.begin(),variant.end(),image.begin()+offset+24),"all original save-body bytes reach disk image");
        NativeEeprom restart;require(restart.load(folder),"new host instance loads committed original record");device=&restart;
        require(deserialize(slot)==0 && bytes(0x80220000,176)==variant,"original decoder restores all bytes after reload");
        for(size_t corrupt:{offset,offset+4}){
            auto damaged=image;damaged[corrupt]^=1;write_file(primary,damaged);
            NativeEeprom bad;require(bad.load(folder),"full-size damaged record reaches original decoder");device=&bad;
            require(deserialize(slot)==4,"original decoder rejects damaged record checksum");
            require(bytes(0x80220000,176)==std::vector<uint8_t>(176,0xcd),"invalid record cannot overwrite destination data");
        }
        write_file(primary,image);
    }
    device=&store;auto before=store.snapshot();fail_next_write=true;
    require(serialize(0,metadata,body)==0,"original serializer demonstrably ignores low-level write error");
    require(store.snapshot()==before,"injected write error does not commit data");
    std::printf("{\"passed\":true,\"checks\":%u,\"original_serializer\":\"0x80006ec8\",\"original_decoder\":\"0x80007064\",\"record_copies_verified\":2,\"original_writes\":%u,\"original_reads\":%u,\"original_serializer_ignores_write_error\":true,\"campaign_progression_verified\":false}\n",checks,writes,reads);
}
