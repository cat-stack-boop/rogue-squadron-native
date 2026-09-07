#pragma once
#include <filesystem>

struct RsPaths {
    bool bundled=false;
    bool diagnostics=true;
    std::filesystem::path rom,renderer,renderer_ini,user_data,reports;
};
bool rs_initialize_paths();
const RsPaths& rs_paths();
std::filesystem::path rs_report_path(const char* name);
