/* Incremental native startup runner. Missing services stop with evidence.
 * No instruction interpreter, JIT, or fabricated game behaviour is included.
 * The first bindings use the upstream native libultra implementation.
 */
#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <vector>
#include "recomp.h"
#include "funcs.h"
#include "ultramodern/ultramodern.hpp"
#include "native_video.hpp"
#include "native_audio.hpp"
#include "native_rsp.h"
#include "cpu_status.h"
#include "native_paths.hpp"
#include "native_event_config.hpp"
#include "native_boot_state.h"
#include "native_entities.hpp"
#include "native_eeprom.hpp"
#ifdef ROGUE_RESULT_FIXTURE
#include "result_fixture.hpp"
#endif
#include <CommonCrypto/CommonDigest.h>
#include <unistd.h>

struct Entry { uint32_t address; unsigned overlay; recomp_func_t* function; const char* name; };
static const Entry entries[] = {
#include "dispatch_entries.inc"
};
static std::atomic<unsigned> active_overlay{0};
static std::atomic<bool> mission_pause_armed{false};
static std::atomic<unsigned> mission_generation{0},mission_delta_frames{0},mission_first_vi{0};
static std::atomic<float> mission_last_delta{0},mission_max_delta{0};
static std::atomic<double> mission_delta_sum{0};
static std::atomic<uint64_t> calls{0};
static std::atomic<uint32_t> last_function{0};
static std::array<std::atomic<uint32_t>,64> recent_calls{};
static std::atomic<unsigned> threads_started{0};
static std::atomic<bool> report_written{false};
static std::atomic<unsigned> vi_ticks{0};
static uint8_t* memory;
static thread_local recomp_context* game_context=nullptr;
static std::vector<uint8_t> cartridge;
static uint32_t pi_queue=0;
static uint32_t staged_sp_task=0;
static std::atomic<unsigned> rom_dma_count{0};
static NativeEeprom eeprom;
static std::once_flag eeprom_loaded;
static std::atomic<unsigned> eeprom_writes{0};
static constexpr uint32_t ram_size = 8 * 1024 * 1024;
std::atomic_bool exited{false};
moodycamel::LightweightSemaphore graphics_shutdown_ready;
bool ultramodern::is_game_started() { return memory != nullptr && !exited.load(); }
extern "C" { int32_t* section_addresses = nullptr; }

[[noreturn]] static void stop(const char* reason, uint32_t address) {
    if (!report_written.exchange(true)) {
        bool bss_clear = true;
        /* Startup stack and native thread descriptors may legitimately modify
         * the BSS later; this flag is only a snapshot, never a completion gate. */
        if (memory) for (uint32_t i=0x100000; i<0x110000; ++i) bss_clear &= memory[i] == 0;
        FILE* out = std::fopen(rs_report_path("startup.json").c_str(), "w");
        if (out) {
            std::fprintf(out, "{\n  \"architecture\": \"arm64\",\n  \"reason\": \"%s\",\n  \"address\": \"0x%08x\",\n  \"last_function\": \"0x%08x\",\n  \"native_calls\": %llu,\n  \"threads_started\": %u,\n  \"vi_ticks\": %u,\n  \"graphics_tasks\": %u,\n  \"presented_frames\": %u,\n  \"bss_sample_zero\": %s\n}\n",
                         reason, address, last_function.load(), (unsigned long long)calls.load(), threads_started.load(), vi_ticks.load(), rs_video_tasks(), rs_video_frames(), bss_clear?"true":"false");
            std::fclose(out);
        }
        FILE* counters=std::fopen(rs_report_path("runtime-counters.json").c_str(),"w");
        if(counters){
            std::fprintf(counters,"{\"audio_tasks\":%u,\"queued_audio_samples\":%llu,\"nonzero_audio_samples\":%llu,\"rom_dma_transfers\":%u,\"active_overlay\":%u,\"eeprom_writes\":%u}\n",
                rs_musyx_count(),(unsigned long long)rs_audio_sample_count(),(unsigned long long)rs_audio_nonzero_count(),rom_dma_count.load(),active_overlay.load(),eeprom_writes.load());
            std::fclose(counters);
        }
        rs_audio_write_report(rs_report_path("audio-timing.json").c_str());
        std::fprintf(stderr, "Native startup stopped: %s at 0x%08x (last function 0x%08x, %llu calls)\n", reason, address, last_function.load(), (unsigned long long)calls.load());
        std::fprintf(stderr,"ROM DMA transfers: %u; recent calls:",rom_dma_count.load());
        uint64_t end=calls.load(),begin=end>64?end-64:0;
        for(auto i=begin;i<end;++i)std::fprintf(stderr," %08x",recent_calls[i%64].load());
        std::fprintf(stderr,"\n");
        if(game_context && rs_paths().diagnostics) {
            FILE* context_file=std::fopen(rs_report_path("stopped-context.json").c_str(),"w");
            if(context_file) {
                std::fprintf(context_file,"{\"gpr\":[");
                gpr registers[32];std::memcpy(registers,&game_context->r0,sizeof(registers));
                for(unsigned i=0;i<32;++i)std::fprintf(context_file,"%s\"%016llx\"",i?",":"",(unsigned long long)registers[i]);
                std::fprintf(context_file,"]}\n");std::fclose(context_file);
            }
        }
        if(memory && rs_paths().diagnostics) {
            // Diagnostic word-swapped memory snapshot, not a resumable savestate.
            FILE* ram_file=std::fopen(rs_report_path("stopped-rdram.bin").c_str(),"wb");
            if(ram_file){std::fwrite(memory,1,ram_size,ram_file);std::fclose(ram_file);}
        }
        std::fflush(nullptr);
    }
    std::_Exit(!rs_paths().diagnostics && std::strcmp(reason,"window_closed")==0?0:3);
}
[[noreturn]] void rs_runtime_stop(const char* reason,uint32_t address){stop(reason,address);}
extern "C" void rs_hle_abort(const char* reason,unsigned address){stop(reason,address);}
extern "C" void rs_cooperative_poll(uint8_t* rdram){
    // Console interrupts preempt these audio-drain loops. Native game threads
    // switch at explicit scheduling points, so deliver pending events here.
    ultramodern::wait_for_external_message_timed(rdram,1);
    ultramodern::check_running_queue(rdram);
}

