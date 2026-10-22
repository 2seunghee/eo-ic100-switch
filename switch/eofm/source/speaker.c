#include "speaker.h"

#include <math.h>

#include "log.h"

#define FRAMES   1024   // per buffer: 21.3 ms at 48 kHz
#define BUF_SIZE ALIGN_UP(FRAMES * 4, 0x1000)
#define N_IN     4
#define N_OUT    6

static struct {
    Thread        thread;
    volatile bool stop;
    Mutex         lock;
    spk_status    st;
    bool          audin_ok, audout_ok, audctl_ok;
    Event         ev;              // buffer event of the reopened input
    bool          ev_ok;
    AudioInBuffer in[N_IN];
    AudioOutBuffer out[N_OUT];
    bool          out_busy[N_OUT];
    void         *mem[N_IN + N_OUT];
    int16_t       peak;
    u64           peak_since;
} S;

static const char *target_name(AudioTarget t) {
    switch (t) {
        case AudioTarget_Speaker:         return "speaker";
        case AudioTarget_Headphone:       return "headphone jack";
        case AudioTarget_Tv:              return "TV";
        case AudioTarget_UsbOutputDevice: return "USB";
        case AudioTarget_Bluetooth:       return "Bluetooth";
        default:                          return "?";
    }
}

static void set_error(const char *msg, Result rc) {
    mutexLock(&S.lock);
    snprintf(S.st.error, sizeof(S.st.error), "%s%s%s", msg, rc ? ": " : "", rc ? rc_str(rc) : "");
    mutexUnlock(&S.lock);
    log_file("speaker: %s\n", S.st.error);
}

// Moves the system output to the console speaker while a USB output device is attached. Two
// audctl calls can do that; which one this firmware honours is only known on the console.
static void move_output(void) {
    Result rc = audctlInitialize();
    S.audctl_ok = R_SUCCEEDED(rc);
    if (!S.audctl_ok) {
        log_file("speaker: audctl: %s (output not moved)\n", rc_str(rc));
        return;
    }
    AudioTarget t = AudioTarget_Invalid;
    rc = audctlGetActiveOutputTarget(&t);
    S.st.before = S.st.now = t;
    log_file("speaker: active output %s (%s)\n", target_name(t), rc_str(rc));
    if (t == AudioTarget_Speaker) return;
    rc = audctlSetOutputTarget(AudioTarget_Speaker);
    audctlGetActiveOutputTarget(&t);
    log_file("speaker: SetOutputTarget(speaker): %s -> active %s\n", rc_str(rc), target_name(t));
    if (t != AudioTarget_Speaker) {
        rc = audctlSetDefaultTarget(AudioTarget_Speaker, 0, 0);
        audctlGetActiveOutputTarget(&t);
        log_file("speaker: SetDefaultTarget(speaker): %s -> active %s\n", rc_str(rc), target_name(t));
    }
    S.st.now = t;
    S.st.moved_output = t == AudioTarget_Speaker;
}

static void restore_output(void) {
    if (!S.audctl_ok) return;
    if (S.st.moved_output && S.st.before != AudioTarget_Invalid) {
        Result r1 = audctlSetOutputTarget(S.st.before);
        Result r2 = audctlSetDefaultTarget(S.st.before, 0, 0);
        AudioTarget t = AudioTarget_Invalid;
        audctlGetActiveOutputTarget(&t);
        log_file("speaker: output back to %s: %s / %s -> active %s\n", target_name(S.st.before), rc_str(r1), rc_str(r2),
                 target_name(t));
    }
    S.st.moved_output = false;
    audctlExit();
    S.audctl_ok = false;
}

static void track_peak(const int16_t *p, size_t samples) {
    for (size_t i = 0; i < samples; i++) {
        int16_t v = p[i] < 0 ? (int16_t)(p[i] == -32768 ? 32767 : -p[i]) : p[i];
        if (v > S.peak) S.peak = v;
    }
    u64 t = now_ms();
    if (t - S.peak_since >= 1000) {
        mutexLock(&S.lock);
        S.st.peak_db = S.peak ? (int)lrintf(20.0f * log10f(S.peak / 32768.0f)) : -99;
        mutexUnlock(&S.lock);
        S.peak = 0;
        S.peak_since = t;
    }
}

static void reclaim_out(void) {
    AudioOutBuffer *rel = NULL;
    u32 n = 0;
    while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&rel, &n)) && n && rel) {
        for (int k = 0; k < N_OUT; k++)
            if (&S.out[k] == rel) S.out_busy[k] = false;
        rel = NULL;
    }
}

