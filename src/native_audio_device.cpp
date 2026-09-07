#include "native_audio_device.hpp"
const AudioDeviceApi& rs_audio_device_api(){
    static const AudioDeviceApi api{
        [](const SDL_AudioSpec* want,SDL_AudioSpec* have)->SDL_AudioDeviceID{
            if(!(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO)!=0)return 0;
            return SDL_OpenAudioDevice(nullptr,0,want,have,0);
        },
        SDL_CloseAudioDevice,SDL_GetQueuedAudioSize,SDL_GetAudioDeviceStatus,
        SDL_QueueAudio,SDL_PauseAudioDevice,SDL_ClearQueuedAudio
    };
    return api;
}
