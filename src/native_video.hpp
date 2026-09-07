#pragma once
#include <cstdint>
#include <memory>
#include "test_controller.hpp"
#include "ultramodern/renderer_context.hpp"

bool rs_video_open(const uint8_t* rom_header);
bool rs_video_poll();
void rs_request_start_pulse();
void rs_observe_test_play_state(TestPlayState);
unsigned rs_video_tasks();
unsigned rs_video_frames();
bool rs_keyboard_input(int,uint16_t*,float*,float*);
void rs_dp_counters(uint32_t*);
void rs_dp_status(uint32_t);
std::unique_ptr<ultramodern::renderer::RendererContext> rs_create_renderer(
    uint8_t*, ultramodern::renderer::WindowHandle, bool);
