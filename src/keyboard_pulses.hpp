#pragma once
#include <array>
#include <cstdint>

// Preserve brief desktop taps across both the SI polling thread and the game's
// slower input consumer. Physical holds still last until the key is released.
class KeyboardPulses {
    std::array<uint64_t,32> until{};
public:
    static constexpr uint64_t minimum_ms=80;
    void press(uint32_t mask,uint64_t now) {
        for(unsigned i=0;i<32;++i)if(mask&(1u<<i))until[i]=now+minimum_ms;
    }
    uint32_t sample(uint32_t held,uint64_t now)const {
        for(unsigned i=0;i<32;++i)if(now<until[i])held|=1u<<i;
        return held;
    }
    void clear(){until.fill(0);}
};
