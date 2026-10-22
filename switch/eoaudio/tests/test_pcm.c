// Host-side unit tests for source/pcm.c: ring, jitter buffer / drift compensation, conversion, meter.
//   make -C switch/eoaudio/tests check        (host C compiler, not devkitPro)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/pcm.h"

static int g_fail, g_pass;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (cond) g_pass++;                                                           \
        else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }     \
    } while (0)

#define CAP 8192
static uint32_t g_store[CAP];

// ------------------------------------------------------------------ ring

static void test_ring(void) {
    pcm_ring r;
    ring_init(&r, g_store, CAP);
    CHECK(ring_capacity(&r) == CAP && ring_fill(&r) == 0);

    int16_t in[2 * 300], out[2 * 300];
    for (int i = 0; i < 300; i++) { in[2 * i] = (int16_t)i; in[2 * i + 1] = (int16_t)-i; }
    // Walk the indices across the wrap point several times.
    uint32_t total = 0;
    for (int round = 0; round < 100; round++) {
        CHECK(ring_write(&r, in, 300) == 300);
        CHECK(ring_fill(&r) == 300);
        CHECK(ring_peek(&r, out, 300) == 300);
        CHECK(memcmp(in, out, sizeof(in)) == 0);
        ring_consume(&r, 300);
        CHECK(ring_fill(&r) == 0);
        total += 300;
    }
    CHECK(r.head == total && r.tail == total);

    // Overflow: only the free space is stored.
    for (int i = 0; i < CAP / 300; i++) ring_write(&r, in, 300);
    uint32_t before = ring_fill(&r);
    uint32_t stored = ring_write(&r, in, 300);
    CHECK(stored == CAP - before && ring_fill(&r) == CAP);
    CHECK(ring_write(&r, in, 1) == 0);
    // Peek/consume never go past what is queued.
    ring_consume(&r, CAP + 100);
    CHECK(ring_fill(&r) == 0 && ring_peek(&r, out, 10) == 0);
    ring_write(&r, in, 50);
    ring_drop_all(&r);
    CHECK(ring_fill(&r) == 0);
}

// ------------------------------------------------------------------ playout simulation

typedef struct {
    double   ppm;          // producer clock error
    uint32_t burst;        // frames per producer burst (recorder buffer)
    double   jitter_us;    // +- uniform jitter on burst delivery
    double   seconds;
    uint32_t pause_at_ms, pause_ms;   // producer stalls (underrun test)
    uint32_t flood_at_ms, flood_frames;
    uint32_t target, deadband, max_fill;
} sim_cfg;

typedef struct {
    playout_stats tot;
    uint32_t fill_min_after_warmup, fill_max_after_warmup;
    uint32_t bad_steps;     // non-slip blocks whose frames are not consecutive
    uint32_t max_jump;      // largest step between consecutive output frames (slip blocks included)
    uint32_t blocks;
} sim_result;

static uint32_t g_rng = 12345;
static double rnd(void) {   // 0..1
    g_rng = g_rng * 1103515245u + 12345u;
    return (double)((g_rng >> 8) & 0xFFFF) / 65535.0;
}

static void add_stats(playout_stats *t, const playout_stats *s) {
    t->drops += s->drops;
    t->inserts += s->inserts;
    t->underruns += s->underruns;
    t->skips += s->skips;
    t->skipped_frames += s->skipped_frames;
    t->primes += s->primes;
    t->silent_blocks += s->silent_blocks;
}

#define WRAP 30000   // the source is a counter 0..WRAP-1 on both channels

