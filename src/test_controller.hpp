#pragma once
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

enum class TestPlayState {unavailable,transition,live,paused,ended};
inline TestPlayState classify_test_play_state(bool mission,unsigned state,unsigned flags,
                                              bool disabled,bool demo,float health){
    if(!mission || demo)return TestPlayState::unavailable;
    if(state==6 || (flags&0x40))return TestPlayState::ended;
    if(state==5 && (flags&1))return TestPlayState::paused;
    if(state==0 && !(flags&1) && !disabled && std::isfinite(health) && health>0)return TestPlayState::live;
    return TestPlayState::transition;
}

// Diagnostic controller input only. The original game handles every button;
// no position, health, pause flag or objective is changed by this helper.
struct TestControllerCommand {
    uint64_t id=0;
    unsigned duration_ms=0;
    uint16_t buttons=0;
    float x=0,y=0;
    bool resume=true;
    static bool parse(const std::string& text,TestControllerCommand& result) {
        if(text.size()>160)return false;
        std::istringstream in(text);
        TestControllerCommand next;unsigned buttons,resume;std::string extra;
        if(!(in>>next.id>>next.duration_ms>>buttons>>next.x>>next.y>>resume) || (in>>extra))return false;
        // Start belongs to the automatic pause sequence. Reserved/D-pad bits
        // are omitted until their behavior has been tested independently.
        constexpr unsigned allowed=0xE03F;
        if(!next.id || next.id>INT64_MAX || next.duration_ms<1 || next.duration_ms>10000 ||
           (buttons&~allowed) || resume>1 || !std::isfinite(next.x) || !std::isfinite(next.y) ||
           std::abs(next.x)>1 || std::abs(next.y)>1)return false;
        next.buttons=static_cast<uint16_t>(buttons);next.resume=resume!=0;result=next;return true;
    }
};

class TestController {
public:
    enum Phase {idle,resume_press,resume_release,active,release,wait_for_play,pause_press,pause_response,settle,complete,failed};
    TestControllerCommand command;
    Phase phase=idle;
    uint64_t phase_started=0,active_started=0,active_finished=0;
    uint64_t recovery_started=0;
    unsigned pause_attempts=0;
    bool pause_observed=false,interrupted=false;
    const char* failure_reason="";
    bool busy()const{return phase!=idle && phase!=complete && phase!=failed;}
    const char* phase_name()const{
        const char* names[]={"idle","resume","resume_release","active","release","waiting_for_play","pause_requested","pause_response","settle","complete","failed"};
        return names[phase];
    }
    bool start(const TestControllerCommand& next,uint64_t now,TestPlayState play){
        if(next.id<=command.id)return false;
        if(busy()){
            // An explicitly live update can renew a short controller lease.
            // Late updates cannot revive an expired lease or interrupt pause
            // recovery; loss of the producer still releases and pauses.
            if(phase!=active || next.resume || next.duration_ms>1000 || play!=TestPlayState::live ||
               now-phase_started>=command.duration_ms)return false;
            command=next;phase_started=now;active_started=now;active_finished=0;return true;
        }
        if((next.resume && play!=TestPlayState::paused) || (!next.resume && play!=TestPlayState::live))return false;
        command=next;phase=next.resume?resume_press:active;phase_started=now;
        active_started=next.resume?0:now;active_finished=0;recovery_started=0;
        pause_attempts=0;pause_observed=false;interrupted=false;failure_reason="";return true;
    }
    bool sample(uint64_t now,uint16_t& buttons,float& x,float& y,TestPlayState play){
        if(!busy())return false;
        auto enter=[&](Phase next){phase=next;phase_started=now;};
        auto fail=[&](const char* reason){failure_reason=reason;enter(failed);};
        const uint64_t elapsed=now-phase_started;
        if(play==TestPlayState::ended){
            if(phase==active){active_finished=now;interrupted=true;}
            fail("mission_ended");
        }else if(recovery_started && now-recovery_started>=30000 && play!=TestPlayState::paused){
            fail("pause_unavailable");
        }
        switch(phase){
            case resume_press:if(elapsed>=120)enter(resume_release);break;
            case resume_release:
                // The original ECC1C routine changes state 5 back to 0 when
                // resume is accepted. A fixed logo delay cannot prove this.
                if(elapsed>=200 && play==TestPlayState::live){enter(active);active_started=now;}
                else if(elapsed>=5000)fail("resume_not_observed");
                break;
            case active:
                if(play!=TestPlayState::live || elapsed>=command.duration_ms){
                    active_finished=now;recovery_started=now;
                    interrupted=play!=TestPlayState::live;enter(release);
                }
                break;
            case release:if(elapsed>=200)enter(wait_for_play);break;
            case wait_for_play:
                if(play==TestPlayState::paused){pause_observed=true;enter(settle);}
                else if(play==TestPlayState::live){
                    if(pause_attempts>=3)fail("pause_not_observed");
                    else{++pause_attempts;enter(pause_press);}
                }
                break;
            case pause_press:
                if(play==TestPlayState::paused){pause_observed=true;enter(settle);}
                else if(play!=TestPlayState::live)enter(release);
                else if(elapsed>=120)enter(pause_response);
                break;
            case pause_response:
                if(play==TestPlayState::paused){pause_observed=true;enter(settle);}
                else if(elapsed>=750)enter(wait_for_play);
                break;
            case settle:
                if(play!=TestPlayState::paused){pause_observed=false;enter(wait_for_play);}
                else if(elapsed>=200)enter(complete);
                break;
            default:break;
        }
        buttons=0;x=0;y=0;
        if(phase==resume_press)buttons=0x8000;
        if(phase==active){buttons=command.buttons;x=command.x;y=command.y;}
        if(phase==pause_press)buttons=0x1000;
        return true;
    }
};
