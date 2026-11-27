/* A probe for TODO 19's translating core (the console's X): can the P4 run
 * code written at run time into PSRAM, does the instruction cache see a
 * rewrite after esp_cache_msync(), and how fast does such code run.
 */
#include <stdint.h>
#include <stdio.h>
#include "esp_cache.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"

#define LINE 128

static uint32_t addi(int rd, int rs, int imm) { return ((uint32_t)(imm & 0xFFF) << 20) | (rs << 15) | (rd << 7) | 0x13; }
#define RET 0x00008067u

static int sync_code(void *p, size_t n)
{
    n = (n + LINE - 1) & ~(size_t)(LINE - 1);
    esp_err_t a = esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    esp_err_t b = esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (a != ESP_OK || b != ESP_OK) {
        printf("JIT msync: %s, %s\n", esp_err_to_name(a), esp_err_to_name(b));
        return 0;
    }
    return 1;
}

/* the same in compiled code, for comparison */
static int __attribute__((noinline)) count_c(int n)
{
    int a = 0;
    for (int i = 0; i < n; i++) {
        __asm__ volatile("addi %0, %0, 1" : "+r"(a));
    }
    return a;
}

void jit_probe(void)
{
    const size_t size = 64 * 1024;
    uint32_t *b = heap_caps_aligned_alloc(LINE, size, MALLOC_CAP_SPIRAM);
    printf("JIT buffer %p (PSRAM)\n", (void *)b);
    if (!b) {
        return;
    }
    int (*f)(void) = (int (*)(void))b;

    b[0] = addi(10, 0, 42);
    b[1] = RET;
    if (!sync_code(b, 8)) {
        heap_caps_free(b);
        return;
    }
    printf("JIT call 1: %d (42 wanted)\n", f());

    b[0] = addi(10, 0, 43);
    sync_code(b, 8);
    printf("JIT call 2 after a rewrite: %d (43 wanted)\n", f());

    /* straight-line code: n x addi a0, a0, 1 */
    const int n = 4096;
    b[0] = addi(10, 0, 0);
    for (int i = 1; i <= n; i++) {
        b[i] = addi(10, 10, 1);
    }
    b[n + 1] = RET;
    sync_code(b, (n + 2) * 4);
    for (int run = 0; run < 3; run++) {
        uint32_t t0 = esp_cpu_get_cycle_count();
        int r = f();
        uint32_t t1 = esp_cpu_get_cycle_count();
        printf("JIT %d addi from PSRAM: %d, %lu cycles (%.2f a instruction)\n", n, r,
               (unsigned long)(t1 - t0), (double)(t1 - t0) / n);
    }
    for (int run = 0; run < 2; run++) {
        uint32_t t0 = esp_cpu_get_cycle_count();
        int r = count_c(n);
        uint32_t t1 = esp_cpu_get_cycle_count();
        printf("compiled loop of %d: %d, %lu cycles\n", n, r, (unsigned long)(t1 - t0));
    }
    heap_caps_free(b);
}