static void simulate(const sim_cfg *c, sim_result *res) {
    static pcm_ring r;
    static playout p;
    ring_init(&r, g_store, CAP);
    playout_init(&p, &r, c->target, c->deadband, c->max_fill);
    memset(res, 0, sizeof(*res));
    res->fill_min_after_warmup = UINT32_MAX;

    const double rate = 48000.0 * (1.0 + c->ppm * 1e-6);
    const double burst_us = c->burst * 1e6 / rate;
    double t_prod = 0, t_cons = 0, t_last_delivery = 0;
    uint32_t src = 0;
    int last_out = -1;
    bool prev_slip = false;
    const double end_us = c->seconds * 1e6;
    bool flooded = false;
    static int16_t buf[2 * 8192], out[2 * 192];
    double jitter = 0;

    while (t_cons < end_us) {
        // Each burst is ready at t_prod and handed over up to jitter_us late, never before the previous one.
        double t_deliver = t_prod + jitter;
        if (t_deliver < t_last_delivery) t_deliver = t_last_delivery;
        if (t_deliver <= t_cons) {
            t_last_delivery = t_deliver;
            jitter = c->jitter_us ? rnd() * c->jitter_us : 0;
            bool paused = c->pause_ms && t_prod >= c->pause_at_ms * 1000.0 &&
                          t_prod < (c->pause_at_ms + c->pause_ms) * 1000.0;
            uint32_t n = c->burst;
            if (c->flood_frames && !flooded && t_prod >= c->flood_at_ms * 1000.0) {
                n += c->flood_frames;
                flooded = true;
            }
            for (uint32_t i = 0; i < n; i++) {
                buf[2 * i] = buf[2 * i + 1] = (int16_t)(src % WRAP);
                src++;
            }
            if (!paused) ring_write(&r, buf, n);
            t_prod += burst_us;
            continue;
        }
        uint32_t slips_before = p.st.drops + p.st.inserts, under_before = p.st.underruns;
        uint32_t silent_before = p.st.silent_blocks, skips_before = p.st.skips;
        uint32_t fill = ring_fill(&r);
        playout_fill(&p, &r, out, 192);
        res->blocks++;
        bool slip = p.st.drops + p.st.inserts != slips_before;
        bool gap = p.st.underruns != under_before || p.st.silent_blocks != silent_before || p.st.skips != skips_before;
        if (t_cons > 2e6 && !c->pause_ms && !c->flood_frames) {
            if (fill < res->fill_min_after_warmup) res->fill_min_after_warmup = fill;
            if (fill > res->fill_max_after_warmup) res->fill_max_after_warmup = fill;
        }
        if (!gap && p.primed) {
            // A slip block that straddles the counter wrap interpolates across the jump: skip its checks.
            bool wraps = last_out >= 0 && out[0] < last_out;
            for (int i = 1; i < 192 && !wraps; i++) wraps = out[2 * i] < out[2 * i - 2];
            for (int i = 0; i < 192 && !(slip && wraps); i++) {
                int v = out[2 * i];
                if (out[2 * i + 1] != v) res->bad_steps++;
                if (last_out >= 0) {
                    int step = v - last_out;
                    if (step == -(WRAP - 1)) step = 1;   // counter wrapped
                    // Inside a slip block (and across its end) the ramp advances by 193/192 or 191/192 per
                    // frame, so after truncation a step is 0, 1 or 2. Everywhere else it must be exactly 1.
                    bool slipping = slip || (i == 0 && prev_slip);
                    if (!slipping && step != 1) res->bad_steps++;
                    if (slipping && (step < 0 || step > 2)) res->bad_steps++;
                    uint32_t a = (uint32_t)(step < 0 ? -step : step);
                    if (a > res->max_jump) res->max_jump = a;
                }
                last_out = v;
            }
            last_out = out[2 * 191];
        } else {
            last_out = -1;
        }
        prev_slip = slip;
        t_cons += 4000;
        if (res->blocks % 250 == 0) {
            playout_stats s;
            playout_take_stats(&p, &s);
            add_stats(&res->tot, &s);
        }
    }
    playout_stats s;
    playout_take_stats(&p, &s);
    add_stats(&res->tot, &s);
}

static sim_cfg base_cfg(void) {
    sim_cfg c;
    memset(&c, 0, sizeof(c));
    c.burst = 240;
    c.seconds = 60;
    c.target = 960;      // 20 ms
    c.deadband = 48;     // 1 ms
    c.max_fill = 960 + 4800;
    return c;
}

