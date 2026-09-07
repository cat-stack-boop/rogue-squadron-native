/* Numerical check of the original projection/trigonometry code after ARM64
 * recompilation. Reference: standard perspective matrix, quantized to N64 16.16. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "recomp.h"
#include "funcs.h"
#include "cpu_status.h"
static uint8_t* ram;
static gpr addr(uint32_t x){return (gpr)(int64_t)(int32_t)x;}
void* rs_checked_memory(uint8_t* rdram,uint32_t a,unsigned n,unsigned swap){
    uint32_t off=(a&0x1fffffffu)^swap;
    if((a&0xff800000u)!=0x80000000u||off>0x800000-n||(off&(n-1)))abort();
    return rdram+off;
}
recomp_func_t* get_function(int32_t a){switch((uint32_t)a){
    case 0x8002701c:return main_8002701C;case 0x8002706c:return main_8002706C;
    case 0x80025130:return main_80025130;case 0x80025000:return main_80025000;
    case 0x8002b460:return main_8002B460;case 0x8002b2e0:return main_8002B2E0;
    case 0x80027104:return main_80027104;case 0x80026dc0:return main_80026DC0;
    case 0x800138b0:return main_800138B0;
    default:fprintf(stderr,"Unexpected projection dependency: %08x\n",a);abort();}}
void switch_error(const char*f,uint32_t a,uint32_t b){fprintf(stderr,"Switch error %s %x %x\n",f,a,b);abort();}
void do_break(uint32_t a){fprintf(stderr,"Break %x\n",a);abort();}
void cop0_status_write(recomp_context*c,gpr v){rs_set_cpu_status(c,v);}
gpr cop0_status_read(recomp_context*c){return c->status_reg;}
static uint32_t bits(float f){uint32_t u;memcpy(&u,&f,4);return u;}
static uint32_t word(unsigned offset){return *(uint32_t*)(ram+offset);}
int main(void){
    ram=calloc(1,0x800000);if(!ram)return 2;
    FILE* input=fopen("roms/rogue_squadron.us.rev1.z64","rb");if(!input)return 2;
    fseek(input,0x1000,SEEK_SET);
    for(unsigned i=0;i<0x95ce0;++i){int c=fgetc(input);if(c==EOF)return 2;ram[(0x400+i)^3]=(uint8_t)c;}
    fclose(input);
    float fovs[]={20,30,45,60,75,90,110,120,140};float aspects[]={1,4.0f/3,16.0f/9,2};
    unsigned cases=0;long long maximum_error=0;
    for(unsigned f=0;f<9;++f)for(unsigned a=0;a<4;++a)for(unsigned s=0;s<2;++s){
        float near=1,far=1000,scale=s?0.5f:1.0f;
        recomp_context ctx={0};ctx.f_odd=&ctx.f0.u32h;ctx.r29=addr(0x807fff00);
        ctx.r4=addr(0x80200000);ctx.r5=addr(0x80200100);ctx.r6=bits(fovs[f]);ctx.r7=bits(aspects[a]);
        *(uint32_t*)(ram+0x7fff10)=bits(near);*(uint32_t*)(ram+0x7fff14)=bits(far);*(uint32_t*)(ram+0x7fff18)=bits(scale);
        main_800271A0(ram,&ctx);
        double cot=1.0/tan(fovs[f]*3.14159265358979323846/360.0);
        double ref[16]={0};ref[0]=cot/aspects[a];ref[5]=cot;ref[10]=(near+far)/(near-far);ref[11]=-1;ref[14]=2.0*near*far/(near-far);
        for(unsigned n=0;n<16;++n){
            unsigned shift=n%2?0:16;
            uint32_t hi=(word(0x200000+(n/2)*4)>>shift)&0xffff,lo=(word(0x200020+(n/2)*4)>>shift)&0xffff;
            int32_t got=(int32_t)((hi<<16)|lo),expected=(int32_t)(ref[n]*scale*65536.0);
            long long delta=llabs((long long)got-expected);if(delta>maximum_error)maximum_error=delta;
            if(delta>8){fprintf(stderr,"Projection mismatch: fov=%g aspect=%g index=%u expected=%d got=%d\n",fovs[f],aspects[a],n,expected,got);return 1;}
        }
        if(ctx.r29!=addr(0x807fff00))return 1;
        ctx.r4=addr(0x80200400);ctx.r5=bits(-2.0f);ctx.r6=bits(0.25f);ctx.r7=bits(3.0f);
        main_8002AD10(ram,&ctx);
        ctx.r4=addr(0x80200000);ctx.r5=addr(0x80200400);ctx.r6=addr(0x80200500);
        main_80026CC0(ram,&ctx);
        double scaling[]={-2,0.25,3,1};
        for(unsigned n=0;n<16;++n){
            unsigned shift=n%2?0:16;
            uint32_t hi=(word(0x200500+(n/2)*4)>>shift)&0xffff,lo=(word(0x200520+(n/2)*4)>>shift)&0xffff;
            int32_t got=(int32_t)((hi<<16)|lo),expected=(int32_t)(ref[n]*scale*scaling[n%4]*65536.0);
            long long delta=llabs((long long)got-expected);if(delta>maximum_error)maximum_error=delta;
            if(delta>8){fprintf(stderr,"Concatenation mismatch fov=%g index=%u expected=%d got=%d\n",fovs[f],n,expected,got);return 1;}
        }
        if(ctx.r29!=addr(0x807fff00))return 1;
        ++cases;
    }
    float model[12]={1.0f,0.25f,-0.5f,-0.75f,1.0f,0.125f,0.5f,-0.25f,1.0f,12.25f,-7.5f,3.125f};
    for(unsigned i=0;i<12;++i)*(uint32_t*)(ram+0x201000+i*4)=bits(model[i]);
    recomp_context ctx={0};ctx.f_odd=&ctx.f0.u32h;ctx.r29=addr(0x807fff00);
    ctx.r31=addr(0x80501234);ctx.r4=addr(0x80201100);ctx.r5=addr(0x80201000);
    main_800137D0(ram,&ctx);
    float rotation_scale,translation_scale;
    uint32_t rs=word(0x1313c),ts=word(0x13140);memcpy(&rotation_scale,&rs,4);memcpy(&translation_scale,&ts,4);
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col){
        unsigned n=row*4+col,shift=n%2?0:16;
        uint32_t hi=(word(0x201100+(n/2)*4)>>shift)&0xffff,lo=(word(0x201120+(n/2)*4)>>shift)&0xffff;
        int32_t got=(int32_t)((hi<<16)|lo),expected;
        if(col==3)expected=row==3?65536:0;
        else expected=(int32_t)lrintf((row==3?model[9+col]:model[col*3+row])*(row==3?translation_scale:rotation_scale));
        if(got!=expected){fprintf(stderr,"Model conversion mismatch row=%u col=%u expected=%d got=%d\n",row,col,expected,got);return 1;}
    }
    unsigned affine_cases=0;
    for(unsigned test=0;test<64;++test){
        float m[12],v[3];
        for(unsigned i=0;i<12;++i){m[i]=((int)((test*13+i*7)%41)-20)*0.25f;*(uint32_t*)(ram+0x201000+i*4)=bits(m[i]);}
        for(unsigned i=0;i<3;++i){v[i]=((int)((test*5+i*11)%29)-14)*0.5f;*(uint32_t*)(ram+0x201200+i*4)=bits(v[i]);}
        recomp_context transform={0};transform.f_odd=&transform.f0.u32h;
        transform.status_reg=0x20000001;
        transform.r4=addr(0x80201000);transform.r5=addr(0x80201200);transform.r6=addr(0x80201210);
        main_80013E38(ram,&transform);
        for(unsigned row=0;row<3;++row){
            double expected=m[9+row];for(unsigned col=0;col<3;++col)expected+=(double)m[row*3+col]*v[col];
            uint32_t output=word(0x201210+row*4);float got;memcpy(&got,&output,4);
            if(!isfinite(got)||fabs(got-expected)>0.00001){fprintf(stderr,"Affine transform mismatch case=%u row=%u expected=%g got=%g\n",test,row,expected,got);return 1;}
        }
        if(transform.status_reg!=0x20000001||transform.mips3_float_mode||transform.f_odd!=&transform.f0.u32h){fprintf(stderr,"Affine transform did not restore FR mode\n");return 1;}
        ++affine_cases;
    }
    printf("{\"projection_cases\":%u,\"concatenation_cases\":%u,\"model_conversion_cases\":1,\"affine_transform_cases\":%u,\"matrix_elements_checked\":%u,\"affine_components_checked\":%u,\"maximum_16_16_error\":%lld,\"game_booted\":false}\n",cases,cases,affine_cases,cases*32+16,affine_cases*3,maximum_error);
    free(ram);return 0;
}
