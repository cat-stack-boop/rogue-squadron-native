/* Executes statically recompiled cartridge routines as ARM64 code.
 * The independent reference for the checksum is macOS system zlib.
 * This is a subsystem test, not game boot or a playable port.
 */
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "recomp.h"
#include "funcs.h"

#define RAM_SIZE (8 * 1024 * 1024)
static uint8_t *ram;
static uint32_t rng = 0x12345678;
static unsigned tests = 0;
static uint8_t *rom;
static unsigned dma_calls = 0;
static gpr address(uint32_t x) { return (gpr)(int64_t)(int32_t)x; }
static uint32_t random_word(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

/* Synchronous cartridge-to-host memory copy. N64 cache operations are redundant
 * for these coherent host data buffers. This probe executes no dynamically
 * generated code and does not map guest instructions as executable memory. */
void rs_invalidate_icache(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void rs_invalidate_dcache(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void rs_writeback_dcache_all(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void rs_dma_read(uint8_t *rdram, recomp_context *ctx) {
    uint32_t src = (uint32_t)ctx->r4 - 0xb0000000u;
    uint32_t dst = (uint32_t)ctx->r5 - 0x80000000u;
    uint32_t count = (uint32_t)ctx->r6;
    if (src > 0x1000000 || count > 0x1000000 - src || dst > RAM_SIZE || count > RAM_SIZE - dst) {
        fprintf(stderr, "Invalid cartridge DMA bounds\n"); exit(3);
    }
    for (uint32_t i = 0; i < count; ++i) rdram[(dst + i) ^ 3] = rom[src + i];
    ++dma_calls;
}

static int test_overlays(void) {
    FILE *input = fopen("roms/rogue_squadron.us.rev1.z64", "rb");
    rom = malloc(0x1000000);
    if (!input || !rom || fread(rom, 1, 0x1000000, input) != 0x1000000) return 2;
    fclose(input);
    uint32_t sources[] = {0x96ce0, 0xfdec0, 0x126660};
    uint32_t lengths[] = {0x671e0, 0x287a0, 0xb840};
    uint32_t bss_lengths[] = {0x1b30, 0x2f20, 0x15b0};
    /* Revisiting and repeating overlays checks the game's current-overlay guard. */
    unsigned order[] = {0, 0, 1, 1, 2, 2, 0, 2, 1, 0};
    *(uint32_t*)(ram + 0x2e520) = 0xffffffffu;
    for (unsigned step = 0; step < sizeof(order)/sizeof(*order); ++step) {
        unsigned id = order[step];
        unsigned before = dma_calls;
        recomp_context ctx = {0};
        ctx.r29 = address(0x807fff00);
        ctx.r4 = id;
        rs_load_overlay(ram, &ctx);
        if (dma_calls - before != (step == 0 || id != order[step-1])) return 1;
        if (*(uint32_t*)(ram + 0x2e520) != id || ctx.r29 != address(0x807fff00)) return 1;
        for (uint32_t i = 0; i < lengths[id]; ++i)
            if (ram[(0x960e0 + i) ^ 3] != rom[sources[id] + i]) return 1;
        for (uint32_t i = 0; i < bss_lengths[id]; ++i)
            if (ram[(0x960e0 + lengths[id] + i) ^ 3] != 0) return 1;
        ++tests;
    }
    free(rom);
    return 0;
}

static int test_audio_feedback(void) {
    /* Independent fixed-point feedback calculation, including period bounds,
     * history shifting and 16-bit output wrapping. The table values are read
     * directly from the inspected original ROM's inline coefficient table. */
    int16_t coefficient[] = {0x0ccd, 0x2ccd, 0x5333, 0x7fff};
    int periods[] = {-1, 0, 39, 40, 41, 79, 80, 119, 120, 121, 32767};
    for (unsigned i = 0; i < 4; ++i)
        *(int16_t*)(ram + ((0x91a48 + i*2) ^ 2)) = coefficient[i];
    for (unsigned p = 0; p < sizeof(periods)/sizeof(*periods); ++p) {
        for (unsigned gain = 0; gain < 4; ++gain) {
            for (unsigned trial = 0; trial < 8; ++trial) {
                int16_t history[180], input[40];
                for (unsigned i = 0; i < 180; ++i) {
                    history[i] = (int16_t)random_word();
                    *(int16_t*)(ram + ((0x200000 + i*2) ^ 2)) = history[i];
                }
                for (unsigned i = 0; i < 40; ++i) {
                    input[i] = (int16_t)random_word();
                    *(int16_t*)(ram + ((0x210000 + i*2) ^ 2)) = input[i];
                }
                history[179] = 80;
                *(int16_t*)(ram + ((0x200000 + 0x166) ^ 2)) = 80;
                unsigned period = periods[p] >= 40 && periods[p] <= 120 ? periods[p] : 80;
                history[179] = period;
                memmove(history, history + 40, 120 * sizeof(int16_t));
                for (unsigned i = 0; i < 40; ++i) {
                    int32_t product = history[120-period+i] * (int32_t)coefficient[gain];
                    history[120+i] = (int16_t)(((product + 0x4000) >> 15) + input[i]);
                }
                recomp_context ctx = {0};
                ctx.r4 = address(0x80200000); ctx.r5 = address(periods[p]);
                ctx.r6 = gain; ctx.r7 = address(0x80210000);
                ctx.r31 = address(0x80091700);
                rs_audio_feedback(ram, &ctx);
                for (unsigned i = 0; i < 180; ++i)
                    if (*(int16_t*)(ram + ((0x200000+i*2)^2)) != history[i]) {
                        fprintf(stderr, "Audio feedback mismatch period=%d gain=%u sample=%u\n", periods[p], gain, i);
                        return 1;
                    }
                if (ctx.r31 != address(0x80091700)) return 1;
                ++tests;
            }
        }
    }
    return 0;
}

static int test_checksum(const uint8_t *bytes, size_t size, uint32_t seed, int null) {
    recomp_context ctx = {0};
    ctx.r29 = address(0x807fff00);
    ctx.r4 = seed;
    ctx.r5 = null ? 0 : address(0x80100000);
    ctx.r6 = size;
    for (size_t i = 0; i < size; ++i) ram[(0x100000 + i) ^ 3] = bytes[i];
    rs_adler32(ram, &ctx);
    uint32_t expected = (uint32_t)adler32(seed, null ? NULL : bytes, (uInt)size);
    ++tests;
    if ((uint32_t)ctx.r2 != expected) {
        fprintf(stderr, "adler32 mismatch size=%zu seed=%08x expected=%08x got=%08x\n",
                size, seed, expected, (uint32_t)ctx.r2);
        return 1;
    }
    if (ctx.r29 != address(0x807fff00)) {
        fprintf(stderr, "Stack pointer was not restored\n");
        return 1;
    }
    return 0;
}

int main(void) {
    ram = calloc(1, RAM_SIZE);
    uint8_t *bytes = malloc(131072);
    if (!ram || !bytes) return 2;
    for (size_t i = 0; i < 131072; ++i) bytes[i] = random_word();
    size_t sizes[] = {0, 1, 2, 15, 16, 17, 255, 256, 5551, 5552, 5553, 11104, 65535, 65536, 131072};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(*sizes); ++i) {
        if (test_checksum(bytes, sizes[i], 1, 0)) return 1;
        if (test_checksum(bytes, sizes[i], 0, 0)) return 1;
        if (test_checksum(bytes, sizes[i], 0x12345678, 0)) return 1;
    }
    if (test_checksum(bytes, 0, 0, 1)) return 1;
    for (unsigned i = 0; i < 100; ++i)
        if (test_checksum(bytes, random_word() % 131072, random_word(), 0)) return 1;
    for (unsigned n = 0; n <= 257; ++n) {
        recomp_context ctx = {0};
        memset(ram + 0x100000, 0xa5, 512);
        ctx.r4 = address(0x80100010);
        ctx.r5 = n;
        rs_clear_memory(ram, &ctx);
        unsigned cleared = (n + 15) & ~15;
        for (unsigned j = 0; j < 512; ++j) {
            uint8_t expected = (j >= 16 && j < 16 + cleared) ? 0 : 0xa5;
            if (ram[0x100000 + j] != expected) {
                fprintf(stderr, "clear mismatch size=%u byte=%u\n", n, j);
                return 1;
            }
        }
        ++tests;
    }
    if (test_overlays()) { fprintf(stderr, "Overlay loader verification failed\n"); return 1; }
    if (test_audio_feedback()) return 1;
    for(unsigned n=0;n<=257;++n){
        recomp_context ctx={0};ctx.r4=address(0x80220000);ctx.r5=address(0x80220000+n);
        rs_audio_cache_cursor(ram,&ctx);
        unsigned blocks=(n+15)/16;if(!blocks)blocks=1;
        if(ctx.r2!=address(0x80220000+blocks*16)){fprintf(stderr,"Cache cursor mismatch at %u\n",n);return 1;}
        ++tests;
    }
    printf("{\"architecture\":\"arm64\",\"checksum_tests\":146,\"memory_clear_tests\":258,\"overlay_tests\":10,\"audio_feedback_tests\":352,\"cache_cursor_tests\":258,\"dma_transfers\":%u,\"passed\":%u,\"game_booted\":false}\n", dma_calls, tests);
    free(bytes); free(ram);
    return 0;
}
