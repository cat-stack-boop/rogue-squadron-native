#include "test_controller.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>

unsigned checks=0;
void check(bool result,const char* name){++checks;if(!result){std::fprintf(stderr,"FAIL: %s\n",name);std::exit(1);}}
int main(){
    constexpr auto live=TestPlayState::live,paused=TestPlayState::paused,transition=TestPlayState::transition,unavailable=TestPlayState::unavailable;
    check(classify_test_play_state(true,0,0,false,false,150)==live,"original state zero is live");
    check(classify_test_play_state(true,5,0x101,false,false,150)==paused,"original state five plus freeze flag is paused");
    for(unsigned state:{1,2,3,4,7})check(classify_test_play_state(true,state,0,false,false,150)==transition,"death/result/intro transition is not live");
    check(classify_test_play_state(true,6,0,false,false,150)==TestPlayState::ended,"original state six ends the mission");
    check(classify_test_play_state(true,0,1,false,false,150)==transition,"frozen live state is not ready");
    check(classify_test_play_state(true,0,0x40,false,false,150)==TestPlayState::ended,"mission exit flag ends the mission");
    check(classify_test_play_state(true,0,0,true,false,150)==transition,"disabled input is not ready");
    check(classify_test_play_state(true,0,0,false,false,0)==transition,"dead player is not ready");
    check(classify_test_play_state(true,0,0,false,false,std::numeric_limits<float>::quiet_NaN())==transition,"invalid health is not ready");
    check(classify_test_play_state(true,5,0,false,false,150)==transition,"pause needs original freeze flag");
    check(classify_test_play_state(false,5,0x101,false,false,150)==unavailable,"other overlays unavailable");
    check(classify_test_play_state(true,0,0,false,true,150)==unavailable,"recorded demo unavailable");
    TestControllerCommand c;
    check(TestControllerCommand::parse("1 750 16384 0.25 -0.5 1",c),"valid firing and steering");
    for(const char* bad:{"", "1 0 0 0 0 1", "1 10001 0 0 0 1", "0 100 0 0 0 1", "-1 100 0 0 0 1", "1 100 4096 0 0 1", "1 100 65536 0 0 1", "1 100 0 1.01 0 1", "1 100 0 0 -1.01 1", "1 100 0 nan 0 1", "1 100 0 0 inf 1", "1 100 0 0 0 2", "1 100 0 0 0 1 junk", "1 100 0 0"})check(!TestControllerCommand::parse(bad,c),"malformed command rejected");
    TestController t;uint16_t b=42;float x=2,y=3;
    auto sample=[&](uint64_t now,TestPlayState state){return t.sample(now,b,x,y,state);};
    check(!sample(100,paused)&&b==42&&x==2&&y==3,"idle preserves keyboard");
    check(!t.start(c,100,unavailable),"cannot resume an unavailable menu");
    check(t.start(c,100,paused),"accept command at pause");
    check(!t.start(c,100,paused),"busy rejects replacement");
    check(sample(100,paused)&&b==0x8000&&x==0&&y==0,"resume uses original A");
    check(sample(220,paused)&&b==0,"resume released");
    check(sample(600,paused)&&b==0&&t.phase==TestController::resume_release,"wait for original resume state");
    check(sample(950,live)&&b==0x4000&&x==0.25f&&y==-0.5f,"active input begins after resume");
    check(sample(1699,live)&&b==0x4000,"input retained to deadline");
    check(sample(1700,live)&&b==0&&x==0&&y==0,"release before pause");
    check(t.active_finished-t.active_started==750,"active duration measured");
    check(sample(1899,live)&&b==0,"weapon release gap");
    check(sample(1900,live)&&b==0&&t.phase==TestController::wait_for_play,"finish release before checking live state");
    check(sample(1901,live)&&b==0x1000,"ordinary Start requested");
    check(sample(1934,paused)&&b==0&&t.pause_observed,"release Start when original pause is observed");
    check(sample(2134,paused)&&b==0&&!t.busy(),"stable pause completes sequence");
    check(!t.start(c,2200,paused),"stale command not replayed");
    c.id=2;c.resume=false;check(t.start(c,2200,live),"active without resume");
    check(sample(2400,transition)&&b==0&&t.interrupted,"death immediately releases controls");
    check(t.active_finished-t.active_started==200,"interrupted duration reported");
    check(sample(2600,transition)&&b==0,"death gets release gap");
    check(sample(10000,transition)&&b==0&&t.phase==TestController::wait_for_play,"no Start during death animation");
    check(sample(10001,live)&&b==0x1000,"Start sent when live play returns");
    check(sample(10050,paused)&&b==0&&t.pause_observed,"pause after death observed");
    check(sample(10250,paused)&&!t.busy(),"recovery completes only after pause");
    c.id=3;check(t.start(c,11000,live),"ignored Start test starts");
    for(uint64_t now=11000;now<16000 && t.busy();now+=10)sample(now,live);
    check(t.phase==TestController::failed&&t.pause_attempts==3&&!t.pause_observed&&b==0,"three ignored Start attempts fail with neutral input");
    check(std::string(t.failure_reason)=="pause_not_observed","ignored Start is reported");
    c.id=4;check(t.start(c,20000,live),"host stall test starts");
    check(sample(50000,live)&&b==0&&t.phase==TestController::release,"host stall releases input without skipping release");
    check(t.active_finished-t.active_started==30000,"host stall exposed in evidence");
    check(sample(80000,transition)&&b==0&&t.phase==TestController::failed,"unavailable recovery is bounded");
    check(std::string(t.failure_reason)=="pause_unavailable","unavailable recovery is reported");
    c.id=5;c.resume=true;check(t.start(c,90000,paused),"resume timeout test starts");
    sample(90120,paused);sample(95120,paused);
    check(t.phase==TestController::failed&&b==0&&t.active_started==0,"missing resume never applies flight controls");
    c.id=6;c.resume=false;check(t.start(c,96000,live),"external pause test starts");
    sample(96100,paused);sample(96300,paused);sample(96301,paused);sample(96501,paused);
    check(t.phase==TestController::complete&&t.pause_attempts==0&&t.pause_observed,"already paused game is not toggled");
    c.id=7;check(t.start(c,97000,live),"mission end test starts");
    sample(97100,TestPlayState::ended);
    check(t.phase==TestController::failed&&b==0&&t.interrupted&&t.pause_attempts==0&&t.active_finished==97100,"mission end cancels controls without waiting or sending Start");
    check(std::string(t.failure_reason)=="mission_ended","mission end is distinguished from unavailable pause");
    TestController stream;c.id=100;c.resume=false;c.duration_ms=500;
    check(stream.start(c,100000,live),"stream lease starts");
    c.id=101;c.x=-0.5f;check(stream.start(c,100100,live),"fresh live update renews lease");
    stream.sample(100599,b,x,y,live);check(b==c.buttons&&x==-0.5f,"renewed controls remain active");
    c.id=102;check(!stream.start(c,100600,live),"expired lease cannot be renewed");
    stream.sample(100600,b,x,y,live);check(b==0&&stream.phase==TestController::release,"lost producer releases controls");
    check(!stream.start(c,100650,live),"updates cannot interrupt recovery");
    stream.sample(100800,b,x,y,live);stream.sample(100801,b,x,y,live);
    check(b==0x1000,"lost producer requests normal pause");
    stream.sample(100850,b,x,y,paused);stream.sample(101050,b,x,y,paused);
    check(stream.phase==TestController::complete&&stream.pause_observed,"lost producer ends in observed pause");
    c.id=103;check(stream.start(c,102000,live),"new stream can start after recovery");
    c.id=104;c.duration_ms=1001;check(!stream.start(c,102100,live),"stream renewals are capped at one second");
    c.duration_ms=500;check(!stream.start(c,102100,transition),"stream cannot renew during transition");
    c.resume=true;check(!stream.start(c,102100,live),"resume commands cannot replace a live lease");
    std::printf("{\"checks\":%u,\"passed\":true}\n",checks);
}