extern "C" void* rs_checked_memory(uint8_t* rdram, uint32_t address, unsigned size, unsigned swap) {
    if ((address & 0xff800000u) != 0x80000000u && (address & 0xff800000u) != 0xa0000000u)
        stop("unimplemented_memory_or_hardware_address", address);
    uint32_t offset = (address & 0x1fffffffu) ^ swap;
    if (offset > ram_size - size || (offset & (size-1))) stop("invalid_memory_access", address);
    return rdram + offset;
}

static void initialize(uint8_t* rdram, recomp_context* ctx) {
    osInitialize();
    uint32_t clock=(uint32_t(cartridge[4])<<24)|(uint32_t(cartridge[5])<<16)|(uint32_t(cartridge[6])<<8)|cartridge[7];
    rs_initialize_boot_state(rdram,ctx,clock);
    std::fprintf(stderr,"Native boot counter rate: %u Hz; VI clock: %u Hz\n",*(uint32_t*)(rdram+0x300a4),*(uint32_t*)(rdram+0x300a8));
}
static void create_thread(uint8_t* rdram, recomp_context* ctx) {
    osCreateThread(rdram, (int32_t)ctx->r4, (OSId)ctx->r5, (int32_t)ctx->r6,
                   (int32_t)ctx->r7, MEM_W(0x10,ctx->r29), MEM_W(0x14,ctx->r29));
}
static void start_thread(uint8_t* rdram, recomp_context* ctx) { osStartThread(rdram, (int32_t)ctx->r4); }
void set_dummy_vi(bool odd);
static void create_vi_manager(uint8_t*, recomp_context*) {
    // preinit already created native VI, graphics, timer, and task workers.
    // Like libultra's VI manager, start with the standard NTSC video mode.
    // The game waits for a retrace before selecting its own mode. Leaving mode
    // null prevents ultramodern from making the registered VI queue current.
    set_dummy_vi(false);
    osViBlack(1);
}
static void vi_black(uint8_t*, recomp_context* ctx){if(rs_paths().diagnostics)std::fprintf(stderr,"VI black: %u\n",(uint8_t)ctx->r4);osViBlack((uint8_t)ctx->r4);}
static void vi_set_mode(uint8_t* rdram,recomp_context* ctx){if(rs_paths().diagnostics)std::fprintf(stderr,"VI mode: %08x\n",(uint32_t)ctx->r4);osViSetMode(rdram,(int32_t)ctx->r4);}
static void vi_features(uint8_t*,recomp_context* ctx){osViSetSpecialFeatures((uint32_t)ctx->r4);}
static void vi_swap(uint8_t* rdram,recomp_context* ctx){osViSwapBuffer(rdram,(int32_t)ctx->r4);}
static void vi_field(uint8_t*,recomp_context* ctx){ctx->r2=ultramodern::renderer::get_vi_regs()->VI_V_CURRENT_LINE_REG&1;}
static void coherent_cache(uint8_t*,recomp_context*){}
static void load_eeprom(){
    if(!eeprom.load(rs_paths().user_data/"saves")){
        std::fprintf(stderr,"Save load failed: %s\n",eeprom.error().c_str());stop("eeprom_load_failed",0);
    }
    auto source=eeprom.load_source();
    if(source==NativeEeprom::LoadSource::backup){
        std::fprintf(stderr,"Save restored from validated backup\n");
        if(!eeprom.preserved_file().empty())std::fprintf(stderr,"Damaged save preserved as %s\n",eeprom.preserved_file().c_str());
    }
    std::ofstream report(rs_report_path("save-load.json"));
    report<<"{\"source\":\""<<(source==NativeEeprom::LoadSource::backup?"backup":source==NativeEeprom::LoadSource::primary?"primary":"blank")
        <<"\",\"preserved_file\":"<<std::quoted(eeprom.preserved_file().string())<<"}\n";
}
static void eeprom_probe(uint8_t*,recomp_context* ctx){std::call_once(eeprom_loaded,load_eeprom);ctx->r2=1;}
static void eeprom_transfer(uint8_t* rdram,recomp_context* ctx,bool write,bool single){
    std::call_once(eeprom_loaded,load_eeprom);
    uint32_t offset=(uint32_t)(ctx->r5&255)*8,size=single?8:(uint32_t)ctx->r7,address=ctx->r6;
    if(offset>512||size>512-offset||(size&7)){ctx->r2=(gpr)-1;return;}
    if(size){rs_checked_memory(rdram,address,1,3);rs_checked_memory(rdram,address+size-1,1,3);}
    std::array<uint8_t,512> bytes{};
    if(write){
        for(uint32_t i=0;i<size;++i)bytes[i]=rdram[((address&0x1fffffffu)+i)^3];
        if(!eeprom.write(offset,std::span<const uint8_t>(bytes.data(),size))){
            // Original serializer 0x80006EC8 discards osEepromLongWrite's
            // return value and reports success. Keep a failed host commit from
            // returning to that path and falsely accepting unsaved progress.
            std::fprintf(stderr,"Save write failed: %s\n",eeprom.error().c_str());stop("eeprom_write_failed",address);
        }
        if(size)++eeprom_writes;
    }else{
        if(!eeprom.read(offset,std::span<uint8_t>(bytes.data(),size))){ctx->r2=(gpr)-1;return;}
        for(uint32_t i=0;i<size;++i)rdram[((address&0x1fffffffu)+i)^3]=bytes[i];
    }
    ctx->r2=0;
}
static void eeprom_long_read(uint8_t* r,recomp_context* c){eeprom_transfer(r,c,false,false);}
static void eeprom_long_write(uint8_t* r,recomp_context* c){eeprom_transfer(r,c,true,false);}
static void eeprom_read(uint8_t* r,recomp_context* c){eeprom_transfer(r,c,false,true);}
static void eeprom_write(uint8_t* r,recomp_context* c){eeprom_transfer(r,c,true,true);}
static void dp_counters(uint8_t* rdram,recomp_context* ctx){
    uint32_t counters[4];rs_dp_counters(counters);
    for(unsigned i=0;i<4;++i)MEM_W(i*4,ctx->r4)=counters[i];
}
static void dp_status(uint8_t*,recomp_context* ctx){rs_dp_status((uint32_t)ctx->r4);}
static void sp_task_load(uint8_t* rdram,recomp_context* ctx){
    uint32_t address=ctx->r4;
    rs_checked_memory(rdram,address,4,0);
    rs_checked_memory(rdram,address+60,4,0);
    if(address&7)stop("unaligned_sp_task",address);
    staged_sp_task=(uint32_t)ctx->r4;
}
static void sp_task_start(uint8_t* rdram,recomp_context*){
    if(!staged_sp_task)stop("sp_start_without_task_load",0);
    auto* task=(OSTask*)(rdram+(staged_sp_task&0x1fffffffu));
    if(rs_paths().diagnostics)std::fprintf(stderr,"Native SP task: type=%u ucode=%08x data=%08x length=%x\n",task->t.type,(uint32_t)task->t.ucode,(uint32_t)task->t.data_ptr,task->t.data_size);
    ultramodern::submit_rsp_task(rdram,(int32_t)staged_sp_task);
}
static void sp_request_yield(uint8_t*,recomp_context*){
    // HLE consumes the submitted list atomically; completion is sent after
    // parsing. The scheduler then sees a completed task rather than a partial one.
}
static void sp_task_yielded(uint8_t*,recomp_context* ctx){ctx->r2=0;}
static void ai_frequency(uint8_t*,recomp_context* ctx){
    uint32_t requested=ctx->r4;
    if(requested==0){ctx->r2=(gpr)-1;return;}
    constexpr uint32_t clock=48681812; // The verified cartridge is NTSC.
    uint32_t divisor=(uint32_t)((float)clock/(float)requested+0.5f);
    if(divisor<132){ctx->r2=(gpr)-1;return;}
    uint32_t actual=clock/divisor;
    ultramodern::set_audio_frequency(actual);ctx->r2=actual;
}
static void ai_next_buffer(uint8_t* rdram,recomp_context* ctx){
    uint32_t address=ctx->r4,bytes=ctx->r5;
    if(rs_audio_dma_full()){ctx->r2=(gpr)-1;return;}
    if(bytes){
        if((address&7)||(bytes&7)||bytes>ram_size)stop("invalid_audio_dma_extent",address);
        rs_checked_memory(rdram,address,1,3);rs_checked_memory(rdram,address+bytes-1,1,3);
        ultramodern::queue_audio_buffer(rdram,(int32_t)address,bytes);
    }
    ctx->r2=0;
}
static void yield_thread(uint8_t* rdram,recomp_context*){
    // Service queued host interrupts at a scheduling point, then enqueue this
    // thread exactly as the cartridge's osYieldThread does.
    ultramodern::wait_for_external_message_timed(rdram,0);
    ultramodern::schedule_running_thread(rdram,ultramodern::this_thread());
    ultramodern::run_next_thread_and_wait(rdram);
}
static void create_pi_manager(uint8_t* rdram,recomp_context* ctx){
    pi_queue=(uint32_t)ctx->r5;
    osCreateMesgQueue(rdram,(int32_t)ctx->r5,(int32_t)ctx->r6,(int32_t)ctx->r7);
}
static void pi_get_queue(uint8_t*,recomp_context* ctx){ctx->r2=(gpr)(int64_t)(int32_t)pi_queue;}
static void pi_start_dma(uint8_t* rdram,recomp_context* ctx){
    uint32_t mb=ctx->r4,direction=ctx->r6,source=ctx->r7;
    uint32_t destination=MEM_W(0x10,ctx->r29),size=MEM_W(0x14,ctx->r29),queue=MEM_W(0x18,ctx->r29);
    uint32_t physical=(source & 0x1fffffffu)|0x10000000u,offset=physical-0x10000000u,dst=destination&0x1fffffffu;
    if(direction!=0)stop("cartridge_dma_write_requires_review",source);
    if(offset>cartridge.size()||size>cartridge.size()-offset||dst>ram_size||size>ram_size-dst)stop("invalid_rom_dma_range",source);
    for(uint32_t i=0;i<size;++i)rdram[(dst+i)^3]=cartridge[offset+i];
    gpr msg=(gpr)(int64_t)(int32_t)mb;
    MEM_H(0,msg)=0xb;MEM_B(2,msg)=ctx->r5;MEM_B(3,msg)=0;
    MEM_W(4,msg)=queue;MEM_W(8,msg)=destination;MEM_W(12,msg)=source;MEM_W(16,msg)=size;MEM_W(20,msg)=0;
    ++rom_dma_count;
    if(rom_dma_count.load()<=8)std::fprintf(stderr,"ROM DMA: %08x -> %08x, %x bytes\n",source,destination,size);
    struct Overlay {uint32_t source,size;};
    static constexpr Overlay overlays[]={{0x96ce0,0x671e0},{0xfdec0,0x287a0},{0x126660,0xb840}};
    static uint32_t loaded[3]{};
    for(unsigned i=0;i<3;++i){
        auto o=overlays[i];
        if(offset>=o.source && offset+size<=o.source+o.size && dst==0x960e0+offset-o.source){
            uint32_t part=offset-o.source;
            if(part==0)loaded[i]=0;
            if(part!=loaded[i])stop("noncontiguous_code_overlay_dma",source);
            loaded[i]+=size;
            if(loaded[i]==o.size){
                active_overlay=i+1;
                if(i==0){mission_pause_armed=true;++mission_generation;}
                std::fprintf(stderr,"Loaded native code overlay %u\n",i+1);
            }
        }
    }
    ultramodern::enqueue_external_message_src((int32_t)queue,(OSMesg)mb,false,ultramodern::EventMessageSource::Pi);
    ctx->r2=0;
}
extern "C" void osSetThreadPri_recomp(uint8_t*,recomp_context*);
extern "C" void osStopThread_recomp(uint8_t*,recomp_context*);
extern "C" void osDestroyThread_recomp(uint8_t*,recomp_context*);
extern "C" void osCreateMesgQueue_recomp(uint8_t*,recomp_context*);
extern "C" void osSetEventMesg_recomp(uint8_t*,recomp_context*);
extern "C" void osRecvMesg_recomp(uint8_t*,recomp_context*);
extern "C" void osSendMesg_recomp(uint8_t*,recomp_context*);
extern "C" void osGetCount_recomp(uint8_t*,recomp_context*);
extern "C" void osGetTime_recomp(uint8_t*,recomp_context*);
extern "C" void osViSetEvent_recomp(uint8_t*,recomp_context*);
extern "C" void osContInit_recomp(uint8_t*,recomp_context*);
extern "C" void osContStartReadData_recomp(uint8_t*,recomp_context*);
extern "C" void osContGetReadData_recomp(uint8_t*,recomp_context*);
extern "C" void osContStartQuery_recomp(uint8_t*,recomp_context*);
extern "C" void osContGetQuery_recomp(uint8_t*,recomp_context*);
extern "C" void osAiSetNextBuffer_recomp(uint8_t*,recomp_context*);
extern "C" void osMotorInit_recomp(uint8_t*,recomp_context*);

