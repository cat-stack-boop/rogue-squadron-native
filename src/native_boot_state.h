#pragma once
#include <stdint.h>
#include <string.h>
#include "cpu_status.h"

// Game-visible side effects of Rev 1 osInitialize (80025e20..80026034).
// Native services replace PIF/cache operations; the cartridge globals still
// need the same values that its original timing and reset code expects.
static inline void rs_initialize_boot_state(uint8_t* rdram,recomp_context* ctx,uint32_t cartridge_clock) {
    *(uint32_t*)(rdram+0x11eca0)=1;
    rs_set_cpu_status(ctx,ctx->status_reg|0x20000000u);
    set_cop1_cs(0x01000800u);
    for(unsigned destination=0;destination<=0x180;destination+=0x80)
        memcpy(rdram+destination,rdram+0x253a0,16);
    uint32_t* clock=(uint32_t*)(rdram+0x300a0);
    uint64_t rate=((uint64_t)clock[0]<<32)|clock[1];
    if(cartridge_clock&~15u)rate=cartridge_clock&~15u;
    rate=(rate*3u)>>2;
    clock[0]=(uint32_t)(rate>>32);clock[1]=(uint32_t)rate;
    if(*(uint32_t*)(rdram+0x30c)==0)memset(rdram+0x31c,0,64);
    uint32_t tv=*(uint32_t*)(rdram+0x300);
    *(uint32_t*)(rdram+0x300a8)=tv==0?0x02f5b2d2u:tv==2?0x02e6025cu:0x02e6d354u;
}
