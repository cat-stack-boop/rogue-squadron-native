#include "native_audio.hpp"
#include "audio_dma_clock.hpp"
#include "native_audio_device.hpp"
#include "ultramodern/events.hpp"
#include <SDL.h>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <vector>
#include <cstdio>
#include <atomic>
#include <string>
namespace {
using Clock=std::chrono::steady_clock;
std::mutex mutex;
std::condition_variable wake;
SDL_AudioDeviceID device=0;
uint32_t frequency=0,prefill_frames=0;
bool playing=false,playback_requested=false;
std::atomic<bool> output_unavailable{false};
uint64_t next_open_ns=0,retry_delay_ns=250000000,last_drain_ns=0;
uint32_t previous_queued_frames=0;
AudioDmaClock dma;
std::once_flag clock_started;
uint64_t generation=0;
std::atomic<uint64_t> sample_count{0},nonzero_count{0};
struct Phase {uint32_t rate=0,device_samples=0;uint64_t start=0,end=0,frames=0,completions=0,empty_submissions=0,max_queued=0,max_pending=0,rebufferings=0,backlog_resets=0,discarded_host_frames=0,open_attempts=0,open_failures=0,device_losses=0,recoveries=0,queue_failures=0,stalled_devices=0,offline_frames=0;};
std::vector<Phase> phases;
uint64_t now_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
// Callers hold the audio-state mutex. Retry deadlines use the same monotonic
// clock as DMA, but never sleep or cancel logical cartridge completions.
void close_output(){
    const auto& io=rs_audio_device_api();
    if(device){
        if(!phases.empty())phases.back().discarded_host_frames+=io.queued(device)/4;
        io.close(device);device=0;
    }
    playing=false;playback_requested=false;previous_queued_frames=0;
}
void unavailable(const char* reason,const char* detail=nullptr){
    if(!output_unavailable.exchange(true))
        std::fprintf(stderr,"Audio output unavailable (%s); retrying while game timing continues.%s%s\n",reason,detail&&*detail?" ":"",detail?detail:"");
}
void lose_output(uint64_t now,const char* reason,const char* detail=nullptr){
    std::string error=detail?detail:"";
    ++phases.back().device_losses;close_output();unavailable(reason,error.c_str());
    retry_delay_ns=250000000;next_open_ns=now+retry_delay_ns;
}
void open_output(uint64_t now){
    if(device || now<next_open_ns)return;
    const auto& io=rs_audio_device_api();auto& phase=phases.back();
    SDL_AudioSpec want{},have{};want.freq=frequency;want.format=AUDIO_S16SYS;want.channels=2;want.samples=256;
    ++phase.open_attempts;device=io.open(&want,&have);
    if(!device){
        ++phase.open_failures;unavailable("device open failed",SDL_GetError());
        next_open_ns=now+retry_delay_ns;retry_delay_ns=std::min<uint64_t>(retry_delay_ns*2,2000000000ULL);return;
    }
    // No SDL format changes are allowed. Only its device period may differ.
    if(have.freq!=int(frequency)||have.format!=AUDIO_S16SYS||have.channels!=2||!have.samples){
        close_output();rs_runtime_stop("unsupported_host_audio_format",frequency);
    }
    prefill_frames=std::max<uint32_t>(3,(frequency*60+1000*have.samples-1)/(1000*have.samples))*have.samples;
    phase.device_samples=have.samples;next_open_ns=0;retry_delay_ns=250000000;
    playing=false;playback_requested=false;previous_queued_frames=0;last_drain_ns=now;
    if(output_unavailable.exchange(false))++phase.recoveries;
    std::fprintf(stderr,"Native audio: %u Hz, stereo S16, %u-frame device period\n",frequency,have.samples);
}
void audio_clock() {
    std::unique_lock lock(mutex);
    for(;;) {
        wake.wait(lock,[]{return !dma.empty();});
        auto version=generation;auto target=dma.next();
        if(wake.wait_until(lock,Clock::time_point(std::chrono::nanoseconds(target)),[&]{return version!=generation;}))continue;
        dma.complete();
        if(!phases.empty())++phases.back().completions;
        lock.unlock();ultramodern::send_ai_message();lock.lock();
    }
}
}
void rs_audio_frequency(uint32_t rate){
    std::lock_guard lock(mutex);
    if(rate<1000||rate>384000)rs_runtime_stop("unsupported_audio_frequency",rate);
    if(rate==frequency)return;
    auto now=now_ns();if(!phases.empty())phases.back().end=now;
    if(frequency)dma.retime(now,frequency,rate);
    ++generation;wake.notify_all();close_output();
    frequency=rate;phases.push_back(Phase{rate,0,now});
    next_open_ns=0;retry_delay_ns=250000000;open_output(now);
    // Logical DMA starts even when the Mac has no working output device.
    std::call_once(clock_started,[]{std::thread(audio_clock).detach();});
}
void rs_audio_samples(int16_t* samples,size_t count){
    std::lock_guard lock(mutex);
    if(!frequency || count%2 || count>UINT32_MAX/2 || (count&&!samples))
        rs_runtime_stop("invalid_native_audio_buffer",(uint32_t)count);
    auto now=now_ns();auto frames=static_cast<uint32_t>(count/2);auto& phase=phases.back();
    const auto& io=rs_audio_device_api();
    open_output(now);
    if(device && (io.status(device)==SDL_AUDIO_STOPPED || (playing && io.status(device)!=SDL_AUDIO_PLAYING)))
        lose_output(now,"device stopped");
    if(device){
        auto queued_before=io.queued(device)/4;
        if(queued_before<previous_queued_frames)last_drain_ns=now;
        // SDL may report PLAYING even if the host driver failed to start.
        // Detect lack of queue consumption across backlog clears as well.
        if(playback_requested && now-last_drain_ns>1000000000ULL && queued_before>=prefill_frames){
            ++phase.stalled_devices;lose_output(now,"device stopped consuming samples");
        }else{
            if(playing && queued_before==0){
                ++phase.empty_submissions;++phase.rebufferings;
                io.pause(device,1);playing=false;playback_requested=false;
            }
            if(queued_before>std::max<uint32_t>(frequency/4,prefill_frames*4)){
                io.pause(device,1);io.clear(device);playing=false;
                ++phase.backlog_resets;phase.discarded_host_frames+=queued_before;queued_before=0;
            }
            std::vector<int16_t> stereo(count);
            for(size_t i=0;i<count;i+=2){stereo[i]=samples[i+1];stereo[i+1]=samples[i];}
            if(io.queue(device,stereo.data(),static_cast<Uint32>(stereo.size()*2))!=0){
                ++phase.queue_failures;lose_output(now,"device rejected samples",SDL_GetError());
            }else{
                previous_queued_frames=queued_before+frames;
                phase.max_queued=std::max<uint64_t>(phase.max_queued,previous_queued_frames);
                if(!playing && previous_queued_frames>=prefill_frames){
                    if(!playback_requested){last_drain_ns=now;playback_requested=true;}
                    io.pause(device,0);playing=true;
                }
            }
        }
    }
    if(!device)phase.offline_frames+=frames;
    // Output loss drops only host PCM. Every original buffer still gets exactly
    // one completion at the cartridge sample rate, so the game cannot deadlock.
    dma.enqueue(now,frames,frequency);phase.frames+=frames;
    phase.max_pending=std::max<uint64_t>(phase.max_pending,dma.pending());wake.notify_all();
    sample_count+=count;
    uint64_t nonzero=0;for(size_t i=0;i<count;++i)nonzero+=samples[i]!=0;nonzero_count+=nonzero;
}
size_t rs_audio_frames_remaining(){std::lock_guard lock(mutex);return device?rs_audio_device_api().queued(device)/4:0;}
bool rs_audio_output_unavailable(){return output_unavailable.load();}
bool rs_audio_dma_full(){std::lock_guard lock(mutex);return dma.pending()>=2;}
uint64_t rs_audio_sample_count(){return sample_count.load();}
uint64_t rs_audio_nonzero_count(){return nonzero_count.load();}
void rs_audio_write_report(const char* path){
    // stop() can be called from an audio error while this mutex is held.
    std::unique_lock lock(mutex,std::try_to_lock);if(!lock.owns_lock())return;
    FILE* out=std::fopen(path,"w");if(!out)return;
    std::fprintf(out,"{\"phases\":[");auto now=now_ns();
    for(size_t i=0;i<phases.size();++i){const auto& p=phases[i];
        std::fprintf(out,"%s{\"rate\":%u,\"device_period_frames\":%u,\"seconds\":%.6f,\"frames_queued\":%llu,\"dma_completions\":%llu,\"empty_submissions_after_playback_start\":%llu,\"max_queued_frames\":%llu,\"max_dma_pending\":%llu,\"rebufferings\":%llu,\"backlog_resets\":%llu,\"discarded_host_frames\":%llu",i?",":"",p.rate,p.device_samples,(p.end?p.end:now)-p.start>0?double((p.end?p.end:now)-p.start)/1e9:0.0,(unsigned long long)p.frames,(unsigned long long)p.completions,(unsigned long long)p.empty_submissions,(unsigned long long)p.max_queued,(unsigned long long)p.max_pending,(unsigned long long)p.rebufferings,(unsigned long long)p.backlog_resets,(unsigned long long)p.discarded_host_frames);
        std::fprintf(out,",\"device_open_attempts\":%llu,\"device_open_failures\":%llu,\"device_losses\":%llu,\"device_recoveries\":%llu,\"queue_failures\":%llu,\"stalled_devices\":%llu,\"offline_frames\":%llu}",(unsigned long long)p.open_attempts,(unsigned long long)p.open_failures,(unsigned long long)p.device_losses,(unsigned long long)p.recoveries,(unsigned long long)p.queue_failures,(unsigned long long)p.stalled_devices,(unsigned long long)p.offline_frames);
    }
    std::fprintf(out,"]}\n");std::fclose(out);
}
