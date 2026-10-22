#pragma once
#include "common.h"
#include "pcm.h"

// The audio source: libnx audrec FinalOutputRecorder (the system's final mix). Runs in its own
// thread, reopens the recorder with back-off when it stops delivering, and publishes one stats
// interval per second for the log.

#define REC_MAX_FRAMES 4800   // per buffer (100 ms); matches the rec_frames limit in config.c

typedef struct {
    u32       frames_per_buffer;   // 240 = 5 ms at 48 kHz
    u32       buffers;             // kept queued at the recorder
    pcm_ring *ring;                // stream mode: converted stereo s16 goes here. NULL = measure only
} rec_config;

typedef struct {
    u64       t_ms;                // end of the interval (now_ms clock)
    u32       dur_ms;
    bool      running;             // recorder open and started during this interval
    u32       session;             // how many times the recorder has been opened
    u32       rate, channels, format;
    u32       events, buffers, partial, max_per_get;
    u64       frames;
    u32       gap_min_us, gap_max_us, gaps;   // time between buffer events
    u64       gap_sum_us;
    u32       ring_dropped;        // frames the ring had no room for
    pcm_meter meter;
    u32       state;               // GetFinalOutputRecorderState: 0 started, 1 stopped
    Result    state_rc;
    u64       end_time;            // GetFinalOutputRecorderBufferEndTime of one released buffer (raw)
    Result    end_time_rc;
    u64       end_time_now_ns;     // system tick in ns when end_time was read
    u64       released;            // 'released' output of the last GetReleased call (raw)
} rec_interval;

Result rec_start(const rec_config *cfg);   // spawns the thread; it keeps retrying on its own
void   rec_stop(void);
bool   rec_running(void);                  // thread alive
bool   rec_take(rec_interval *out);        // newest complete interval; false if none since the last call
u64    rec_frames_total(void);             // frames delivered since start (any thread): liveness
