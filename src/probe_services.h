#pragma once
#include "recomp.h"
void rs_invalidate_icache(uint8_t*, recomp_context*);
void rs_invalidate_dcache(uint8_t*, recomp_context*);
void rs_writeback_dcache_all(uint8_t*, recomp_context*);
void rs_dma_read(uint8_t*, recomp_context*);
