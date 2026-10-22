// eoaudio: background Atmosphère sysmodule that plays the Switch's final audio mix through the
// Samsung EO-IC100 USB-C earphone (docs/PLAN.md §5 B). config.ini picks the mode:
//   stream   default: final mix -> jitter buffer -> isochronous OUT; the console speaker/TV is
//            silenced while the earphone plays (with a self-check that the recording survives),
//            eoaudio steps aside when another output is active; remote -> volume / pause
//   capture  diagnostics: record the final mix and log level and timing, hold the earphone without
//            sound, log the remote, one speaker-mute test per boot
//   tone     diagnostics: test tone over the same USB path (no recorder)
//   off      do nothing
// Every run logs to its own file, sdmc:/switch/eoaudio/logs/eoaudio_YYYYMMDD_HHMMSS.txt.
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"
#include "config.h"
#include "device.h"
#include "isoc.h"
#include "log.h"
#include "media.h"
#include "osd.h"
#include "pcm.h"
#include "policy.h"
#include "recorder.h"
#include "speaker.h"
#include "state.h"
#include "tone.h"

#define INNER_HEAP_SIZE 0x100000  // nv transfer memory 256 KiB, thread stacks, USB/recorder buffers; the overlay framebuffers use osd.c's own heap region
#define CONFIG_PATH     APP_DIR "/config.ini"
#define STATE_PATH      APP_DIR "/state.ini"
#define GUARD_PATH      APP_DIR "/bootguard.txt"
#define GUARD_LIMIT     3          // runs in a row that ended within GUARD_OK_MS: stay off
#define GUARD_OK_MS     60000ULL
#define RING_FRAMES     16384      // 341 ms
#define POLL_MS         250        // audctl snapshot / output policy
#define STREAM_OK_BATCHES 12       // ~50 ms of delivered audio before the speaker is touched
#define CAPTURE_STALE_MS  5000     // no recorder frames for this long: the speaker must play again

// ------------------------------------------------------------------ libnx sysmodule boilerplate

u32 __nx_applet_type = AppletType_None;   // sysmodules must not use applet services
u32 __nx_fs_num_sessions = 1;
// The volume overlay needs nvdrv; the default transfer memory (8 MiB) is far too much for a sysmodule.
u32 __nx_nv_transfermem_size = 0x40000;
NvServiceType __nx_nv_service_type = NvServiceType_System;
void __libnx_init_time(void);              // libnx: sets the local time zone for localtime()

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

static bool g_time_ok, g_fs_ok, g_sd_ok;

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    if (R_SUCCEEDED(timeInitialize())) {
        __libnx_init_time();
        g_time_ok = true;
    }
    // No abort without the SD card: a sysmodule that aborts during boot takes the console down
    // with it, every boot. eoaudio then runs on defaults without a log.
    g_fs_ok = R_SUCCEEDED(fsInitialize());
    g_sd_ok = g_fs_ok && R_SUCCEEDED(fsdevMountSdmc());
    // sm stays open: the other services are opened later, depending on the mode.
}

void __appExit(void) {
    if (g_sd_ok) fsdevUnmountAll();
    if (g_fs_ok) fsExit();
    if (g_time_ok) timeExit();
    smExit();
}

// ------------------------------------------------------------------ state

static eo_config     g_cfg;
static eo_state      g_state;
static bool          g_state_dirty;
static u64           g_state_dirty_ms;
static bool          g_usb_ok;
static pol           g_pol;
static spk_state     g_spk;            // newest audctl snapshot
static bool          g_spk_ok;
static pcm_ring      g_ring;
static uint32_t      g_ring_store[RING_FRAMES];
static playout       g_play;
static Mutex         g_play_mutex;
static playout_stats g_play_acc;       // merged by the streaming thread, taken by the monitor
static u32           g_target_frames;
static tone_gen      g_tone;
static u64           g_stream_t0;
static u64           g_rec_alive_ms;   // last recorder interval with frames (stream mode)
static u64           g_app_pid, g_app_program;
static bool          g_app_known;      // pm:dmnt answered at least once

u64 app_pid(void) { return __atomic_load_n(&g_app_pid, __ATOMIC_RELAXED); }

// Recorder intervals summed up for one log line.
typedef struct {
    u32          intervals, buffers, partial, max_per_get, gaps, gap_min, gap_max, ring_dropped;
    u64          frames, gap_sum, dur_ms;
    pcm_meter    meter;
    rec_interval last;
} rec_acc;

typedef struct {
    rec_acc       rec;
    playout_stats play;
    iso_stats     iso_prev;    // iso counters at the start of the period
    u32           iso_max_gap;
    u32           quiet;       // periods of digital silence skipped (capture mode)
} period;

static struct {
    u32    ticks;
    period detail, health;     // 1 s (log_level=debug) and health_s
} MON;

static void racc_reset(rec_acc *a) {
    memset(a, 0, sizeof(*a));
    a->gap_min = UINT32_MAX;
}

static void racc_add(rec_acc *a, const rec_interval *ri) {
    a->intervals++;
    a->buffers += ri->buffers;
    a->partial += ri->partial;
    a->frames += ri->frames;
    a->dur_ms += ri->dur_ms;
    a->ring_dropped += ri->ring_dropped;
    if (ri->max_per_get > a->max_per_get) a->max_per_get = ri->max_per_get;
    if (ri->gaps) {
        if (ri->gap_min_us < a->gap_min) a->gap_min = ri->gap_min_us;
        if (ri->gap_max_us > a->gap_max) a->gap_max = ri->gap_max_us;
        a->gap_sum += ri->gap_sum_us;
        a->gaps += ri->gaps;
    }
    meter_merge(&a->meter, &ri->meter);
    a->last = *ri;
}

