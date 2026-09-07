// Linked only into rogue_result_fixture, never the player executable.
#include "result_fixture.hpp"
#include "funcs.h"
#include "native_paths.hpp"
#include "native_audio.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>

namespace {
bool requested=false,score_supplied=false;
unsigned first_live_vi=0;
gpr address(uint32_t a){return (gpr)(int64_t)(int32_t)a;}
void report(uint8_t* rdram,const char* phase){
    std::ofstream out(rs_report_path("result-fixture.json"));
    out<<"{\"synthetic_completion\":true,\"synthetic_score\":"<<(score_supplied?"true":"false")
       <<",\"phase\":\""<<phase<<"\",\"completion_routine\":\"0x800ecd50\",\"completion_argument\":3"
       <<",\"mission_state\":"<<MEM_W(0,address(0x800fe610))
       <<",\"result_code\":"<<unsigned(MEM_BU(0,address(0x801216e4)))
       <<",\"max_unlocked_level\":"<<unsigned(MEM_BU(0,address(0x801216ff)))
       <<",\"first_medal\":"<<unsigned(MEM_BU(0,address(0x801216ec)))
       <<",\"saved_max_level\":"<<unsigned(MEM_BU(0,address(0x8012b179)))
       <<",\"saved_medal_bits\":"<<unsigned(MEM_BU(0,address(0x8012b17a)))<<"}\n";
}
}

bool rs_result_fixture_validate(){
    const char* dir=std::getenv("ROGUE_USER_DATA_DIR");
    const char* mode=std::getenv("ROGUE_RESULT_FIXTURE_MODE");
    if(!dir || !mode || std::strcmp(mode,"success_gold")!=0)return false;
    std::error_code error;
    auto path=std::filesystem::weakly_canonical(dir,error);
    auto root=std::filesystem::weakly_canonical(std::filesystem::path(ROGUE_PROJECT_ROOT)/"runtime/campaign-tests",error);
    if(error || !path.is_absolute() || path.parent_path()!=root || !path.filename().string().starts_with("result-"))return false;
    std::ifstream marker(path/".result-fixture");std::string line;std::getline(marker,line);
    return line=="Synthetic completion fixture; no real player achievements";
}

void rs_result_fixture_tick(uint8_t* rdram,recomp_context* caller,TestPlayState play,unsigned vi){
    if(requested)return;
    if(play!=TestPlayState::live){first_live_vi=0;return;}
    if(MEM_BU(0,address(0x80121710))!=0)rs_runtime_stop("result_fixture_expected_first_mission",0);
    if(!first_live_vi){first_live_vi=vi;report(rdram,"waiting_after_normal_mission_entry");}
    if(vi-first_live_vi<180)return;
    requested=true;
    recomp_context call{};call.r29=caller->r29;call.f_odd=&call.f0.u32h;call.r4=3;
    // The same original function used for mission termination. Argument 3
    // enters its success sequence; objective completion itself is not tested.
    mission_800ECD50(rdram,&call);
    if(call.r2!=1 || MEM_W(0,address(0x800fe610))!=3)rs_runtime_stop("result_fixture_completion_rejected",0);
    std::fprintf(stderr,"SYNTHETIC RESULT FIXTURE: original success sequence requested after 180 live VI ticks\n");
    report(rdram,"completion_requested");
}

void rs_result_fixture_stats(uint8_t* rdram,recomp_context* ctx){
    if(requested && !score_supplied){
        if((ctx->r5&255)!=0 || (ctx->r6&255)!=0)rs_runtime_stop("result_fixture_unexpected_result_screen",0);
        auto table=uint32_t(MEM_W(0,address(0x800bfe8c)));
        if((table&0xff800000)!=0x80000000)rs_runtime_stop("result_fixture_missing_medal_table",table);
        const gpr gold=address(table+24); // level 0, third (gold) medal row
        uint32_t bits=MEM_W(0,gold);float minutes;std::memcpy(&minutes,&bits,4);
        auto enemies=MEM_HU(4,gold),accuracy=MEM_HU(6,gold),friendlies=MEM_HU(8,gold),bonus=MEM_HU(10,gold);
        if(!std::isfinite(minutes)||minutes<=0||minutes>99||accuracy>100||friendlies>255||bonus>255)
            rs_runtime_stop("result_fixture_invalid_medal_row",table+24);
        const auto stats=ctx->r4;
        std::ofstream before(rs_report_path("result-fixture-score.json"));
        before<<"{\"synthetic\":true,\"original_elapsed_seconds\":"<<uint32_t(MEM_W(0,stats))
              <<",\"original_kills\":"<<MEM_HU(12,stats)
              <<",\"gold_time_seconds\":"<<uint32_t(minutes*60)
              <<",\"gold_enemies\":"<<enemies<<",\"gold_accuracy\":"<<accuracy
              <<",\"gold_friendlies\":"<<friendlies<<",\"gold_bonus\":"<<bonus<<"}\n";
        MEM_W(0,stats)=uint32_t(minutes*60)-1;
        MEM_W(4,stats)=100;MEM_W(8,stats)=100;
        MEM_H(12,stats)=enemies;MEM_B(14,stats)=friendlies;MEM_B(15,stats)=bonus;
        score_supplied=true;report(rdram,"synthetic_gold_score_supplied");
        std::fprintf(stderr,"SYNTHETIC RESULT FIXTURE: supplied score meeting the loaded cartridge's gold thresholds\n");
    }
    menu_800B75EC(rdram,ctx);
    if(score_supplied)report(rdram,"results_closed");
}
