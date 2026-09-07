#pragma once
#include "recomp.h"
#ifdef __cplusplus
extern "C" {
#endif
void rs_cooperative_poll(uint8_t*);
#ifdef __cplusplus
}
#endif

#ifdef RS_CHECK_MEMORY
#ifdef __cplusplus
extern "C" {
#endif
void* rs_checked_memory(uint8_t*, uint32_t, unsigned, unsigned);
#ifdef __cplusplus
}
#endif
#undef MEM_W
#undef MEM_H
#undef MEM_HU
#undef MEM_B
#undef MEM_BU
#define MEM_W(offset, reg) (*(int32_t*)rs_checked_memory(rdram, (uint32_t)((reg)+(offset)), 4, 0))
#define MEM_H(offset, reg) (*(int16_t*)rs_checked_memory(rdram, (uint32_t)((reg)+(offset)), 2, 2))
#define MEM_HU(offset, reg) (*(uint16_t*)rs_checked_memory(rdram, (uint32_t)((reg)+(offset)), 2, 2))
#define MEM_B(offset, reg) (*(int8_t*)rs_checked_memory(rdram, (uint32_t)((reg)+(offset)), 1, 3))
#define MEM_BU(offset, reg) (*(uint8_t*)rs_checked_memory(rdram, (uint32_t)((reg)+(offset)), 1, 3))
#endif

/* N64Recomp emits MEM_WU for LWU, but its bundled runtime header currently
 * provides only signed MEM_W. LWU zero-extends the loaded 32-bit word. */
#ifndef MEM_WU
#define MEM_WU(offset, reg) ((uint32_t)MEM_W(offset, reg))
#endif