static TestPlayState inspect_test_play_state(uint8_t* rdram){
    if(active_overlay.load()!=1)return TestPlayState::unavailable;
    uint32_t bits=MEM_W(0,(gpr)(int32_t)0x80128a4c);float health;
    std::memcpy(&health,&bits,4);
    return classify_test_play_state(true,MEM_W(0,(gpr)(int32_t)0x800fe610),
        MEM_W(0,(gpr)(int32_t)0x800fe60c),MEM_BU(0,(gpr)(int32_t)0x80121726),
        MEM_W(0,(gpr)(int32_t)0x80121720)&0x20,health);
}
static void observe_entities(uint8_t* rdram){
    auto result=inspect_rogue_entities(rdram);
    auto temporary=rs_report_path("entities.json.tmp");
    {
        std::ofstream out(temporary);
        out<<"{\"vi_ticks\":"<<vi_ticks.load()<<",\"level_id\":"<<unsigned(MEM_BU(0,(gpr)(int32_t)0x80121710))
           <<",\"registry_valid\":"<<(result.registry_valid?"true":"false")
           <<",\"health_index\":"<<result.health_index<<",\"entities\":[";
        bool first=true;
        for(auto& e:result.entities){
            if(!first)out<<',';first=false;
            out<<"{\"handle\":"<<e.handle<<",\"object\":"<<e.object<<",\"callback\":"<<e.callback
               <<",\"kind\":"<<e.kind<<",\"flags\":"<<e.flags<<",\"name\":"<<std::quoted(e.name)
               <<",\"position\":["<<e.position[0]<<','<<e.position[1]<<','<<e.position[2]<<"],\"health\":";
            if(e.health)out<<*e.health;else out<<"null";
            out<<'}';
        }
        out<<"]}\n";
    }
    std::error_code error;std::filesystem::rename(temporary,rs_report_path("entities.json"),error);
}
static void controller_read(uint8_t* rdram,recomp_context* ctx) {
    gpr data=ctx->r4;
    osContGetReadData_recomp(rdram,ctx);
#ifdef ROGUE_RESULT_FIXTURE
    rs_result_fixture_tick(rdram,ctx,inspect_test_play_state(rdram),vi_ticks.load());
#endif
    if(rs_paths().diagnostics)rs_observe_test_play_state(inspect_test_play_state(rdram));
    // Explicit diagnostic fixture: the original GAMEFLO!/bonus-level flags.
    // These codes are deliberately not persisted by the cartridge. The runner
    // permits this only with separate campaign-test saves; normal play is intact.
    static const bool test_level_select=[] {
        const char* value=std::getenv("ROGUE_TEST_LEVEL_SELECT");
        return rs_paths().diagnostics && value && std::strcmp(value,"1")==0;
    }();
    if(test_level_select)MEM_W(0,(gpr)(int32_t)0x80121728)|=0x1c000001u;
    if(rs_paths().diagnostics && (MEM_HU(0,data)||MEM_B(2,data)||MEM_B(3,data))) {
        gpr cache=(gpr)(int32_t)0x80121758;
        std::fprintf(stderr,"Controller packet: raw=%04x axes=(%d,%d) error=%u buffer=%08x cached=%04x axes=(%d,%d) present=%u disabled=%u\n",
            MEM_HU(0,data),MEM_B(2,data),MEM_B(3,data),MEM_BU(4,data),(uint32_t)data,
            MEM_HU(0,cache),MEM_B(2,cache),MEM_B(3,cache),MEM_BU(4,cache),MEM_BU(0,(gpr)(int32_t)0x80121726));
    }
    // Revision 1's mission initializer clears this 16-byte structure at
    // 800ec1ec; the projectile routine at 80055c60 increments its shot count.
    // Observe it from the running game thread so diagnostics do not race a
    // partially written native frame or alter the original mission state.
    static unsigned diagnostic_polls=0;
    static const unsigned diagnostic_interval=[] {
        const char* value=std::getenv("ROGUE_TEST_CONTROLLER");
        return value && std::strcmp(value,"1")==0?3u:30u;
    }();
    if(rs_paths().diagnostics && active_overlay.load()==1 && (++diagnostic_polls%diagnostic_interval)==0) {
        if(diagnostic_interval==3 && diagnostic_polls%6==0)observe_entities(rdram);
        gpr stats=(gpr)(int32_t)0x80121748;
        auto temporary=rs_report_path("mission-stats.json.tmp");
        FILE* out=std::fopen(temporary.c_str(),"w");
        if(out) {
            std::fprintf(out,"{\"vi_ticks\":%u,\"overlay\":1,\"completion_time\":%u,\"shots_fired\":%u,\"shots_landed\":%u,\"enemies_destroyed\":%u,\"friendlies_saved\":%u,\"bonus\":%u,\"raw_buttons\":%u,\"raw_x\":%d,\"raw_y\":%d,\"level_id\":%u,\"craft_id\":%u,\"controller_setting\":%u,\"mission_state\":%u,\"input_disabled\":%u,\"lives\":%u",
                vi_ticks.load(),(uint32_t)MEM_W(0,stats),(uint32_t)MEM_W(4,stats),(uint32_t)MEM_W(8,stats),
                MEM_HU(12,stats),MEM_BU(14,stats),MEM_BU(15,stats),MEM_HU(0,data),MEM_B(2,data),MEM_B(3,data),
                MEM_BU(0,(gpr)(int32_t)0x80121710),MEM_BU(1,(gpr)(int32_t)0x80121710),MEM_BU(5,(gpr)(int32_t)0x80121710),
                (uint32_t)MEM_W(0,(gpr)(int32_t)0x800fe610),MEM_BU(0,(gpr)(int32_t)0x80121726),MEM_BU(0,(gpr)(int32_t)0x801216e0));
            auto number=[&](double value){if(std::isfinite(value))std::fprintf(out,"%.9g",value);else std::fputs("null",out);};
            std::fprintf(out,",\"mission_flags\":%u",(uint32_t)MEM_W(0,(gpr)(int32_t)0x800fe60c));
            std::fputs(",\"frame_delta_seconds\":",out);number(mission_last_delta.load());
            std::fputs(",\"frame_delta_max\":",out);number(mission_max_delta.load());
            std::fputs(",\"frame_delta_sum\":",out);number(mission_delta_sum.load());
            std::fprintf(out,",\"frame_delta_calls\":%u,\"frame_vi_elapsed\":%u,\"player\":{",mission_delta_frames.load(),vi_ticks.load()-mission_first_vi.load());
            gpr player=(gpr)(int32_t)0x80128988;
            auto value=[&](unsigned offset){float f;uint32_t u=MEM_W(offset,player);std::memcpy(&f,&u,4);return f;};
            const char* names[]={"position","forward","up","velocity"};unsigned offsets[]={8,0x14,0x20,0x2c};
            for(unsigned i=0;i<4;++i){std::fprintf(out,"%s\"%s\":[",i?",":"",names[i]);for(unsigned j=0;j<3;++j){if(j)std::fputc(',',out);number(value(offsets[i]+4*j));}std::fputc(']',out);}
            std::fputs(",\"health\":",out);number(value(0xc4));std::fputs(",\"max_health\":",out);number(value(0xc8));
            std::fputs("}}\n",out);
            std::fclose(out);
            std::error_code error;std::filesystem::rename(temporary,rs_report_path("mission-stats.json"),error);
        }
    }
}

