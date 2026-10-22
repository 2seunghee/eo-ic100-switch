#include "pcm.h"

#include <math.h>
#include <string.h>

// ------------------------------------------------------------------ ring

static inline uint32_t load_acq(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void store_rel(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

void ring_init(pcm_ring *r, uint32_t *storage, uint32_t capacity_pow2) {
    r->buf = storage;
    r->mask = capacity_pow2 - 1;
    r->head = r->tail = 0;
}

uint32_t ring_capacity(const pcm_ring *r) { return r->mask + 1; }

uint32_t ring_fill(const pcm_ring *r) { return load_acq(&r->head) - load_acq(&r->tail); }

uint32_t ring_write(pcm_ring *r, const int16_t *stereo, uint32_t frames) {
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t space = ring_capacity(r) - (head - load_acq(&r->tail));
    if (frames > space) frames = space;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t v;
        memcpy(&v, stereo + 2 * i, sizeof(v));
        r->buf[(head + i) & r->mask] = v;
    }
    store_rel(&r->head, head + frames);
    return frames;
}

uint32_t ring_peek(const pcm_ring *r, int16_t *out, uint32_t frames) {
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    uint32_t fill = load_acq(&r->head) - tail;
    if (frames > fill) frames = fill;
    for (uint32_t i = 0; i < frames; i++) memcpy(out + 2 * i, &r->buf[(tail + i) & r->mask], sizeof(uint32_t));
    return frames;
}

void ring_consume(pcm_ring *r, uint32_t frames) {
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    uint32_t fill = load_acq(&r->head) - tail;
    if (frames > fill) frames = fill;
    store_rel(&r->tail, tail + frames);
}

void ring_drop_all(pcm_ring *r) { store_rel(&r->tail, load_acq(&r->head)); }

// ------------------------------------------------------------------ playout

void playout_stats_clear(playout_stats *s) {
    memset(s, 0, sizeof(*s));
    s->fill_min = UINT32_MAX;
}

void playout_init(playout *p, const pcm_ring *r, uint32_t target, uint32_t deadband, uint32_t max_fill) {
    uint32_t cap = ring_capacity(r);
    uint32_t limit = cap > 2 * PLAYOUT_MAX_BLOCK ? cap - 2 * PLAYOUT_MAX_BLOCK : cap / 2;
    if (max_fill > limit) max_fill = limit;
    if (target > max_fill / 2) target = max_fill / 2;
    p->target = target;
    p->deadband = deadband;
    p->max_fill = max_fill;
    playout_reset(p);
    playout_stats_clear(&p->st);
}

void playout_reset(playout *p) {
    p->primed = false;
    p->ema = 0.0f;
    p->since_slip = 0;
}

void playout_take_stats(playout *p, playout_stats *out) {
    *out = p->st;
    playout_stats_clear(&p->st);
}

static inline int16_t lerp16(int16_t a, int16_t b, uint32_t frac16) {
    return (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)frac16) / 65536));
}

