#include "policy.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOUD_DB          -50.0f   // audible sound in the recording: a method can be checked now
#define QUIET_DB         -75.0f   // below this (or digital silence) the recording counts as silent
#define MAX_REASSERT     3        // mute undone by someone else: re-apply this often per session
#define MAX_INCONCLUSIVE 3        // checks that ended in a quiet game moment before giving up
#define STRIKES_SILENCED 2        // recordings that came back after an undo before a method is ruled out

static void plog(pol *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void plog(pol *p, const char *fmt, ...) {
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (p->env.log) p->env.log(p->env.ctx, msg);
}

const char *pol_target_name(int target) {
    switch (target) {
    case POL_T_SPEAKER:   return "speaker";
    case POL_T_HEADPHONE: return "headphone jack";
    case POL_T_TV:        return "TV";
    case POL_T_USB:       return "USB output device";
    case POL_T_BLUETOOTH: return "Bluetooth";
    default:              return "invalid";
    }
}

const char *pol_method_name(int method) {
    switch (method) {
    case POL_M_MUTE:    return "mute";
    case POL_M_VOLUME0: return "volume 0";
    case POL_M_MASTER0: return "master volume 0";
    default:            return "none";
    }
}

const char *pol_phase_name(pol_phase ph) {
    static const char *names[] = {"off", "aside", "wait", "check-skip", "check", "undo-skip", "undo-check", "active", "none"};
    return ph <= PH_NONE ? names[ph] : "?";
}

static bool is_other(int t) { return t == POL_T_HEADPHONE || t == POL_T_USB || t == POL_T_BLUETOOTH; }
static bool is_main(int t) { return t == POL_T_SPEAKER || t == POL_T_TV; }

void pol_init(pol *p, const pol_env *env, int speaker_mode, bool follow, const eo_state *st) {
    memset(p, 0, sizeof(*p));
    p->env = *env;
    p->speaker_mode = speaker_mode;
    p->follow = follow;
    if (st) memcpy(p->verdict, st->verdict, sizeof(p->verdict));
    p->phase = PH_OFF;
}

static int next_method(const pol *p) {
    // Master volume first: the console's volume buttons change the target volume and unmute the
    // target (seen on HOS 22.1.0), but leave the master volume alone.
    static const int order_auto[] = {POL_M_MASTER0, POL_M_MUTE, POL_M_VOLUME0, POL_M_NONE};
    static const int order_mst0[] = {POL_M_MASTER0, POL_M_NONE};
    static const int order_mute[] = {POL_M_MUTE, POL_M_NONE};
    static const int order_vol0[] = {POL_M_VOLUME0, POL_M_NONE};
    const int *order = p->speaker_mode == POL_SPK_AUTO      ? order_auto
                       : p->speaker_mode == POL_SPK_MUTE    ? order_mute
                       : p->speaker_mode == POL_SPK_VOLUME0 ? order_vol0
                       : p->speaker_mode == POL_SPK_MASTER0 ? order_mst0
                                                            : NULL;
    for (; order && *order; order++)
        if (p->verdict[p->target][*order] != VERDICT_SILENCED && !(p->failed_mask & (1u << *order))) return *order;
    return POL_M_NONE;
}

static bool apply(pol *p, int method) {
    if (!p->env.silence(p->env.ctx, p->target, method)) {
        p->failed_mask |= 1u << method;
        plog(p, "policy: audctl refused %s on the %s, not using it this session", pol_method_name(method),
             pol_target_name(p->target));
        return false;
    }
    p->applied = method;
    p->have_volume = false;   // volume tracking restarts with the next snapshot
    return true;
}

static void undo(pol *p, const char *why) {
    if (!p->applied) return;
    p->env.restore(p->env.ctx);
    plog(p, "policy: %s back on (%s)", pol_target_name(p->target), why);
    p->applied = POL_M_NONE;
}

static void set_verdict(pol *p, int method, eo_verdict v) {
    if (p->verdict[p->target][method] == v) return;
    p->verdict[p->target][method] = v;
    if (p->env.verdict) p->env.verdict(p->env.ctx, p->target, method, v);
}

static void begin_session(pol *p, int target) {
    p->phase = PH_WAIT;
    p->target = target;
    p->cand = POL_M_NONE;
    p->failed_mask = 0;
    p->reasserts = 0;
    p->inconclusive = 0;
    memset(p->strikes, 0, sizeof(p->strikes));
    p->have_volume = false;
    p->none_logged = false;
}

static void give_up(pol *p) {
    p->phase = PH_NONE;
    if (p->none_logged) return;
    p->none_logged = true;
    if (p->speaker_mode == POL_SPK_OFF)
        plog(p, "policy: speaker_mute=off: the %s stays on", pol_target_name(p->target));
    else
        plog(p, "policy: no usable way to silence the %s (master volume 0: %s, mute: %s, volume 0: %s): it stays on",
             pol_target_name(p->target), st_verdict_name(p->verdict[p->target][POL_M_MASTER0]),
             st_verdict_name(p->verdict[p->target][POL_M_MUTE]), st_verdict_name(p->verdict[p->target][POL_M_VOLUME0]));
}

void pol_stop(pol *p, const char *why) {
    undo(p, why);
    if (p->phase != PH_ASIDE) p->phase = PH_OFF;
}

static void watchdog(pol *p, const pol_input *in) {
    if (p->applied == POL_M_MUTE && !in->mute && p->have_volume && in->volume != p->last_volume) {
        // The console's volume buttons unmute the target (seen on HOS 22.1.0): that is the user
        // changing the console volume, not someone fighting us. Mute again, every time.
        plog(p, "policy: console volume %d -> %d unmuted the %s: muting it again", p->last_volume, in->volume,
             pol_target_name(p->target));
        p->env.reassert(p->env.ctx);
        if (p->follow) {
            char why[64];
            snprintf(why, sizeof(why), "console volume %d -> %d", p->last_volume, in->volume);
            p->env.volume_steps(p->env.ctx, in->volume - p->last_volume, why);
        }
        p->last_volume = in->volume;
        return;
    }
    if (p->applied == POL_M_MUTE && !in->mute) {
        p->have_volume = false;
        if (p->reasserts < MAX_REASSERT) {
            p->reasserts++;
            plog(p, "policy: the %s was unmuted by someone else, muting it again (%d/%d)", pol_target_name(p->target),
                 p->reasserts, MAX_REASSERT);
            p->env.reassert(p->env.ctx);
        } else {
            plog(p, "policy: the %s keeps getting unmuted, leaving it on for this session", pol_target_name(p->target));
            undo(p, "kept getting unmuted");
            p->none_logged = true;
            p->phase = PH_NONE;
        }
        return;
    }
    if (p->applied == POL_M_MASTER0 && in->have_master && in->master > 0.001f) {
        plog(p, "policy: master volume %.2f set by someone else while the %s is silenced: setting it to 0 again",
             (double)in->master, pol_target_name(p->target));
        p->env.reassert(p->env.ctx);
        return;
    }
    if (p->applied == POL_M_VOLUME0 && in->volume != in->volume_min) {
        plog(p, "policy: console volume %d while the %s is silenced with volume 0: set back to %d (in this mode only "
                "the earphone's own buttons change the earphone volume)",
             in->volume, pol_target_name(p->target), in->volume_min);
        p->env.reassert(p->env.ctx);
        return;
    }
    if (p->applied == POL_M_MUTE || p->applied == POL_M_MASTER0) {   // console volume: unmute detection, follow
        if (p->follow && p->have_volume && in->volume != p->last_volume) {
            char why[64];
            snprintf(why, sizeof(why), "console volume %d -> %d", p->last_volume, in->volume);
            p->env.volume_steps(p->env.ctx, in->volume - p->last_volume, why);
        }
        p->last_volume = in->volume;
        p->have_volume = true;
    }
}

void pol_poll(pol *p, const pol_input *in) {
    if (!in->valid) return;
    char why[96];
    if (is_other(in->target)) {
        if (p->phase != PH_ASIDE) {
            snprintf(why, sizeof(why), "the active output is the %s", pol_target_name(in->target));
            undo(p, why);
            p->phase = PH_ASIDE;
            plog(p, "policy: %s: stepping aside (earphone stream held, nothing silenced)", why);
            p->env.hold(p->env.ctx, true, why);
        }
        return;
    }
    if (!is_main(in->target)) return;   // invalid while outputs switch: change nothing
    if (p->phase == PH_ASIDE) {
        snprintf(why, sizeof(why), "the active output is the %s again", pol_target_name(in->target));
        plog(p, "policy: %s: the earphone may play again", why);
        p->env.hold(p->env.ctx, false, why);
        p->phase = PH_OFF;
    }
    if (!in->streaming) {
        if (p->phase != PH_OFF) pol_stop(p, "earphone stream stopped");
        return;
    }
    if (p->phase == PH_OFF) {
        begin_session(p, in->target);
    } else if (in->target != p->target) {
        snprintf(why, sizeof(why), "output changed from the %s to the %s", pol_target_name(p->target),
                 pol_target_name(in->target));
        undo(p, why);
        plog(p, "policy: %s", why);
        begin_session(p, in->target);
    }
    p->capture_ok = in->capture_ok;
    if (!in->capture_ok && p->applied) {
        // Without a recording the earphone is silent too: the console must not be.
        undo(p, "the recording stopped delivering audio");
        if (p->phase != PH_NONE) p->phase = PH_WAIT;
    }

    switch (p->phase) {
    case PH_WAIT: {
        if (!in->stream_ok || !in->capture_ok) break;
        int m = next_method(p);
        if (m == POL_M_NONE) {
            give_up(p);
            break;
        }
        if (p->verdict[p->target][m] == VERDICT_KEPT) {
            if (apply(p, m))
                plog(p, "policy: %s silenced with %s (state.ini: it keeps the recording on this firmware)",
                     pol_target_name(p->target), pol_method_name(m));
            if (p->applied) p->phase = PH_ACTIVE;
            break;
        }
        if (p->cand != m) {
            p->cand = m;
            plog(p, "policy: %s on the %s is unverified: waiting for audible sound to check it",
                 pol_method_name(m), pol_target_name(p->target));
        }
        break;
    }
    case PH_ACTIVE:
        watchdog(p, in);
        break;
    default:
        break;
    }
}

void pol_interval(pol *p, float rms_db, bool digital_silence) {
    bool loud = !digital_silence && rms_db > LOUD_DB;
    bool quiet = digital_silence || rms_db < QUIET_DB;
    float level = digital_silence ? -120.0f : rms_db;
    switch (p->phase) {
    case PH_WAIT:
        if (p->cand == POL_M_NONE || p->cand != next_method(p) || !loud || !p->capture_ok) break;
        p->before_db = level;
        if (!apply(p, p->cand)) {
            p->cand = POL_M_NONE;
            break;
        }
        p->phase = PH_CHECK_SKIP;
        plog(p, "policy: checking %s on the %s (recording %.1f dBFS before)", pol_method_name(p->cand),
             pol_target_name(p->target), (double)level);
        break;
    case PH_CHECK_SKIP:
        p->phase = PH_CHECK;
        break;
    case PH_CHECK:
        if (quiet) {
            p->after_db = level;
            undo(p, "self-check: the recording went quiet");
            p->phase = PH_UNDO_SKIP;
            plog(p, "policy: the recording went %s with %s (%.1f -> %.1f dBFS): undone, checking whether it comes back",
                 digital_silence ? "silent" : "quiet", pol_method_name(p->cand), (double)p->before_db, (double)level);
        } else {
            set_verdict(p, p->cand, VERDICT_KEPT);
            p->phase = PH_ACTIVE;
            plog(p, "policy: %s keeps the recording (%.1f -> %.1f dBFS): the %s stays silenced", pol_method_name(p->cand),
                 (double)p->before_db, (double)level, pol_target_name(p->target));
        }
        break;
    case PH_UNDO_SKIP:
        p->phase = PH_UNDO_CHECK;
        break;
    case PH_UNDO_CHECK:
        p->phase = PH_WAIT;
        if (loud) {
            int m = p->cand;
            if (++p->strikes[m] >= STRIKES_SILENCED) {
                set_verdict(p, m, VERDICT_SILENCED);
                p->cand = POL_M_NONE;
                plog(p, "policy: the recording came back (%.1f dBFS) after undoing %s again: %s silences the recording "
                        "on the %s, not using it",
                     (double)level, pol_method_name(m), pol_method_name(m), pol_target_name(p->target));
            } else {
                plog(p, "policy: the recording came back (%.1f dBFS) after undoing %s: checking once more",
                     (double)level, pol_method_name(m));
            }
        } else if (++p->inconclusive >= MAX_INCONCLUSIVE) {
            plog(p, "policy: still quiet after undoing %s (%.1f dBFS) %d times: no clear answer", pol_method_name(p->cand),
                 (double)level, p->inconclusive);
            p->none_logged = true;
            p->phase = PH_NONE;
            plog(p, "policy: the %s stays on for this session", pol_target_name(p->target));
        } else {
            plog(p, "policy: still quiet after undoing %s (%.1f dBFS), probably a quiet moment in the game: retrying "
                    "later (%d/%d)",
                 pol_method_name(p->cand), (double)level, p->inconclusive, MAX_INCONCLUSIVE);
        }
        break;
    default:
        break;
    }
}