static void observe_mission_input(uint8_t* rdram,recomp_context* ctx) {
    static unsigned observed_generation=~0u;
    unsigned generation=mission_generation.load();
    if(observed_generation!=generation) {
        observed_generation=generation;mission_delta_frames=0;mission_delta_sum=0;
        mission_max_delta=0;mission_first_vi=vi_ticks.load();
    }
    // The original mission loop passes its clamped frame delta in f12 at
    // 800ec39c. Keep every original instruction and only observe the argument.
    float delta=ctx->f12.fl;mission_last_delta=delta;
    mission_max_delta=std::max(mission_max_delta.load(),delta);
    // This observer is the only writer; controller diagnostics only read it.
    mission_delta_sum=mission_delta_sum.load()+delta;++mission_delta_frames;
    mission_800B065C(rdram,ctx);
}

static void trace_menu_frame(uint8_t* rdram,recomp_context* ctx) {
    // Read-only trace at the original menu's per-frame timing call. s5 is
    // the state selected by the switch at 800ab9f8. Preserve the real routine.
    uint32_t state=ctx->r21;
    menu_800BD404(rdram,ctx);
    if(state>17 || !rs_paths().diagnostics)return;
    static uint32_t previous=~0u;
    static unsigned frames=0;
    if(state!=previous || (++frames%120)==0) {
        std::fprintf(stderr,"Menu frame: state=%u dt=%g sp=%08x flags=",state,ctx->f0.fl,(uint32_t)ctx->r29);
        for(unsigned i=0x7f;i<0x100;++i)std::fprintf(stderr,"%02x",MEM_BU(ctx->r29,i));
        std::fprintf(stderr,"\n");previous=state;
    }
}