static void test_playout_steady(void) {
    sim_cfg c = base_cfg();
    sim_result r;
    simulate(&c, &r);
    printf("  steady:  drops %u inserts %u underruns %u primes %u fill %u..%u bad %u\n", r.tot.drops,
           r.tot.inserts, r.tot.underruns, r.tot.primes, r.fill_min_after_warmup, r.fill_max_after_warmup,
           r.bad_steps);
    CHECK(r.tot.primes == 1);
    CHECK(r.tot.underruns == 0);
    CHECK(r.tot.drops + r.tot.inserts <= 4);
    CHECK(r.bad_steps == 0);
    CHECK(r.fill_min_after_warmup >= 960 - 300 && r.fill_max_after_warmup <= 960 + 300);
}

static void test_playout_drift(double ppm) {
    sim_cfg c = base_cfg();
    c.ppm = ppm;
    sim_result r;
    simulate(&c, &r);
    double expect = fabs(48000.0 * c.seconds * ppm * 1e-6);
    printf("  %+5.0f ppm: drops %u inserts %u (expect ~%.0f) underruns %u fill %u..%u bad %u\n", ppm, r.tot.drops,
           r.tot.inserts, expect, r.tot.underruns, r.fill_min_after_warmup, r.fill_max_after_warmup, r.bad_steps);
    // The deadband and the start-up offset absorb up to a few hundred frames before slips begin.
    uint32_t main = ppm > 0 ? r.tot.drops : r.tot.inserts, other = ppm > 0 ? r.tot.inserts : r.tot.drops;
    CHECK(main + 350 >= expect && main <= expect + 100);
    CHECK(other <= 10);
    CHECK(r.tot.underruns == 0 && r.tot.primes == 1 && r.tot.skips == 0);
    CHECK(r.bad_steps == 0);
    CHECK(r.fill_min_after_warmup >= 960 - 600 && r.fill_max_after_warmup <= 960 + 600);
}

static void test_playout_jitter(void) {
    sim_cfg c = base_cfg();
    c.jitter_us = 4000;   // bursts up to 4 ms early or late
    c.ppm = 80;
    sim_result r;
    simulate(&c, &r);
    printf("  jitter:  drops %u inserts %u underruns %u fill %u..%u bad %u\n", r.tot.drops, r.tot.inserts,
           r.tot.underruns, r.fill_min_after_warmup, r.fill_max_after_warmup, r.bad_steps);
    CHECK(r.tot.underruns == 0);
    CHECK(r.bad_steps == 0);
}

static void test_playout_big_bursts(void) {
    sim_cfg c = base_cfg();
    c.burst = 1024;   // a recorder that hands out 21 ms at a time needs a larger target and deadband
    c.target = 1600;
    c.deadband = 256;
    c.max_fill = 1600 + 4800;
    sim_result r;
    simulate(&c, &r);
    printf("  bursts:  drops %u inserts %u underruns %u fill %u..%u bad %u\n", r.tot.drops, r.tot.inserts,
           r.tot.underruns, r.fill_min_after_warmup, r.fill_max_after_warmup, r.bad_steps);
    CHECK(r.tot.underruns == 0 && r.bad_steps == 0);
    CHECK(r.tot.drops + r.tot.inserts <= 8);
}

static void test_playout_pause(void) {
    sim_cfg c = base_cfg();
    c.seconds = 10;
    c.pause_at_ms = 3000;
    c.pause_ms = 60;   // producer stalls for 60 ms: one underrun, then it waits for the target again
    sim_result r;
    simulate(&c, &r);
    printf("  pause:   underruns %u primes %u silent %u bad %u\n", r.tot.underruns, r.tot.primes, r.tot.silent_blocks,
           r.bad_steps);
    CHECK(r.tot.underruns == 1);
    CHECK(r.tot.primes == 2);
    CHECK(r.bad_steps == 0);
}

