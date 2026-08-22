#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void metrics_reset(void);
void metrics_record_inference(uint32_t time_us, uint32_t cycles);
void metrics_print_summary(void);
uint32_t metrics_get_count(void);
uint32_t metrics_get_avg_time_us(void);

#ifdef __cplusplus
}
#endif