static void trace_craft_frame(uint8_t* rdram,recomp_context* ctx) {
    menu_8009BDF0(rdram,ctx);
    static unsigned frames=0;
    if(rs_paths().diagnostics && (++frames%60)==0) {
        auto f=[&](uint32_t a){float result;uint32_t bits=MEM_W((gpr)(int32_t)a,0);std::memcpy(&result,&bits,4);return result;};
        uint32_t fade=MEM_W((gpr)(int32_t)0x800bdb9c,0);
        unsigned alpha=0;
        if((fade&0xff800000u)==0x80000000u) {
            uint32_t data=MEM_W((gpr)(int32_t)fade,12);
            if((data&0xff800000u)==0x80000000u)alpha=MEM_BU((gpr)(int32_t)data,0x2f);
        }
        std::fprintf(stderr,"Craft frame: result=%08x state=%u selection=%u exit=%08x dt=%g motion=%g fade=%08x alpha=%u fade_frames=%u fade_rate=%g\n",
            (uint32_t)ctx->r2,MEM_BU((gpr)(int32_t)0x800bd724,0),MEM_BU((gpr)(int32_t)0x800bea48,0),
            (uint32_t)MEM_W((gpr)(int32_t)0x800bea34,0),f(0x800be964),f(0x800bea54),fade,alpha,
            MEM_BU((gpr)(int32_t)0x800bdba0,0),f(0x800bfa54));
    }
}

