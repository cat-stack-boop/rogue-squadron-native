/* Native SDL/OpenGL frontend for GLideN64's graphics-only plugin API.
 * The game CPU is statically compiled elsewhere; no emulator core is loaded.
 */
#include "native_video.hpp"
#include "native_paths.hpp"
#include "keyboard_pulses.hpp"
#include "native_gamepad.hpp"
#include "native_audio.hpp"
#include "test_controller.hpp"
#include <SDL.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#define M64P_CORE_PROTOTYPES
#include "m64p_config.h"
#include "m64p_vidext.h"
#include "m64p_plugin.h"

namespace {
SDL_Window* window;
SDL_GLContext gl_context;
std::atomic<unsigned> tasks{0}, frames{0};
std::atomic<uint16_t> controller_buttons{0};
std::atomic<bool> requested_start_pulse{false};
KeyboardPulses keyboard_pulses;
NativeGamepad gamepad;
bool input_focused=false;
constexpr uint32_t stick_right=1u<<16,stick_left=1u<<17,stick_up=1u<<18,stick_down=1u<<19;
constexpr uint32_t fine_steering=1u<<20;
struct KeyBinding {SDL_Scancode key;uint32_t mask;};
constexpr KeyBinding key_bindings[]={
    {SDL_SCANCODE_Z,0x8000},{SDL_SCANCODE_X,0x4000},{SDL_SCANCODE_SPACE,0x2000},{SDL_SCANCODE_RETURN,0x1000},
    {SDL_SCANCODE_A,0x20},{SDL_SCANCODE_S,0x10},{SDL_SCANCODE_I,8},{SDL_SCANCODE_K,4},
    {SDL_SCANCODE_J,2},{SDL_SCANCODE_L,1},{SDL_SCANCODE_RIGHT,stick_right},{SDL_SCANCODE_LEFT,stick_left},
    {SDL_SCANCODE_UP,stick_up},{SDL_SCANCODE_DOWN,stick_down},
    {SDL_SCANCODE_LSHIFT,fine_steering},{SDL_SCANCODE_RSHIFT,fine_steering}};
std::atomic<float> controller_x{0},controller_y{0};
TestController test_controller;
// One packed atomic keeps the observation and its timestamp consistent across
// the original game thread and SDL event thread. Stale observations fail closed.
std::atomic<uint64_t> test_play_observation{0};
void sample_test_controller(uint64_t now,uint16_t& buttons,float& x,float& y){
    static const bool enabled=[] {
        const char* value=std::getenv("ROGUE_TEST_CONTROLLER");
        return rs_paths().diagnostics && value && std::strcmp(value,"1")==0;
    }();
    if(!enabled)return;
    uint64_t observation=test_play_observation.load();
    // The game thread may publish a timestamp just after this poll sampled now.
    TestPlayState play=now<=(observation>>3)+500?static_cast<TestPlayState>(observation&7):TestPlayState::unavailable;
    static uint64_t last_check=0,last_seen=0;
    static uint64_t rejected_id=0;
    static int reported_phase=-1;
    if(now-last_check>=30){
        last_check=now;std::ifstream in(rs_report_path("controller-command.txt"));
        char text[162]{};
        if(in.getline(text,sizeof text)){
            TestControllerCommand command;
            if(TestControllerCommand::parse(text,command) && command.id>last_seen){
                last_seen=command.id;
                if(!test_controller.start(command,now,play)){
                    rejected_id=command.id;
                    std::fprintf(stderr,"Diagnostic controller command %llu rejected: busy or wrong play state\n",(unsigned long long)command.id);
                }
                reported_phase=-1;
            }
        }
    }
    test_controller.sample(now,buttons,x,y,play);
    if(reported_phase!=test_controller.phase){
        reported_phase=test_controller.phase;
        auto path=rs_report_path("controller-state.json"),temporary=rs_report_path("controller-state.json.tmp");
        {std::ofstream out(temporary);out<<"{\"id\":"<<test_controller.command.id<<",\"phase\":\""<<test_controller.phase_name()
          <<"\",\"requested_duration_ms\":"<<test_controller.command.duration_ms
          <<",\"active_started_ms\":"<<test_controller.active_started<<",\"active_finished_ms\":"<<test_controller.active_finished
          <<",\"rejected_id\":"<<rejected_id<<",\"pause_attempts\":"<<test_controller.pause_attempts
          <<",\"interrupted\":"<<(test_controller.interrupted?"true":"false")
          <<",\"pause_state_observed\":"<<(test_controller.pause_observed?"true":"false")
          <<",\"failure_reason\":\""<<test_controller.failure_reason<<"\""
          <<",\"pause_verified\":false}\n";}
        std::error_code error;std::filesystem::rename(temporary,path,error);
    }
}
// Logical window dimensions belong to Cocoa's event thread. Drawable pixel
// dimensions are published separately for GLideN64 on the renderer thread.
uint64_t pack_size(int w,int h){return (uint64_t(uint32_t(w))<<32)|uint32_t(h);}
std::atomic<uint64_t> requested_window_size{0},drawable_size{0};
std::atomic<bool> toggle_fullscreen{false};
uint8_t header[64], dmem[4096], imem[4096];
unsigned ram_bytes=8*1024*1024, sp_status=0x203, mi_intr=0, dpc[8]{};
using Section=std::map<std::string,std::string>;
using Ini=std::map<std::string,Section>;
Ini config;
std::string data_dir, ini_dir;
std::mutex config_lock;
std::mutex register_command_lock;
std::vector<uint32_t> register_commands;
std::atomic<uint32_t> counter_snapshot[4]{};
void apply_dp_commands(){
    std::vector<uint32_t> pending;
    {std::lock_guard lock(register_command_lock);pending.swap(register_commands);}
    for(uint32_t flags:pending){
        for(unsigned bit=0;bit<3;++bit){
            bool clear=flags&(1u<<(bit*2)),set=flags&(2u<<(bit*2));
            if(clear!=set){if(set)dpc[3]|=1u<<bit;else dpc[3]&=~(1u<<bit);}
        }
        if(flags&0x200)dpc[4]=0;if(flags&0x100)dpc[5]=0;if(flags&0x80)dpc[6]=0;if(flags&0x40)dpc[7]=0;
    }
    for(unsigned i=0;i<4;++i)counter_snapshot[i]=dpc[4+i];
}

std::string trim(std::string s) {
    auto first=s.find_first_not_of(" \r\n\t");
    if(first==std::string::npos) return {};
    auto last=s.find_last_not_of(" \r\n\t");
    return s.substr(first,last-first+1);
}
Ini read_ini(const char* path) {
    Ini result; std::ifstream file(path); std::string line,section;
    while(std::getline(file,line)) {
        line=trim(line);
        if(line.empty() || line[0]=='#' || line[0]==';') continue;
        if(line.front()=='[' && line.back()==']') { section=line.substr(1,line.size()-2); continue; }
        auto eq=line.find('='); if(eq==std::string::npos) continue;
        result[section][trim(line.substr(0,eq))]=trim(line.substr(eq+1));
    }
    return result;
}
Section& section(m64p_handle h) { return *static_cast<Section*>(h); }
m64p_error set_default(m64p_handle h,const char* n,std::string v) {
    if(!h || !n) return M64ERR_INPUT_ASSERT;
    section(h).try_emplace(n,std::move(v)); return M64ERR_SUCCESS;
}
const std::string& get(m64p_handle h,const char* n) {
    static const std::string empty;
    if(!h || !n) return empty;
    auto it=section(h).find(n); return it==section(h).end()?empty:it->second;
}
void debug(void*,int level,const char* msg) { std::fprintf(stderr,"GLideN64[%d]: %s\n",level,msg); }
void check_interrupts() { /* ultramodern completes SP/DP tasks after send_dl returns. */ }
SDL_GLattr gl_attribute(m64p_GLattr attr) {
    switch(attr) {
        case M64P_GL_DOUBLEBUFFER:return SDL_GL_DOUBLEBUFFER;
        case M64P_GL_BUFFER_SIZE:return SDL_GL_BUFFER_SIZE;
        case M64P_GL_DEPTH_SIZE:return SDL_GL_DEPTH_SIZE;
        case M64P_GL_RED_SIZE:return SDL_GL_RED_SIZE;
        case M64P_GL_GREEN_SIZE:return SDL_GL_GREEN_SIZE;
        case M64P_GL_BLUE_SIZE:return SDL_GL_BLUE_SIZE;
        case M64P_GL_ALPHA_SIZE:return SDL_GL_ALPHA_SIZE;
        case M64P_GL_MULTISAMPLEBUFFERS:return SDL_GL_MULTISAMPLEBUFFERS;
        case M64P_GL_MULTISAMPLESAMPLES:return SDL_GL_MULTISAMPLESAMPLES;
        case M64P_GL_CONTEXT_MAJOR_VERSION:return SDL_GL_CONTEXT_MAJOR_VERSION;
        case M64P_GL_CONTEXT_MINOR_VERSION:return SDL_GL_CONTEXT_MINOR_VERSION;
        default:return SDL_GL_CONTEXT_PROFILE_MASK;
    }
}
}

