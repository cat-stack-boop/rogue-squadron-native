// Compare native initialization against the statically compiled cartridge
// routine, stubbing only hardware I/O and coherent-cache operations.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include "recomp.h"
#include "funcs.h"
#include "native_boot_state.h"
static uint32_t boot_clock;
static unsigned cases;
static gpr address(uint32_t a){return (gpr)(int64_t)(int32_t)a;}
void* rs_checked_memory(uint8_t* r,uint32_t a,unsigned n,unsigned swap) {
    uint32_t off=(a&0x1fffffff)^swap;
    if((a&0xff800000)!=0x80000000||off>0x800000-n||(off&(n-1)))abort();
    return r+off;
}
static void read_status(uint8_t* r,recomp_context* c){(void)r;c->r2=c->status_reg;}
static void write_status(uint8_t* r,recomp_context* c){(void)r;rs_set_cpu_status(c,c->r4);}
static void write_fpcsr(uint8_t* r,recomp_context* c){(void)r;set_cop1_cs(c->r4);}
static void read_pif(uint8_t* rdram,recomp_context* c){MEM_W(0,c->r5)=0x42;c->r2=0;}
static void write_pif(uint8_t* r,recomp_context* c){(void)r;if((uint32_t)c->r4!=0x1fc007fc||c->r5!=0x4a)abort();c->r2=0;}
static void read_pi(uint8_t* rdram,recomp_context* c){if(c->r4!=4)abort();MEM_W(0,c->r5)=boot_clock;c->r2=0;}
static void clear_memory(uint8_t* r,recomp_context* c){memset(r+((uint32_t)c->r4&0x1fffffff),0,c->r5);}
static void coherent_cache(uint8_t* r,recomp_context* c){(void)r;(void)c;}
recomp_func_t* get_function(int32_t a){switch((uint32_t)a) {
    case 0x80026040:return read_status;case 0x8002b090:return write_status;
    case 0x8002af90:return write_fpcsr;case 0x8002b530:return read_pif;
    case 0x8002b5e0:return write_pif;case 0x8002cbb0:case 0x80026140:return coherent_cache;
    case 0x8002aaf0:return read_pi;case 0x800241f0:return clear_memory;
    default:fprintf(stderr,"Unexpected original boot dependency %08x\n",(uint32_t)a);abort();
}}
int main(void) {
    uint8_t* original=malloc(0x800000);uint8_t* native=malloc(0x800000);
    if(!original||!native)return 2;
    const uint32_t headers[]={0,15,62500000,0x0500000f};
    const uint64_t defaults[]={62500000,0x10000002aULL,UINT64_MAX};
    for(unsigned h=0;h<4;++h)for(unsigned d=0;d<3;++d)
    for(unsigned tv=0;tv<3;++tv)for(unsigned reset=0;reset<2;++reset)for(unsigned fr=0;fr<2;++fr) {
        memset(original,0xcc,0x800000);
        *(uint32_t*)(original+0x300)=tv;*(uint32_t*)(original+0x30c)=reset;
        *(uint32_t*)(original+0x300a0)=(uint32_t)(defaults[d]>>32);
        *(uint32_t*)(original+0x300a4)=(uint32_t)defaults[d];
        for(unsigned i=0;i<16;++i)original[0x253a0+i]=(uint8_t)(i^0xa5);
        memcpy(native,original,0x800000);
        recomp_context a={0},b={0};a.r29=b.r29=address(0x807fff00);
        rs_set_cpu_status(&a,1u|(fr?0x04000000u:0));rs_set_cpu_status(&b,a.status_reg);
        boot_clock=headers[h];fesetround(FE_DOWNWARD);
        main_80025E20(original,&a);
        if(fegetround()!=FE_TONEAREST)return 1;
        fesetround(FE_UPWARD);rs_initialize_boot_state(native,&b,headers[h]);
        unsigned offsets[]={0,0x80,0x100,0x180,0x11eca0,0x300a0,0x31c};
        unsigned sizes[]={16,16,16,16,4,12,64};
        for(unsigned i=0;i<7;++i)if(memcmp(original+offsets[i],native+offsets[i],sizes[i])) {
            fprintf(stderr,"Boot state mismatch at %x, header %x, tv %u, reset %u\n",offsets[i],headers[h],tv,reset);return 1;
        }
        if(a.r29!=b.r29||a.status_reg!=b.status_reg||a.mips3_float_mode!=b.mips3_float_mode||fegetround()!=FE_TONEAREST)return 1;
        ++cases;
    }
    printf("{\"passed\":true,\"cases\":%u,\"original_routine\":\"0x80025e20\",\"counter_rate_before\":62500000,\"counter_rate_after\":46875000,\"seconds_reported_for_one_native_second_before\":0.75,\"seconds_reported_after\":1.0}\n",cases);
    free(original);free(native);return 0;
}