extern "C" recomp_func_t* get_function(int32_t signed_address) {
    uint32_t address = signed_address;
    uint64_t count = ++calls;
    recent_calls[(count-1)%64]=address;
    last_function = address;
    // B065C clears the pad cache while input is disabled or state==7 (intro),
    // and replaces it with recorded input for a demo. Wait for usable input.
    static const bool review_pause_enabled=[] {
        const char* value=std::getenv("ROGUE_PAUSE_ON_MISSION");
        return rs_paths().diagnostics && value && std::strcmp(value,"1")==0;
    }();
    if(review_pause_enabled && address==0x800b065c && active_overlay.load()==1 && mission_pause_armed.load()) {
        uint8_t* rdram=memory;
        if(inspect_test_play_state(rdram)==TestPlayState::live && mission_pause_armed.exchange(false)) {
            rs_request_start_pulse();
            std::fprintf(stderr,"Diagnostic mission-entry Start input requested for level %u\n",MEM_BU(0,(gpr)(int32_t)0x80121710));
        }
    }
    if(address==0x800bd404 && active_overlay.load()==2)return trace_menu_frame;
    if(address==0x8009bdf0 && active_overlay.load()==2)return trace_craft_frame;
    if(address==0x800b065c && active_overlay.load()==1 && rs_paths().diagnostics)return observe_mission_input;
    // The wall-clock watchdog bounds diagnostics; valid scenes can legitimately
    // execute more than a million native function calls during startup.
    if (rs_paths().diagnostics && count < 250) std::fprintf(stderr, "call %llu: 0x%08x overlay=%u\n", (unsigned long long)count, address, active_overlay.load());
    switch (address) {
        case 0x80025e20: return initialize;
        case 0x80025180: return create_thread;
        case 0x8002bae0: return start_thread;
        case 0x8002c1f0: return create_vi_manager;
        case 0x8002c080: return vi_black;
        case 0x8002b0a0: return osSetThreadPri_recomp;
        case 0x8002bbf0: return osStopThread_recomp;
        case 0x80025250: return osDestroyThread_recomp;
        case 0x80025150: return osCreateMesgQueue_recomp;
        case 0x8002af30: return osSetEventMesg_recomp;
        case 0x8002ab90: return osRecvMesg_recomp;
        case 0x8002adf0: return osSendMesg_recomp;
        case 0x80029e50: return create_pi_manager;
        case 0x80029e20: return pi_get_queue;
        case 0x80029d70: return pi_start_dma;
        case 0x80026090: case 0x80026140: case 0x8002cc30: case 0x8002cbb0: return coherent_cache;
        case 0x80025db0: return osGetCount_recomp;
        case 0x80025d30: return osGetTime_recomp;
        case 0x8002e480: return yield_thread;
        case 0x8002c620: return osViSetEvent_recomp;
        case 0x8002c680: return vi_set_mode;
        case 0x8002c6d0: return vi_features;
        case 0x8002c1d0: return vi_field;
        case 0x8002c830: return vi_swap;
        // Pure libc routines are safe to execute as their original native code:
        // strlen, strchr, and memcpy respectively; no console hardware access.
        case 0x8002bca0: return main_8002BCA0;
        case 0x8002bcc4: return main_8002BCC4;
        case 0x8002bcfc: return main_8002BCFC;
        case 0x80024d00: return osContInit_recomp;
        case 0x80024a70: return osContStartQuery_recomp;
        case 0x80024af4: return osContGetQuery_recomp;
        case 0x80024b20: return osContStartReadData_recomp;
        case 0x80024bb0: return controller_read;
        case 0x80023ef0: return ai_frequency;
        case 0x80024010: return ai_next_buffer;
        case 0x80026440: return osMotorInit_recomp;
        // Verified hardware-independent projection, matrix and trig routines.
        case 0x800271a0: return main_800271A0;
        case 0x8002701c: return main_8002701C;
        case 0x8002706c: return main_8002706C;
        case 0x80025130: return main_80025130;
        case 0x80025000: return main_80025000;
        case 0x8002b460: return main_8002B460;
        case 0x8002b2e0: return main_8002B2E0;
        case 0x8002ad10: return main_8002AD10; // guScale: original matrix conversion.
        case 0x80026cc0: return main_80026CC0; // guMtxCatL
        case 0x80027104: return main_80027104; // guMtxL2F
        case 0x80026dc0: return main_80026DC0; // guMtxCatF
        case 0x8002bad0: return main_8002BAD0; // Original two-instruction sqrtf leaf.
        case 0x8002b6e0: return sp_task_load;
        case 0x8002b898: return sp_task_start;
        case 0x80025330: return dp_counters;
        case 0x80025380: return dp_status;
        case 0x80026050: return main_80026050; // Save/clear context interrupt-mask bit.
        case 0x80026074: return main_80026074; // Restore context interrupt-mask bit.
        case 0x8002ba40: return sp_request_yield;
        case 0x8002ba60: return sp_task_yielded;
        case 0x80024410: return eeprom_probe;
        case 0x800242b0: return eeprom_long_read;
        case 0x80024320: return eeprom_long_write;
        case 0x80024490: return eeprom_read;
        case 0x8002485c: return eeprom_write;
        case 0x8002b650: return main_8002B650; // Original sprintf and O32 varargs.
        case 0x80025dc0: return main_80025DC0; // Original 64-bit division helper.
        case 0x80025df0: return main_80025DF0; // Original 64-bit remainder helper.
    }
    // Bare N64 OS routines must be identified and bound to native services.
    // They must never accidentally execute direct hardware code in this runner.
    bool printf_helpers=(address>=0x8002cc60&&address<0x8002e480)||address==0x8002b6ac;
    if (address >= 0x80024800 && address < 0x8002e480 && !printf_helpers)
        stop("native_libultra_binding_required", address);
    unsigned loaded = active_overlay.load();
#ifdef ROGUE_RESULT_FIXTURE
    if(loaded==2 && address==0x800B75EC)return rs_result_fixture_stats;
#endif
    auto entry=std::lower_bound(std::begin(entries),std::end(entries),address,
        [](const Entry& e,uint32_t value){return e.address<value;});
    for(;entry!=std::end(entries)&&entry->address==address;++entry)
        if(entry->overlay==0||entry->overlay==loaded)return entry->function;
    stop("native_function_mapping_required", address);
}