static void play(const AudioInBuffer *in) {
    reclaim_out();
    for (int k = 0; k < N_OUT; k++) {
        if (S.out_busy[k]) continue;
        size_t n = in->data_size < S.out[k].buffer_size ? in->data_size : S.out[k].buffer_size;
        memcpy(S.out[k].buffer, in->buffer, n);
        S.out[k].data_size = n;
        S.out_busy[k] = true;
        audoutAppendAudioOutBuffer(&S.out[k]);
        mutexLock(&S.lock);
        S.st.frames += n / 4;
        mutexUnlock(&S.lock);
        return;
    }
    mutexLock(&S.lock);
    S.st.dropped++;
    mutexUnlock(&S.lock);
}

// After the input is reopened on the earphone's microphone, libnx's buffer event still belongs to
// the default input it opened first; this one is registered on the reopened input (S.ev). Without
// it the pump polls. 0.1.3 on the console got no buffer back at all (while the audio sysmodule did
// stream the microphone), so every step is logged.
static void pump(void *arg) {
    u64 last_ok = now_ms(), last_report = now_ms();
    u32 polls = 0, fails = 0;
    Result last_rc = 0;
    while (!S.stop) {
        if (S.ev_ok) eventWait(&S.ev, 20000000ULL);
        AudioInBuffer *rel = NULL;
        u32 n = 0;
        Result rc = audinGetReleasedAudioInBuffer(&rel, &n);
        polls++;
        if (R_FAILED(rc)) fails++, last_rc = rc;
        if (now_ms() - last_report >= 5000) {
            last_report = now_ms();
            AudioInState st = AudioInState_Stopped;
            Result rs = audinGetAudioInState(&st);
            log_file("speaker: %.1f s played, peak %d dBFS, %u dropped; %u polls, %u failed (last %s), input state %d (%s)\n",
                     S.st.frames / 48000.0, S.st.peak_db, S.st.dropped, polls, fails, fails ? rc_str(last_rc) : "-", st,
                     rc_str(rs));
        }
        if (R_FAILED(rc) || !n || !rel) {
            if (now_ms() - last_ok > 3000 && !S.st.error[0]) set_error("no audio from the USB microphone for 3 s", 0);
            if (!S.ev_ok) svcSleepThread(2000000ULL);
            continue;
        }
        while (rel && n) {
            last_ok = now_ms();
            track_peak(rel->buffer, rel->data_size / 2);
            play(rel);
            rel->data_size = rel->buffer_size;
            audinAppendAudioInBuffer(rel);
            rel = NULL;
            n = 0;
            audinGetReleasedAudioInBuffer(&rel, &n);
        }
    }
}

static void free_buffers(void) {
    for (int k = 0; k < N_IN + N_OUT; k++) {
        free(S.mem[k]);
        S.mem[k] = NULL;
    }
}

