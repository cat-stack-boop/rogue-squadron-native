#pragma once
#include <cstddef>
#include <cstdint>
void rs_audio_frequency(uint32_t);
void rs_audio_samples(int16_t*,size_t);
size_t rs_audio_frames_remaining();
uint64_t rs_audio_sample_count();
uint64_t rs_audio_nonzero_count();
void rs_audio_write_report(const char* path);
bool rs_audio_dma_full();
bool rs_audio_output_unavailable();
[[noreturn]] void rs_runtime_stop(const char*,uint32_t);
