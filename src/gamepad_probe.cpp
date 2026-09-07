// Exercise the production adapter through actual SDL virtual-device events and
// polling. This verifies the host input path, not physical USB/Bluetooth hardware.
#include "native_gamepad.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks=0;
uint64_t now=1000;
void require(bool pass,const char* label){
    if(!pass){std::fprintf(stderr,"FAIL: %s (%s)\n",label,SDL_GetError());std::exit(1);}
    ++checks;
}
void pump(NativeGamepad& pad){
    SDL_GameControllerUpdate();
    SDL_Event event;
    while(SDL_PollEvent(&event))pad.handle_event(event,now);
}
void neutral(const GamepadInput& input,const char* label){require(!input.buttons && input.x==0 && input.y==0,label);}
struct VirtualPad {
    SDL_Joystick* joystick;
    SDL_JoystickID id;
    VirtualPad(){
        SDL_VirtualJoystickDesc desc{};
        desc.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;desc.type=SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes=SDL_CONTROLLER_AXIS_MAX;desc.nbuttons=SDL_CONTROLLER_BUTTON_MAX;
        desc.axis_mask=(1u<<SDL_CONTROLLER_AXIS_MAX)-1;desc.button_mask=(1u<<SDL_CONTROLLER_BUTTON_MAX)-1;
        desc.name="Rogue native input regression";
        int index=SDL_JoystickAttachVirtualEx(&desc);
        require(index>=0,"SDL attaches virtual controller");
        joystick=SDL_JoystickOpen(index);require(joystick!=nullptr,"SDL opens virtual controller");
        id=SDL_JoystickInstanceID(joystick);
        axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768);axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768);
    }
    void axis(int which,Sint16 value){require(SDL_JoystickSetVirtualAxis(joystick,which,value)==0,"virtual axis accepted");}
    void button(int which,bool down){require(SDL_JoystickSetVirtualButton(joystick,which,down?1:0)==0,"virtual button accepted");}
    void detach(){
        for(int i=0;i<SDL_NumJoysticks();++i)if(SDL_JoystickGetDeviceInstanceID(i)==id){
            require(SDL_JoystickDetachVirtual(i)==0,"SDL detaches virtual controller");break;
        }
        SDL_JoystickClose(joystick);joystick=nullptr;
    }
    ~VirtualPad(){if(joystick)detach();}
};
}

