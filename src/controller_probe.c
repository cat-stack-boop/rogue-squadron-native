#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "recomp.h"
#include "funcs.h"
static uint8_t* ram;
void* rs_checked_memory(uint8_t* r,uint32_t a,unsigned n,unsigned swap){unsigned off=(a&0x1fffffff)^swap;if((a&0xff800000)!=0x80000000||off>0x800000-n)abort();return r+off;}
recomp_func_t* get_function(int32_t a){switch((uint32_t)a){case 0x8006ebb8:return main_8006EBB8;case 0x8006ebf8:return main_8006EBF8;case 0x800b04b8:return mission_800B04B8;default:fprintf(stderr,"Unexpected input dependency %08x\n",a);abort();}}
void switch_error(const char*f,uint32_t a,uint32_t b){abort();}
void do_break(uint32_t a){abort();}
int main(){
    ram=calloc(1,0x800000);FILE* file=fopen("roms/rogue_squadron.us.rev1.z64","rb");if(!file)return 2;
    fseek(file,0x1000,SEEK_SET);for(unsigned i=0;i<0x95ce0;++i)ram[(0x400+i)^3]=fgetc(file);fclose(file);
    int values[]={-82,0,82};unsigned cases=0;
    for(unsigned port=0;port<4;++port)for(unsigned x=0;x<3;++x)for(unsigned y=0;y<3;++y){
        unsigned pad=0x121758+port*6;ram[(pad+2)^3]=(uint8_t)values[x];ram[(pad+3)^3]=(uint8_t)values[y];ram[(pad+4)^3]=1;
        *(uint16_t*)(ram+(pad^2))=0x4004;
        recomp_context ctx={0};ctx.f_odd=&ctx.f0.u32h;ctx.r29=(gpr)(int64_t)(int32_t)0x807fff00;ctx.r4=port;
        main_8006EBB8(ram,&ctx);if(fabsf(ctx.f0.fl-values[x]/128.0f)>0.00001f)return 1;
        ctx.r4=port;main_8006EBF8(ram,&ctx);if(fabsf(ctx.f0.fl-values[y]/128.0f)>0.00001f)return 1;
        ctx.r4=port;main_8006E8A4(ram,&ctx);
        uint32_t expected=0x4004|(values[x]<0?0x800000:values[x]>0?0x400000:0)|(values[y]<0?0x100000:values[y]>0?0x200000:0);
        if((uint32_t)ctx.r2!=expected){fprintf(stderr,"Input mismatch port=%u x=%d y=%d expected=%08x got=%08x\n",port,values[x],values[y],expected,(uint32_t)ctx.r2);return 1;}
        ++cases;
    }
    file=fopen("roms/rogue_squadron.us.rev1.z64","rb");if(!file)return 2;
    fseek(file,0x96ce0,SEEK_SET);for(unsigned i=0;i<0x671e0;++i){int b=fgetc(file);if(b==EOF)return 2;ram[(0x960e0+i)^3]=(uint8_t)b;}fclose(file);
    const int flight_values[]={-127,-82,-20,-2,-1,0,1,2,20,82,127};unsigned flight_cases=0;
    for(unsigned state=0;state<=5;state+=5)for(unsigned port=0;port<4;++port)for(unsigned x=0;x<11;++x)for(unsigned y=0;y<11;++y) {
        memset(ram+0xfd9c8,0,0x200);memset(ram+0x121758,0,24);
        *(uint32_t*)(ram+0xfe610)=state;*(uint32_t*)(ram+0x121720)=0;ram[0x121726^3]=0;
        unsigned pad=0x121758+6*port;ram[(pad+4)^3]=1;ram[(pad+2)^3]=(uint8_t)flight_values[x];ram[(pad+3)^3]=(uint8_t)flight_values[y];
        recomp_context ctx={0};ctx.f_odd=&ctx.f0.u32h;ctx.r29=(gpr)(int64_t)(int32_t)0x807fff00;ctx.f12.fl=1.0f/60;
        mission_800B065C(ram,&ctx);
        float expected[2]={fmaxf(-1,fminf(1,flight_values[x]/75.0f)),fmaxf(-1,fminf(1,flight_values[y]/80.0f))};
        for(unsigned axis=0;axis<2;++axis){if(fabsf(expected[axis])<0.015f)expected[axis]=0;float actual;memcpy(&actual,ram+0xfd9c8+8*port+4*axis,4);if(fabsf(actual-expected[axis])>0.000001f){fprintf(stderr,"Flight axis mismatch port %u axis %u: %g != %g\n",port,axis,actual,expected[axis]);return 1;}}
        ++flight_cases;
    }
    for(unsigned disabled=0;disabled<2;++disabled) {
        memset(ram+0xfd9c8,0,0x200);memset(ram+0x121758,0,24);ram[(0x121758+4)^3]=1;
        *(uint32_t*)(ram+0xfe610)=disabled?5:7;ram[0x121726^3]=disabled;
        ram[(0x121758+2)^3]=82;ram[(0x121758+3)^3]=82;*(uint16_t*)(ram+(0x121758^2))=0x5000;
        recomp_context ctx={0};ctx.f_odd=&ctx.f0.u32h;ctx.r29=(gpr)(int64_t)(int32_t)0x807fff00;ctx.f12.fl=1.0f/60;
        mission_800B065C(ram,&ctx);
        if(*(uint32_t*)(ram+0xfd9c8)||*(uint32_t*)(ram+0xfd9cc)||*(uint16_t*)(ram+(0x121758^2)))return 1;
        ++flight_cases;
    }
    memset(ram+0xfd9c8,0,0x200);memset(ram+0x121758,0,24);ram[(0x121758+4)^3]=1;
    *(uint32_t*)(ram+0xfe610)=5;ram[0x121726^3]=0;*(uint16_t*)(ram+(0x121758^2))=0x5000;
    *(uint16_t*)(ram+(0xfdba0^2))=0x1000;*(uint16_t*)(ram+(0xfdb98^2))=0x1000;
    recomp_context ctx={0};ctx.f_odd=&ctx.f0.u32h;ctx.r29=(gpr)(int64_t)(int32_t)0x807fff00;ctx.f12.fl=1.0f/60;mission_800B065C(ram,&ctx);
    if(*(uint16_t*)(ram+(0x121758^2))!=0x4000)return 1;++flight_cases;
    printf("{\"passed\":true,\"original_input_cases\":%u,\"original_flight_cases\":%u,\"fine_input_survives_deadzone\":true}\n",cases,flight_cases);free(ram);
}