static void test_playout_flood(void) {
    sim_cfg c = base_cfg();
    c.seconds = 10;
    c.flood_at_ms = 3000;
    c.flood_frames = 6000;   // 125 ms arrives at once (e.g. the stream thread was stalled)
    sim_result r;
    simulate(&c, &r);
    printf("  flood:   skips %u (%u frames) underruns %u bad %u\n", r.tot.skips, r.tot.skipped_frames,
           r.tot.underruns, r.bad_steps);
    CHECK(r.tot.skips == 1 && r.tot.skipped_frames > 4800);
    CHECK(r.tot.underruns == 0);
}

static void test_playout_slip_shape(void) {
    // A drop/insert block must be a smooth resampling of the input: feed a ramp and check the output
    // block starts at the expected frame and ends one frame before the next block.
    static pcm_ring r;
    static playout p;
    ring_init(&r, g_store, CAP);
    playout_init(&p, &r, 960, 48, 960 + 4800);
    static int16_t in[2 * 4000], out[2 * 192];
    for (int i = 0; i < 4000; i++) in[2 * i] = in[2 * i + 1] = (int16_t)(i * 4);
    ring_write(&r, in, 4000);   // fill far above target: the controller drops frames
    // The error is above 4 x deadband, so every block drops one frame: 193 in, 192 out.
    playout_fill(&p, &r, out, 192);
    CHECK(p.primed && p.st.primes == 1 && p.st.drops == 1);
    CHECK(out[0] == 0);
    CHECK(out[2 * 191] >= 191 * 4 && out[2 * 191] <= 192 * 4);
    for (int i = 1; i < 192; i++) {
        int step = out[2 * i] - out[2 * i - 2];
        CHECK(step >= 3 && step <= 5);
    }
    // Each block continues exactly where the previous one stopped reading.
    playout_fill(&p, &r, out, 192);
    CHECK(p.st.drops == 2 && out[0] == 193 * 4);
    playout_fill(&p, &r, out, 192);
    CHECK(p.st.drops == 3 && out[0] == 386 * 4);
    CHECK(ring_fill(&r) == 4000 - 3 * 193);
}

// ------------------------------------------------------------------ convert / meter

static void test_convert(void) {
    int16_t out[2 * 4];
    int16_t st[8] = {1, -1, 2, -2, 3, -3, 4, -4};
    pcm_to_stereo16(st, PCM_FMT_S16, 2, 4, out);
    CHECK(memcmp(out, st, sizeof(st)) == 0);

    int16_t mono[4] = {100, -200, 300, -32768};
    pcm_to_stereo16(mono, PCM_FMT_S16, 1, 4, out);
    CHECK(out[0] == 100 && out[1] == 100 && out[6] == -32768 && out[7] == -32768);

    // 5.1: L = FL + FC/2 + BL/2, R = FR + FC/2 + BR/2, saturated; LFE dropped.
    int16_t six[12] = {1000, 2000, 400, 30000, 200, -200, 30000, 30000, 20000, 0, 20000, 20000};
    pcm_to_stereo16(six, PCM_FMT_S16, 6, 2, out);
    CHECK(out[0] == 1000 + 200 + 100 && out[1] == 2000 + 200 - 100);
    CHECK(out[2] == 32767 && out[3] == 32767);

    float fl[4] = {0.5f, -1.0f, 2.0f, -2.0f};
    pcm_to_stereo16(fl, PCM_FMT_FLOAT, 2, 2, out);
    CHECK(out[0] == 16384 && out[1] == -32768 && out[2] == 32767 && out[3] == -32768);

    int32_t s32[2] = {0x40000000, -0x40000000};
    pcm_to_stereo16(s32, PCM_FMT_S32, 2, 1, out);
    CHECK(out[0] == 16384 && out[1] == -16384);

    int16_t quad[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    pcm_to_stereo16(quad, PCM_FMT_S16, 4, 2, out);
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 5 && out[3] == 6);

    pcm_to_stereo16(quad, 6 /* ADPCM: unsupported */, 2, 2, out);
    CHECK(out[0] == 0 && out[3] == 0);
    CHECK(pcm_bytes_per_sample(PCM_FMT_S16) == 2 && pcm_bytes_per_sample(PCM_FMT_FLOAT) == 4 &&
          pcm_bytes_per_sample(PCM_FMT_S24) == 0);
}

