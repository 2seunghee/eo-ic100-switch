// Portable PCM helpers for eoaudio. No libnx dependency: unit-tested on the host (tests/test_pcm.c).
//   pcm_ring  single-producer / single-consumer ring of interleaved stereo s16 frames
//   playout   jitter buffer on the consumer side: waits until `target` frames are queued, then keeps
//             the fill near the target by dropping or inserting one frame now and then. The slip is
//             spread over a whole output block by linear interpolation, so it never clicks. This
//             absorbs the clock difference between the recorder (audio renderer) and the USB frames.
//   convert   any recorder format / channel count to stereo s16
//   meter     RMS and peak per channel
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ------------------------------------------------------------------ ring

typedef struct {
    uint32_t *buf;          // one stereo s16 frame per element
    uint32_t  mask;         // capacity - 1; the capacity is a power of two
    uint32_t  head, tail;   // free-running frame counters: head is written by the producer, tail by the consumer
} pcm_ring;

void     ring_init(pcm_ring *r, uint32_t *storage, uint32_t capacity_pow2);
uint32_t ring_capacity(const pcm_ring *r);
uint32_t ring_fill(const pcm_ring *r);                                     // frames queued
uint32_t ring_write(pcm_ring *r, const int16_t *stereo, uint32_t frames);   // producer; returns frames stored
uint32_t ring_peek(const pcm_ring *r, int16_t *out, uint32_t frames);      // consumer; copies, does not consume
void     ring_consume(pcm_ring *r, uint32_t frames);                         // consumer
void     ring_drop_all(pcm_ring *r);                                         // consumer: discard everything queued

// ------------------------------------------------------------------ playout

#define PLAYOUT_MAX_BLOCK 1024   // frames per playout_fill() call
#define PLAYOUT_SLIP_GAP  25     // blocks between slips while the error is small (25 x 4 ms = 100 ms)

typedef struct {
    uint32_t drops, inserts;          // one-frame slips (drift correction)
    uint32_t underruns;               // blocks that ran dry and were padded with silence
    uint32_t skips, skipped_frames;   // fill above max_fill: cut back to the target
    uint32_t primes;                  // (re)starts after the target was reached
    uint32_t silent_blocks;           // blocks of silence while waiting for the target
    uint32_t fill_min, fill_max;      // queued frames seen at block start
    uint64_t fill_sum;
    uint32_t fill_n;
} playout_stats;

typedef struct {
    uint32_t      target, deadband, max_fill;   // frames
    bool          primed;
    float         ema;                          // smoothed fill (time constant 64 blocks)
    uint32_t      since_slip;                   // blocks since the last slip
    playout_stats st;
    uint32_t      tmp[PLAYOUT_MAX_BLOCK + 2];
} playout;

// target/max_fill are clamped to what the ring can hold.
void playout_init(playout *p, const pcm_ring *r, uint32_t target, uint32_t deadband, uint32_t max_fill);
void playout_reset(playout *p);                  // wait for the target again; keeps the settings
// Writes exactly `frames` (<= PLAYOUT_MAX_BLOCK) stereo frames to out.
void playout_fill(playout *p, pcm_ring *r, int16_t *out, uint32_t frames);
void playout_take_stats(playout *p, playout_stats *out);   // copy, then reset (consumer thread)
void playout_stats_clear(playout_stats *s);

// ------------------------------------------------------------------ convert

// Sample formats, same values as libnx PcmFormat.
enum { PCM_FMT_S8 = 1, PCM_FMT_S16 = 2, PCM_FMT_S24 = 3, PCM_FMT_S32 = 4, PCM_FMT_FLOAT = 5 };

uint32_t pcm_bytes_per_sample(int fmt);   // 0 = not supported here (S8, S24, ADPCM)
// Interleaved `channels`-channel samples to stereo s16: 1 channel is duplicated, 2 are copied,
// 6 or more are read as 5.1 (FL FR FC LFE BL BR) and mixed down, 3..5 use the first two channels.
void pcm_to_stereo16(const void *in, int fmt, uint32_t channels, uint32_t frames, int16_t *out);
// Trailing 16-bit words equal to `pattern`: tells how much of a pre-filled buffer was not written.
uint32_t pcm_trailing_u16(const void *buf, uint32_t bytes, uint16_t pattern);
void pcm_fill_u16(void *buf, uint32_t bytes, uint16_t pattern);

// ------------------------------------------------------------------ gain
// Digital volume, used only where the earphone's own volume control (FU5) is unavailable, and for
// muting the earphone output.

#define PCM_UNITY_Q15 32768u

uint32_t pcm_gain_q15_from_db(float db);   // 0 dB and above = unity, -100 dB or less = 0
void     pcm_apply_gain_q15(int16_t *samples, uint32_t count, uint32_t gain_q15);   // in place

// ------------------------------------------------------------------ meter

typedef struct {
    uint64_t sumsq[2];
    uint32_t peak[2];
    uint64_t frames;
} pcm_meter;

void  meter_reset(pcm_meter *m);
void  meter_add(pcm_meter *m, const int16_t *stereo, uint32_t frames);
void  meter_merge(pcm_meter *into, const pcm_meter *m);
float meter_rms_db(const pcm_meter *m, int ch);    // dBFS (full-scale sine = -3); -120 if silent or empty
float meter_peak_db(const pcm_meter *m, int ch);
float meter_rms_db_both(const pcm_meter *m);       // both channels together
float db_from_power(double mean_square);           // mean square of s16 samples -> dBFS
