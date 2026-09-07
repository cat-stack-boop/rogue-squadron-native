#include "native_eeprom.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
enum class ReadResult {missing,complete,wrong_size,error};
ReadResult read_image(const std::filesystem::path& path,NativeEeprom::Image& image,std::string& error){
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0){
        if(errno==ENOENT)return ReadResult::missing;
        error="Cannot read "+path.filename().string()+": "+std::strerror(errno);return ReadResult::error;
    }
    struct stat info{};
    if(fstat(fd,&info)!=0 || !S_ISREG(info.st_mode)){
        error="Save is not a readable regular file: "+path.filename().string();close(fd);return ReadResult::error;
    }
    if(info.st_size!=off_t(image.size())){close(fd);return ReadResult::wrong_size;}
    size_t offset=0;
    while(offset<image.size()){
        auto n=::read(fd,image.data()+offset,image.size()-offset);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){error="Cannot read complete save: "+path.filename().string();close(fd);return ReadResult::error;}
        offset+=size_t(n);
    }
    close(fd);return ReadResult::complete;
}
uint32_t big_endian(const NativeEeprom::Image& data,size_t offset){
    return uint32_t(data[offset])<<24|uint32_t(data[offset+1])<<16|uint32_t(data[offset+2])<<8|data[offset+3];
}
uint32_t adler(const NativeEeprom::Image& data,size_t offset,size_t count){
    uint32_t a=1,b=0;
    for(size_t i=0;i<count;++i){a=(a+data[offset+i])%65521;b=(b+a)%65521;}
    return b<<16|a;
}
int temporary_file(const std::filesystem::path& pattern,std::filesystem::path& result){
    auto text=pattern.string();std::vector<char> name(text.begin(),text.end());name.push_back(0);
    int fd=mkstemp(name.data());if(fd>=0){result=name.data();fcntl(fd,F_SETFD,FD_CLOEXEC);}return fd;
}
}

bool NativeEeprom::complete_checksums(const Image& data){
    struct Check {size_t checksum,offset,size;};
    for(auto c:{Check{0,4,12},Check{16,20,12},Check{0x24,0x28,16},Check{0x20,0x38,176},
                Check{0xec,0xf0,16},Check{0xe8,0x100,176}})
        if(big_endian(data,c.checksum)!=adler(data,c.offset,c.size))return false;
    return true;
}

bool NativeEeprom::replace(const std::filesystem::path& path,const Image& data,bool& committed){
    committed=false;std::filesystem::path temporary;
    int fd=temporary_file(directory/(path.filename().string()+".tmp.XXXXXX"),temporary);
    if(fd<0){last_error="Cannot create save temporary file: "+std::string(std::strerror(errno));return false;}
    auto fail=[&](const char* operation){
        int problem=errno;if(fd>=0)close(fd);unlink(temporary.c_str());
        last_error=std::string(operation)+": "+std::strerror(problem);return false;
    };
    size_t offset=0;
    while(offset<data.size()){
        auto n=::write(fd,data.data()+offset,data.size()-offset);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){if(n==0)errno=EIO;return fail("Cannot write save");}
        offset+=size_t(n);
    }
    if(fsync(fd)!=0)return fail("Cannot flush save");
    if(close(fd)!=0){fd=-1;return fail("Cannot close save");}fd=-1;
    if(rename(temporary.c_str(),path.c_str())!=0)return fail("Cannot replace save");
    committed=true;
    // Persist the directory entry as well as the contents. If this fails after
    // rename, callers still update the in-memory EEPROM to match the new file.
    int parent=open(directory.c_str(),O_RDONLY|O_CLOEXEC);
    if(parent<0)return fail("Cannot open save directory for flush");
    int synced=fsync(parent),problem=errno;close(parent);
    if(synced!=0){errno=problem;return fail("Cannot flush save directory");}
    return true;
}

bool NativeEeprom::load(const std::filesystem::path& folder){
    std::lock_guard lock(mutex);directory=folder;last_error.clear();preserved.clear();
    loaded=false;backup_done=false;source=LoadSource::blank;
    std::error_code error;std::filesystem::create_directories(directory,error);
    if(error){last_error="Cannot create save directory: "+error.message();return false;}
    const auto primary=directory/"rogue-squadron.eep",backup=directory/"rogue-squadron.previous.eep";
    auto result=read_image(primary,image,last_error);
    if(result==ReadResult::error)return false; // Do not conceal permissions/I/O errors.
    if(result==ReadResult::complete){
        // The original cartridge owns header and redundant-record repair.
        source=LoadSource::primary;launch_image=image;loaded=true;return true;
    }
    Image previous;std::string backup_error;
    auto backup_result=read_image(backup,previous,backup_error);
    if(result==ReadResult::missing && backup_result==ReadResult::missing){
        image.fill(0xff);launch_image=image;loaded=true;return true;
    }
    if(backup_result!=ReadResult::complete || !complete_checksums(previous)){
        last_error="Save is missing or not 512 bytes, and no checksum-valid backup is available";
        if(!backup_error.empty())last_error+=". "+backup_error;
        return false;
    }
    if(result==ReadResult::wrong_size){
        int fd=temporary_file(directory/"rogue-squadron.damaged.XXXXXX",preserved);
        if(fd<0){last_error="Cannot preserve damaged save: "+std::string(std::strerror(errno));return false;}
        close(fd);
        if(rename(primary.c_str(),preserved.c_str())!=0){
            int problem=errno;unlink(preserved.c_str());preserved.clear();
            last_error="Cannot preserve damaged save: "+std::string(std::strerror(problem));return false;
        }
    }
    bool committed=false;
    if(!replace(primary,previous,committed))return false;
    image=previous;launch_image=image;source=LoadSource::backup;
    backup_done=true;loaded=true;return true;
}

bool NativeEeprom::read(size_t offset,std::span<uint8_t> bytes)const{
    std::lock_guard lock(mutex);
    if(!loaded || offset>image.size() || bytes.size()>image.size()-offset)return false;
    std::copy_n(image.begin()+offset,bytes.size(),bytes.begin());return true;
}
bool NativeEeprom::write(size_t offset,std::span<const uint8_t> bytes){
    std::lock_guard lock(mutex);last_error.clear();
    if(!loaded || offset>image.size() || bytes.size()>image.size()-offset){last_error="Invalid EEPROM write range";return false;}
    if(bytes.empty())return true;
    if(!backup_done){
        // Preserve the valid image from launch, not an intermediate record from
        // a multi-block cartridge transaction. Never overwrite a valid backup
        // with blank or damaged data.
        if(complete_checksums(launch_image)){
            bool committed=false;
            if(!replace(directory/"rogue-squadron.previous.eep",launch_image,committed))return false;
        }
        backup_done=true;
    }
    auto next=image;std::copy(bytes.begin(),bytes.end(),next.begin()+offset);
    bool committed=false;bool result=replace(directory/"rogue-squadron.eep",next,committed);
    if(committed)image=next;
    return result;
}
NativeEeprom::Image NativeEeprom::snapshot()const{std::lock_guard lock(mutex);return image;}
std::string NativeEeprom::error()const{std::lock_guard lock(mutex);return last_error;}
NativeEeprom::LoadSource NativeEeprom::load_source()const{std::lock_guard lock(mutex);return source;}
std::filesystem::path NativeEeprom::preserved_file()const{std::lock_guard lock(mutex);return preserved;}