static void play_merge(playout_stats *into, const playout_stats *s) {
    into->drops += s->drops;
    into->inserts += s->inserts;
    into->underruns += s->underruns;
    into->skips += s->skips;
    into->skipped_frames += s->skipped_frames;
    into->primes += s->primes;
    into->silent_blocks += s->silent_blocks;
    if (s->fill_n) {
        if (s->fill_min < into->fill_min) into->fill_min = s->fill_min;
        if (s->fill_max > into->fill_max) into->fill_max = s->fill_max;
        into->fill_sum += s->fill_sum;
        into->fill_n += s->fill_n;
    }
}

static void period_reset(period *p, const iso_stats *now) {
    racc_reset(&p->rec);
    playout_stats_clear(&p->play);
    if (now) p->iso_prev = *now;
    else memset(&p->iso_prev, 0, sizeof(p->iso_prev));
    p->iso_max_gap = 0;
}

// ------------------------------------------------------------------ state.ini

static void state_load(void) {
    static char text[2048];
    st_init(&g_state);
    long n = read_file(STATE_PATH, text, sizeof(text));
    if (n >= 0) st_parse(&g_state, text, (size_t)n);
    u32 v = hosversionGet();
    char hos[16];
    snprintf(hos, sizeof(hos), "%u.%u.%u", HOSVER_MAJOR(v), HOSVER_MINOR(v), HOSVER_MICRO(v));
    bool any = false;
    for (int t = 0; t < ST_TARGETS; t++)
        for (int m = 0; m < ST_METHODS; m++) any |= g_state.verdict[t][m] != VERDICT_UNKNOWN;
    if (any && strcmp(g_state.hos, hos) != 0) {
        log_printf("state: speaker verdicts were measured on HOS %s, this is %s: forgotten", g_state.hos, hos);
        st_forget_verdicts(&g_state);
    }
    snprintf(g_state.hos, sizeof(g_state.hos), "%s", hos);
    char line[256];
    int k = st_format(&g_state, line, sizeof(line));
    for (char *c = line; k > 0 && *c; c++)
        if (*c == '\n') *c = ' ';
    log_printf("state: %s%s", n >= 0 ? "" : "(no state.ini yet) ", k > 0 ? line : "");
}

static void state_touch(void) {
    g_state_dirty = true;
    g_state_dirty_ms = now_ms();
}

static void state_save(void) {
    char text[1024];
    int n = st_format(&g_state, text, sizeof(text));
    bool ok = n > 0 && save_file(STATE_PATH, text, (size_t)n);
    g_state_dirty = false;
    LOGD("state: saved %s (%s)", STATE_PATH, ok ? "OK" : "failed");
    if (!ok) log_printf("state: could not write %s", STATE_PATH);
}

// ------------------------------------------------------------------ audio path (device hooks)

static void fill_stream(void *ctx, int16_t *out, u32 frames) {
    playout_fill(&g_play, &g_ring, out, frames);
    pcm_apply_gain_q15(out, frames * 2, dev_output_gain_q15());
}

static void fill_tone(void *ctx, int16_t *out, u32 frames) {
    tone_fill(&g_tone, out, frames);
    pcm_apply_gain_q15(out, frames * 2, dev_output_gain_q15());
}

static void tick_stream(void *ctx) {   // streaming thread, about once per second
    playout_stats s;
    playout_take_stats(&g_play, &s);
    mutexLock(&g_play_mutex);
    play_merge(&g_play_acc, &s);
    mutexUnlock(&g_play_mutex);
}

static bool audio_start(void *ctx, UsbHsClientIfSession *as, const struct usb_endpoint_descriptor *epd,
                        u32 packet_bytes) {
    iso_config ic = {16, (u16)g_cfg.iso_urbs, (u16)g_cfg.iso_batches, packet_bytes, (u32)g_cfg.iso_lead};
    period_reset(&MON.detail, NULL);
    period_reset(&MON.health, NULL);
    Result rc;
    if (g_cfg.mode == MODE_TONE) {
        tone_init(&g_tone, g_cfg.tone_hz, 48000, g_cfg.tone_dbfs, TONE_BOTH);
        rc = iso_start(as, epd, &ic, fill_tone, NULL, NULL);
        log_printf("audio: test tone %.0f Hz at %.0f dBFS: %s", (double)g_cfg.tone_hz, (double)g_cfg.tone_dbfs,
                   rc_str(rc));
    } else {
        ring_init(&g_ring, g_ring_store, RING_FRAMES);
        u32 rec_frames = (u32)g_cfg.rec_frames;
        u32 target = (u32)g_cfg.target_ms * 48;
        if (target < rec_frames + 2 * 192) target = rec_frames + 2 * 192;   // at least one recorder buffer
        u32 deadband = rec_frames / 4 > 48 ? rec_frames / 4 : 48;
        playout_init(&g_play, &g_ring, target, deadband, target + 4800);
        g_target_frames = g_play.target;
        mutexLock(&g_play_mutex);
        playout_stats_clear(&g_play_acc);
        mutexUnlock(&g_play_mutex);
        rec_config rcfg = {rec_frames, (u32)g_cfg.rec_buffers, &g_ring};
        rc = rec_start(&rcfg);
        if (R_FAILED(rc)) {
            log_printf("audio: recorder thread: %s", rc_str(rc));
            return false;
        }
        rc = iso_start(as, epd, &ic, fill_stream, tick_stream, NULL);
        if (R_FAILED(rc)) rec_stop();
        log_printf("audio: final mix -> earphone, jitter buffer %.1f ms (deadband %u frames), %d x %d ms in flight, "
                   "latency ~%u ms + the recorder's own: %s",
                   g_play.target / 48.0, deadband, g_cfg.iso_batches, g_cfg.iso_urbs,
                   (unsigned)(g_play.target / 48 + rec_frames / 48 + (u32)(g_cfg.iso_batches * g_cfg.iso_urbs)),
                   rc_str(rc));
    }
    if (R_FAILED(rc)) return false;
    g_stream_t0 = now_ms();
    g_rec_alive_ms = 0;   // this stream's recorder has not delivered yet
    return true;
}

