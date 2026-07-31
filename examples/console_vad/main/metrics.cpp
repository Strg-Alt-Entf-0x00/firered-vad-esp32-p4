#include "metrics.h"
#include <stdio.h>
#include <inttypes.h>

static uint32_t g_count = 0;
static uint32_t g_min_time = 0xFFFFFFFF;
static uint32_t g_max_time = 0;
static uint64_t g_sum_time = 0;
static uint64_t g_sum_cycles = 0;

void metrics_reset(void) {
    g_count = 0;
    g_min_time = 0xFFFFFFFF;
    g_max_time = 0;
    g_sum_time = 0;
    g_sum_cycles = 0;
}

void metrics_record_inference(uint32_t time_us, uint32_t cycles) {
    g_count++;
    if (time_us < g_min_time) g_min_time = time_us;
    if (time_us > g_max_time) g_max_time = time_us;
    g_sum_time += time_us;
    g_sum_cycles += cycles;
}

void metrics_print_summary(void) {
    if (g_count == 0) {
        printf("No metrics recorded.\n");
        return;
    }
    printf("\n=== Performance Metrics ===\n");
    printf("Total Inferences: %" PRIu32 "\n", g_count);
    printf("Latency per frame (us):\n");
    printf("  Min: %" PRIu32 " us\n", g_min_time);
    printf("  Max: %" PRIu32 " us\n", g_max_time);
    printf("  Avg: %" PRIu32 " us\n", (uint32_t)(g_sum_time / g_count));
    printf("CPU Cycles per frame:\n");
    printf("  Avg: %" PRIu32 "\n", (uint32_t)(g_sum_cycles / g_count));
    printf("===========================\n\n");
}
