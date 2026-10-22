#include "recorder.h"

#include "log.h"

#define REC_MAX_BUFS  32
#define REC_SENTINEL  0x5A5A    // pre-filled into every buffer: what is still there was not recorded
#define REC_STALL_MS  10000     // no buffer for this long: close and reopen the recorder (capture)
#define REC_STALL_STREAM_MS 3000   // same while the earphone is waiting for audio (stream)
#define REC_MAX_MEMORY (256 * 1024)

static struct {
    rec_config     cfg;
    Thread         thread;
    bool           thread_ok;
    volatile bool  stop;

    Mutex          mutex;       // pub, pub_seq
    rec_interval   pub;
    u32            pub_seq, taken_seq;
    rec_interval   cur;         // recorder thread only
    u64            cur_start_ms;
    u64            last_event_us;
    bool           probe_due;

    AudrecRecorder rec;
    Event          ev;
    u8            *region;
    u32            cap, data_size, frame_bytes;
    u32            rate, channels, format;
    u32            sessions;
    int16_t        stereo[2 * REC_MAX_FRAMES];
    u64            frames_total;
} R;

u64 rec_frames_total(void) { return __atomic_load_n(&R.frames_total, __ATOMIC_RELAXED); }

// ------------------------------------------------------------------ commands libnx does not wrap

// IFinalOutputRecorder 0: GetFinalOutputRecorderState -> u32 (0 started, 1 stopped).
static Result rec_get_state(u32 *state) { return serviceDispatchOut(&R.rec.s, 0, *state); }

// IFinalOutputRecorder 7: GetFinalOutputRecorderBufferEndTime(u64 BufferClientPtr) -> u64.
static Result rec_get_end_time(u64 tag, u64 *out) { return serviceDispatchInOut(&R.rec.s, 7, tag, *out); }

// ------------------------------------------------------------------ stats

static void interval_reset(u64 now) {
    memset(&R.cur, 0, sizeof(R.cur));
    R.cur.gap_min_us = UINT32_MAX;
    R.cur.state = UINT32_MAX;
    R.cur.end_time_rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    R.cur_start_ms = now;
    R.probe_due = true;
}

static void interval_publish(bool running) {
    u64 now = now_ms();
    if (now - R.cur_start_ms < 1000) return;
    R.cur.t_ms = now;
    R.cur.dur_ms = (u32)(now - R.cur_start_ms);
    R.cur.running = running;
    R.cur.session = R.sessions;
    R.cur.rate = R.rate;
    R.cur.channels = R.channels;
    R.cur.format = R.format;
    if (running) R.cur.state_rc = rec_get_state(&R.cur.state);
    mutexLock(&R.mutex);
    R.pub = R.cur;
    R.pub_seq++;
    mutexUnlock(&R.mutex);
    interval_reset(now);
}

bool rec_take(rec_interval *out) {
    mutexLock(&R.mutex);
    bool fresh = R.pub_seq != R.taken_seq;
    if (fresh) {
        *out = R.pub;
        R.taken_seq = R.pub_seq;
    }
    mutexUnlock(&R.mutex);
    return fresh;
}

// ------------------------------------------------------------------ session

static Result append(u32 i) {
    u8 *buf = R.region + (size_t)i * R.cap;
    pcm_fill_u16(buf, R.data_size, REC_SENTINEL);
    armDCacheFlush(buf, R.cap);
    FinalOutputRecorderBuffer b;
    memset(&b, 0, sizeof(b));
    b.sample_buffer_ptr = (u64)(uintptr_t)buf;
    b.sample_buffer_capacity = R.cap;
    b.data_size = R.data_size;
    return audrecRecorderAppendFinalOutputRecorderBuffer(&R.rec, (u64)(uintptr_t)buf, &b);
}

static void process(u32 i) {
    u8 *buf = R.region + (size_t)i * R.cap;
    armDCacheFlush(buf, R.cap);   // the service wrote this behind the CPU's back
    u32 left = pcm_trailing_u16(buf, R.data_size, REC_SENTINEL) * 2;
    u32 bytes = R.data_size - left;
    bytes -= bytes % R.frame_bytes;
    u32 frames = bytes / R.frame_bytes;
    R.cur.buffers++;
    R.cur.frames += frames;
    __atomic_fetch_add(&R.frames_total, frames, __ATOMIC_RELAXED);
    if (frames < R.cfg.frames_per_buffer) R.cur.partial++;
    if (!frames) return;
    pcm_to_stereo16(buf, (int)R.format, R.channels, frames, R.stereo);
    meter_add(&R.cur.meter, R.stereo, frames);
    if (R.cfg.ring && R.rate == 48000) R.cur.ring_dropped += frames - ring_write(R.cfg.ring, R.stereo, frames);
}

