#include "native_eeprom.hpp"
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs=std::filesystem;
using Image=NativeEeprom::Image;
static unsigned checks=0;
static void require(bool good,const char* why){
    if(!good){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}++checks;
}
static std::vector<uint8_t> read_file(const fs::path& path){
    std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
static void put(const fs::path& path,std::span<const uint8_t> bytes){
    fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());require(bool(out),"fixture file is written");
}
static bool same(const fs::path& path,const Image& image){return read_file(path)==std::vector<uint8_t>(image.begin(),image.end());}
int main(int argc,char** argv){
    require(argc==3,"fixture and disposable directory arguments supplied");
    auto fixture_bytes=read_file(argv[1]);require(fixture_bytes.size()==512,"original save fixture is 512 bytes");
    Image fixture;std::copy(fixture_bytes.begin(),fixture_bytes.end(),fixture.begin());
    require(NativeEeprom::complete_checksums(fixture),"native checksum gate accepts independently verified original save");
    for(size_t offset:{0u,16u,0x24u,0x20u,0xecu,0xe8u}){
        auto damaged=fixture;damaged[offset]^=1;
        require(!NativeEeprom::complete_checksums(damaged),"each of the six backup checksums is enforced");
    }
    fs::path root=argv[2];fs::create_directories(root);
    const auto primary="rogue-squadron.eep",backup="rogue-squadron.previous.eep";
    std::array<uint8_t,8> patch{1,2,3,4,5,6,7,8},other{8,7,6,5,4,3,2,1};
    {
        auto dir=root/"normal";put(dir/primary,fixture);NativeEeprom store;
        require(store.load(dir) && store.load_source()==NativeEeprom::LoadSource::primary,"existing primary loads");
        require(store.snapshot()==fixture,"loading does not rewrite cartridge data");
        std::array<uint8_t,8> read{};
        require(store.read(504,read) && std::equal(read.begin(),read.end(),fixture.begin()+504),"last EEPROM block reads correctly");
        require(!store.read(508,read) && !store.write(508,patch),"out-of-range transfers are rejected");
        require(store.write(432,patch),"first cartridge block write commits");
        auto after=fixture;std::copy(patch.begin(),patch.end(),after.begin()+432);
        require(same(dir/primary,after) && store.snapshot()==after,"disk and EEPROM memory agree after write");
        require(same(dir/backup,fixture),"first write checkpoints original valid launch image");
        require(store.write(440,other),"second cartridge block write commits");
        require(same(dir/backup,fixture),"backup is not replaced by an intermediate transaction");
        NativeEeprom restarted;require(restarted.load(dir) && restarted.snapshot()==store.snapshot(),"written bytes survive a new store instance");

        // A kernel-enforced short write fails without changing the primary or
        // the in-memory EEPROM. This is an actual filesystem failure, not a mock.
        auto before=store.snapshot();struct rlimit limits{};require(getrlimit(RLIMIT_FSIZE,&limits)==0,"read file-size limits");
        auto limited=limits;limited.rlim_cur=128;
        auto handler=std::signal(SIGXFSZ,SIG_IGN);require(setrlimit(RLIMIT_FSIZE,&limited)==0,"inject short write limit");
        bool written=store.write(448,patch);
        int restored=setrlimit(RLIMIT_FSIZE,&limits);std::signal(SIGXFSZ,handler);
        require(restored==0,"restore file-size limits");
        require(!written && !store.error().empty(),"failed write reports an error");
        require(same(dir/primary,before) && store.snapshot()==before,"failed write preserves primary and memory");
        require(same(dir/backup,fixture),"failed write preserves backup");

        // Kill a child inside an incomplete temporary-file write. Atomic rename
        // must leave the previous committed image usable on restart.
        pid_t child=fork();require(child>=0,"fork interrupted-write child");
        if(child==0){
            struct rlimit core{0,0};setrlimit(RLIMIT_CORE,&core);std::signal(SIGXFSZ,SIG_DFL);
            if(setrlimit(RLIMIT_FSIZE,&limited)!=0)std::_Exit(3);
            store.write(448,patch);std::_Exit(4);
        }
        int status=0;require(waitpid(child,&status,0)==child,"interrupted writer is reaped");
        require(WIFSIGNALED(status)&&WTERMSIG(status)==SIGXFSZ,"kernel interrupted writer during temporary-file creation");
        bool partial=false;
        for(const auto& file:fs::directory_iterator(dir))
            if(file.path().filename().string().find(".tmp.")!=std::string::npos && file.file_size()==128)partial=true;
        require(partial,"interruption left a partially written temporary file");
        require(same(dir/primary,before) && same(dir/backup,fixture),"interruption preserves both committed files");
        NativeEeprom after_crash;require(after_crash.load(dir) && after_crash.snapshot()==before,"restart ignores partial orphan temporary file");
    }
    {
        auto dir=root/"truncated";put(dir/primary,std::span(fixture).first(111));put(dir/backup,fixture);NativeEeprom store;
        require(store.load(dir) && store.load_source()==NativeEeprom::LoadSource::backup,"truncated primary recovers from validated backup");
        require(store.snapshot()==fixture && same(dir/primary,fixture),"recovered data is restored to primary file");
        require(read_file(store.preserved_file())==std::vector<uint8_t>(fixture.begin(),fixture.begin()+111),"damaged original bytes are preserved separately");
        require(store.write(432,patch) && same(dir/backup,fixture),"recovery does not overwrite backup on first write");
    }
    {
        auto dir=root/"missing";put(dir/backup,fixture);NativeEeprom store;
        require(store.load(dir) && store.load_source()==NativeEeprom::LoadSource::backup,"missing primary recovers from valid backup");
        require(store.preserved_file().empty() && same(dir/primary,fixture),"missing-file recovery creates no fake damaged file");
    }
    {
        auto dir=root/"new";NativeEeprom store;
        require(store.load(dir) && store.load_source()==NativeEeprom::LoadSource::blank,"first launch exposes blank cartridge EEPROM");
        auto blank=store.snapshot();require(std::all_of(blank.begin(),blank.end(),[](auto b){return b==0xff;}),"new EEPROM starts erased");
        require(store.write(0,patch),"original game's initial write creates a save");
        require(!fs::exists(dir/backup),"erased data is not installed as a recovery backup");
    }
    {
        auto dir=root/"no-recovery";put(dir/primary,std::span(fixture).first(19));auto invalid=fixture;invalid[0]^=1;put(dir/backup,invalid);NativeEeprom store;
        require(!store.load(dir),"truncated primary plus invalid backup is rejected");
        require(read_file(dir/primary)==std::vector<uint8_t>(fixture.begin(),fixture.begin()+19) && same(dir/backup,invalid),"failed recovery preserves all user data");
    }
    {
        auto dir=root/"cartridge-repair";auto partial=fixture;partial[0x38]^=1;put(dir/primary,partial);put(dir/backup,fixture);NativeEeprom store;
        require(store.load(dir) && store.load_source()==NativeEeprom::LoadSource::primary,"full-size damaged record is left to original cartridge recovery");
        require(store.snapshot()==partial,"host does not silently roll back a redundant cartridge record");
        require(store.write(432,patch) && same(dir/backup,fixture),"damaged launch image cannot overwrite valid backup");
    }
    std::printf("{\"passed\":true,\"checks\":%u,\"interrupted_temporary_bytes\":128,\"fixture\":\"original independently verified EEPROM\",\"campaign_progression_verified\":false}\n",checks);
}
