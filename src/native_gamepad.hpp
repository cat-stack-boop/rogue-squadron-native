#pragma once
#include "keyboard_pulses.hpp"
#include <SDL.h>
#include <cstdint>

struct GamepadInput {
    uint16_t buttons=0;
    float x=0, y=0;
};

// SDL calls stay on the window/event thread. The frontend publishes the sampled
// controller state to the original game's SI input callback.
class NativeGamepad {
    SDL_GameController* controller=nullptr;
    SDL_JoystickID instance=-1;
    KeyboardPulses pulses;
    bool initialized=false, focused=true;
    void open_available();
    void close_controller();
public:
    NativeGamepad()=default;
    NativeGamepad(const NativeGamepad&)=delete;
    NativeGamepad& operator=(const NativeGamepad&)=delete;
    ~NativeGamepad();
    bool initialize();
    void set_focus(bool value);
    void handle_event(const SDL_Event&,uint64_t now);
    GamepadInput sample(uint64_t now);
    SDL_JoystickID selected_instance()const{return instance;}
};