static void audio_stop(void *ctx) {
    iso_stop();
    if (g_cfg.mode == MODE_STREAM) {
        rec_stop();
        pol_stop(&g_pol, "earphone stream stopped");
    }
    spk_restore();   // belt and braces: nothing may stay silenced without a stream
    log_printf("audio: stopped after %llu s", (unsigned long long)((now_ms() - g_stream_t0) / 1000));
}

static bool audio_alive(void *ctx, Result *why) { return !iso_exited(why); }

// While the speaker is silenced with mute and volume_follow=1, the console volume level is the one
// volume: the console buttons, the HOME slider and the earphone's Vol+/Vol- all change it, and the
// earphone volume is mapped from it (top level = volume_max_db, each level below = volume_step_db).
static volatile bool        g_console_vol;
static volatile AudioTarget g_console_target;

static float console_db(s32 level) {
    s32 vmin = 0, vmax = 0;
    spk_volume_range(&vmin, &vmax);
    // Console level 0 is silent: one step below volume_min_db is the earphone's level 0.
    if (level <= vmin) return g_cfg.volume_min_db - g_cfg.volume_step_db;
    float db = g_cfg.volume_max_db - (float)(vmax - level) * g_cfg.volume_step_db;
    return db < g_cfg.volume_min_db ? g_cfg.volume_min_db : db;
}

static bool remote_volume_key(void *ctx, int dir, const char *why) {   // remote thread
    if (!g_console_vol) return false;
    s32 level = 0;
    Result rc = spk_step_volume(g_console_target, dir, &level);
    log_printf("remote: %s -> console volume %d (%s); the earphone follows", why, level, rc_str(rc));
    if (R_FAILED(rc)) return false;
    // A volume set through audctl shows no system volume bar: show the earphone's own right away.
    dev_volume_set_db(console_db(level), why);
    dev_volume_show();
    return true;
}

// Middle button with play_button=media: press media_button only while a media app runs.
static void remote_play_key(void *ctx) {   // remote thread
    u64 program = __atomic_load_n(&g_app_program, __ATOMIC_RELAXED);
    if (!app_pid() || !cfg_media_app(&g_cfg, program)) {
        log_printf("remote: middle button: no action (%s%016llx is not in media_apps)", app_pid() ? "" : "no app; ",
                   (unsigned long long)program);
        return;
    }
    log_printf("remote: middle button in media app %016llx -> press %s", (unsigned long long)program,
               cfg_button_name(g_cfg.media_button));
    media_press(g_cfg.media_button);
}

static const dev_hooks k_hooks = {audio_start, audio_stop, audio_alive, remote_volume_key, remote_play_key};

static bool streaming_now(void) {
    return g_usb_ok && dev_get_state() == DEV_READY && dev_streaming() && iso_active() && !iso_exited(NULL) &&
           !iso_failing();
}

// ------------------------------------------------------------------ output policy (stream mode)

static bool pe_silence(void *ctx, int target, int method) {
    return R_SUCCEEDED(spk_silence((AudioTarget)target, (spk_method)method));
}
static void pe_reassert(void *ctx) { spk_reassert(); }
static void pe_restore(void *ctx) { spk_restore(); }
// The earphone stream is held while another output is active (policy) or while enabled=0.
static bool g_aside;

static void apply_hold(const char *why) {
    if (g_usb_ok) dev_set_hold(g_aside || !g_cfg.enabled, why);
}

static void pe_hold(void *ctx, bool hold, const char *why) {
    g_aside = hold;
    apply_hold(why);
}
static void pe_volume_steps(void *ctx, int steps, const char *why) {
    if (g_console_vol) dev_volume_set_db(console_db(g_spk.volume), why);
    else dev_volume_steps(steps, why);
}
static void pe_verdict(void *ctx, int target, int method, eo_verdict v) {
    if (target <= 0 || target >= ST_TARGETS || method <= 0 || method >= ST_METHODS) return;
    g_state.verdict[target][method] = v;
    state_touch();
    log_printf("state: verdict.%s.%s=%s (HOS %s)", st_target_key(target), st_method_key(method), st_verdict_name(v),
               g_state.hos);
}
static void pe_log(void *ctx, const char *msg) { log_printf("%s", msg); }

static const pol_env k_pol_env = {pe_silence, pe_reassert, pe_restore, pe_hold, pe_volume_steps, pe_verdict, pe_log,
                                  NULL};

