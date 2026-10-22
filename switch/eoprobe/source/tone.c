#include "tone.h"

#include <math.h>

#define TWO_PI 6.28318530717958647692f

void tone_init(tone_gen *t, float freq_hz, uint32_t rate, float amplitude_dbfs, tone_pattern pattern) {
    t->phase = 0.0f;
    t->step = TWO_PI * freq_hz / (float)rate;
    t->amplitude = 32767.0f * powf(10.0f, amplitude_dbfs / 20.0f);
    t->rate = rate;
    t->frame = 0;
    t->pattern = pattern;
}

void tone_fill(tone_gen *t, int16_t *out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        float v = sinf(t->phase) * t->amplitude;
        t->phase += t->step;
        if (t->phase >= TWO_PI) t->phase -= TWO_PI;
        int16_t s = (int16_t)v;
        int16_t l = s, r = s;
        if (t->pattern == TONE_LR_CYCLE) {
            uint64_t ms = (t->frame * 1000) / t->rate % 3500;
            if (ms < 1000) r = 0;
            else if (ms < 2000) l = 0;
            else if (ms >= 3000) l = r = 0;
        }
        out[2 * i] = l;
        out[2 * i + 1] = r;
        t->frame++;
    }
}
