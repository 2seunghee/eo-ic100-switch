#include "speaker.h"

#include <math.h>
#include <unistd.h>

#include "log.h"

#define RESTORE_PATH APP_DIR "/speaker_restore.txt"

// audctl is opened for each call and closed again: system services have a fixed number of
// sessions, and a background module should not sit on one of audctl's.
static struct {
    bool       ok;        // audctl answered at start-up
    s32        vmin, vmax;
    spk_method method;    // what eoaudio silenced, SPK_M_NONE if nothing
    spk_state  saved;     // that target's state before (saved.target = the silenced target)
} S;

const char *spk_target_name(AudioTarget t) {
    switch (t) {
    case AudioTarget_Speaker:         return "speaker";
    case AudioTarget_Headphone:       return "headphone jack";
    case AudioTarget_Tv:              return "TV";
    case AudioTarget_UsbOutputDevice: return "USB output device";
    case AudioTarget_Bluetooth:       return "Bluetooth";
    default:                          return "invalid";
    }
}

const char *spk_method_name(spk_method m) {
    switch (m) {
    case SPK_M_TARGET_MUTE:    return "SetTargetMute";
    case SPK_M_TARGET_VOLUME0: return "SetTargetVolume(min)";
    case SPK_M_MASTER0:        return "SetSystemOutputMasterVolume(0)";
    default:                   return "none";
    }
}

spk_method spk_silenced(void) { return S.method; }
AudioTarget spk_silenced_target(void) { return S.method ? S.saved.target : AudioTarget_Invalid; }

void spk_volume_range(s32 *min, s32 *max) {
    *min = S.vmin;
    *max = S.vmax;
}

static bool ctl_open(void) { return S.ok && R_SUCCEEDED(audctlInitialize()); }
static void ctl_close(void) { audctlExit(); }

// State of one target (audctl open).
static bool read_target(AudioTarget t, spk_state *s) {
    memset(s, 0, sizeof(*s));
    s->target = t;
    if (R_FAILED(audctlGetTargetVolume(&s->volume, t))) return false;
    if (R_FAILED(audctlIsTargetMute(&s->mute, t))) s->mute = false;
    s->have_master = hosversionAtLeast(4, 0, 0) && R_SUCCEEDED(audctlGetSystemOutputMasterVolume(&s->master));
    s->ok = true;
    return true;
}

bool spk_read(spk_state *s) {
    memset(s, 0, sizeof(*s));
    if (!ctl_open()) return false;
    AudioTarget t = AudioTarget_Invalid;
    Result rc = hosversionAtLeast(13, 0, 0) ? audctlGetActiveOutputTarget(&t) : audctlGetDefaultTarget(&t);
    bool ok = (R_SUCCEEDED(rc) && t != AudioTarget_Invalid) || R_SUCCEEDED(audctlGetDefaultTarget(&t));
    ok = ok && read_target(t, s);
    ctl_close();
    return ok;
}

static Result put_back(spk_method m, const spk_state *s) {
    switch (m) {
    case SPK_M_TARGET_MUTE:    return audctlSetTargetMute(s->target, s->mute);
    case SPK_M_TARGET_VOLUME0: return audctlSetTargetVolume(s->target, s->volume);
    case SPK_M_MASTER0:        return s->have_master ? audctlSetSystemOutputMasterVolume(s->master) : 0;
    default:                   return 0;
    }
}

