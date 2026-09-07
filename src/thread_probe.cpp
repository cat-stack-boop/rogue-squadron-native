#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/rsp.hpp"
#include "lightweightsemaphore.h"
#include "native_event_config.hpp"

// Exercise the same receive primitive as osRecvMesg without starting OS threads.
bool do_recv(uint8_t*,int32_t,int32_t,bool);

std::atomic_bool exited{false};
moodycamel::LightweightSemaphore graphics_shutdown_ready;
namespace ultramodern { bool is_game_started(){return false;} }
void run_thread_function(uint8_t*,uint64_t,uint64_t,uint64_t) {
    throw std::runtime_error("Queue probe must not spawn game threads");
}
static unsigned checks;
static unsigned audio_callbacks;
static uint32_t consumed_audio_word;
static void require(bool value,const char* what) {
    if(!value){std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}
    ++checks;
}
int main() {
    std::vector<uint8_t> ram(8*1024*1024);
    uint8_t* rdram=ram.data();
    const int32_t a=(int32_t)0x80001000,b=(int32_t)0x80002000,c=(int32_t)0x80003000;
    const int32_t q=(int32_t)0x80000400;
    auto thread=[&](int32_t p){return reinterpret_cast<OSThread*>(rdram+(uint32_t(p)&0x1fffffff));};
    auto empty=[&](int32_t queue){return ultramodern::thread_queue_empty(rdram,queue);};
    auto pop=[&](int32_t queue){return ultramodern::thread_queue_pop(rdram,queue);};
    for(int32_t queue:{q,ultramodern::running_queue}) {
        require(empty(queue),"queue starts empty");
        require(!ultramodern::thread_queue_remove(rdram,queue,a),"remove absent from empty");
        thread(a)->priority=5;thread(b)->priority=9;thread(c)->priority=5;
        for(int32_t p:{a,b,c})ultramodern::schedule_running_thread(rdram,p);
        // The running queue uses a host head; normal message queues use RDRAM.
        if(queue==q)for(int i=0;i<3;++i)ultramodern::thread_queue_insert(rdram,q,pop(ultramodern::running_queue));
        require(pop(queue)==b,"highest priority first");
        require(pop(queue)==a,"equal priorities preserve arrival order");
        require(pop(queue)==c,"second equal-priority thread follows");
        for(int32_t p:{a,b,c})ultramodern::thread_queue_insert(rdram,queue,p);
        require(ultramodern::thread_queue_remove(rdram,queue,a),"remove middle");
        require(thread(b)->next==c,"middle removal retains tail");
        require(!ultramodern::thread_queue_remove(rdram,queue,a),"remove absent nonempty");
        require(ultramodern::thread_queue_remove(rdram,queue,c),"remove tail");
        require(ultramodern::thread_queue_remove(rdram,queue,b),"remove head");
        require(empty(queue),"queue empty after removals");
        thread(a)->state=OSThreadState::QUEUED;
        thread(b)->state=OSThreadState::QUEUED;
        ultramodern::thread_queue_insert(rdram,queue,a);
        ultramodern::thread_queue_insert(rdram,queue,b);
        osStopThread(rdram,a);
        require(thread(a)->state==OSThreadState::STOPPED,"stop marks queued thread stopped");
        require(thread(b)->next==0,"stop removes queued thread");
        osStopThread(rdram,a);
        require(pop(queue)==b,"repeated stop leaves remaining thread intact");
        require(empty(queue),"no stopped thread remains runnable");
    }
    ultramodern::set_message_queue_control(rs_message_queue_control());
    const int32_t mq=(int32_t)0x80005000,slots=(int32_t)0x80005080,out=(int32_t)0x80005084;
    auto* queue=reinterpret_cast<OSMesgQueue*>(rdram+0x5000);
    for(auto source:{ultramodern::EventMessageSource::Timer,ultramodern::EventMessageSource::Sp,
                    ultramodern::EventMessageSource::Si,ultramodern::EventMessageSource::Ai,
                    ultramodern::EventMessageSource::Pi,ultramodern::EventMessageSource::Dp}) {
        osCreateMesgQueue(rdram,mq,slots,1);
        ultramodern::enqueue_external_message(mq,0x1111,false,false);
        ultramodern::wait_for_external_message_timed(rdram,0);
        ultramodern::enqueue_external_message_src(mq,0x2222,false,source);
        ultramodern::wait_for_external_message_timed(rdram,0);
        require(queue->validCount==1,"full queue retains first message");
        require(do_recv(rdram,mq,out,false)&&*reinterpret_cast<int32_t*>(rdram+0x5084)==0x1111,"consume original message");
        ultramodern::wait_for_external_message_timed(rdram,0);
        require(do_recv(rdram,mq,out,false)&&*reinterpret_cast<int32_t*>(rdram+0x5084)==0x2222,"deferred hardware completion arrives");
        ultramodern::wait_for_external_message_timed(rdram,0);
        require(queue->validCount==0,"completion delivered exactly once");
    }
    osCreateMesgQueue(rdram,mq,slots,1);
    osSetEventMesg(rdram,OS_EVENT_SP,mq,0x3333);
    ultramodern::rsp::set_callbacks({nullptr,[](uint8_t* memory,const OSTask* task) {
        ++audio_callbacks;
        consumed_audio_word=*reinterpret_cast<uint32_t*>(memory+(uint32_t(task->t.data_ptr)&0x1fffffff));
        return true;
    }});
    auto* task=reinterpret_cast<OSTask*>(rdram+0x6000);
    task->t.type=M_AUDTASK;task->t.data_ptr=(int32_t)0x80007000;task->t.data_size=1;
    *reinterpret_cast<uint32_t*>(rdram+0x7000)=0x12345678;
    ultramodern::submit_rsp_task(rdram,(int32_t)0x80006000);
    require(audio_callbacks==1,"audio payload consumed before submission returns");
    *reinterpret_cast<uint32_t*>(rdram+0x7000)=0x69657870; // Heap reused for texture names.
    task->t.data_ptr=0;
    require(consumed_audio_word==0x12345678,"later heap reuse cannot alter consumed audio");
    ultramodern::wait_for_external_message_timed(rdram,0);
    require(do_recv(rdram,mq,out,false)&&*reinterpret_cast<int32_t*>(rdram+0x5084)==0x3333,"audio completion follows payload consumption");
    ultramodern::wait_for_external_message_timed(rdram,0);
    require(queue->validCount==0&&audio_callbacks==1,"native audio completes exactly once");
    std::printf("{\"passed\":true,\"checks\":%u,\"tests\":\"native queues, thread stopping, completion delivery and audio payload lifetime\"}\n",checks);
}
