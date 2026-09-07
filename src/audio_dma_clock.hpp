#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>

// Logical AI DMA completion times, in monotonic nanoseconds. Host device
// buffering supplies a small playback lead; it must not dictate game VI timing.
class AudioDmaClock {
    std::deque<uint64_t> deadlines;
    uint64_t last_end=0;
public:
    void reset(){deadlines.clear();last_end=0;}
    void retime(uint64_t now,uint32_t old_rate,uint32_t new_rate) {
        // Changing AI's DAC rate changes the duration of the remaining samples;
        // it does not cancel buffers or their completion interrupts.
        for(auto& end:deadlines) {
            uint64_t remaining=end>now?end-now:0;
            end=now+(remaining*old_rate+new_rate/2)/new_rate;
        }
        last_end=deadlines.empty()?0:deadlines.back();
    }
    void enqueue(uint64_t now,uint32_t frames,uint32_t rate) {
        if(last_end==0 || now>last_end+100000000ULL)last_end=now;
        last_end+=(uint64_t(frames)*1000000000ULL+rate/2)/rate;
        deadlines.push_back(last_end);
    }
    bool empty()const{return deadlines.empty();}
    size_t pending()const{return deadlines.size();}
    uint64_t next()const{return deadlines.front();}
    void complete(){deadlines.pop_front();}
};