static Result apply(spk_method m, AudioTarget t) {
    switch (m) {
    case SPK_M_TARGET_MUTE:    return audctlSetTargetMute(t, true);
    case SPK_M_TARGET_VOLUME0: return audctlSetTargetVolume(t, S.vmin);
    case SPK_M_MASTER0:        return audctlSetSystemOutputMasterVolume(0.0f);
    default:                   return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
}

void spk_init(void) {
    Result rc = audctlInitialize();
    S.ok = R_SUCCEEDED(rc);
    if (S.ok) {
        audctlGetTargetVolumeMin(&S.vmin);
        audctlGetTargetVolumeMax(&S.vmax);
    }
    log_printf("speaker: audctl: %s, target volume range %d..%d", rc_str(rc), S.vmin, S.vmax);

    char text[256];
    if (S.ok && read_file(RESTORE_PATH, text, sizeof(text)) >= 0) {
        int method = 0, target = 0, volume = 0, mute = 0, have_master = 0;
        float master = 1.0f;
        int n = sscanf(text, "method=%d target=%d volume=%d mute=%d master=%f have_master=%d", &method, &target,
                       &volume, &mute, &master, &have_master);
        if (n >= 4 && method > SPK_M_NONE && method <= SPK_M_MASTER0) {
            spk_state s;
            memset(&s, 0, sizeof(s));
            s.target = (AudioTarget)target;
            s.volume = volume;
            s.mute = mute != 0;
            s.master = master;
            s.have_master = n >= 6 && have_master;
            rc = put_back((spk_method)method, &s);
            log_printf("speaker: the last run ended with the %s silenced (%s): put back volume %d, mute %d, "
                       "master %.2f: %s",
                       spk_target_name(s.target), spk_method_name((spk_method)method), s.volume, s.mute,
                       (double)s.master, rc_str(rc));
        }
        unlink(RESTORE_PATH);
    }
    if (S.ok) audctlExit();
}

void spk_exit(void) { spk_restore(); }

Result spk_silence(AudioTarget target, spk_method m) {
    if (!ctl_open()) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (S.method) {
        put_back(S.method, &S.saved);
        S.method = SPK_M_NONE;
    }
    spk_state s;
    if (!read_target(target, &s)) {
        ctl_close();
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    char text[160];
    int n = snprintf(text, sizeof(text), "method=%d\ntarget=%d\nvolume=%d\nmute=%d\nmaster=%.4f\nhave_master=%d\n", m,
                     s.target, s.volume, s.mute, (double)s.master, s.have_master);
    save_file(RESTORE_PATH, text, (size_t)n);   // first, so that a crash right after is covered
    Result rc = (m == SPK_M_MASTER0 && !s.have_master) ? MAKERESULT(Module_Libnx, LibnxError_NotFound) : apply(m, target);
    ctl_close();
    log_printf("speaker: %s on the %s (was volume %d, mute %d, master %.2f): %s", spk_method_name(m),
               spk_target_name(target), s.volume, s.mute, (double)s.master, rc_str(rc));
    if (R_FAILED(rc)) {
        unlink(RESTORE_PATH);
        return rc;
    }
    S.method = m;
    S.saved = s;
    return 0;
}

Result spk_reassert(void) {
    if (S.method == SPK_M_NONE) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    if (!ctl_open()) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    Result rc = apply(S.method, S.saved.target);
    ctl_close();
    log_printf("speaker: %s on the %s again: %s", spk_method_name(S.method), spk_target_name(S.saved.target), rc_str(rc));
    return rc;
}

Result spk_step_volume(AudioTarget target, int dir, s32 *out) {
    if (!ctl_open()) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    s32 v = 0;
    Result rc = audctlGetTargetVolume(&v, target);
    if (R_SUCCEEDED(rc)) {
        s32 nv = v + dir;
        if (nv < S.vmin) nv = S.vmin;
        if (nv > S.vmax) nv = S.vmax;
        if (nv != v) rc = audctlSetTargetVolume(target, nv);
        v = nv;
    }
    ctl_close();
    if (out) *out = v;
    return rc;
}

void spk_restore(void) {
    if (S.method == SPK_M_NONE) return;
    Result rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (ctl_open()) {
        rc = put_back(S.method, &S.saved);
        ctl_close();
    }
    log_printf("speaker: undo %s on the %s (volume %d, mute %d): %s", spk_method_name(S.method),
               spk_target_name(S.saved.target), S.saved.volume, S.saved.mute, rc_str(rc));
    if (R_SUCCEEDED(rc)) unlink(RESTORE_PATH);   // otherwise the next start puts it back
    S.method = SPK_M_NONE;
}

void spk_emergency_restore(void) {
    if (S.method == SPK_M_NONE) return;
    if (R_SUCCEEDED(audctlInitialize())) {
        if (R_SUCCEEDED(put_back(S.method, &S.saved))) unlink(RESTORE_PATH);
        audctlExit();
    }
    S.method = SPK_M_NONE;
}
