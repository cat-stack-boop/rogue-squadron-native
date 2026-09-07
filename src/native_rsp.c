/* Direct native implementation of the game's identified MusyX v1 task.
 * Only mixer code is linked: no RSP interpreter, plugin fallback, or unknown-task
 * success path. Completion notification belongs to ultramodern's task worker. */
#include "native_rsp.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "hle_internal.h"
#include "hle_external.h"
#include "memory.h"
static struct hle_t hle;
static uint8_t dmem[4096],imem[4096];
static unsigned status;
static atomic_uint completed;
static uint32_t active_task[16];
extern void rs_hle_abort(const char*,unsigned);
unsigned rs_hle_checked_dram(unsigned address,size_t bytes,unsigned alignment){
    unsigned physical=address&0xffffff;
    if(bytes>0x800000 || physical>0x800000-bytes || (physical&(alignment-1)))
        rs_hle_abort("native_rsp_dram_extent",address);
    return physical;
}
_Noreturn void rs_hle_voice_error(const char* reason,unsigned address){
    fprintf(stderr,"MusyX descriptor failure %s at %08x; task:",reason,address);
    for(unsigned i=0;i<16;++i)fprintf(stderr," %08x",active_task[i]);
    fprintf(stderr,"\nDescriptor bytes:");
    unsigned start=(address&0xffffff)&~15u;
    if(start<=0x800000-0x60)for(unsigned i=0;i<0x60;++i)fprintf(stderr,"%02x",hle.dram[(start+i)^3]);
    fprintf(stderr,"\n");rs_hle_abort(reason,address);__builtin_unreachable();
}
void rsp_break(struct hle_t* state,unsigned bits){*state->sp_status|=bits|SP_STATUS_HALT|SP_STATUS_BROKE;}
void HleVerboseMessage(void* user,const char* format,...){(void)user;(void)format;}
void HleWarnMessage(void* user,const char* format,...){
    (void)user;va_list args;va_start(args,format);fputs("MusyX warning: ",stderr);vfprintf(stderr,format,args);fputc('\n',stderr);va_end(args);
}
void HleInfoMessage(void* user,const char* format,...){(void)user;(void)format;}
void HleErrorMessage(void* user,const char* format,...){
    (void)user;va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);rs_hle_abort("native_musyx_error",0);
}
unsigned rs_musyx_count(void){return atomic_load(&completed);}
int rs_run_musyx(uint8_t* ram,const uint32_t* task){
    if(task[0]!=2)return 0;
    uint32_t microdata=task[6]&0xffffffu,commands=task[12]&0xffffffu,count=task[13];
    if(microdata>0x800000-0x14||commands>0x800000-0xa10||count==0||count>64)return 0;
    if((uint64_t)commands+(uint64_t)count*0xa10>0x800000)return 0;
    uint32_t first=*(uint32_t*)(ram+microdata),signature=*(uint32_t*)(ram+microdata+0x10);
    if(first==1||signature!=1){fprintf(stderr,"Unknown audio microcode: data=%08x first=%08x signature=%08x\n",microdata,first,signature);return 0;}
    hle.dram=ram;hle.dmem=dmem;hle.imem=imem;hle.sp_status=&status;status=0;
    uint32_t words[16];memcpy(words,task,64);
    unsigned indexes[]={2,4,6,8,10,11,12,14};
    for(unsigned i=0;i<8;++i)words[indexes[i]]&=0x1fffffffu;
    memcpy(dmem+0xfc0,words,64);
    memcpy(active_task,words,64);
    musyx_v1_task(&hle);
    if(!(status&SP_STATUS_TASKDONE))return 0;
    unsigned n=atomic_fetch_add(&completed,1)+1;
    if(n<=4)fprintf(stderr,"Native MusyX v1 completed: task %u, %u subframes\n",n,count);
    return 1;
}
