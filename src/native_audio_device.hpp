#pragma once
#include <SDL.h>

// The app links the real SDL implementation. The recovery probe links a fault
// injecting implementation around SDL's dummy device, without shipping hooks.
struct AudioDeviceApi {
    SDL_AudioDeviceID (*open)(const SDL_AudioSpec*,SDL_AudioSpec*);
    void (*close)(SDL_AudioDeviceID);
    Uint32 (*queued)(SDL_AudioDeviceID);
    SDL_AudioStatus (*status)(SDL_AudioDeviceID);
    int (*queue)(SDL_AudioDeviceID,const void*,Uint32);
    void (*pause)(SDL_AudioDeviceID,int);
    void (*clear)(SDL_AudioDeviceID);
};
const AudioDeviceApi& rs_audio_device_api();
