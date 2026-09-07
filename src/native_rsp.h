#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int rs_run_musyx(uint8_t*,const uint32_t*);
unsigned rs_musyx_count(void);
#ifdef __cplusplus
}
#endif