void playout_fill(playout *p, pcm_ring *r, int16_t *out, uint32_t n) {
    if (n > PLAYOUT_MAX_BLOCK) {
        memset(out + 2 * PLAYOUT_MAX_BLOCK, 0, (size_t)(n - PLAYOUT_MAX_BLOCK) * 4);
        n = PLAYOUT_MAX_BLOCK;
    }
    if (n == 0) return;
    playout_stats *st = &p->st;
    uint32_t fill = ring_fill(r);
    if (fill < st->fill_min) st->fill_min = fill;
    if (fill > st->fill_max) st->fill_max = fill;
    st->fill_sum += fill;
    st->fill_n++;

    if (!p->primed) {
        if (fill < p->target || fill < n + 1) {
            memset(out, 0, (size_t)n * 4);
            st->silent_blocks++;
            return;
        }
        p->primed = true;
        p->ema = (float)fill;
        p->since_slip = 0;
        st->primes++;
    }
    if (fill > p->max_fill) {
        uint32_t cut = fill - p->target;
        ring_consume(r, cut);
        fill = p->target;
        p->ema = (float)fill;
        st->skips++;
        st->skipped_frames += cut;
    }

    p->ema += ((float)fill - p->ema) * (1.0f / 64.0f);
    float err = p->ema - (float)p->target;
    float aerr = fabsf(err);
    // Far off (e.g. right after a scheduling hiccup): one slip per block. Otherwise at most one
    // slip per PLAYOUT_SLIP_GAP blocks, which still covers 200+ ppm of clock difference.
    uint32_t gap = aerr > 4.0f * (float)p->deadband ? 1 : PLAYOUT_SLIP_GAP;
    if (p->since_slip < UINT32_MAX) p->since_slip++;
    int adj = 0;
    if (aerr > (float)p->deadband && p->since_slip >= gap) adj = err > 0 ? 1 : -1;
    if (adj && fill < n + (uint32_t)adj + 1) adj = 0;   // interpolation looks one frame ahead
    if (adj) p->since_slip = 0;

    uint32_t m = n + (uint32_t)adj;   // input frames this block consumes
    uint32_t need = adj ? m + 1 : n;
    if (fill < need) {
        uint32_t got = ring_peek(r, out, fill < n ? fill : n);
        memset(out + 2 * got, 0, (size_t)(n - got) * 4);
        ring_consume(r, got);
        p->primed = false;
        st->underruns++;
        return;
    }
    if (!adj) {
        ring_peek(r, out, n);
        ring_consume(r, n);
        return;
    }
    int16_t *in = (int16_t *)p->tmp;
    ring_peek(r, in, m + 1);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t pos = (uint64_t)i * m * 65536u / n;   // 16.16 input position of output frame i
        uint32_t k = (uint32_t)(pos >> 16), frac = (uint32_t)(pos & 0xFFFF);
        out[2 * i] = lerp16(in[2 * k], in[2 * k + 2], frac);
        out[2 * i + 1] = lerp16(in[2 * k + 1], in[2 * k + 3], frac);
    }
    ring_consume(r, m);
    if (adj > 0) st->drops++;
    else st->inserts++;
}

// ------------------------------------------------------------------ convert

uint32_t pcm_bytes_per_sample(int fmt) {
    switch (fmt) {
    case PCM_FMT_S16:   return 2;
    case PCM_FMT_S32:   return 4;
    case PCM_FMT_FLOAT: return 4;
    default:            return 0;
    }
}

static inline int32_t sample_at(const void *in, int fmt, size_t idx) {
    switch (fmt) {
    case PCM_FMT_S16: {
        int16_t v;
        memcpy(&v, (const uint8_t *)in + idx * 2, 2);
        return v;
    }
    case PCM_FMT_S32: {
        int32_t v;
        memcpy(&v, (const uint8_t *)in + idx * 4, 4);
        return v / 65536;
    }
    case PCM_FMT_FLOAT: {
        float v;
        memcpy(&v, (const uint8_t *)in + idx * 4, 4);
        if (!(v == v)) return 0;   // NaN
        v *= 32768.0f;
        if (v > 32767.0f) return 32767;
        if (v < -32768.0f) return -32768;
        return (int32_t)v;
    }
    default:
        return 0;
    }
}

static inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

void pcm_to_stereo16(const void *in, int fmt, uint32_t channels, uint32_t frames, int16_t *out) {
    if (channels == 0 || pcm_bytes_per_sample(fmt) == 0) {
        memset(out, 0, (size_t)frames * 4);
        return;
    }
    if (fmt == PCM_FMT_S16 && channels == 2) {
        memmove(out, in, (size_t)frames * 4);
        return;
    }
    for (uint32_t f = 0; f < frames; f++) {
        size_t base = (size_t)f * channels;
        int32_t l, r;
        if (channels == 1) {
            l = r = sample_at(in, fmt, base);
        } else if (channels >= 6) {
            int32_t c = sample_at(in, fmt, base + 2) / 2;
            l = sample_at(in, fmt, base) + c + sample_at(in, fmt, base + 4) / 2;
            r = sample_at(in, fmt, base + 1) + c + sample_at(in, fmt, base + 5) / 2;
        } else {
            l = sample_at(in, fmt, base);
            r = sample_at(in, fmt, base + 1);
        }
        out[2 * f] = sat16(l);
        out[2 * f + 1] = sat16(r);
    }
}