static void test_sentinel(void) {
    uint8_t buf[64];
    pcm_fill_u16(buf, sizeof(buf), 0x5A5A);
    CHECK(pcm_trailing_u16(buf, sizeof(buf), 0x5A5A) == 32);
    memset(buf, 0, 40);
    CHECK(pcm_trailing_u16(buf, sizeof(buf), 0x5A5A) == 12);
    memset(buf, 0, sizeof(buf));
    CHECK(pcm_trailing_u16(buf, sizeof(buf), 0x5A5A) == 0);
}

static void test_gain(void) {
    CHECK(pcm_gain_q15_from_db(0.0f) == PCM_UNITY_Q15 && pcm_gain_q15_from_db(3.0f) == PCM_UNITY_Q15);
    CHECK(pcm_gain_q15_from_db(-100.0f) == 0 && pcm_gain_q15_from_db(-150.0f) == 0);
    uint32_t g6 = pcm_gain_q15_from_db(-6.0206f);   // one half
    CHECK(g6 >= 16383 && g6 <= 16385);
    CHECK(pcm_gain_q15_from_db(-20.0f) == 3277);
    int16_t s[6] = {32767, -32768, 1000, -1000, 1, -1};
    int16_t keep[6];
    memcpy(keep, s, sizeof(s));
    pcm_apply_gain_q15(s, 6, PCM_UNITY_Q15);
    CHECK(memcmp(s, keep, sizeof(s)) == 0);
    pcm_apply_gain_q15(s, 6, 16384);
    CHECK(s[0] == 16383 && s[1] == -16384 && s[2] == 500 && s[3] == -500 && s[4] == 0 && s[5] == 0);
    pcm_apply_gain_q15(s, 6, 0);
    CHECK(s[0] == 0 && s[1] == 0 && s[2] == 0);
}

static void test_meter(void) {
    static int16_t sine[2 * 4800];
    for (int i = 0; i < 4800; i++) {
        sine[2 * i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * 1000.0 * i / 48000.0));
        sine[2 * i + 1] = (int16_t)lrint(3276.7 * sin(2 * M_PI * 1000.0 * i / 48000.0));
    }
    pcm_meter m;
    meter_reset(&m);
    CHECK(meter_rms_db(&m, 0) == -120.0f);
    meter_add(&m, sine, 2400);
    meter_add(&m, sine + 4800, 2400);
    CHECK(fabsf(meter_rms_db(&m, 0) - -3.01f) < 0.05f);
    CHECK(fabsf(meter_rms_db(&m, 1) - -23.01f) < 0.05f);
    CHECK(fabsf(meter_peak_db(&m, 0)) < 0.01f);
    pcm_meter m2;
    meter_reset(&m2);
    static int16_t zero[2 * 100];
    meter_add(&m2, zero, 100);
    CHECK(meter_rms_db(&m2, 0) == -120.0f && meter_peak_db(&m2, 1) == -120.0f);
    meter_merge(&m2, &m);
    CHECK(m2.frames == 4900 && m2.peak[0] == 32767);
    CHECK(meter_rms_db_both(&m) < meter_rms_db(&m, 0) && meter_rms_db_both(&m) > meter_rms_db(&m, 1));
}

int main(void) {
    test_ring();
    test_playout_steady();
    test_playout_drift(+200);
    test_playout_drift(-200);
    test_playout_drift(+1000);
    test_playout_drift(-1000);
    test_playout_jitter();
    test_playout_big_bursts();
    test_playout_pause();
    test_playout_flood();
    test_playout_slip_shape();
    test_convert();
    test_sentinel();
    test_gain();
    test_meter();
    printf("test_pcm: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
