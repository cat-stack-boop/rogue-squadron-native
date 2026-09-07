#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>

class NativeEeprom {
public:
    using Image=std::array<uint8_t,512>;
    enum class LoadSource {blank,primary,backup};
    bool load(const std::filesystem::path& folder);
    bool read(size_t offset,std::span<uint8_t> bytes)const;
    bool write(size_t offset,std::span<const uint8_t> bytes);
    Image snapshot()const;
    std::string error()const;
    LoadSource load_source()const;
    std::filesystem::path preserved_file()const;
    static bool complete_checksums(const Image&);
private:
    mutable std::mutex mutex;
    Image image{},launch_image{};
    std::filesystem::path directory,preserved;
    std::string last_error;
    LoadSource source=LoadSource::blank;
    bool loaded=false,backup_done=false;
    bool replace(const std::filesystem::path&,const Image&,bool& committed);
};
