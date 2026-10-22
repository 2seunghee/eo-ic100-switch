#pragma once
#include <stdint.h>

typedef enum {
    TONE_BOTH = 0,     // continuous tone in both ears
    TONE_LR_CYCLE,     // 1 s left, 1 s right, 1 s both, 0.5 s silence (checks channel order)
} tone_pattern;

typedef struct {
    float    phase, step, amplitude;
    uint32_t rate;
    uint64_t frame;
    tone_pattern pattern;
} tone_gen;

// amplitude_dbfs: e.g. -30.0f. Keep it low: the earphone volume is unknown until you hear it.
void tone_init(tone_gen *t, float freq_hz, uint32_t rate, float amplitude_dbfs, tone_pattern pattern);
// Interleaved stereo signed 16-bit.
void tone_fill(tone_gen *t, int16_t *out, uint32_t frames);
