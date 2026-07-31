#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void metrics_reset(void);
void metrics_record_inference(uint32_t time_us, uint32_t cycles);
void metrics_print_summary(void);

#ifdef __cplusplus
}
#endif