static Result open_recorder(void) {
    FinalOutputRecorderParameter in = {48000, 2};
    FinalOutputRecorderParameterInternal out;
    // aruid 0 is what other sysmodules pass to audio services; the others are fallbacks for stage 1.
    u64 aruids[3], self = 0;
    int n = 0;
    aruids[n++] = 0;
    if (R_SUCCEEDED(svcGetProcessId(&self, CUR_PROCESS_HANDLE)) && self) aruids[n++] = self;
    u64 app = app_pid();
    if (app && app != self) aruids[n++] = app;
    Result rc = 0;
    for (int i = 0; i < n; i++) {
        memset(&out, 0, sizeof(out));
        rc = audrecOpenFinalOutputRecorder(&R.rec, &in, aruids[i], &out);
        if (R.sessions == 0 || R_FAILED(rc))
            log_printf("rec: OpenFinalOutputRecorder(48000 Hz, 2 ch, aruid 0x%llx%s): %s", (unsigned long long)aruids[i],
                       i == 0 ? "" : aruids[i] == self ? " = own pid" : " = application pid", rc_str(rc));
        if (R_SUCCEEDED(rc)) break;
    }
    if (R_FAILED(rc)) return rc;
    R.sessions++;
    R.rate = out.sample_rate;
    R.channels = out.channel_count;
    R.format = out.sample_format;
    log_printf("rec: recorder #%u open: sample_rate %u, channel_count %u, sample_format %u, state %u", R.sessions,
               out.sample_rate, out.channel_count, out.sample_format, out.state);
    return 0;
}

