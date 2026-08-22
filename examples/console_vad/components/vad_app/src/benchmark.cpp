#include "benchmark.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char* TAG = "Benchmark";

#define MAX_SECTIONS 32

typedef struct {
    const char* name;
    uint32_t start_cycles;
    uint32_t min_cycles;
    uint32_t max_cycles;
    uint64_t total_cycles;
    uint32_t num_runs;
    bool active;
} section_state_t;

static section_state_t g_sections[MAX_SECTIONS];
static uint32_t g_num_sections = 0;
static uint32_t g_cpu_freq_mhz = 0;

void benchmark_init(void) {
    memset(g_sections, 0, sizeof(g_sections));
    g_num_sections = 0;
    
#ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360
    g_cpu_freq_mhz = 360;
#elif defined(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400)
    g_cpu_freq_mhz = 400;
#else
    g_cpu_freq_mhz = 240;  // Default fallback
#endif
    
    ESP_LOGI(TAG, "Benchmark framework initialized (CPU: %u MHz)", g_cpu_freq_mhz);
}

uint32_t benchmark_start(const char* name) {
    if (g_num_sections >= MAX_SECTIONS) {
        ESP_LOGE(TAG, "Too many benchmark sections!");
        return 0xFFFFFFFF;
    }
    
    // Find existing section or create new
    uint32_t section_id = g_num_sections;
    for (uint32_t i = 0; i < g_num_sections; i++) {
        if (strcmp(g_sections[i].name, name) == 0) {
            section_id = i;
            break;
        }
    }
    
    if (section_id == g_num_sections) {
        // New section
        g_sections[section_id].name = name;
        g_sections[section_id].min_cycles = 0xFFFFFFFF;
        g_sections[section_id].max_cycles = 0;
        g_sections[section_id].total_cycles = 0;
        g_sections[section_id].num_runs = 0;
        g_num_sections++;
    }
    
    g_sections[section_id].start_cycles = esp_cpu_get_cycle_count();
    g_sections[section_id].active = true;
    
    return section_id;
}

void benchmark_end(uint32_t section_id) {
    if (section_id >= g_num_sections || !g_sections[section_id].active) {
        return;
    }
    
    uint32_t end_cycles = esp_cpu_get_cycle_count();
    uint32_t elapsed = end_cycles - g_sections[section_id].start_cycles;
    
    g_sections[section_id].total_cycles += elapsed;
    g_sections[section_id].num_runs++;
    
    if (elapsed < g_sections[section_id].min_cycles) {
        g_sections[section_id].min_cycles = elapsed;
    }
    if (elapsed > g_sections[section_id].max_cycles) {
        g_sections[section_id].max_cycles = elapsed;
    }
    
    g_sections[section_id].active = false;
}

void benchmark_print_results(void) {
    if (g_num_sections == 0) {
        printf("No benchmark data.\n");
        return;
    }
    
    printf("\n========================================\n");
    printf("Benchmark Results (CPU: %" PRIu32 " MHz)\n", g_cpu_freq_mhz);
    printf("========================================\n\n");
    
    uint64_t total_cycles_all = 0;
    for (uint32_t i = 0; i < g_num_sections; i++) {
        total_cycles_all += g_sections[i].total_cycles;
    }
    
    for (uint32_t i = 0; i < g_num_sections; i++) {
        section_state_t* s = &g_sections[i];
        if (s->num_runs == 0) continue;
        
        float avg_cycles = (float)s->total_cycles / s->num_runs;
        float avg_us = avg_cycles / g_cpu_freq_mhz;
        float min_us = (float)s->min_cycles / g_cpu_freq_mhz;
        float max_us = (float)s->max_cycles / g_cpu_freq_mhz;
        float pct = (float)s->total_cycles * 100.0f / total_cycles_all;
        
        printf("%-30s\n", s->name);
        printf("  Runs:   %" PRIu32 "\n", s->num_runs);
        printf("  Avg:    %.2f us  (%" PRIu32 " cycles)\n", avg_us, (uint32_t)avg_cycles);
        printf("  Min:    %.2f us  (%" PRIu32 " cycles)\n", min_us, s->min_cycles);
        printf("  Max:    %.2f us  (%" PRIu32 " cycles)\n", max_us, s->max_cycles);
        printf("  Total:  %.1f%% of measured time\n", pct);
        printf("\n");
    }
    
    printf("========================================\n");
    float total_avg_us = (float)total_cycles_all / g_cpu_freq_mhz / (g_sections[0].num_runs > 0 ? g_sections[0].num_runs : 1);
    printf("Total Average: %.2f us per run\n", total_avg_us);
    printf("========================================\n\n");
}

void benchmark_reset(void) {
    for (uint32_t i = 0; i < g_num_sections; i++) {
        g_sections[i].min_cycles = 0xFFFFFFFF;
        g_sections[i].max_cycles = 0;
        g_sections[i].total_cycles = 0;
        g_sections[i].num_runs = 0;
        g_sections[i].active = false;
    }
}

uint32_t benchmark_get_cpu_freq_mhz(void) {
    return g_cpu_freq_mhz;
}

#else
// Non-ESP platform stubs
void benchmark_init(void) {}
uint32_t benchmark_start(const char* name) { (void)name; return 0; }
void benchmark_end(uint32_t section_id) { (void)section_id; }
void benchmark_print_results(void) { printf("Benchmarking not supported on this platform.\n"); }
void benchmark_reset(void) {}
uint32_t benchmark_get_cpu_freq_mhz(void) { return 0; }
#endif