extern "C" {
EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type* type,int* ver,int* api,const char** name,int* caps) {
    if(type)*type=M64PLUGIN_CORE; if(ver)*ver=0x020600; if(api)*api=0x020100;
    if(name)*name="Rogue Squadron native host"; if(caps)*caps=0; return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL ConfigOpenSection(const char* n,m64p_handle* h) {
    if(!n || !h)return M64ERR_INPUT_ASSERT; *h=&config[n]; return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL ConfigDeleteSection(const char* n) {config[n].clear();return M64ERR_SUCCESS;}
EXPORT m64p_error CALL ConfigSaveFile() {
    std::ofstream out(data_dir+"/settings.ini"); if(!out)return M64ERR_FILES;
    for(auto& [name,values]:config) {out<<'['<<name<<"]\n";for(auto& [k,v]:values)out<<k<<'='<<v<<'\n';}
    return out?M64ERR_SUCCESS:M64ERR_FILES;
}
EXPORT m64p_error CALL ConfigSaveSection(const char*) {return ConfigSaveFile();}
EXPORT m64p_error CALL ConfigSetDefaultInt(m64p_handle h,const char* n,int v,const char*) {return set_default(h,n,std::to_string(v));}
EXPORT m64p_error CALL ConfigSetDefaultBool(m64p_handle h,const char* n,int v,const char*) {return set_default(h,n,std::to_string(v));}
EXPORT m64p_error CALL ConfigSetDefaultFloat(m64p_handle h,const char* n,float v,const char*) {return set_default(h,n,std::to_string(v));}
EXPORT m64p_error CALL ConfigSetDefaultString(m64p_handle h,const char* n,const char* v,const char*) {return set_default(h,n,v?v:"");}
EXPORT int CALL ConfigGetParamInt(m64p_handle h,const char* n) {try{return std::stoi(get(h,n));}catch(...){return 0;}}
EXPORT int CALL ConfigGetParamBool(m64p_handle h,const char* n) {return ConfigGetParamInt(h,n)!=0;}
EXPORT float CALL ConfigGetParamFloat(m64p_handle h,const char* n) {try{return std::stof(get(h,n));}catch(...){return 0;}}
EXPORT const char* CALL ConfigGetParamString(m64p_handle h,const char* n) {return get(h,n).c_str();}
EXPORT const char* CALL ConfigGetUserConfigPath(){return data_dir.c_str();}
EXPORT const char* CALL ConfigGetUserDataPath(){return data_dir.c_str();}
EXPORT const char* CALL ConfigGetUserCachePath(){return data_dir.c_str();}
EXPORT const char* CALL ConfigGetSharedDataFilepath(const char* file) {
    static thread_local std::string path;path=ini_dir+"/"+file;return path.c_str();
}
EXPORT m64p_error CALL ConfigExternalOpen(const char* path,m64p_handle* out) {
    if(!path||!out)return M64ERR_INPUT_ASSERT;
    if(!std::filesystem::is_regular_file(path))return M64ERR_FILES;
    *out=new Ini(read_ini(path));return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL ConfigExternalClose(m64p_handle h){delete static_cast<Ini*>(h);return M64ERR_SUCCESS;}
EXPORT m64p_error CALL ConfigExternalGetParameter(m64p_handle h,const char* s,const char* p,char* out,int len) {
    if(!h||!s||!p||!out||len<1)return M64ERR_INPUT_ASSERT;
    const auto& all=*static_cast<Ini*>(h);auto sec=all.find(s);if(sec==all.end())return M64ERR_INPUT_NOT_FOUND;
    auto value=sec->second.find(p);if(value==sec->second.end())return M64ERR_INPUT_NOT_FOUND;
    if(value->second.size()>=static_cast<size_t>(len))return M64ERR_INPUT_INVALID;
    std::memcpy(out,value->second.c_str(),value->second.size()+1);return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL VidExt_Init(){return window?M64ERR_SUCCESS:M64ERR_NOT_INIT;}
EXPORT m64p_error CALL VidExt_Quit(){SDL_GL_MakeCurrent(window,nullptr);return M64ERR_SUCCESS;}
EXPORT m64p_error CALL VidExt_ListFullscreenModes(m64p_2d_size* sizes,int* count) {
    if(!count)return M64ERR_INPUT_ASSERT;int n=SDL_GetNumDisplayModes(0),got=0;
    for(int i=0;i<n && got<*count;++i){SDL_DisplayMode m{};if(SDL_GetDisplayMode(0,i,&m)==0)sizes[got++]={static_cast<unsigned>(m.w),static_cast<unsigned>(m.h)};}
    *count=got;return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL VidExt_ListFullscreenRates(m64p_2d_size size,int* rates,int* count) {
    if(!count)return M64ERR_INPUT_ASSERT;int got=0;
    for(int i=0;i<SDL_GetNumDisplayModes(0)&&got<*count;++i){SDL_DisplayMode m{};if(SDL_GetDisplayMode(0,i,&m)==0&&m.w==size.uiWidth&&m.h==size.uiHeight)rates[got++]=m.refresh_rate;}
    *count=got;return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL VidExt_SetVideoMode(int w,int h,int,m64p_video_mode,m64p_video_flags) {
    if(w<=0||h<=0)return M64ERR_INPUT_INVALID;
    requested_window_size=pack_size(w,h);
    if(SDL_GL_MakeCurrent(window,gl_context)!=0){std::fprintf(stderr,"MakeCurrent: %s\n",SDL_GetError());return M64ERR_SYSTEM_FAIL;}
    // The native VI scheduler supplies 60 Hz pacing. SDL's Cocoa swap-interval
    // wait can stall indefinitely here (confirmed by a process stack sample).
    SDL_GL_SetSwapInterval(0);return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL VidExt_SetVideoModeWithRate(int w,int h,int,int b,m64p_video_mode m,m64p_video_flags f){return VidExt_SetVideoMode(w,h,b,m,f);}
EXPORT m64p_error CALL VidExt_ResizeWindow(int w,int h){
    if(w<=0||h<=0)return M64ERR_INPUT_INVALID;
    requested_window_size=pack_size(w,h);return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL VidExt_SetCaption(const char*){return M64ERR_SUCCESS;}
EXPORT m64p_error CALL VidExt_ToggleFullScreen(){toggle_fullscreen=true;return M64ERR_SUCCESS;}
EXPORT m64p_function CALL VidExt_GL_GetProcAddress(const char* n){return reinterpret_cast<m64p_function>(SDL_GL_GetProcAddress(n));}
EXPORT m64p_error CALL VidExt_GL_SetAttribute(m64p_GLattr attr,int value) {
    // The main thread already created an OpenGL 4.1 core context, satisfying
    // the renderer's 3.3 core request. Never create Cocoa windows on the gfx thread.
    if(attr==M64P_GL_SWAP_CONTROL && SDL_GL_GetCurrentContext())SDL_GL_SetSwapInterval(value);
    return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL VidExt_GL_GetAttribute(m64p_GLattr attr,int* value) {
    if(!value)return M64ERR_INPUT_ASSERT;
    if(attr==M64P_GL_SWAP_CONTROL){*value=SDL_GL_GetSwapInterval();return M64ERR_SUCCESS;}
    if(attr==M64P_GL_CONTEXT_PROFILE_MASK){*value=M64P_GL_CONTEXT_PROFILE_CORE;return M64ERR_SUCCESS;}
    return SDL_GL_GetAttribute(gl_attribute(attr),value)==0?M64ERR_SUCCESS:M64ERR_SYSTEM_FAIL;
}
EXPORT m64p_error CALL VidExt_GL_SwapBuffers(){SDL_GL_SwapWindow(window);++frames;return M64ERR_SUCCESS;}
EXPORT uint32_t CALL VidExt_GL_GetDefaultFramebuffer(){return 0;}
}

bool rs_video_open(const uint8_t* rom_header) {
    std::fprintf(stderr,"Video setup: configuration\n");
    data_dir=(rs_paths().user_data/"gliden64").string();
    ini_dir=rs_paths().renderer_ini.string();
    std::filesystem::create_directories(data_dir);
    config=read_ini((data_dir+"/settings.ini").c_str());
    config["Video-General"]["ScreenWidth"]="640";
    config["Video-General"]["ScreenHeight"]="480";
    config["Video-General"]["Fullscreen"]="0";
    config["Video-General"]["VerticalSync"]="0";
    config["Video-GLideN64"]["ThreadedVideo"]="0";
    for(unsigned i=0;i<64;++i)header[i^3]=rom_header[i];
    std::fprintf(stderr,"Video setup: SDL video initialization\n");
    // SDL's native Spaces transition failed on the target Mac. Desktop
    // fullscreen is verified to enter/leave while preserving the GL context.
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES,"0");
    if(SDL_Init(SDL_INIT_VIDEO)!=0){std::fprintf(stderr,"SDL: %s\n",SDL_GetError());return false;}
    std::fprintf(stderr,"Video setup: SDL audio initialization\n");
    if(SDL_InitSubSystem(SDL_INIT_AUDIO)!=0)std::fprintf(stderr,"SDL audio initialization will be retried: %s\n",SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,4);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
    std::fprintf(stderr,"Video setup: create window\n");
    window=SDL_CreateWindow("Rogue Squadron — native port development",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,640,480,SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if(!window)return false;
    SDL_SetWindowMinimumSize(window,320,240);
    gamepad.initialize();
    input_focused=(SDL_GetWindowFlags(window)&SDL_WINDOW_INPUT_FOCUS)!=0;
    gamepad.set_focus(input_focused);
    std::fprintf(stderr,"Video setup: create OpenGL context\n");
    gl_context=SDL_GL_CreateContext(window);if(!gl_context)return false;
    SDL_GL_SetSwapInterval(0);
    std::ofstream report(rs_report_path("graphics-context.txt"));
    report<<"GL vendor: "<<glGetString(GL_VENDOR)<<"\nGL renderer: "<<glGetString(GL_RENDERER)<<"\nGL version: "<<glGetString(GL_VERSION)<<'\n';
    // Do not swap on the main thread before Cocoa's event loop is pumping.
    // Presentation begins on the renderer thread after the main loop starts.
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_MakeCurrent(window,nullptr);return true;
}
void rs_request_start_pulse(){requested_start_pulse=true;}
void rs_observe_test_play_state(TestPlayState state){
    test_play_observation=(SDL_GetTicks64()<<3)|static_cast<unsigned>(state);
}
bool rs_video_poll() {
    static bool audio_unavailable=false;
    if(bool current=rs_audio_output_unavailable();current!=audio_unavailable){
        audio_unavailable=current;
        SDL_SetWindowTitle(window,current?"Rogue Squadron — audio reconnecting":"Rogue Squadron — native port development");
    }
    if(requested_start_pulse.exchange(false))keyboard_pulses.press(0x1000,SDL_GetTicks64());
    SDL_Event event;while(SDL_PollEvent(&event)){
        if(event.type==SDL_QUIT)return false;
        if(event.type==SDL_WINDOWEVENT && event.window.windowID==SDL_GetWindowID(window)){
            if(event.window.event==SDL_WINDOWEVENT_FOCUS_LOST){input_focused=false;keyboard_pulses.clear();gamepad.set_focus(false);}
            if(event.window.event==SDL_WINDOWEVENT_FOCUS_GAINED){input_focused=true;gamepad.set_focus(true);}
        }
        gamepad.handle_event(event,SDL_GetTicks64());
        if(input_focused && event.type==SDL_KEYDOWN && !event.key.repeat){
            if(event.key.keysym.scancode==SDL_SCANCODE_F11)toggle_fullscreen=true;
            if(event.key.keysym.scancode==SDL_SCANCODE_ESCAPE && (SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN))toggle_fullscreen=true;
        }
        // Command shortcuts belong to macOS; for example Command+X must not
        // fire the ship's weapons while a menu action is being processed.
        if(input_focused && event.type==SDL_KEYDOWN&&!event.key.repeat && !(event.key.keysym.mod&KMOD_GUI)){
            for(auto binding:key_bindings)if(binding.key==event.key.keysym.scancode){
                keyboard_pulses.press(binding.mask,SDL_GetTicks64());
                if(rs_paths().diagnostics)std::fprintf(stderr,"Controller key pressed: %08x\n",binding.mask);
            }
        }
    }
    const auto* key=SDL_GetKeyboardState(nullptr);
    uint32_t held=0;
    bool keyboard_active=input_focused && !(SDL_GetModState()&KMOD_GUI);
    if(keyboard_active)for(auto binding:key_bindings)if(key[binding.key])held|=binding.mask;
    if(!keyboard_active)keyboard_pulses.clear();
    uint32_t value=keyboard_active?keyboard_pulses.sample(held,SDL_GetTicks64()):0;
    uint16_t buttons=static_cast<uint16_t>(value);
    const float scale=(value&fine_steering)?0.25f:1.0f;
    float x=scale*(((value&stick_right)?1:0)-((value&stick_left)?1:0));
    float y=scale*(((value&stick_up)?1:0)-((value&stick_down)?1:0));
    auto pad=gamepad.sample(SDL_GetTicks64());
    buttons|=pad.buttons;
    // Steering keys take priority while pressed; an idle keyboard allows the
    // full analog range. Never add two sticks and exceed the guest input range.
    if(!(value&(stick_right|stick_left|stick_up|stick_down))){x=pad.x;y=pad.y;}
    sample_test_controller(SDL_GetTicks64(),buttons,x,y);
    controller_buttons=buttons;controller_x=x;controller_y=y;
    // Consume explicit renderer requests once; never undo a user's resize on
    // every event poll. Resizing/fullscreen operations remain on the main thread.
    if(auto size=requested_window_size.exchange(0)){
        if(!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN))SDL_SetWindowSize(window,int(size>>32),int(uint32_t(size)));
    }
    if(toggle_fullscreen.exchange(false)){
        if(SDL_SetWindowFullscreen(window,(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN)?0:SDL_WINDOW_FULLSCREEN_DESKTOP)!=0)
            std::fprintf(stderr,"Could not change fullscreen mode: %s\n",SDL_GetError());
    }
    int w,h,pixel_w,pixel_h;SDL_GetWindowSize(window,&w,&h);SDL_GL_GetDrawableSize(window,&pixel_w,&pixel_h);
    if(pixel_w>0 && pixel_h>0)drawable_size=pack_size(pixel_w,pixel_h);
    static uint64_t last_window=0,last_drawable=0;static Uint32 last_flags=0;
    const auto window_size=pack_size(w,h),pixels=pack_size(pixel_w,pixel_h);
    const auto flags=SDL_GetWindowFlags(window)&(SDL_WINDOW_FULLSCREEN_DESKTOP|SDL_WINDOW_INPUT_FOCUS|SDL_WINDOW_MINIMIZED);
    if(window_size!=last_window || pixels!=last_drawable || flags!=last_flags){
        last_window=window_size;last_drawable=pixels;last_flags=flags;
        auto path=rs_report_path("window-state.json"),temporary=rs_report_path("window-state.json.tmp");
        {std::ofstream out(temporary);out<<"{\"width\":"<<w<<",\"height\":"<<h
            <<",\"drawable_width\":"<<pixel_w<<",\"drawable_height\":"<<pixel_h
            <<",\"fullscreen\":"<<((flags&SDL_WINDOW_FULLSCREEN)?"true":"false")
            <<",\"focused\":"<<(input_focused?"true":"false")<<",\"presented_frames\":"<<frames.load()<<"}\n";}
        std::error_code error;std::filesystem::rename(temporary,path,error);
    }
    return true;
}
unsigned rs_video_tasks(){return tasks.load();}
unsigned rs_video_frames(){return frames.load();}
bool rs_keyboard_input(int port,uint16_t* buttons,float* x,float* y){
    if(port!=0)return false;
    *buttons=controller_buttons.load();*x=controller_x.load();*y=controller_y.load();
    if((*buttons||*x||*y) && rs_paths().diagnostics)std::fprintf(stderr,"Controller input delivered: %04x stick=(%g,%g)\n",*buttons,*x,*y);
    return true;
}
void rs_dp_counters(uint32_t* result){
    // Expose the graphics backend's counters. GLideN64 does not count N64 RDP
    // cycles, so these remain zero; no fabricated timing estimate is supplied.
    for(unsigned i=0;i<4;++i)result[i]=counter_snapshot[i].load();
}
void rs_dp_status(uint32_t flags){
    // Reading/resetting performance counters must never wait behind a vsync.
    std::lock_guard lock(register_command_lock);register_commands.push_back(flags);
}

class GlideRenderer final:public ultramodern::renderer::RendererContext {
    void* lib=nullptr;bool initialized=false;
    unsigned last_capture=0;
    unsigned history_bucket=0;
    void (*process)()=nullptr;void (*update)()=nullptr;void (*close)()=nullptr;
    void (*resize_output)(int,int)=nullptr;
    uint64_t applied_drawable_size=0,reported_render_size=0;
    void (*read_screen)(void*,int*,int*,int)=nullptr;
    void (*debug_frame)(unsigned int*)=nullptr;
    unsigned debug_values[4]{};
    template<class T>T symbol(const char* name){auto p=dlsym(lib,name);if(!p)throw std::runtime_error(name);return reinterpret_cast<T>(p);}
public:
    explicit GlideRenderer(uint8_t* ram) {
        chosen_api=ultramodern::renderer::GraphicsApi::Auto;
        setup_result=ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        lib=dlopen(rs_paths().renderer.c_str(),RTLD_NOW|RTLD_LOCAL);
        if(!lib){std::fprintf(stderr,"Renderer load: %s\n",dlerror());return;}
        auto startup=symbol<m64p_error(*)(void*,void*,void(*)(void*,int,const char*))>("PluginStartup");
        if(startup(dlopen(nullptr,RTLD_NOW),nullptr,debug)!=M64ERR_SUCCESS)return;
        GFX_INFO info{};info.HEADER=header;info.RDRAM=ram;info.DMEM=dmem;info.IMEM=imem;
        info.MI_INTR_REG=&mi_intr;info.DPC_START_REG=&dpc[0];info.DPC_END_REG=&dpc[1];info.DPC_CURRENT_REG=&dpc[2];
        info.DPC_STATUS_REG=&dpc[3];info.DPC_CLOCK_REG=&dpc[4];info.DPC_BUFBUSY_REG=&dpc[5];info.DPC_PIPEBUSY_REG=&dpc[6];info.DPC_TMEM_REG=&dpc[7];
        auto* vi=ultramodern::renderer::get_vi_regs();
        info.VI_STATUS_REG=&vi->VI_STATUS_REG;info.VI_ORIGIN_REG=&vi->VI_ORIGIN_REG;info.VI_WIDTH_REG=&vi->VI_WIDTH_REG;
        info.VI_INTR_REG=&vi->VI_INTR_REG;info.VI_V_CURRENT_LINE_REG=&vi->VI_V_CURRENT_LINE_REG;info.VI_TIMING_REG=&vi->VI_TIMING_REG;
        info.VI_V_SYNC_REG=&vi->VI_V_SYNC_REG;info.VI_H_SYNC_REG=&vi->VI_H_SYNC_REG;info.VI_LEAP_REG=&vi->VI_LEAP_REG;
        info.VI_H_START_REG=&vi->VI_H_START_REG;info.VI_V_START_REG=&vi->VI_V_START_REG;info.VI_V_BURST_REG=&vi->VI_V_BURST_REG;
        info.VI_X_SCALE_REG=&vi->VI_X_SCALE_REG;info.VI_Y_SCALE_REG=&vi->VI_Y_SCALE_REG;
        info.CheckInterrupts=check_interrupts;info.version=2;info.SP_STATUS_REG=&sp_status;info.RDRAM_SIZE=&ram_bytes;
        if(!symbol<int(*)(GFX_INFO)>("InitiateGFX")(info))return;
        initialized=symbol<int(*)()>("RomOpen")()!=0;
        process=symbol<void(*)()>("ProcessDList");update=symbol<void(*)()>("UpdateScreen");close=symbol<void(*)()>("RomClosed");
        read_screen=symbol<void(*)(void*,int*,int*,int)>("ReadScreen2");
        resize_output=symbol<void(*)(int,int)>("ResizeVideoOutput");
        debug_frame=symbol<void(*)(unsigned int*)>("RogueDebugFrame");
        setup_result=initialized?ultramodern::renderer::SetupResult::Success:ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    bool valid()override{return initialized;}
    bool update_config(const ultramodern::renderer::GraphicsConfig&,const ultramodern::renderer::GraphicsConfig&)override{return false;}
    void enable_instant_present()override{}
    void send_dl(const OSTask* task)override {
        if(rs_paths().diagnostics)std::fprintf(stderr,"GPU HLE begin: ucode=%08x data=%08x\n",(uint32_t)task->t.ucode,(uint32_t)task->t.data_ptr);
        apply_dp_commands();
        static_assert(sizeof(task->t)==64);
        uint32_t words[16];std::memcpy(words,&task->t,64);
        // libultra normally translates task pointers before DMA to SP DMEM.
        // GLideN64 reads those physical addresses directly from the task header.
        for(unsigned index:{2u,4u,6u,8u,10u,11u,12u,14u})words[index]&=0x1fffffffu;
        std::memcpy(dmem+0xfc0,words,64);sp_status=0x203;process();debug_frame(debug_values);++tasks;
        if(rs_paths().diagnostics)std::fprintf(stderr,"GPU HLE completed: %u tasks\n",tasks.load());
    }
    void send_dummy_workload(uint32_t)override{}
    void update_screen()override {
        apply_dp_commands();
        // Resize GLideN64 only on its owning thread, before VI_UpdateScreen
        // recreates the framebuffer/viewport. Retina displays use pixel sizes.
        auto size=drawable_size.load();
        if(size && size!=applied_drawable_size){
            resize_output(int(size>>32),int(uint32_t(size)));applied_drawable_size=size;
        }
        update();
        int render_w=0,render_h=0;read_screen(nullptr,&render_w,&render_h,1);
        auto render_size=pack_size(render_w,render_h);
        if(render_w>0 && render_h>0 && render_size!=reported_render_size){
            reported_render_size=render_size;
            auto path=rs_report_path("renderer-size.json"),temporary=rs_report_path("renderer-size.json.tmp");
            {std::ofstream out(temporary);out<<"{\"width\":"<<render_w<<",\"height\":"<<render_h
                <<",\"presented_frames\":"<<frames.load()<<"}\n";}
            std::error_code error;std::filesystem::rename(temporary,path,error);
        }
        if(rs_paths().diagnostics && tasks.load()>last_capture && (tasks.load()<5 || tasks.load()-last_capture>=15)){
            int w=0,h=0;read_screen(nullptr,&w,&h,1);
            if(w>0&&h>0&&w<=4096&&h<=4096){
                std::vector<uint8_t> pixels(w*h*3);read_screen(pixels.data(),&w,&h,1);
                // OpenGL returns the bottom row first; SDL surfaces start at the top.
                for(int y=0;y<h/2;++y)for(int x=0;x<w*3;++x)
                    std::swap(pixels[y*w*3+x],pixels[(h-1-y)*w*3+x]);
                auto* surface=SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(),w,h,24,w*3,SDL_PIXELFORMAT_RGB24);
                if(surface){
                    SDL_SaveBMP(surface,rs_report_path("first-game-frame.bmp").c_str());
                    auto* regs=ultramodern::renderer::get_vi_regs();
                    std::ofstream state(rs_report_path("frame-state.json"));
                    state<<"{\"task\":"<<tasks.load()<<",\"origin\":"<<regs->VI_ORIGIN_REG
                         <<",\"width\":"<<regs->VI_WIDTH_REG<<",\"status\":"<<regs->VI_STATUS_REG
                         <<",\"h_start\":"<<regs->VI_H_START_REG<<",\"v_start\":"<<regs->VI_V_START_REG
                         <<",\"x_scale\":"<<regs->VI_X_SCALE_REG<<",\"y_scale\":"<<regs->VI_Y_SCALE_REG
                         <<",\"triangles\":"<<debug_values[0]<<",\"microcode\":"<<debug_values[1]
                         <<",\"color_image\":"<<debug_values[2]<<",\"color_width\":"<<debug_values[3]<<"}\n";
                    state.close();
                    unsigned bucket=tasks.load()/120;
                    if(bucket>history_bucket&&bucket<=30){
                        std::filesystem::create_directories(rs_report_path("frame-history"));
                        std::string path=(rs_report_path("frame-history")/("frame-"+std::to_string(tasks.load())+".bmp")).string();
                        SDL_SaveBMP(surface,path.c_str());history_bucket=bucket;
                        std::filesystem::copy_file(rs_report_path("frame-state.json"),path+".json",std::filesystem::copy_options::overwrite_existing);
                    }
                    SDL_FreeSurface(surface);last_capture=tasks.load();
                }
            }
        }
    }
    void shutdown()override{if(initialized){close();initialized=false;}}
    uint32_t get_display_framerate()const override{return 60;}
    float get_resolution_scale()const override{return 1.0f;}
};
std::unique_ptr<ultramodern::renderer::RendererContext> rs_create_renderer(uint8_t* ram,ultramodern::renderer::WindowHandle,bool) {
    return std::make_unique<GlideRenderer>(ram);
}
