#pragma once
#include "recomp.h"

/* N64Recomp has separate storage for all 32 floating-point registers. Its
 * generated odd-register loads/stores use this pointer to select FR=0 pairs
 * or FR=1 independent registers. Matches librecomp/src/recomp.cpp's FR binding.
 * Interrupt mask bits remain stored for the original save/restore helpers;
 * native event delivery occurs at the existing scheduling points. */
static inline void rs_set_cpu_status(recomp_context* ctx,gpr value) {
    ctx->status_reg=(uint32_t)value;
    ctx->mips3_float_mode=(ctx->status_reg&0x04000000u)!=0;
    ctx->f_odd=ctx->mips3_float_mode?&ctx->f1.u32l:&ctx->f0.u32h;
}
