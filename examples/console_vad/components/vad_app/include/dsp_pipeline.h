#ifndef DSP_PIPELINE_H
#define DSP_PIPELINE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    float target_rms;         // Target RMS level for speech (e.g. 2000.0)
    float max_gain;           // Maximum allowed gain multiplier (e.g. 20.0x)
    float min_gain;           // Minimum allowed gain multiplier (e.g. 1.0x)
    float attack_speed;       // Speed of gain reduction (e.g. 0.05)
    float decay_speed;        // Speed of gain increase (e.g. 0.005)
    float noise_gate_rms;     // RMS threshold below which gain is not increased (e.g. 150.0)
    
    // Internal state
    float current_gain;
    float smoothed_rms;
} agc_config_t;

/**
 * @brief Initialize the DSP pipeline
 */
void dsp_pipeline_init(void);

/**
 * @brief Get pointer to the AGC configuration to read/modify it
 */
agc_config_t* dsp_pipeline_get_agc_config(void);

/**
 * @brief Process an audio frame through the DSP pipeline (AGC, NS, etc.)
 * 
 * @param buffer The 16-bit PCM audio buffer to process in-place
 * @param num_samples Number of samples in the buffer
 */
void dsp_pipeline_process(int16_t* buffer, size_t num_samples);

#ifdef __cplusplus
}
#endif

#endif // DSP_PIPELINE_H