extern "C" void cop0_status_write(recomp_context* ctx, gpr value) { rs_set_cpu_status(ctx,value); }
extern "C" gpr cop0_status_read(recomp_context* ctx) { return (gpr)(int64_t)(int32_t)ctx->status_reg; }
extern "C" void switch_error(const char*, uint32_t vram, uint32_t) { stop("unresolved_jump_table", vram); }
extern "C" void do_break(uint32_t vram) { stop("original_game_break_instruction", vram); }
extern "C" void recomp_syscall_handler(uint8_t*, recomp_context*, int32_t vram) { stop("unimplemented_syscall", vram); }

void run_thread_function(uint8_t* rdram, uint64_t address, uint64_t sp, uint64_t arg) {
    ++threads_started;
    recomp_context ctx{};
    ctx.r29 = sp; ctx.r4 = arg; ctx.f_odd = &ctx.f0.u32h;
    game_context=&ctx;
    get_function((int32_t)address)(rdram, &ctx);
    game_context=nullptr;
}

int main() {
#ifdef ROGUE_RESULT_FIXTURE
    if(!rs_result_fixture_validate()){
        std::fprintf(stderr,"Result fixture requires a marked isolated result-* save directory and explicit success_gold mode.\n");return 2;
    }
#endif
    if(!rs_initialize_paths())return 2;
    if(rs_paths().bundled && !rs_paths().diagnostics) {
        if(std::freopen(rs_report_path("runtime.log").c_str(),"w",stderr))dup2(fileno(stderr),fileno(stdout));
        std::setvbuf(stderr,nullptr,_IONBF,0);std::setvbuf(stdout,nullptr,_IOLBF,0);
    }
    FILE* input = std::fopen(rs_paths().rom.c_str(), "rb");
    auto& rom=cartridge;rom.resize(0x1000000);
    if (!input || std::fread(rom.data(),1,rom.size(),input)!=rom.size() || std::fgetc(input)!=EOF)
        stop("cartridge_read_or_size_error",0);
    std::fclose(input);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(rom.data(),static_cast<CC_LONG>(rom.size()),digest);
    char hex[65];for(unsigned i=0;i<32;++i)std::snprintf(hex+i*2,3,"%02x",digest[i]);
    if(std::strcmp(hex,"4813551d01d3a3474df3a51f84c31059cd2a9d1eeae7885dda51a88ee6b9f88d")!=0)
        stop("cartridge_identity_mismatch",0);
    memory = (uint8_t*)std::malloc(ram_size);
    if (!memory) return 2;
    std::memset(memory, 0xcc, ram_size);
    for (uint32_t i=0; i<0x100000; ++i) memory[(0x400+i)^3] = rom[0x1000+i];
    *(uint32_t*)(memory+0x300)=1; // NTSC
    *(uint32_t*)(memory+0x308)=0xb0000000;
    *(uint32_t*)(memory+0x30c)=0;
    *(uint32_t*)(memory+0x318)=ram_size;
    unsigned run_seconds=rs_paths().bundled?0:30;
    if(const char* value=std::getenv("ROGUE_RUN_SECONDS"))run_seconds=std::strtoul(value,nullptr,10);
    if(run_seconds>3600)return 2;
#ifdef ROGUE_RESULT_FIXTURE
    if(!run_seconds || run_seconds>240)return 2;
#endif
    if(run_seconds)std::thread([run_seconds] { std::this_thread::sleep_for(std::chrono::seconds(run_seconds)); stop("startup_timeout",last_function.load()); }).detach();
    std::fprintf(stderr,"Native startup phase: create macOS video/audio context\n");
    if(!rs_video_open(rom.data()))stop("native_video_context_failed",0);
    ultramodern::set_callbacks(
        {.init=nullptr,.run_task=[](uint8_t* ram,const OSTask* task)->bool {
            uint32_t words[16];std::memcpy(words,&task->t,64);
            if(rs_run_musyx(ram,words))return true;
            stop("rsp_audio_or_custom_task_required",task->t.type);}},
        {.create_render_context=rs_create_renderer},
        {.queue_samples=rs_audio_samples,.get_frames_remaining=rs_audio_frames_remaining,.set_frequency=rs_audio_frequency},
        {.poll_input=nullptr,.get_input=rs_keyboard_input,.set_rumble=nullptr,
         .get_connected_device_info=[](int port){return ultramodern::input::connected_device_info_t{
            port==0?ultramodern::input::Device::Controller:ultramodern::input::Device::None,
            ultramodern::input::Pak::None};}}, {},
        {.vi_callback=[] {++vi_ticks;},.gfx_init_callback=nullptr}, {}, {});
    ultramodern::set_message_queue_control(rs_message_queue_control());
    std::thread([]{
        ultramodern::preinit(memory,{});
        std::fprintf(stderr,"Native startup phase: execute original entrypoint\n");
        recomp_context ctx{};ctx.f_odd=&ctx.f0.u32h;
        get_function(0x80000400)(memory,&ctx);
    }).detach();
    // Cocoa events stay on the main thread throughout renderer and game setup.
    for (;;) {
        if(!rs_video_poll())stop("window_closed",last_function.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