bool spk_start(void) {
    if (S.st.running) return true;
    mutexLock(&S.lock);
    memset(&S.st, 0, sizeof(S.st));
    S.st.peak_db = -99;
    mutexUnlock(&S.lock);
    S.stop = false;

    Result rc = audinInitialize();
    S.audin_ok = R_SUCCEEDED(rc);
    if (!S.audin_ok) {
        set_error("audin (system audio input) not available", rc);
        return false;
    }
    char names[4][0x100];
    memset(names, 0, sizeof(names));
    u32 count = 0;
    rc = audinListAudioIns(&names[0][0], 4, &count);
    log_file("speaker: audio inputs (%s): %u\n", rc_str(rc), count);
    for (u32 i = 0; i < count && i < 4; i++) log_file("speaker:   [%u] %.255s\n", i, names[i]);
    // libnx opened the default input ("BuiltInHeadset", 0.1.2 on the console); the earphone's
    // microphone is listed as "Uac_<n>". Reopen on that one.
    int uac = -1;
    for (u32 i = 0; i < count && i < 4; i++)
        if (!strncmp(names[i], "Uac", 3)) uac = (int)i;
    if (uac < 0) {
        set_error("the earphone's microphone is not among the audio inputs", 0);
        audinExit();
        S.audin_ok = false;
        return false;
    }
    serviceClose(audinGetServiceSession_AudioIn());
    char opened[0x100] = {0};
    u32 rate = 0, ch = 0;
    PcmFormat fmt = PcmFormat_Invalid;
    AudioInState ast;
    rc = audinOpenAudioIn(names[uac], opened, 48000, 0x00020000, &rate, &ch, &fmt, &ast);   // libnx's own default value
    log_file("speaker: open %s: %s -> %.255s, %u Hz, %u ch, format %d\n", names[uac], rc_str(rc), opened, rate, ch, fmt);
    S.st.in_rate = rate;
    S.st.in_channels = ch;
    if (R_FAILED(rc)) {
        set_error("opening the earphone's microphone failed", rc);
        audinExit();
        S.audin_ok = false;
        return false;
    }
    if (S.st.in_rate != 48000 || S.st.in_channels != 2 || fmt != PcmFormat_Int16) {
        set_error("the audio input is not 48 kHz 16-bit stereo", 0);
        audinExit();
        S.audin_ok = false;
        return false;
    }
    Handle evh = INVALID_HANDLE;   // IAudioIn RegisterBufferEvent (cmd 4), as libnx does for its first input
    rc = serviceDispatch(audinGetServiceSession_AudioIn(), 4, .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
                         .out_handles = &evh);
    S.ev_ok = R_SUCCEEDED(rc);
    if (S.ev_ok) eventLoadRemote(&S.ev, evh, true);
    log_file("speaker: buffer event: %s\n", rc_str(rc));

    move_output();

    rc = audoutInitialize();
    S.audout_ok = R_SUCCEEDED(rc);
    if (!S.audout_ok) {
        set_error("audout (system audio output) not available", rc);
        spk_stop();
        return false;
    }
    for (int k = 0; k < N_IN + N_OUT; k++) {
        S.mem[k] = memalign(0x1000, BUF_SIZE);
        if (!S.mem[k]) {
            set_error("out of memory", 0);
            spk_stop();
            return false;
        }
        memset(S.mem[k], 0, BUF_SIZE);
    }
    for (int k = 0; k < N_IN; k++) {
        S.in[k] = (AudioInBuffer){.next = NULL, .buffer = S.mem[k], .buffer_size = BUF_SIZE, .data_size = FRAMES * 4};
        Result ra = audinAppendAudioInBuffer(&S.in[k]);
        if (k == 0 || R_FAILED(ra)) log_file("speaker: append input buffer %d: %s\n", k, rc_str(ra));
    }
    for (int k = 0; k < N_OUT; k++) {
        S.out[k] = (AudioOutBuffer){.next = NULL, .buffer = S.mem[N_IN + k], .buffer_size = BUF_SIZE, .data_size = 0};
        S.out_busy[k] = false;
    }
    // One buffer of silence ahead, so that capture jitter does not starve the output.
    S.out[0].data_size = FRAMES * 4;
    S.out_busy[0] = true;
    audoutAppendAudioOutBuffer(&S.out[0]);

    Result ri = audinStartAudioIn(), ro = audoutStartAudioOut();
    log_file("speaker: start audin %s, audout %s\n", rc_str(ri), rc_str(ro));
    if (R_FAILED(ri) || R_FAILED(ro)) {
        set_error(R_FAILED(ri) ? "starting the audio input failed" : "starting the audio output failed", R_FAILED(ri) ? ri : ro);
        spk_stop();
        return false;
    }
    S.peak_since = now_ms();
    rc = threadCreate(&S.thread, pump, NULL, NULL, 0x4000, 0x2B, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&S.thread);
    if (R_FAILED(rc)) {
        set_error("audio thread", rc);
        spk_stop();
        return false;
    }
    S.st.running = true;
    return true;
}

void spk_stop(void) {
    if (S.st.running) {
        S.stop = true;
        threadWaitForExit(&S.thread);
        threadClose(&S.thread);
        S.st.running = false;
    }
    if (S.ev_ok) {
        eventClose(&S.ev);
        S.ev_ok = false;
    }
    if (S.audin_ok) {
        audinStopAudioIn();
        audinExit();
        S.audin_ok = false;
    }
    if (S.audout_ok) {
        audoutStopAudioOut();
        audoutExit();
        S.audout_ok = false;
    }
    restore_output();
    free_buffers();
    log_file("speaker: stopped after %llu frames (%u buffers dropped)\n", (unsigned long long)S.st.frames, S.st.dropped);
}

void spk_get(spk_status *out) {
    mutexLock(&S.lock);   // a zeroed Mutex is an unlocked one: fine before the first start
    *out = S.st;
    mutexUnlock(&S.lock);
}