static Result run_session(void) {
    Result rc = open_recorder();
    if (R_FAILED(rc)) return rc;

    u32 bps = pcm_bytes_per_sample((int)R.format);
    if (!bps || R.channels == 0 || R.channels > 8) {
        log_printf("rec: unsupported format %u / %u channels", R.format, R.channels);
        audrecRecorderClose(&R.rec);
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    if (R.rate != 48000) log_printf("rec: rate is %u Hz, not 48000: levels are logged but nothing is played", R.rate);
    R.frame_bytes = R.channels * bps;
    R.data_size = R.cfg.frames_per_buffer * R.frame_bytes;
    R.cap = ALIGN_UP(R.data_size, 0x1000);
    if ((size_t)R.cap * R.cfg.buffers > REC_MAX_MEMORY) {
        R.cfg.buffers = REC_MAX_MEMORY / R.cap < 2 ? 2 : REC_MAX_MEMORY / R.cap;
        log_printf("rec: %u-byte buffers: queueing only %u to stay within %u KiB", R.cap, R.cfg.buffers,
                   REC_MAX_MEMORY / 1024);
    }
    R.region = memalign(0x1000, (size_t)R.cap * R.cfg.buffers);
    if (!R.region) {
        audrecRecorderClose(&R.rec);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    bool have_ev = false, started = false;
    rc = audrecRecorderRegisterBufferEvent(&R.rec, &R.ev);
    have_ev = R_SUCCEEDED(rc);
    if (R_FAILED(rc)) log_printf("rec: RegisterBufferEvent: %s", rc_str(rc));
    for (u32 i = 0; i < R.cfg.buffers && R_SUCCEEDED(rc); i++) {
        rc = append(i);
        if (R_FAILED(rc)) log_printf("rec: AppendFinalOutputRecorderBuffer #%u (%u bytes): %s", i, R.data_size, rc_str(rc));
    }
    if (R_SUCCEEDED(rc)) {
        rc = audrecRecorderStart(&R.rec);
        started = R_SUCCEEDED(rc);
        u32 st = UINT32_MAX;
        Result rs = rec_get_state(&st);
        log_printf("rec: Start: %s; %u buffers x %u frames (%u bytes, %.1f ms); GetState: %u (%s)", rc_str(rc),
                   R.cfg.buffers, R.cfg.frames_per_buffer, R.data_size, R.cfg.frames_per_buffer / 48.0, st, rc_str(rs));
    }

    u32 idle_ms = 0;
    R.last_event_us = 0;
    interval_reset(now_ms());
    while (started && !R.stop) {
        rc = eventWait(&R.ev, 100000000ULL);
        if (R_FAILED(rc)) {
            idle_ms += 100;
            if (idle_ms == 3000) {
                u32 st = UINT32_MAX;
                Result rs = rec_get_state(&st);
                log_printf("rec: no buffer released for 3 s (state %u, %s)", st, rc_str(rs));
            }
            if (idle_ms >= (R.cfg.ring ? REC_STALL_STREAM_MS : REC_STALL_MS)) {
                rc = MAKERESULT(Module_Libnx, LibnxError_Timeout);
                break;
            }
            interval_publish(true);
            continue;
        }
        u64 t = now_us();
        u64 tags[REC_MAX_BUFS];
        u64 count = REC_MAX_BUFS, released = 0;
        rc = audrecRecorderGetReleasedFinalOutputRecorderBuffers(&R.rec, tags, &count, &released);
        if (R_FAILED(rc)) {
            log_printf("rec: GetReleasedFinalOutputRecorderBuffers: %s", rc_str(rc));
            break;
        }
        R.cur.events++;
        R.cur.released = released;
        if (count > REC_MAX_BUFS) count = REC_MAX_BUFS;
        if (count > R.cur.max_per_get) R.cur.max_per_get = (u32)count;
        if (count && R.last_event_us) {
            u32 gap = (u32)(t - R.last_event_us);
            if (gap < R.cur.gap_min_us) R.cur.gap_min_us = gap;
            if (gap > R.cur.gap_max_us) R.cur.gap_max_us = gap;
            R.cur.gap_sum_us += gap;
            R.cur.gaps++;
        }
        if (count) {
            R.last_event_us = t;
            idle_ms = 0;
        }
        for (u64 k = 0; k < count && R_SUCCEEDED(rc); k++) {
            u64 off = tags[k] - (u64)(uintptr_t)R.region;
            if (tags[k] < (u64)(uintptr_t)R.region || off % R.cap || off / R.cap >= R.cfg.buffers) {
                log_printf("rec: released an unknown buffer 0x%llx", (unsigned long long)tags[k]);
                continue;
            }
            u32 i = (u32)(off / R.cap);
            process(i);
            if (R.probe_due) {
                R.probe_due = false;
                R.cur.end_time_rc = rec_get_end_time(tags[k], &R.cur.end_time);
                R.cur.end_time_now_ns = armTicksToNs(armGetSystemTick());
            }
            rc = append(i);
            if (R_FAILED(rc)) log_printf("rec: re-append #%u: %s", i, rc_str(rc));
        }
        if (R_FAILED(rc)) break;
        interval_publish(true);
    }

    if (started) {
        Result rs = audrecRecorderStop(&R.rec);
        if (R_FAILED(rs)) log_printf("rec: Stop: %s", rc_str(rs));
    }
    audrecRecorderClose(&R.rec);   // the service may write into the buffers until here
    if (have_ev) eventClose(&R.ev);
    free(R.region);
    R.region = NULL;
    return R.stop ? 0 : rc;
}

static void sleep_ms_interruptible(u32 ms) {
    for (u32 t = 0; t < ms && !R.stop; t += 100) {
        svcSleepThread(100000000ULL);
        interval_publish(false);
    }
}

static void rec_thread(void *arg) {
    u32 backoff = 1000;
    Result rc = audrecInitialize();
    log_printf("rec: audrecInitialize (audrec:u): %s", rc_str(rc));
    for (int tries = 1; R_FAILED(rc) && !R.stop; tries++) {
        sleep_ms_interruptible(tries < 10 ? 1000 : 30000);
        rc = audrecInitialize();
        if (R_SUCCEEDED(rc) || tries < 3) log_printf("rec: audrecInitialize retry %d: %s", tries, rc_str(rc));
    }
    interval_reset(now_ms());
    while (!R.stop) {
        u64 t0 = now_ms();
        rc = run_session();
        if (R.stop) break;
        u64 ran = now_ms() - t0;
        if (ran > 30000) backoff = 1000;
        log_printf("rec: recorder session ended (%s) after %llu ms, reopening in %u s", rc_str(rc),
                   (unsigned long long)ran, backoff / 1000);
        sleep_ms_interruptible(backoff);
        backoff = backoff * 2 > 60000 ? 60000 : backoff * 2;
    }
    audrecExit();
}

Result rec_start(const rec_config *cfg) {
    if (R.thread_ok) return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    R.cfg = *cfg;
    if (R.cfg.frames_per_buffer > REC_MAX_FRAMES) R.cfg.frames_per_buffer = REC_MAX_FRAMES;
    if (R.cfg.frames_per_buffer < 48) R.cfg.frames_per_buffer = 48;
    if (R.cfg.buffers > REC_MAX_BUFS) R.cfg.buffers = REC_MAX_BUFS;
    if (R.cfg.buffers < 2) R.cfg.buffers = 2;
    R.stop = false;
    mutexInit(&R.mutex);
    Result rc = thread_spawn(&R.thread, rec_thread, NULL, 0x4000, PRIO_REC);
    R.thread_ok = R_SUCCEEDED(rc);
    return rc;
}

void rec_stop(void) {
    if (!R.thread_ok) return;
    R.stop = true;
    threadWaitForExit(&R.thread);
    threadClose(&R.thread);
    R.thread_ok = false;
}

bool rec_running(void) { return R.thread_ok; }
