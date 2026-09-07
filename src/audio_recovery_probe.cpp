// Exercise the actual SDL queue and DMA clock with a dummy output device.
#include "native_audio.hpp"
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static std::atomic<unsigned> completions{0};
namespace ultramodern { void send_ai_message(){++completions;} }
[[noreturn]] void rs_runtime_stop(const char* why,uint32_t a) {
    std::fprintf(stderr,"FAIL %s %08x\n",why,a);std::_Exit(1);
}
static unsigned checks=0;
static void require(bool good,const char* why) {
    if(!good)rs_runtime_stop(why,0);++checks;
}
template<class F> static bool wait_for(F ready) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!ready() && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return ready();
}
int main(int argc,char** argv) {
    SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    require(SDL_Init(SDL_INIT_AUDIO)==0,"dummy audio initialization");
    rs_audio_frequency(22047);
    std::vector<int16_t> buffer(384,1234);
    auto submit=[&](std::vector<int16_t>& samples) {
        require(wait_for([]{return !rs_audio_dma_full();}),"DMA FIFO admits next buffer");
        rs_audio_samples(samples.data(),samples.size());
    };
    for(unsigned i=0;i<8;++i)submit(buffer);
    require(wait_for([]{return rs_audio_frames_remaining()==0;}),"initial audio drains");
    require(wait_for([]{return completions.load()==8;}),"initial DMAs complete");
    submit(buffer);
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    require(rs_audio_frames_remaining()==192,"underrun pauses playback to rebuild lead");
    require(completions.load()==9,"rebuffering does not delay logical DMA");
    for(unsigned i=0;i<7;++i)submit(buffer);
    require(wait_for([]{return rs_audio_frames_remaining()==0;}),"rebuffered stream resumes");
    require(wait_for([]{return completions.load()==16;}),"rebuffered DMAs complete");
    // Inject half a second of queued PCM, representing a stalled host device.
    std::vector<int16_t> backlog(22048,1234);
    submit(backlog);
    submit(buffer);
    require(rs_audio_frames_remaining()==192,"stale host backlog is discarded");
    require(wait_for([]{return completions.load()==18;}),"backlog reset preserves all DMA completions");
    rs_audio_write_report(argc>1?argv[1]:"reports/audio-recovery-timing.json");
    std::printf("{\"passed\":true,\"checks\":%u,\"dma_completions\":%u}\n",checks,completions.load());
    std::fflush(nullptr);std::_Exit(0);
}