// Every POLL_MS: audctl snapshot, change log, output policy.
static void out_poll(void) {
    spk_state s;
    bool ok = spk_read(&s);
    if (ok && (!g_spk_ok || s.target != g_spk.target || s.volume != g_spk.volume || s.mute != g_spk.mute ||
               s.have_master != g_spk.have_master || (s.have_master && fabsf(s.master - g_spk.master) > 0.001f))) {
        spk_method sm = spk_silenced();
        log_printf("audio out: active output %s, volume %d%s, master %.2f%s%s", spk_target_name(s.target), s.volume,
                   s.mute ? " (muted)" : "", (double)s.master, sm ? ", silenced by eoaudio: " : "",
                   sm ? spk_method_name(sm) : "");
    }
    if (ok) g_spk = s;
    g_spk_ok = ok;
    if (g_cfg.mode != MODE_STREAM) return;
    bool streaming = streaming_now();
    // Recorder liveness from its frame counter (every poll), not the 1 s statistics, so that a
    // known silencing method is applied ~0.2 s after the stream starts instead of ~1.5 s.
    static u64 last_frames;
    u64 frames = rec_frames_total();
    if (frames != last_frames) {
        last_frames = frames;
        if (streaming) g_rec_alive_ms = now_ms();
    }
    bool capture = g_rec_alive_ms && now_ms() - g_rec_alive_ms < CAPTURE_STALE_MS;
    s32 vmin = 0, vmax = 0;
    spk_volume_range(&vmin, &vmax);
    pol_input in = {ok, streaming, streaming && iso_batches() >= STREAM_OK_BATCHES, capture,
                    ok ? (int)s.target : 0, s.volume, vmin, s.mute, s.have_master, s.master};
    pol_poll(&g_pol, &in);
    bool console = g_cfg.volume_follow && g_pol.phase == PH_ACTIVE && (g_pol.applied == POL_M_MUTE || g_pol.applied == POL_M_MASTER0);
    if (console && !g_console_vol) {
        g_console_target = (AudioTarget)g_pol.target;
        g_console_vol = true;
        dev_volume_set_db(console_db(s.volume), "earphone volume now follows the console volume");
    } else if (!console && g_console_vol) {
        g_console_vol = false;
        log_printf("volume: the earphone keeps its own volume again (Vol+/Vol- change it directly)");
    }
}

// ------------------------------------------------------------------ application tracking

static void app_poll(void) {
    static bool warned;
    u64 pid = 0, program = 0;
    Result rc = pmdmntInitialize();
    if (R_SUCCEEDED(rc)) {
        g_app_known = true;
        if (R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid))) pmdmntGetProgramId(&program, pid);
        else pid = 0;
        pmdmntExit();
    } else if (!warned) {
        warned = true;
        log_printf("app: pm:dmnt unavailable (%s): the running game is not logged", rc_str(rc));
    }
    if (pid == g_app_pid && program == g_app_program) return;
    if (pid) log_printf("app: application %016llx running (pid %llu)", (unsigned long long)program, (unsigned long long)pid);
    else if (g_app_pid) log_printf("app: application %016llx closed", (unsigned long long)g_app_program);
    __atomic_store_n(&g_app_pid, pid, __ATOMIC_RELAXED);
    __atomic_store_n(&g_app_program, program, __ATOMIC_RELAXED);
}

static bool game_running(void) { return !g_app_known || app_pid() != 0; }

// ------------------------------------------------------------------ capture mode: speaker mute test

typedef enum { MT_OFF = 0, MT_WAIT, MT_BASE, MT_SETTLE, MT_MEASURE, MT_RECOVER, MT_DONE } mt_phase;

static struct {
    mt_phase    phase;
    spk_method  method;
    AudioTarget target;
    int         secs, loud;
    pcm_meter   base, acc;
    float       result[SPK_M_MASTER0 + 1];
    bool        failed[SPK_M_MASTER0 + 1];
} MT;

static const char *mt_verdict(float base, float db) {
    if (db < -80.0f) return "capture SILENCED";
    if (db > base - 6.0f) return "capture kept";
    return "capture reduced";
}

static void mt_apply(void) {
    Result rc = spk_silence(MT.target, MT.method);
    MT.failed[MT.method] = R_FAILED(rc);
    MT.secs = 0;
    MT.phase = R_FAILED(rc) ? MT_RECOVER : MT_SETTLE;
}

static void mt_report(void) {
    float base = meter_rms_db_both(&MT.base);
    log_printf("mute test: done on the %s. Recording level with it playing: %.1f dBFS", spk_target_name(MT.target),
               (double)base);
    for (int m = SPK_M_TARGET_MUTE; m <= SPK_M_MASTER0; m++) {
        if (MT.failed[m]) {
            log_printf("mute test:   %-31s could not be applied (see the speaker: line above)",
                       spk_method_name((spk_method)m));
            continue;
        }
        const char *v = mt_verdict(base, MT.result[m]);
        log_printf("mute test:   %-31s %6.1f dBFS -> %s", spk_method_name((spk_method)m), (double)MT.result[m], v);
        // speaker_mute=auto in stream mode uses a "kept" result directly. A silent result is only
        // logged: one quiet moment in the game would look the same, and the stream mode's own
        // check (undo, see the recording come back, twice) is what rules a method out.
        if (m <= SPK_M_TARGET_VOLUME0 && MT.target > 0 && MT.target < ST_TARGETS && strcmp(v, "capture kept") == 0)
            pe_verdict(NULL, MT.target, m, VERDICT_KEPT);
    }
}

