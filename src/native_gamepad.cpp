#include "native_gamepad.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
struct ButtonBinding {SDL_GameControllerButton button;uint16_t n64;};
// SDL's positional convention: south=A, east=B, west=X, north=Y.
constexpr ButtonBinding bindings[]={
    {SDL_CONTROLLER_BUTTON_A,0x8000}, // thrust / menu confirm
    {SDL_CONTROLLER_BUTTON_B,0x4000}, // blasters / menu back
    {SDL_CONTROLLER_BUTTON_X,0x0002}, // secondary weapon
    {SDL_CONTROLLER_BUTTON_Y,0x0001}, // craft special
    {SDL_CONTROLLER_BUTTON_START,0x1000},
    {SDL_CONTROLLER_BUTTON_BACK,0x0004}, // blaster linking
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,0x0020}, // camera
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0x0010}, // roll
    {SDL_CONTROLLER_BUTTON_RIGHTSTICK,0x0008}, // look + left stick
    {SDL_CONTROLLER_BUTTON_DPAD_UP,0x0800},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN,0x0400},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,0x0200},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT,0x0100}
};
constexpr int trigger_threshold=8192;
float axis(Sint16 value){return value<0?value/32768.f:value/32767.f;}
void stick(Sint16 raw_x,Sint16 raw_y,float& x,float& y){
    x=axis(raw_x);y=-axis(raw_y); // N64 positive Y is up.
    float length=std::hypot(x,y);
    constexpr float deadzone=0.15f;
    if(length<=deadzone){x=y=0;return;}
    float scale=(std::min(length,1.f)-deadzone)/((1.f-deadzone)*length);
    x*=scale;y*=scale;
}
}

NativeGamepad::~NativeGamepad(){
    close_controller();
    if(initialized)SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}
bool NativeGamepad::initialize(){
    if(initialized)return true;
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS,"0");
    if(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)!=0){
        std::fprintf(stderr,"Gamepad initialization failed; keyboard remains available: %s\n",SDL_GetError());
        return false;
    }
    initialized=true;open_available();return true;
}
void NativeGamepad::open_available(){
    if(!initialized || controller)return;
    for(int i=0;i<SDL_NumJoysticks();++i){
        if(!SDL_IsGameController(i))continue;
        controller=SDL_GameControllerOpen(i);
        if(!controller)continue;
        instance=SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
        const char* name=SDL_GameControllerName(controller);
        std::fprintf(stderr,"Gamepad connected: %s (instance %d)\n",name?name:"unnamed",instance);
        return;
    }
}
void NativeGamepad::close_controller(){
    if(controller){SDL_GameControllerClose(controller);controller=nullptr;}
    instance=-1;pulses.clear();
}
void NativeGamepad::set_focus(bool value){
    focused=value;
    if(!focused)pulses.clear();
}
void NativeGamepad::handle_event(const SDL_Event& event,uint64_t now){
    if(event.type==SDL_CONTROLLERDEVICEADDED){open_available();return;}
    if(event.type==SDL_CONTROLLERDEVICEREMOVED && event.cdevice.which==instance){
        std::fprintf(stderr,"Gamepad disconnected (instance %d)\n",instance);
        close_controller();open_available();return;
    }
    if(!focused || !controller)return;
    if(event.type==SDL_CONTROLLERBUTTONDOWN && event.cbutton.which==instance){
        for(auto binding:bindings)if(event.cbutton.button==binding.button)pulses.press(binding.n64,now);
    }
    if(event.type==SDL_CONTROLLERAXISMOTION && event.caxis.which==instance && event.caxis.value>trigger_threshold){
        if(event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERLEFT)pulses.press(0x2000,now);
        if(event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERRIGHT)pulses.press(0x4000,now);
    }
}
GamepadInput NativeGamepad::sample(uint64_t now){
    // Check attachment as well as events so a missed removal never leaves a
    // held fire/thrust button in the state delivered to the original game.
    if(controller && !SDL_GameControllerGetAttached(controller)){close_controller();open_available();}
    if(!focused || !controller)return {};
    uint16_t held=0;
    for(auto binding:bindings)if(SDL_GameControllerGetButton(controller,binding.button))held|=binding.n64;
    if(SDL_GameControllerGetAxis(controller,SDL_CONTROLLER_AXIS_TRIGGERLEFT)>trigger_threshold)held|=0x2000;
    if(SDL_GameControllerGetAxis(controller,SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>trigger_threshold)held|=0x4000;
    GamepadInput input;input.buttons=static_cast<uint16_t>(pulses.sample(held,now));
    stick(SDL_GameControllerGetAxis(controller,SDL_CONTROLLER_AXIS_LEFTX),
          SDL_GameControllerGetAxis(controller,SDL_CONTROLLER_AXIS_LEFTY),input.x,input.y);
    return input;
}
