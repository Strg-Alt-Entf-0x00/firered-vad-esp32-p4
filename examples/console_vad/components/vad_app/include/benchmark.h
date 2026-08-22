#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Benchmark result structure
 */
typedef struct {
    const char* name;
    uint32_t cycles_min;
    uint32_t cycles_max;
    uint64_t cycles_total;
    uint32_t num_runs;
    float avg_us;
    float min_us;
    float max_us;
} benchmark_result_t;

/**
 * @brief Initialize benchmarking framework
 */
void benchmark_init(void);

/**
 * @brief Start a benchmark section
 * @param name Section name for reporting
 * @return Section ID for use in benchmark_end()
 */
uint32_t benchmark_start(const char* name);

/**
 * @brief End a benchmark section
 * @param section_id ID returned from benchmark_start()
 */
void benchmark_end(uint32_t section_id);

/**
 * @brief Print all benchmark results
 */
void benchmark_print_results(void);

/**
 * @brief Reset all benchmark counters
 */
void benchmark_reset(void);

/**
 * @brief Get CPU frequency in MHz
 * @return CPU frequency
 */
uint32_t benchmark_get_cpu_freq_mhz(void);

#ifdef __cplusplus
}
#endif