int main(){
    // Do not read any of the user's physical controllers during this test.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI,"0");
    SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT,"0");
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI,"0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    {
        NativeGamepad pad;require(pad.initialize(),"production gamepad subsystem initializes");
        require(pad.selected_instance()==-1,"physical devices excluded from test");
        neutral(pad.sample(now),"no controller produces neutral input");
        VirtualPad first;pump(pad);
        require(pad.selected_instance()==first.id,"hotplug selects new controller");
        neutral(pad.sample(now),"resting controller does not fire or steer");

        // Confirm/cancel and combat controls must reach the original N64 masks.
        struct Expected {int button;uint16_t n64;const char* purpose;};
        for(auto test:{Expected{SDL_CONTROLLER_BUTTON_A,0x8000,"south confirms menus / thrusts"},
                      Expected{SDL_CONTROLLER_BUTTON_B,0x4000,"east cancels menus / fires"},
                      Expected{SDL_CONTROLLER_BUTTON_X,2,"west fires secondary weapon"},
                      Expected{SDL_CONTROLLER_BUTTON_Y,1,"north operates craft special"},
                      Expected{SDL_CONTROLLER_BUTTON_START,0x1000,"start pauses"},
                      Expected{SDL_CONTROLLER_BUTTON_BACK,4,"back switches weapon linking"},
                      Expected{SDL_CONTROLLER_BUTTON_LEFTSHOULDER,0x20,"left shoulder switches camera"},
                      Expected{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0x10,"right shoulder rolls"},
                      Expected{SDL_CONTROLLER_BUTTON_RIGHTSTICK,8,"right stick click enables look"},
                      Expected{SDL_CONTROLLER_BUTTON_DPAD_UP,0x800,"d-pad up reaches original shortcut"},
                      Expected{SDL_CONTROLLER_BUTTON_DPAD_DOWN,0x400,"d-pad down reaches original shortcut"},
                      Expected{SDL_CONTROLLER_BUTTON_DPAD_LEFT,0x200,"d-pad left reaches original shortcut"},
                      Expected{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,0x100,"d-pad right reaches original shortcut"}}){
            first.button(test.button,true);pump(pad);
            require(pad.sample(now).buttons==test.n64,test.purpose);
            first.button(test.button,false);pump(pad);now+=33;
            require(pad.sample(now).buttons==test.n64,"brief press survives original game's input polling");
            now+=100;neutral(pad.sample(now),"released button expires without sticking");
        }

        first.axis(SDL_CONTROLLER_AXIS_LEFTX,2000);first.axis(SDL_CONTROLLER_AXIS_LEFTY,-2000);pump(pad);
        neutral(pad.sample(now),"center drift stays in radial dead zone");
        first.axis(SDL_CONTROLLER_AXIS_LEFTX,32767);first.axis(SDL_CONTROLLER_AXIS_LEFTY,0);pump(pad);
        auto input=pad.sample(now);require(std::abs(input.x-1)<1e-6 && input.y==0,"full right retains full scale");
        first.axis(SDL_CONTROLLER_AXIS_LEFTX,-32768);pump(pad);
        require(std::abs(pad.sample(now).x+1)<1e-6,"full left retains full scale");
        first.axis(SDL_CONTROLLER_AXIS_LEFTX,0);first.axis(SDL_CONTROLLER_AXIS_LEFTY,-32768);pump(pad);
        require(std::abs(pad.sample(now).y-1)<1e-6,"SDL up becomes N64 positive Y");
        first.axis(SDL_CONTROLLER_AXIS_LEFTY,32767);pump(pad);
        require(std::abs(pad.sample(now).y+1)<1e-6,"SDL down becomes N64 negative Y");
        first.axis(SDL_CONTROLLER_AXIS_LEFTX,32767);pump(pad);input=pad.sample(now);
        require(input.x>0 && input.y<0 && std::abs(std::hypot(input.x,input.y)-1)<1e-6,"diagonal input stays within unit circle");
        first.axis(SDL_CONTROLLER_AXIS_LEFTY,0);
        float previous=0;
        for(int raw:{0,4900,6000,10000,16000,24000,32767}){
            first.axis(SDL_CONTROLLER_AXIS_LEFTX,raw);pump(pad);input=pad.sample(now);
            require(input.x>=previous && input.x<=1 && input.y==0,"analog steering increases smoothly after dead zone");
            previous=input.x;
        }
        first.axis(SDL_CONTROLLER_AXIS_LEFTX,0);
        first.axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT,32767);first.axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,32767);pump(pad);
        require(pad.sample(now).buttons==0x6000,"both triggers brake and fire simultaneously");
        first.axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768);first.axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768);pump(pad);now+=100;
        neutral(pad.sample(now),"released triggers return to neutral");

        first.button(SDL_CONTROLLER_BUTTON_A,true);first.axis(SDL_CONTROLLER_AXIS_LEFTX,24000);pump(pad);
        pad.set_focus(false);neutral(pad.sample(now),"focus loss immediately releases held thrust and steering");
        first.button(SDL_CONTROLLER_BUTTON_X,true);pump(pad);
        neutral(pad.sample(now),"background button events cannot control game");
        first.button(SDL_CONTROLLER_BUTTON_A,false);first.button(SDL_CONTROLLER_BUTTON_X,false);
        first.axis(SDL_CONTROLLER_AXIS_LEFTX,0);pump(pad);pad.set_focus(true);
        neutral(pad.sample(now),"refocus does not replay background taps");

        VirtualPad spare;pump(pad);
        require(pad.selected_instance()==first.id,"connecting another device does not steal control");
        spare.button(SDL_CONTROLLER_BUTTON_B,true);pump(pad);
        neutral(pad.sample(now),"unselected device cannot fire");
        spare.button(SDL_CONTROLLER_BUTTON_B,false);pump(pad);
        first.button(SDL_CONTROLLER_BUTTON_A,true);pump(pad);first.detach();pump(pad);
        require(pad.selected_instance()==spare.id,"unplug selects remaining controller");
        neutral(pad.sample(now),"unplug clears old device's held and retained buttons");
        spare.button(SDL_CONTROLLER_BUTTON_B,true);pump(pad);
        require(pad.sample(now).buttons==0x4000,"replacement controller can fire");
        spare.detach();
        neutral(pad.sample(now),"attachment check releases input before removal event is read");
        pump(pad);require(pad.selected_instance()==-1,"last unplug removes controller");
        VirtualPad reconnected;pump(pad);
        require(pad.selected_instance()==reconnected.id,"controller reconnect works without app restart");
        neutral(pad.sample(now),"reconnect has no stale input");
    }
    SDL_Quit();
    std::printf("{\"passed\":true,\"checks\":%u,\"backend\":\"SDL virtual game controller\",\"physical_hardware_verified\":false}\n",checks);
}
