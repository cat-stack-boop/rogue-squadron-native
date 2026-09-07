#include "native_paths.hpp"
#include <SDL.h>
#include <mach-o/dyld.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <vector>

namespace {
RsPaths paths;
int instance_lock=-1;
std::filesystem::path override_path(const char* key,const std::filesystem::path& fallback) {
    const char* value=std::getenv(key);
    if(!value || !*value)return fallback;
    std::filesystem::path result(value);
    if(!result.is_absolute())throw std::runtime_error(std::string(key)+" must be an absolute path");
    return result;
}
std::filesystem::path executable_path() {
    uint32_t size=1024;
    std::vector<char> buffer(size);
    if(_NSGetExecutablePath(buffer.data(),&size)!=0) {
        buffer.resize(size);
        if(_NSGetExecutablePath(buffer.data(),&size)!=0)throw std::runtime_error("Cannot resolve executable path");
    }
    return std::filesystem::canonical(buffer.data());
}
}

bool rs_initialize_paths() {
    try {
        auto executable=executable_path();
        auto contents=executable.parent_path().parent_path();
        paths.bundled=executable.parent_path().filename()=="MacOS" &&
            contents.filename()=="Contents" && contents.parent_path().extension()==".app";
        const char* diagnostics=std::getenv("ROGUE_DIAGNOSTICS");
        paths.diagnostics=diagnostics?std::string(diagnostics)=="1":!paths.bundled;
        setenv("ROGUE_DIAGNOSTICS",paths.diagnostics?"1":"0",1);
        if(paths.bundled) {
            auto resources=contents/"Resources";
            paths.rom=resources/"rogue_squadron.us.rev1.z64";
            paths.renderer=contents/"Frameworks/GLideN64.dylib";
            paths.renderer_ini=resources/"gliden64";
            paths.user_data=override_path("ROGUE_USER_DATA_DIR",{});
            if(paths.user_data.empty()) {
                char* pref=SDL_GetPrefPath("","Rogue Squadron Native");
                if(!pref)throw std::runtime_error(SDL_GetError());
                paths.user_data=pref;SDL_free(pref);
            }
            paths.reports=override_path("ROGUE_DIAGNOSTICS_DIR",paths.user_data/"diagnostics");
        } else {
#ifdef ROGUE_PROJECT_ROOT
            const std::filesystem::path project(ROGUE_PROJECT_ROOT);
#else
            const auto project=std::filesystem::current_path();
#endif
            paths.rom=project/"roms/rogue_squadron.us.rev1.z64";
            paths.renderer=project/"build/gliden64/plugin/Release/mupen64plus-video-GLideN64.dylib";
            paths.renderer_ini=project/"vendor/GLideN64/ini";
            paths.user_data=override_path("ROGUE_USER_DATA_DIR",project/"runtime");
            paths.reports=override_path("ROGUE_DIAGNOSTICS_DIR",project/"reports");
        }
        std::filesystem::create_directories(paths.user_data);
        instance_lock=open((paths.user_data/"session.lock").c_str(),O_CREAT|O_RDWR,0600);
        if(instance_lock<0 || flock(instance_lock,LOCK_EX|LOCK_NB)!=0) {
            std::fprintf(stderr,"Another game instance is using this save directory, or its lock cannot be opened.\n");
            return false;
        }
        std::filesystem::create_directories(paths.reports);
        std::filesystem::create_directories(paths.user_data/"saves");
        // Incidental third-party log files must never modify the signed bundle.
        std::filesystem::current_path(paths.user_data);
        std::ofstream out(paths.reports/"runtime-paths.json");
        out<<"{\"bundled\":"<<(paths.bundled?"true":"false")
           <<",\"diagnostics\":"<<(paths.diagnostics?"true":"false")
           <<",\"executable\":"<<std::quoted(executable.string())
           <<",\"rom\":"<<std::quoted(paths.rom.string())
           <<",\"renderer\":"<<std::quoted(paths.renderer.string())
           <<",\"renderer_ini\":"<<std::quoted(paths.renderer_ini.string())
           <<",\"user_data\":"<<std::quoted(paths.user_data.string())
           <<",\"reports\":"<<std::quoted(paths.reports.string())<<"}\n";
        return true;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"Native path setup: %s\n",e.what());return false;
    }
}
const RsPaths& rs_paths(){return paths;}
std::filesystem::path rs_report_path(const char* name){return paths.reports/name;}
