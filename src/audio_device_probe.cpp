// Failure injection is linked only into this executable. Successful operations
// use a real SDL dummy output queue; game DMA uses the production audio clock.
#include "native_audio.hpp"
#include "native_audio_device.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
std::atomic<unsigned> completions{0};
unsigned checks=0,submitted=0,opens=0,closes=0,live_devices=0;
unsigned fail_opens=2,fail_queues=0;
bool stopped=false,frozen=false;
int last_rate=0;
using Clock=std::chrono::steady_clock;
SDL_AudioDeviceID open_device(const SDL_AudioSpec* want,SDL_AudioSpec* have){
    ++opens;last_rate=want->freq;
    if(fail_opens){--fail_opens;SDL_SetError("injected device-open failure");return 0;}
    auto device=SDL_OpenAudioDevice(nullptr,0,want,have,0);if(device)++live_devices;return device;
}
void close_device(SDL_AudioDeviceID device){
    ++closes;--live_devices;stopped=false;frozen=false;SDL_CloseAudioDevice(device);
}
int queue(SDL_AudioDeviceID device,const void* data,Uint32 bytes){
    if(fail_queues){--fail_queues;return SDL_SetError("injected queue failure");}
    return SDL_QueueAudio(device,data,bytes);
}
void pause(SDL_AudioDeviceID device,int value){SDL_PauseAudioDevice(device,frozen?1:value);}
SDL_AudioStatus status(SDL_AudioDeviceID device){
    if(stopped)return SDL_AUDIO_STOPPED;
    // Simulate a driver that claims playback started but consumes no samples.
    return frozen?SDL_AUDIO_PLAYING:SDL_GetAudioDeviceStatus(device);
}
void require(bool good,const char* why){
    if(!good){std::fprintf(stderr,"FAIL: %s\n",why);std::_Exit(1);}++checks;
}
template<class F> bool wait_for(F ready){
    auto end=Clock::now()+std::chrono::seconds(3);
    while(!ready() && Clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return ready();
}
std::vector<int16_t> samples(384,1700);
void submit(){
    require(wait_for([]{return !rs_audio_dma_full();}),"logical DMA FIFO continues to admit buffers");
    rs_audio_samples(samples.data(),samples.size());++submitted;
}
void finish_dma(){require(wait_for([]{return completions.load()==submitted;}),"every submitted buffer completes exactly once");}
void recover(){
    auto end=Clock::now()+std::chrono::seconds(3);
    while(rs_audio_output_unavailable() && Clock::now()<end)submit();
    require(!rs_audio_output_unavailable(),"output recovers without restarting game");
    finish_dma();require(live_devices==1,"recovery retains exactly one live device");
}
}
namespace ultramodern {void send_ai_message(){++completions;}}
[[noreturn]] void rs_runtime_stop(const char* why,uint32_t address){
    std::fprintf(stderr,"FAIL runtime stopped: %s %08x\n",why,address);std::_Exit(1);
}
const AudioDeviceApi& rs_audio_device_api(){
    static const AudioDeviceApi api{open_device,close_device,SDL_GetQueuedAudioSize,status,queue,pause,SDL_ClearQueuedAudio};return api;
}
int main(int argc,char** argv){
    SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    require(SDL_Init(SDL_INIT_AUDIO)==0,"SDL dummy audio initializes");
    rs_audio_frequency(22047);
    require(rs_audio_output_unavailable() && opens==1,"initial device-open failure is recoverable");
    for(unsigned i=0;i<8;++i)submit();finish_dma();
    require(opens==1,"failed device is not reopened for every audio buffer");
    require(completions.load()==8,"audio DMA continues while no host device exists");
    require(rs_audio_frames_remaining()==0,"offline PCM is not accumulated for late playback");
    recover();require(opens==3,"two failed opens followed by successful retry");
    for(unsigned i=0;i<8;++i)submit();finish_dma();
    require(wait_for([]{return rs_audio_frames_remaining()==0;}),"recovered SDL queue actually drains");

    fail_queues=1;submit();finish_dma();
    require(rs_audio_output_unavailable() && live_devices==0,"queue failure closes failed output without stopping game");
    recover();
    for(unsigned i=0;i<8;++i)submit();finish_dma();
    require(wait_for([]{return rs_audio_frames_remaining()==0;}),"queue-error recovery restores playback");

    stopped=true;submit();finish_dma();
    require(rs_audio_output_unavailable() && live_devices==0,"device removal releases stopped device");
    // A rate change during output loss must retain pending DMA completions.
    submit();submit();unsigned pending_submitted=submitted;
    fail_opens=1;rs_audio_frequency(32006);
    require(rs_audio_output_unavailable() && last_rate==32006,"new cartridge rate is used even during an outage");
    finish_dma();require(completions.load()==pending_submitted,"rate change preserves queued completions while offline");
    recover();require(last_rate==32006,"reopened output uses new sample rate");
    for(unsigned i=0;i<12;++i)submit();finish_dma();
    require(wait_for([]{return rs_audio_frames_remaining()==0;}),"new-rate playback drains");

    frozen=true;auto end=Clock::now()+std::chrono::seconds(3);unsigned old_closes=closes;
    while(!rs_audio_output_unavailable() && Clock::now()<end)submit();
    require(rs_audio_output_unavailable() && closes>old_closes,"non-consuming driver is reopened instead of clearing backlog forever");
    finish_dma();recover();
    for(unsigned i=0;i<12;++i)submit();finish_dma();
    require(wait_for([]{return rs_audio_frames_remaining()==0;}),"stalled-device recovery restores queue consumption");
    // Intentional prefill silence is not a failed driver. A game can stop
    // submitting audio after an underrun and resume much later.
    submit();finish_dma();unsigned idle_opens=opens;
    require(rs_audio_frames_remaining()==192,"underrun leaves one intentionally paused prefill buffer");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    for(unsigned i=0;i<12;++i)submit();finish_dma();
    require(!rs_audio_output_unavailable() && opens==idle_opens,"idle prefill does not cause a false stalled-device reconnect");
    require(wait_for([]{return rs_audio_frames_remaining()==0;}),"playback resumes after a long idle prefill");
    require(completions.load()==submitted,"all fault scenarios preserve the exact completion count");
    rs_audio_write_report(argc>1?argv[1]:"reports/audio-device-timing.json");
    std::printf("{\"passed\":true,\"checks\":%u,\"submitted_buffers\":%u,\"dma_completions\":%u,\"open_attempts\":%u,\"closed_devices\":%u,\"live_devices\":%u,\"physical_device_faults_verified\":false}\n",checks,submitted,completions.load(),opens,closes,live_devices);
    std::fflush(nullptr);std::_Exit(0);
}
