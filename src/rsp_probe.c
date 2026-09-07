#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include "native_rsp.h"
static jmp_buf failure;
static const char* reason;
static unsigned fault_address;
_Noreturn void rs_hle_abort(const char* why,unsigned address){reason=why;fault_address=address;longjmp(failure,1);}
static uint8_t* ram;
static void word(unsigned p,uint32_t v){*(uint32_t*)(ram+p)=v;}
static void half(unsigned p,uint16_t v){*(uint16_t*)(ram+(p^2))=v;}
static void byte(unsigned p,uint8_t v){ram[p^3]=v;}
static void setup(void){memset(ram,0,0x800000);word(0x410,1);word(0x1008,0x3000);word(0x1054,0x5000);}
int main(void){
    ram=malloc(0x800000);if(!ram)return 2;
    uint32_t task[16]={0};task[0]=2;task[6]=0x400;task[12]=0x1000;task[13]=1;
    setup();memset(ram+0x5000,0xa5,768);
    if(setjmp(failure)||!rs_run_musyx(ram,task))return 1;
    for(unsigned i=0;i<768;++i)if(ram[0x5000+i]!=0)return 1;
    setup();half(0x103c,20);byte(0x104c,29);word(0x1050,0x4000);
    if(!setjmp(failure)){rs_run_musyx(ram,task);return 1;}
    if(strcmp(reason,"musyx_adpcm_sample_extent")||fault_address!=0x1010)return 1;
    setup();half(0x103c,0xfd35);byte(0x104c,1);word(0x1050,0x4000);word(0x1034,0x6000);
    if(!setjmp(failure)){rs_run_musyx(ram,task);return 1;}
    if(strcmp(reason,"musyx_adpcm_copy_overrun")||fault_address!=0x1034)return 1;
    setup();word(0x1008,0x900000);
    if(!setjmp(failure)){rs_run_musyx(ram,task);return 1;}
    if(strcmp(reason,"native_rsp_dram_extent"))return 1;
    puts("{\"passed\":true,\"cases\":4,\"tests\":\"native MusyX output and malformed descriptor rejection\"}");free(ram);
}
