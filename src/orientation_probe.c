/* Independent double-precision references for the cartridge's rotation
 * builders and player-facing composer. Executes original compiled functions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
#include "recomp.h"
#include "funcs.h"
#include "cpu_status.h"
static uint8_t* ram;
static unsigned cases,components;
static double max_error;
static uint32_t rng=0x19781998;
static gpr addr(uint32_t a){return (gpr)(int64_t)(int32_t)a;}
static uint32_t bits(float f){uint32_t u;memcpy(&u,&f,4);return u;}
void* rs_checked_memory(uint8_t* r,uint32_t a,unsigned n,unsigned swap) {
    uint32_t off=(a&0x1fffffff)^swap;
    if((a&0xff800000)!=0x80000000||off>0x800000-n||(off&(n-1)))abort();return r+off;
}
void cop0_status_write(recomp_context* c,gpr v){rs_set_cpu_status(c,v);}
gpr cop0_status_read(recomp_context* c){return c->status_reg;}
recomp_func_t* get_function(int32_t a){switch((uint32_t)a) {
    case 0x80018128:return main_80018128;case 0x800181b4:return main_800181B4;
    case 0x80018240:return main_80018240;case 0x80017edc:return main_80017EDC;
    case 0x80017f78:return main_80017F78;case 0x80018014:return main_80018014;
    case 0x80013c14:return main_80013C14;case 0x8002b2e0:return main_8002B2E0;
    case 0x80025000:return main_80025000;
    default:fprintf(stderr,"Unexpected orientation dependency %08x\n",(uint32_t)a);abort();
}}
static void axis_matrix(unsigned axis,double degrees,double* m) {
    const double angle=degrees*3.14159265358979323846/180.0,s=sin(angle),c=cos(angle);
    memset(m,0,9*sizeof(double));m[0]=m[4]=m[8]=1;
    if(axis==0){m[4]=m[8]=c;m[5]=-s;m[7]=s;}
    if(axis==1){m[0]=m[8]=c;m[2]=s;m[6]=-s;}
    if(axis==2){m[0]=m[4]=c;m[1]=-s;m[3]=s;}
}
static void multiply(const double* a,const double* b,double* out) {
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c){out[r*3+c]=0;for(unsigned k=0;k<3;++k)out[r*3+c]+=a[r*3+k]*b[k*3+c];}
}
static recomp_context context(void) {
    recomp_context c={0};c.r29=addr(0x807fff00);c.r16=0x12345678;
    c.f20.u64=0x1122334455667788ULL;c.f22.u64=0x8877665544332211ULL;
    rs_set_cpu_status(&c,0x2000ff01);return c;
}
static void validate_context(recomp_context* c) {
    if(c->r29!=addr(0x807fff00)||c->r16!=0x12345678||c->status_reg!=0x2000ff01||c->mips3_float_mode||
       c->f20.u64!=0x1122334455667788ULL||c->f22.u64!=0x8877665544332211ULL) {
        fprintf(stderr,"Orientation routine did not preserve its caller context\n");exit(1);
    }
}
static void compare(unsigned offset,double expected,const char* name) {
    float got;memcpy(&got,ram+0x200000+offset,4);double error=fabs((double)got-expected);
    if(!isfinite(got)||error>0.00001){fprintf(stderr,"%s +%x: got %.9g expected %.12g\n",name,offset,got,expected);exit(1);}
    if(error>max_error)max_error=error;++components;
}
static void compose(float x,float y,float z,int print) {
    double rx[9],ry[9],rz[9],yx[9],m[9];axis_matrix(0,x,rx);axis_matrix(1,y,ry);axis_matrix(2,z,rz);multiply(ry,rx,yx);multiply(rz,yx,m);
    recomp_context c=context();c.f_odd=&c.f0.u32h;c.r4=addr(0x80200000);c.r5=bits(x);c.r6=bits(y);c.r7=bits(z);
    memset(ram+0x200000,0x5a,48);main_800515D8(ram,&c);validate_context(&c);
    for(unsigned i=0;i<3;++i){compare(12+4*i,m[3*i+2],"Euler forward");compare(24+4*i,m[3*i+1],"Euler up");}
    for(unsigned i=0;i<12;++i)if(ram[0x200000+i]!=0x5a||ram[0x200000+36+i]!=0x5a){fprintf(stderr,"Composer changed position/velocity\n");exit(1);}
    ++cases;
    if(print){float* v=(float*)(ram+0x200000);printf("\"trench_forward\":[%.9g,%.9g,%.9g],\"trench_up\":[%.9g,%.9g,%.9g],",v[3],v[4],v[5],v[6],v[7],v[8]);}
}
int main(void) {
    ram=calloc(1,0x800000);if(!ram)return 2;
    FILE* f=fopen("roms/rogue_squadron.us.rev1.z64","rb");if(!f)return 2;
    fseek(f,0x1000,SEEK_SET);for(unsigned i=0;i<0x95ce0;++i){int b=fgetc(f);if(b==EOF)return 2;ram[(0x400+i)^3]=(uint8_t)b;}fclose(f);fesetround(FE_TONEAREST);
    float angles[]={-360,-180,-90,-1,0,1,90,180,341.039093f,359,360};
    recomp_func_t* constructors[]={main_80018128,main_800181B4,main_80018240};
    for(unsigned axis=0;axis<3;++axis)for(unsigned a=0;a<sizeof(angles)/sizeof(*angles);++a) {
        double ref[9];axis_matrix(axis,angles[a],ref);recomp_context c=context();c.f_odd=&c.f0.u32h;c.r4=addr(0x80200000);c.r5=bits(angles[a]);
        constructors[axis](ram,&c);validate_context(&c);for(unsigned i=0;i<12;++i)compare(4*i,i<9?ref[i]:0,"Axis rotation");++cases;
    }
    float grid[]={-180,-90,0,90,180};
    for(unsigned x=0;x<5;++x)for(unsigned y=0;y<5;++y)for(unsigned z=0;z<5;++z)compose(grid[x],grid[y],grid[z],0);
    for(unsigned i=0;i<100;++i){float a[3];for(unsigned j=0;j<3;++j){rng=rng*1664525u+1013904223u;a[j]=(int32_t)(rng%720001u)/1000.0f-360.0f;}compose(a[0],a[1],a[2],0);}
    printf("{");compose(341.039093f,331.521912f,343.709229f,1);
    printf("\"passed\":true,\"cases\":%u,\"components\":%u,\"max_absolute_error\":%.12g,\"reference\":\"double-precision Rz*Ry*Rx\"}\n",cases,components,max_error);free(ram);return 0;
}
