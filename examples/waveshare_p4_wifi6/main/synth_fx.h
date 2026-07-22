#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNTH_FX_READY = 0,
    SYNTH_FX_WAKEWORD,
    SYNTH_FX_ACK,
    SYNTH_FX_ERROR
} SYNTH_FX_type_t;

void synth_fx_init(void);
void synth_fx_play(SYNTH_FX_type_t type);

#ifdef __cplusplus
}
#endif