uint32_t pcm_trailing_u16(const void *buf, uint32_t bytes, uint16_t pattern) {
    const uint8_t *b = buf;
    uint32_t words = bytes / 2, n = 0;
    while (n < words) {
        uint16_t v;
        memcpy(&v, b + (size_t)(words - 1 - n) * 2, 2);
        if (v != pattern) break;
        n++;
    }
    return n;
}

void pcm_fill_u16(void *buf, uint32_t bytes, uint16_t pattern) {
    uint8_t *b = buf;
    for (uint32_t i = 0; i + 1 < bytes; i += 2) memcpy(b + i, &pattern, 2);
}

// ------------------------------------------------------------------ gain

uint32_t pcm_gain_q15_from_db(float db) {
    if (!(db < 0.0f)) return PCM_UNITY_Q15;   // also NaN
    if (db <= -100.0f) return 0;
    double g = pow(10.0, db / 20.0) * PCM_UNITY_Q15;
    return (uint32_t)(g + 0.5);
}

void pcm_apply_gain_q15(int16_t *s, uint32_t count, uint32_t gain_q15) {
    if (gain_q15 >= PCM_UNITY_Q15) return;
    if (gain_q15 == 0) {
        memset(s, 0, (size_t)count * 2);
        return;
    }
    for (uint32_t i = 0; i < count; i++) s[i] = (int16_t)(((int32_t)s[i] * (int32_t)gain_q15) / 32768);
}

// ------------------------------------------------------------------ meter

void meter_reset(pcm_meter *m) { memset(m, 0, sizeof(*m)); }

void meter_add(pcm_meter *m, const int16_t *stereo, uint32_t frames) {
    uint64_t s0 = 0, s1 = 0;
    uint32_t p0 = m->peak[0], p1 = m->peak[1];
    for (uint32_t i = 0; i < frames; i++) {
        int32_t l = stereo[2 * i], r = stereo[2 * i + 1];
        s0 += (uint64_t)(l * l);
        s1 += (uint64_t)(r * r);
        uint32_t al = (uint32_t)(l < 0 ? -l : l), ar = (uint32_t)(r < 0 ? -r : r);
        if (al > p0) p0 = al;
        if (ar > p1) p1 = ar;
    }
    m->sumsq[0] += s0;
    m->sumsq[1] += s1;
    m->peak[0] = p0;
    m->peak[1] = p1;
    m->frames += frames;
}

void meter_merge(pcm_meter *into, const pcm_meter *m) {
    for (int c = 0; c < 2; c++) {
        into->sumsq[c] += m->sumsq[c];
        if (m->peak[c] > into->peak[c]) into->peak[c] = m->peak[c];
    }
    into->frames += m->frames;
}

float db_from_power(double mean_square) {
    if (mean_square <= 0.0) return -120.0f;
    double db = 10.0 * log10(mean_square / (32768.0 * 32768.0));
    return db < -120.0 ? -120.0f : (float)db;
}

float meter_rms_db(const pcm_meter *m, int ch) {
    if (!m->frames || ch < 0 || ch > 1) return -120.0f;
    return db_from_power((double)m->sumsq[ch] / (double)m->frames);
}

float meter_rms_db_both(const pcm_meter *m) {
    if (!m->frames) return -120.0f;
    return db_from_power(((double)m->sumsq[0] + (double)m->sumsq[1]) / (2.0 * (double)m->frames));
}

float meter_peak_db(const pcm_meter *m, int ch) {
    if (!m->frames || ch < 0 || ch > 1 || m->peak[ch] == 0) return -120.0f;
    return (float)(20.0 * log10((double)m->peak[ch] / 32768.0));
}