// Fed with every 1-second recorder interval (capture mode).
static void mute_test_feed(const rec_interval *ri) {
    if (MT.phase == MT_OFF || MT.phase == MT_DONE) return;
    if (MT.phase != MT_WAIT && (!ri->running || !game_running())) {
        spk_restore();
        log_printf("mute test: aborted (%s); it starts again after the next 10 s of game audio",
                   ri->running ? "the game was closed" : "the recorder stopped");
        MT.phase = MT_WAIT;
        MT.loud = 0;
        return;
    }
    switch (MT.phase) {
    case MT_WAIT:
        MT.loud = ri->running && game_running() && g_spk_ok && meter_rms_db_both(&ri->meter) > -50.0f ? MT.loud + 1 : 0;
        if (MT.loud >= 10) {
            MT.target = g_spk.target;
            log_printf("mute test: 10 s of game audio, starting on the %s. It goes quiet 3 times for ~4 s. "
                       "Please don't touch the volume until 'mute test: done'.",
                       spk_target_name(MT.target));
            MT.phase = MT_BASE;
            MT.secs = 0;
            meter_reset(&MT.base);
        }
        break;
    case MT_BASE:
        meter_merge(&MT.base, &ri->meter);
        if (++MT.secs >= 3) {
            MT.method = SPK_M_TARGET_MUTE;
            mt_apply();
        }
        break;
    case MT_SETTLE:   // the interval in which the change was made
        if (++MT.secs >= 1) {
            MT.phase = MT_MEASURE;
            MT.secs = 0;
            meter_reset(&MT.acc);
        }
        break;
    case MT_MEASURE:
        meter_merge(&MT.acc, &ri->meter);
        if (++MT.secs >= 3) {
            MT.result[MT.method] = meter_rms_db_both(&MT.acc);
            spk_restore();
            MT.phase = MT_RECOVER;
            MT.secs = 0;
        }
        break;
    case MT_RECOVER:
        if (++MT.secs < 2) break;
        if (MT.method < SPK_M_MASTER0) {
            MT.method++;
            mt_apply();
        } else {
            mt_report();
            MT.phase = MT_DONE;
        }
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------------ periodic log

static int fmt_rec(char *out, size_t size, const rec_acc *a) {
    if (!a->intervals) return snprintf(out, size, "rec -");
    const rec_interval *l = &a->last;
    if (!l->running) return snprintf(out, size, "rec not running (opened %u time(s))", l->session);
    double secs = a->dur_ms ? a->dur_ms / 1000.0 : 1.0;
    bool silent = a->meter.peak[0] == 0 && a->meter.peak[1] == 0;
    char gaps[48] = "-";
    if (a->gaps)
        snprintf(gaps, sizeof(gaps), "%.1f/%.1f/%.1f ms", a->gap_min / 1000.0, a->gap_sum / 1000.0 / a->gaps,
                 a->gap_max / 1000.0);
    return snprintf(out, size, "rec #%u %u Hz %u ch fmt %u, %u buf %llu fr (%.0f/s) partial %u, gap %s, "
                               "L %.1f R %.1f dBFS%s",
                    l->session, l->rate, l->channels, l->format, a->buffers, (unsigned long long)a->frames,
                    a->frames / secs, a->partial, gaps, (double)meter_rms_db(&a->meter, 0),
                    (double)meter_rms_db(&a->meter, 1), silent ? " (digital silence)" : "");
}

static int fmt_out(char *out, size_t size, const period *p, const iso_stats *s) {
    if (!iso_active()) return snprintf(out, size, "usb not streaming");
    const iso_stats *q = &p->iso_prev;
    int n = snprintf(out, size, "usb %llu batches, failed %llu/%llu, resync %llu, timeout %llu, max gap %.1f ms",
                     (unsigned long long)(s->batches - q->batches),
                     (unsigned long long)(s->report_errors - q->report_errors),
                     (unsigned long long)(s->submit_errors - q->submit_errors),
                     (unsigned long long)(s->resyncs - q->resyncs), (unsigned long long)(s->timeouts - q->timeouts),
                     p->iso_max_gap / 1000.0);
    if (g_cfg.mode != MODE_STREAM || n < 0 || (size_t)n >= size) return n;
    const playout_stats *ps = &p->play;
    double avg = ps->fill_n ? (double)ps->fill_sum / ps->fill_n / 48.0 : 0.0;
    n += snprintf(out + n, size - (size_t)n,
                  " | buffer %.1f ms (%.1f..%.1f, target %.1f), slips -%u/+%u, underruns %u, skips %u, silent %u, "
                  "ring drops %u",
                  avg, ps->fill_n ? ps->fill_min / 48.0 : 0.0, ps->fill_n ? ps->fill_max / 48.0 : 0.0,
                  g_target_frames / 48.0, ps->drops, ps->inserts, ps->underruns, ps->skips, ps->silent_blocks,
                  p->rec.ring_dropped);
    return n;
}

static void log_health(const iso_stats *s) {
    char dev[192] = "usb:hs off", rec[192], out[320], spk[96] = "audio out ?";
    if (g_usb_ok) dev_status(dev, sizeof(dev));
    fmt_rec(rec, sizeof(rec), &MON.health.rec);
    fmt_out(out, sizeof(out), &MON.health, s);
    if (g_spk_ok)
        snprintf(spk, sizeof(spk), "%s vol %d%s%s%s", spk_target_name(g_spk.target), g_spk.volume,
                 g_spk.mute ? " muted" : "", spk_silenced() ? " by eoaudio " : "",
                 spk_silenced() ? spk_method_name(spk_silenced()) : "");
    if (g_cfg.mode == MODE_STREAM)
        log_printf("health: %s | %s, policy %s | %s | %s", dev, spk, pol_phase_name(g_pol.phase), rec, out);
    else
        log_printf("health: %s | %s | %s | %s", dev, spk, rec, out);
}

// ------------------------------------------------------------------ live config reload

static u32 g_cfg_hash;

static u32 fnv1a(const char *p, long n) {
    u32 h = 2166136261u;
    for (long i = 0; i < n; i++) h = (h ^ (u8)p[i]) * 16777619u;
    return h;
}

static void cfg_on_reload_warn(void *ctx, int line, const char *msg) {
    log_printf("config: reload: line %d: %s", line, msg);
}

// Every 2 s: if config.ini changed (an editor, or Ultrahand's set-ini-val), apply it. Keys that
// shape threads and buffers keep their start-up values until the next start.
static void config_poll(void) {
    static char text[8192];
    long n = read_file(CONFIG_PATH, text, sizeof(text));
    if (n < 0) return;
    u32 h = fnv1a(text, n);
    if (h == g_cfg_hash) return;
    g_cfg_hash = h;
    eo_config nc;
    cfg_defaults(&nc);
    cfg_parse(&nc, text, (size_t)n, cfg_on_reload_warn, NULL);
    eo_config old = g_cfg;
    // Restart-only keys.
    nc.config_version = old.config_version;
    nc.start_delay_s = old.start_delay_s;
    nc.log_keep = old.log_keep;
    nc.target_ms = old.target_ms;
    nc.rec_frames = old.rec_frames;
    nc.rec_buffers = old.rec_buffers;
    nc.mute_test = old.mute_test;
    nc.acquire = old.acquire;
    memcpy(nc.pids, old.pids, sizeof(nc.pids));
    nc.pid_count = old.pid_count;
    nc.iso_urbs = old.iso_urbs;
    nc.iso_batches = old.iso_batches;
    nc.iso_lead = old.iso_lead;
    if (nc.mode != old.mode) {
        log_printf("config: mode=%s takes effect after a restart (running: %s); use enabled=0/1 to switch "
                   "eoaudio off and on right away",
                   cfg_mode_name(nc.mode), cfg_mode_name(old.mode));
        nc.mode = old.mode;
    }
    g_cfg = nc;   // device.c and the remote thread read g_cfg directly
    char desc[640];
    cfg_describe(&g_cfg, desc, sizeof(desc));
    log_printf("config: reloaded: %s%s", g_cfg.enabled ? "" : "DISABLED, ", desc);
    log_set_debug(g_cfg.log_level == LOGLVL_DEBUG);
    if (nc.speaker_mute != old.speaker_mute || nc.volume_follow != old.volume_follow) {
        g_pol.speaker_mode = (int)nc.speaker_mute;
        g_pol.follow = nc.volume_follow;
        pol_stop(&g_pol, "speaker settings changed");   // re-evaluated with the new settings
    }
    if (nc.enabled != old.enabled) apply_hold(nc.enabled ? "enabled=1 in config.ini" : "enabled=0 in config.ini");
    if (g_usb_ok && (nc.volume_min_db != old.volume_min_db || nc.volume_max_db != old.volume_max_db ||
                     nc.volume_step_db != old.volume_step_db))
        dev_volume_refresh("volume range changed in config.ini");
    if (nc.volume_osd && !old.volume_osd && g_cfg.mode != MODE_CAPTURE) osd_init();
}

// Free memory in the System pool (sysmodules, the Tesla overlay loader) and the Applet pool, so a
// log shows what eoaudio leaves to the rest of the system. Every minute, and at start.
static void log_pools(const char *when) {
    static const struct {
        u64         id;
        const char *name;
    } pools[] = {{PhysicalMemorySystemInfo_System, "system"}, {PhysicalMemorySystemInfo_Applet, "applet"}};
    char buf[160];
    int n = 0;
    for (size_t i = 0; i < sizeof(pools) / sizeof(pools[0]); i++) {
        u64 total = 0, used = 0;
        Result r1 = svcGetSystemInfo(&total, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, pools[i].id);
        Result r2 = svcGetSystemInfo(&used, SystemInfoType_UsedPhysicalMemorySize, INVALID_HANDLE, pools[i].id);
        if (R_FAILED(r1) || R_FAILED(r2))
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s pool %s", n ? ", " : "", pools[i].name,
                          rc_str(R_FAILED(r1) ? r1 : r2));
        else
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s pool %llu KiB free of %llu", n ? ", " : "",
                          pools[i].name, (unsigned long long)((total - used) / 1024),
                          (unsigned long long)(total / 1024));
        if (n >= (int)sizeof(buf)) break;
    }
    u64 mine = 0;
    svcGetInfo(&mine, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    struct mallinfo mi = mallinfo();
    log_printf("memory (%s): %s; eoaudio uses %llu KiB (heap %u of %u KiB in use, peak %u)", when, buf,
               (unsigned long long)(mine / 1024), (unsigned)(mi.uordblks / 1024), INNER_HEAP_SIZE / 1024,
               (unsigned)(mi.arena / 1024));
}

static void monitor(u64 now, u64 t_start) {
    MON.ticks++;
    if (MON.ticks % 2 == 0) config_poll();
    if (MON.ticks % 2 == 1) app_poll();

    rec_interval ri;
    if (rec_take(&ri)) {
        racc_add(&MON.detail.rec, &ri);
        racc_add(&MON.health.rec, &ri);
        bool silent = ri.meter.peak[0] == 0 && ri.meter.peak[1] == 0;
        if (ri.running && ri.frames) g_rec_alive_ms = now;
        if (g_cfg.mode == MODE_CAPTURE) mute_test_feed(&ri);
        if (g_cfg.mode == MODE_STREAM && ri.running) pol_interval(&g_pol, meter_rms_db_both(&ri.meter), silent);
    }
    iso_stats s;
    memset(&s, 0, sizeof(s));
    if (iso_active()) {
        iso_take(&s);
        if (s.max_gap_us > MON.detail.iso_max_gap) MON.detail.iso_max_gap = s.max_gap_us;
        if (s.max_gap_us > MON.health.iso_max_gap) MON.health.iso_max_gap = s.max_gap_us;
        mutexLock(&g_play_mutex);
        playout_stats ps = g_play_acc;
        playout_stats_clear(&g_play_acc);
        mutexUnlock(&g_play_mutex);
        play_merge(&MON.detail.play, &ps);
        play_merge(&MON.health.play, &ps);
    }

    if (log_debug_on()) {
        char rec[192], out[320];
        fmt_rec(rec, sizeof(rec), &MON.detail.rec);
        fmt_out(out, sizeof(out), &MON.detail, &s);
        log_printf("detail: %s | %s", rec, out);
    }
    period_reset(&MON.detail, iso_active() ? &s : NULL);

    if (MON.ticks % 60 == 0) log_pools("every minute");
    if (MON.ticks % (u32)g_cfg.health_s == 0) {
        // Capture mode without a game and without the earphone: digital silence is logged rarely.
        bool silent = MON.health.rec.intervals && MON.health.rec.meter.peak[0] == 0 && MON.health.rec.meter.peak[1] == 0;
        bool idle = g_cfg.mode == MODE_CAPTURE && silent && !iso_active() &&
                    (!g_usb_ok || dev_get_state() == DEV_IDLE) && (MT.phase == MT_OFF || MT.phase == MT_WAIT ||
                                                                    MT.phase == MT_DONE);
        if (!idle || MON.health.quiet++ % 6 == 0) log_health(&s);
        if (!idle) MON.health.quiet = 0;
        u32 quiet = MON.health.quiet;
        period_reset(&MON.health, iso_active() ? &s : NULL);
        MON.health.quiet = quiet;
    }

    float db;
    if (g_usb_ok && dev_take_volume_change(&db)) {
        g_state.volume_db = db;
        g_state.have_volume = true;
        state_touch();
    }
    if (g_state_dirty && now - g_state_dirty_ms >= 2000) state_save();

    static bool guard_cleared;
    if (!guard_cleared && now - t_start > GUARD_OK_MS) {
        guard_cleared = true;
        unlink(GUARD_PATH);
    }
}

// ------------------------------------------------------------------ startup

typedef struct {
    int  count;
    char msgs[8][160];
} cfg_warnings;

static void on_cfg_warn(void *ctx, int line, const char *msg) {
    cfg_warnings *w = ctx;
    if (w->count < 8) {
        if (line) snprintf(w->msgs[w->count], sizeof(w->msgs[0]), "line %d: %s", line, msg);
        else snprintf(w->msgs[w->count], sizeof(w->msgs[0]), "%s", msg);
    }
    w->count++;
}

typedef enum { CFG_LOADED, CFG_CREATED, CFG_REPLACED } cfg_origin;

static int  g_old_version;
static char g_old_path[96];

static cfg_origin load_config(cfg_warnings *w) {
    static char text[8192];
    cfg_defaults(&g_cfg);
    long n = read_file(CONFIG_PATH, text, sizeof(text));
    cfg_origin origin = CFG_CREATED;
    if (n >= 0) {
        eo_config probe;
        cfg_defaults(&probe);
        cfg_parse(&probe, text, (size_t)n, NULL, NULL);
        if (probe.config_version >= CFG_VERSION) {
            g_cfg_hash = fnv1a(text, n);
            cfg_parse(&g_cfg, text, (size_t)n, on_cfg_warn, w);
            return CFG_LOADED;
        }
        // An older config.ini (v0.1 has no config_version): keep it as a backup and start from the
        // current defaults.
        g_old_version = probe.config_version ? probe.config_version : 1;
        snprintf(g_old_path, sizeof(g_old_path), APP_DIR "/config.ini.v%d.bak", g_old_version);
        unlink(g_old_path);
        rename(CONFIG_PATH, g_old_path);
        origin = CFG_REPLACED;
    }
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    save_file(CONFIG_PATH, cfg_default_text, strlen(cfg_default_text));
    g_cfg_hash = fnv1a(cfg_default_text, (long)strlen(cfg_default_text));
    cfg_parse(&g_cfg, cfg_default_text, strlen(cfg_default_text), on_cfg_warn, w);
    return origin;
}

static int guard_read(void) {
    char t[16];
    return read_file(GUARD_PATH, t, sizeof(t)) < 0 ? 0 : atoi(t);
}

static void sleep_s(int s) {
    for (int i = 0; i < s * 10; i++) svcSleepThread(100000000ULL);
}

int main(int argc, char **argv) {
    u64 t_start = now_ms();
    cfg_warnings warns = {0};
    cfg_origin origin = load_config(&warns);
    log_init(g_cfg.log_keep);
    log_set_debug(g_cfg.log_level == LOGLVL_DEBUG);

    u64 mem_total = 0, mem_used = 0;
    svcGetInfo(&mem_total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    log_printf("eoaudio %s: SD %s, clock %s, memory %llu of %llu KiB in use (heap %u KiB)", EOAUDIO_VERSION,
               g_sd_ok ? "ok" : "NOT MOUNTED", g_time_ok ? "ok" : "unavailable", (unsigned long long)(mem_used / 1024),
               (unsigned long long)(mem_total / 1024), INNER_HEAP_SIZE / 1024);
    log_pools("start");
    if (origin == CFG_CREATED) log_printf("config: %s did not exist, wrote the defaults", CONFIG_PATH);
    if (origin == CFG_REPLACED)
        log_printf("config: %s was a version %d file; kept as %s, wrote the version %d defaults", CONFIG_PATH,
                   g_old_version, g_old_path, CFG_VERSION);
    for (int i = 0; i < warns.count && i < 8; i++) log_printf("config: %s", warns.msgs[i]);
    if (warns.count > 8) log_printf("config: ... and %d more problem(s)", warns.count - 8);
    char desc[640];
    cfg_describe(&g_cfg, desc, sizeof(desc));
    log_printf("config: %s", desc);

    if (g_cfg.mode == MODE_OFF) {
        log_printf("mode=off: nothing to do, exiting");
        log_exit();
        return 0;
    }
    // Crash-loop guard: a run that ends (crash or reboot) within 60 s counts. After three in a row
    // eoaudio stays off until bootguard.txt is deleted; 60 s of running clears it.
    int guard = guard_read();
    if (guard >= GUARD_LIMIT) {
        log_printf("boot guard: the last %d runs ended within %llu s of starting (crash or quick reboots), so "
                   "eoaudio stays off. Delete %s to try again.",
                   guard, GUARD_OK_MS / 1000, GUARD_PATH);
        log_exit();
        return 0;
    }
    char gtext[16];
    int gl = snprintf(gtext, sizeof(gtext), "%d\n", guard + 1);
    save_file(GUARD_PATH, gtext, (size_t)gl);
    if (guard) log_printf("boot guard: %d earlier run(s) ended within %llu s", guard, GUARD_OK_MS / 1000);
    state_load();
    log_sync();

    sleep_s(g_cfg.start_delay_s);
    mutexInit(&g_play_mutex);
    playout_stats_clear(&g_play_acc);
    period_reset(&MON.detail, NULL);
    period_reset(&MON.health, NULL);
    spk_init();
    app_poll();
    if (g_cfg.mode != MODE_CAPTURE && g_cfg.volume_osd) osd_init();
    pol_init(&g_pol, &k_pol_env, (int)g_cfg.speaker_mute, g_cfg.volume_follow, &g_state);

    if (g_cfg.mode == MODE_CAPTURE) {
        MT.phase = g_cfg.mute_test ? MT_WAIT : MT_OFF;
        rec_config rcfg = {(u32)g_cfg.rec_frames, (u32)g_cfg.rec_buffers, NULL};
        Result rc = rec_start(&rcfg);
        if (R_FAILED(rc)) log_printf("rec: thread: %s", rc_str(rc));
    }
    if (g_cfg.mode != MODE_CAPTURE || g_cfg.acquire) {
        Result rc = usbHsInitialize();
        g_usb_ok = R_SUCCEEDED(rc);
        log_printf("usb:hs: usbHsInitialize: %s", rc_str(rc));
        float vol = g_state.have_volume ? g_state.volume_db : g_cfg.volume_db;
        if (vol > g_cfg.volume_max_db) vol = g_cfg.volume_max_db;
        if (vol < g_cfg.volume_min_db) vol = g_cfg.volume_min_db;
        if (g_state.have_volume) log_printf("volume: %.1f dB from state.ini", (double)vol);
        if (g_usb_ok) dev_init(&g_cfg, g_cfg.mode != MODE_CAPTURE, vol, &k_hooks, NULL);
        if (!g_cfg.enabled) apply_hold("enabled=0 in config.ini");
    }
    log_printf("running: mode=%s%s", cfg_mode_name(g_cfg.mode),
               g_cfg.mode != MODE_CAPTURE ? "" : MT.phase == MT_WAIT ? ", mute test armed" : ", mute test off");
    log_sync();

    // A sysmodule runs until the console powers off (or pm terminates it): no exit path.
    u64 next_tick = now_ms() + 1000, next_poll = now_ms();
    for (;;) {
        Waiter w[6];
        int n = g_usb_ok ? dev_waiters(w, 6) : 0;
        u64 now = now_ms();
        u64 timeout = g_usb_ok ? dev_timeout_ns() : 1000000000ULL;
        u64 due = next_poll < next_tick ? next_poll : next_tick;
        u64 due_ns = due > now ? (due - now) * 1000000ULL : 0;
        if (due_ns < timeout) timeout = due_ns;
        bool usb_event = false;
        if (n) {
            s32 idx = -1;
            usb_event = R_SUCCEEDED(waitObjects(&idx, w, n, timeout)) && idx >= 0;
        } else {
            svcSleepThread(timeout);
        }
        if (g_usb_ok) dev_step(usb_event);
        now = now_ms();
        if (now >= next_poll) {
            // Poll fast while the earphone is in: the speaker goes off ~50 ms after the stream starts
            // and back on ~0.1 s after it fails, and a console volume press that unmutes a muted
            // speaker is undone quickly.
            next_poll = now + (g_usb_ok && dev_get_state() == DEV_READY ? 50 : POLL_MS);
            out_poll();
        }
        if (now >= next_tick) {
            next_tick += 1000;
            if (next_tick <= now) next_tick = now + 1000;
            monitor(now, t_start);
        }
    }
}
