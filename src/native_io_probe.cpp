#include "audio_dma_clock.hpp"
#include "keyboard_pulses.hpp"
#include <cstdio>
#include <cstdlib>

static unsigned checks=0;
static void require(bool good,const char* name){if(!good){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}++checks;}
int main(){
    for(uint32_t rate:{22047u,32006u,48000u}){
        AudioDmaClock clock;uint64_t start=1000000000ULL;clock.enqueue(start,192,rate);
        unsigned completions=0;uint64_t end=start+1000000000ULL;
        while(clock.next()<=end){
            auto due=clock.next();clock.complete();++completions;
            // Native scheduling delay must not accumulate into a slower clock.
            clock.enqueue(due+500000,192,rate);
        }
        require(completions==rate/192,"DMA completion count follows sample rate");
        require(completions>60,"DMA clock is independent of 60-Hz VI");
        require(clock.pending()==1,"one buffer remains scheduled");
        clock.reset();require(clock.empty(),"explicit reset clears pending buffers");
        clock.enqueue(7000000000ULL,192,rate);
        require(clock.next()>7000000000ULL,"restarted stream has a new clock origin");
    }
    {
        AudioDmaClock clock;uint64_t start=1000000000ULL;
        clock.enqueue(start,192,22047);clock.enqueue(start+1000000,192,22047);
        auto original_first=clock.next();auto change=start+4000000;
        clock.retime(change,22047,32006);
        auto first=clock.next();
        require(clock.pending()==2,"rate change preserves queued DMA completions");
        require(first>change&&first<original_first,"higher rate shortens only remaining duration");
        clock.complete();auto second=clock.next();
        require(second>first,"retimed FIFO order is preserved");
        auto expected=(192ULL*1000000000ULL+16003)/32006;
        require(second-first>=expected-1&&second-first<=expected+1,"following buffer has new-rate duration");
        clock.retime(second+10000000,32006,22047);
        require(clock.pending()==1&&clock.next()==second+10000000,"overdue completion is retained");
        clock.complete();require(clock.empty(),"both preserved completions can finish");
    }
    KeyboardPulses pulses;
    require(pulses.sample(0,0)==0,"no phantom keys initially");
    pulses.press(0x8000,5);
    for(uint64_t now:{5u,16u,33u,66u,84u})require(pulses.sample(0,now)==0x8000,"brief tap survives delayed game polling");
    require(pulses.sample(0,85)==0,"tap expires without a stuck key");
    require(pulses.sample(0x8000,300)==0x8000,"physical hold remains held");
    pulses.press(0x8000,100);pulses.press(0x1000,140);
    require(pulses.sample(0,190)==0x1000,"different keys expire independently");
    pulses.press(1u<<16,200);require(pulses.sample(0,240)&(1u<<16),"arrow taps are retained");
    pulses.clear();require(pulses.sample(0,241)==0,"focus loss clears pulses");
    std::printf("{\"passed\":true,\"checks\":%u,\"tests\":\"sample-clock DMA and keyboard tap retention\"}\n",checks);
}
