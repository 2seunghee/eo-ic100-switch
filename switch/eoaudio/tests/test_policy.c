// Host-side unit tests for source/policy.c (speaker silencing with self-check, stepping aside,
// console volume follow) and source/state.c (state.ini).
//   make -C switch/eoaudio/tests check
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/policy.h"
#include "../source/state.h"

static int g_fail, g_pass;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (cond) g_pass++;                                                           \
        else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }     \
    } while (0)

// ------------------------------------------------------------------ fake console

typedef struct {
    int        silenced_target, silenced_method;   // what is applied right now (0 = nothing)
    int        silence_calls, restore_calls, reassert_calls, bad_calls;
    bool       hold;
    int        hold_calls;
    int        steps, step_calls;
    eo_verdict verdicts[ST_TARGETS][ST_METHODS];
    int        verdict_calls;
    bool       refuse[4];             // audctl refuses this method
    bool       silences_capture[4];   // the world: this method also silences the recording
    int        logs;
    bool       verbose;
} fake;

static bool f_silence(void *ctx, int target, int method) {
    fake *f = ctx;
    f->silence_calls++;
    if (f->refuse[method]) return false;
    if (f->silenced_method) f->bad_calls++;   // the policy must undo before applying another method
    f->silenced_target = target;
    f->silenced_method = method;
    return true;
}
static void f_reassert(void *ctx) {
    fake *f = ctx;
    f->reassert_calls++;
    if (!f->silenced_method) f->bad_calls++;
}
static void f_restore(void *ctx) {
    fake *f = ctx;
    f->restore_calls++;
    if (!f->silenced_method) f->bad_calls++;
    f->silenced_method = f->silenced_target = 0;
}
static void f_hold(void *ctx, bool hold, const char *why) {
    (void)why;
    fake *f = ctx;
    f->hold = hold;
    f->hold_calls++;
}
static void f_steps(void *ctx, int steps, const char *why) {
    (void)why;
    fake *f = ctx;
    f->steps += steps;
    f->step_calls++;
}
static void f_verdict(void *ctx, int target, int method, eo_verdict v) {
    fake *f = ctx;
    f->verdicts[target][method] = v;
    f->verdict_calls++;
}
static void f_log(void *ctx, const char *msg) {
    fake *f = ctx;
    f->logs++;
    if (f->verbose) printf("    %s\n", msg);
}

static pol_env env_for(fake *f) {
    pol_env e = {f_silence, f_reassert, f_restore, f_hold, f_steps, f_verdict, f_log, f};
    return e;
}

// The older scenarios were written for auto = mute -> volume 0; they start with master volume 0
// already ruled out. test_master0 covers master volume 0 itself.
static void init_pol(pol *p, const pol_env *e, int mode, bool follow, const eo_state *st) {
    pol_init(p, e, mode, follow, st);
    for (int t = 0; t < ST_TARGETS; t++)
        if (p->verdict[t][POL_M_MASTER0] == VERDICT_UNKNOWN) p->verdict[t][POL_M_MASTER0] = VERDICT_SILENCED;
}

static bool g_capture = true;   // the recorder delivers (pol_input.capture_ok)

static void poll(pol *p, bool streaming, bool ok, int target, int volume, bool mute) {
    pol_input in = {true, streaming, ok, g_capture, target, volume, 0, mute, false, 0.0f};
    pol_poll(p, &in);
}

// The recording the console delivers for one second of game sound at `game_db` (-120 = silence).
static void second(pol *p, fake *f, float game_db) {
    bool silent = game_db <= -120.0f || (f->silenced_method && f->silences_capture[f->silenced_method]);
    pol_interval(p, silent ? -120.0f : game_db, silent);
}

static void start_stream(pol *p, int target) {
    poll(p, true, false, target, 8, false);
    poll(p, true, true, target, 8, false);
}

// ------------------------------------------------------------------ scenarios

static void test_mute_keeps_recording(void) {
    fake f = {0};
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, true, NULL);
    poll(&p, false, false, POL_T_SPEAKER, 8, false);
    CHECK(p.phase == PH_OFF && f.silence_calls == 0);
    poll(&p, true, false, POL_T_SPEAKER, 8, false);   // stream not healthy yet
    CHECK(p.phase == PH_WAIT && f.silence_calls == 0);
    poll(&p, true, true, POL_T_SPEAKER, 8, false);
    CHECK(p.phase == PH_WAIT && p.cand == POL_M_MUTE && f.silence_calls == 0);
    second(&p, &f, -100.0f);   // nothing audible yet: no check
    CHECK(f.silence_calls == 0);
    second(&p, &f, -20.0f);
    CHECK(f.silenced_method == POL_M_MUTE && p.phase == PH_CHECK_SKIP);
    second(&p, &f, -20.0f);
    second(&p, &f, -22.0f);
    CHECK(p.phase == PH_ACTIVE && f.verdicts[POL_T_SPEAKER][POL_M_MUTE] == VERDICT_KEPT && f.verdict_calls == 1);
    // Console volume follow while muted.
    poll(&p, true, true, POL_T_SPEAKER, 8, true);
    poll(&p, true, true, POL_T_SPEAKER, 10, true);
    CHECK(f.steps == 2 && f.step_calls == 1);
    poll(&p, true, true, POL_T_SPEAKER, 9, true);
    CHECK(f.steps == 1 && f.step_calls == 2);
    // Unplugged: back on at once.
    poll(&p, false, false, POL_T_SPEAKER, 9, true);
    CHECK(f.silenced_method == 0 && f.restore_calls == 1 && p.phase == PH_OFF && f.bad_calls == 0);
}

static void test_fallback_to_volume0(void) {
    fake f = {0};
    f.silences_capture[POL_M_MUTE] = true;
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, true, NULL);
    start_stream(&p, POL_T_SPEAKER);
    for (int round = 0; round < 2; round++) {
        second(&p, &f, -20.0f);   // loud: apply mute
        CHECK(f.silenced_method == POL_M_MUTE);
        second(&p, &f, -20.0f);   // transition
        second(&p, &f, -20.0f);   // the recording is silent with mute
        CHECK(f.silenced_method == 0 && p.phase == PH_UNDO_SKIP);
        second(&p, &f, -20.0f);   // transition
        second(&p, &f, -20.0f);   // back after the undo: a strike
        CHECK(p.phase == PH_WAIT);
    }
    CHECK(f.verdicts[POL_T_SPEAKER][POL_M_MUTE] == VERDICT_SILENCED);
    poll(&p, true, true, POL_T_SPEAKER, 8, false);
    CHECK(p.cand == POL_M_VOLUME0);
    second(&p, &f, -20.0f);
    CHECK(f.silenced_method == POL_M_VOLUME0);
    second(&p, &f, -20.0f);
    second(&p, &f, -21.0f);
    CHECK(p.phase == PH_ACTIVE && f.verdicts[POL_T_SPEAKER][POL_M_VOLUME0] == VERDICT_KEPT);
    // In volume-0 mode a console volume press is undone and does not move the earphone volume.
    poll(&p, true, true, POL_T_SPEAKER, 0, false);
    poll(&p, true, true, POL_T_SPEAKER, 3, false);
    CHECK(f.reassert_calls == 1 && f.step_calls == 0);
    pol_stop(&p, "unplugged");
    CHECK(f.silenced_method == 0 && f.bad_calls == 0);
}

static void test_nothing_works(void) {
    fake f = {0};
    f.silences_capture[POL_M_MUTE] = f.silences_capture[POL_M_VOLUME0] = true;
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, true, NULL);
    start_stream(&p, POL_T_SPEAKER);
    for (int i = 0; i < 40 && p.phase != PH_NONE; i++) {
        second(&p, &f, -20.0f);
        poll(&p, true, true, POL_T_SPEAKER, 8, false);
    }
    CHECK(p.phase == PH_NONE && f.silenced_method == 0);
    CHECK(f.verdicts[POL_T_SPEAKER][POL_M_MUTE] == VERDICT_SILENCED &&
          f.verdicts[POL_T_SPEAKER][POL_M_VOLUME0] == VERDICT_SILENCED);
    int calls = f.silence_calls;
    for (int i = 0; i < 10; i++) {
        second(&p, &f, -20.0f);
        poll(&p, true, true, POL_T_SPEAKER, 8, false);
    }
    CHECK(f.silence_calls == calls);   // no more attempts this session

    // The next session starts from state.ini: nothing to try, not even an audible check.
    eo_state st;
    st_init(&st);
    memcpy(st.verdict, f.verdicts, sizeof(st.verdict));
    fake g = {0};
    pol q;
    pol_env eg = env_for(&g);
    init_pol(&q, &eg, POL_SPK_AUTO, true, &st);
    start_stream(&q, POL_T_SPEAKER);
    CHECK(q.phase == PH_NONE && g.silence_calls == 0);
    CHECK(f.bad_calls == 0 && g.bad_calls == 0);
}

static void test_quiet_game_moment(void) {
    fake f = {0};   // mute keeps the recording, but the game goes silent exactly during the check
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, true, NULL);
    start_stream(&p, POL_T_SPEAKER);
    second(&p, &f, -20.0f);
    second(&p, &f, -20.0f);
    second(&p, &f, -120.0f);   // silent: undone
    CHECK(f.silenced_method == 0);
    second(&p, &f, -120.0f);
    second(&p, &f, -120.0f);   // still silent after the undo: inconclusive, no verdict
    CHECK(p.phase == PH_WAIT && p.inconclusive == 1 && f.verdict_calls == 0);
    second(&p, &f, -25.0f);    // audible again: next check
    second(&p, &f, -25.0f);
    second(&p, &f, -24.0f);
    CHECK(p.phase == PH_ACTIVE && f.verdicts[POL_T_SPEAKER][POL_M_MUTE] == VERDICT_KEPT);

    // A single coincidence (quiet while muted, loud right after the undo) is not enough to rule mute out.
    fake g = {0};
    pol q;
    pol_env eg = env_for(&g);
    init_pol(&q, &eg, POL_SPK_AUTO, true, NULL);
    start_stream(&q, POL_T_SPEAKER);
    second(&q, &g, -20.0f);
    second(&q, &g, -20.0f);
    second(&q, &g, -120.0f);
    second(&q, &g, -20.0f);
    second(&q, &g, -20.0f);   // strike 1
    CHECK(q.strikes[POL_M_MUTE] == 1 && g.verdicts[POL_T_SPEAKER][POL_M_MUTE] == VERDICT_UNKNOWN);
    second(&q, &g, -20.0f);   // checked again
    second(&q, &g, -20.0f);
    second(&q, &g, -21.0f);
    CHECK(q.phase == PH_ACTIVE && g.verdicts[POL_T_SPEAKER][POL_M_MUTE] == VERDICT_KEPT);

    // Three inconclusive checks: leave the speaker on for this session.
    fake h = {0};
    pol r;
    pol_env eh = env_for(&h);
    init_pol(&r, &eh, POL_SPK_AUTO, true, NULL);
    start_stream(&r, POL_T_SPEAKER);
    for (int i = 0; i < 3; i++) {
        second(&r, &h, -20.0f);
        second(&r, &h, -20.0f);
        second(&r, &h, -120.0f);
        second(&r, &h, -120.0f);
        second(&r, &h, -120.0f);
    }
    CHECK(r.phase == PH_NONE && h.silenced_method == 0 && h.verdict_calls == 0);
    CHECK(f.bad_calls == 0 && g.bad_calls == 0 && h.bad_calls == 0);
}

static void test_known_good(void) {
    eo_state st;
    st_init(&st);
    st.verdict[POL_T_SPEAKER][POL_M_MUTE] = VERDICT_KEPT;
    fake f = {0};
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, true, &st);
    poll(&p, true, false, POL_T_SPEAKER, 8, false);
    CHECK(f.silence_calls == 0);   // not before the stream is healthy
    poll(&p, true, true, POL_T_SPEAKER, 8, false);
    CHECK(f.silenced_method == POL_M_MUTE && p.phase == PH_ACTIVE);   // no waiting for sound
}

static void test_step_aside(void) {
    eo_state st;
    st_init(&st);
    st.verdict[POL_T_SPEAKER][POL_M_MUTE] = VERDICT_KEPT;
    fake f = {0};
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, true, &st);
    start_stream(&p, POL_T_SPEAKER);
    CHECK(f.silenced_method == POL_M_MUTE);
    poll(&p, true, true, POL_T_HEADPHONE, 8, false);   // 3.5 mm headphones plugged in
    CHECK(f.silenced_method == 0 && f.hold && p.phase == PH_ASIDE);
    poll(&p, false, false, POL_T_HEADPHONE, 8, false);   // the stream stops because it is held
    poll(&p, false, false, POL_T_USB, 8, false);
    CHECK(f.hold_calls == 1 && f.silence_calls == 1);
    poll(&p, false, false, POL_T_INVALID, 8, false);     // switching outputs: no decision
    CHECK(f.hold);
    poll(&p, false, false, POL_T_SPEAKER, 8, false);
    CHECK(!f.hold && p.phase == PH_OFF && f.hold_calls == 2);
    start_stream(&p, POL_T_SPEAKER);
    CHECK(f.silenced_method == POL_M_MUTE && p.phase == PH_ACTIVE);
    // Bluetooth audio also makes it step aside; nothing is applied while aside.
    poll(&p, true, true, POL_T_BLUETOOTH, 8, false);
    second(&p, &f, -20.0f);
    CHECK(f.silenced_method == 0 && p.phase == PH_ASIDE && f.bad_calls == 0);
}

static void test_unmuted_by_system(void) {
    eo_state st;
    st_init(&st);
    st.verdict[POL_T_SPEAKER][POL_M_MUTE] = VERDICT_KEPT;
    fake f = {0};
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, true, &st);
    start_stream(&p, POL_T_SPEAKER);
    for (int i = 0; i < 3; i++) poll(&p, true, true, POL_T_SPEAKER, 8, false);   // reads back unmuted
    CHECK(f.reassert_calls == 3 && p.phase == PH_ACTIVE);
    poll(&p, true, true, POL_T_SPEAKER, 8, false);
    CHECK(p.phase == PH_NONE && f.silenced_method == 0 && f.bad_calls == 0);
    CHECK(f.step_calls == 0);   // re-muting is not a volume change
}

static void test_volume_buttons_unmute(void) {
    // HOS 22.1.0: each console volume press unmutes the speaker. That must be re-muted every time,
    // without counting towards the "someone keeps unmuting it" limit (seen in the 14:47 log).
    eo_state st;
    st_init(&st);
    st.verdict[POL_T_SPEAKER][POL_M_MUTE] = VERDICT_KEPT;
    fake f = {0};
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, false, &st);
    start_stream(&p, POL_T_SPEAKER);
    poll(&p, true, true, POL_T_SPEAKER, 4, true);   // muted, volume 4
    for (int v = 5; v <= 12; v++) poll(&p, true, true, POL_T_SPEAKER, v, false);   // presses: unmuted each time
    CHECK(f.reassert_calls == 8 && p.phase == PH_ACTIVE && f.silenced_method == POL_M_MUTE);
    CHECK(f.step_calls == 0);   // follow is off
    poll(&p, true, true, POL_T_SPEAKER, 12, true);
    CHECK(p.reasserts == 0 && f.bad_calls == 0);
}

static void test_target_change(void) {
    eo_state st;
    st_init(&st);
    st.verdict[POL_T_SPEAKER][POL_M_MUTE] = VERDICT_KEPT;
    fake f = {0};
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, true, &st);
    start_stream(&p, POL_T_SPEAKER);
    CHECK(f.silenced_target == POL_T_SPEAKER);
    poll(&p, true, true, POL_T_TV, 8, false);   // docked: the TV has no verdict yet
    CHECK(f.silenced_method == 0 && p.phase == PH_WAIT && p.target == POL_T_TV && p.cand == POL_M_MUTE);
    second(&p, &f, -20.0f);
    CHECK(f.silenced_target == POL_T_TV && f.silenced_method == POL_M_MUTE);
    second(&p, &f, -20.0f);
    second(&p, &f, -20.0f);
    CHECK(f.verdicts[POL_T_TV][POL_M_MUTE] == VERDICT_KEPT && f.bad_calls == 0);
}

static void test_off_and_refused(void) {
    fake f = {0};
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_OFF, true, NULL);
    start_stream(&p, POL_T_SPEAKER);
    second(&p, &f, -20.0f);
    CHECK(p.phase == PH_NONE && f.silence_calls == 0);
    poll(&p, true, true, POL_T_HEADPHONE, 8, false);   // stepping aside still works with speaker_mute=off
    CHECK(f.hold && p.phase == PH_ASIDE);

    fake g = {0};
    g.refuse[POL_M_MUTE] = true;
    pol q;
    pol_env eg = env_for(&g);
    init_pol(&q, &eg, POL_SPK_AUTO, true, NULL);
    start_stream(&q, POL_T_SPEAKER);
    second(&q, &g, -20.0f);   // mute refused
    CHECK(g.silenced_method == 0 && (q.failed_mask & (1u << POL_M_MUTE)));
    poll(&q, true, true, POL_T_SPEAKER, 8, false);
    CHECK(q.cand == POL_M_VOLUME0);
    second(&q, &g, -20.0f);
    CHECK(g.silenced_method == POL_M_VOLUME0);

    fake h = {0};
    pol r;
    pol_env eh = env_for(&h);
    init_pol(&r, &eh, POL_SPK_MUTE, true, NULL);   // only mute: no fallback to volume 0
    h.silences_capture[POL_M_MUTE] = true;
    start_stream(&r, POL_T_SPEAKER);
    for (int i = 0; i < 20 && r.phase != PH_NONE; i++) {
        second(&r, &h, -20.0f);
        poll(&r, true, true, POL_T_SPEAKER, 8, false);
    }
    CHECK(r.phase == PH_NONE && h.silenced_method == 0);
    CHECK(h.verdicts[POL_T_SPEAKER][POL_M_VOLUME0] == VERDICT_UNKNOWN);
    CHECK(f.bad_calls == 0 && g.bad_calls == 0 && h.bad_calls == 0);
}

static void test_stop_mid_check(void) {
    fake f = {0};
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, true, NULL);
    start_stream(&p, POL_T_SPEAKER);
    second(&p, &f, -20.0f);
    CHECK(f.silenced_method == POL_M_MUTE && p.phase == PH_CHECK_SKIP);
    pol_stop(&p, "unplugged");
    CHECK(f.silenced_method == 0 && p.phase == PH_OFF);
    second(&p, &f, -20.0f);   // late interval: nothing happens
    CHECK(f.silence_calls == 1 && f.bad_calls == 0);
    pol_stop(&p, "again");   // idempotent
    CHECK(f.restore_calls == 1);
}

static void test_invalid_snapshot(void) {
    eo_state st;
    st_init(&st);
    st.verdict[POL_T_SPEAKER][POL_M_MUTE] = VERDICT_KEPT;
    fake f = {0};
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, true, &st);
    start_stream(&p, POL_T_SPEAKER);
    pol_input bad = {false, false, false, false, POL_T_HEADPHONE, 0, 0, false, false, 0.0f};
    pol_poll(&p, &bad);   // audctl unreadable: nothing changes
    CHECK(f.silenced_method == POL_M_MUTE && !f.hold);
}

static void test_capture_lost(void) {
    // A remembered "kept" verdict must not silence the speaker while the recorder delivers nothing:
    // the earphone would be silent as well.
    eo_state st;
    st_init(&st);
    st.verdict[POL_T_SPEAKER][POL_M_MUTE] = VERDICT_KEPT;
    fake f = {0};
    pol p;
    pol_env e = env_for(&f);
    init_pol(&p, &e, POL_SPK_AUTO, true, &st);
    g_capture = false;
    start_stream(&p, POL_T_SPEAKER);
    CHECK(f.silence_calls == 0 && p.phase == PH_WAIT);
    g_capture = true;
    poll(&p, true, true, POL_T_SPEAKER, 8, false);
    CHECK(f.silenced_method == POL_M_MUTE && p.phase == PH_ACTIVE);
    g_capture = false;   // the recorder stalls: speaker back on at once
    poll(&p, true, true, POL_T_SPEAKER, 8, true);
    CHECK(f.silenced_method == 0 && p.phase == PH_WAIT);
    g_capture = true;
    poll(&p, true, true, POL_T_SPEAKER, 8, false);
    CHECK(f.silenced_method == POL_M_MUTE && p.phase == PH_ACTIVE);

    // Unverified method: a loud interval right after the recorder stalled must not start a check,
    // and a stall in the middle of a check undoes it.
    fake g = {0};
    pol q;
    pol_env eg = env_for(&g);
    init_pol(&q, &eg, POL_SPK_AUTO, true, NULL);
    start_stream(&q, POL_T_SPEAKER);
    g_capture = false;
    poll(&q, true, true, POL_T_SPEAKER, 8, false);
    second(&q, &g, -20.0f);
    CHECK(g.silence_calls == 0);
    g_capture = true;
    poll(&q, true, true, POL_T_SPEAKER, 8, false);
    second(&q, &g, -20.0f);
    CHECK(g.silenced_method == POL_M_MUTE && q.phase == PH_CHECK_SKIP);
    g_capture = false;
    poll(&q, true, true, POL_T_SPEAKER, 8, true);
    CHECK(g.silenced_method == 0 && q.phase == PH_WAIT && g.verdict_calls == 0);
    g_capture = true;
    CHECK(f.bad_calls == 0 && g.bad_calls == 0);
}

static void test_master0(void) {
    // auto tries master volume 0 first; the console volume buttons change the target volume (and
    // unmute it) without touching the master volume, so nothing has to be re-applied on a press.
    fake f = {0};
    pol p;
    pol_env e = env_for(&f);
    pol_init(&p, &e, POL_SPK_AUTO, true, NULL);
    start_stream(&p, POL_T_SPEAKER);
    CHECK(p.cand == POL_M_MASTER0);
    second(&p, &f, -20.0f);
    CHECK(f.silenced_method == POL_M_MASTER0);
    second(&p, &f, -20.0f);
    second(&p, &f, -21.0f);
    CHECK(p.phase == PH_ACTIVE && f.verdicts[POL_T_SPEAKER][POL_M_MASTER0] == VERDICT_KEPT);
    pol_input in = {true, true, true, true, POL_T_SPEAKER, 8, 0, false, true, 0.0f};
    pol_poll(&p, &in);
    in.volume = 9;   // console volume press: follow, no re-apply needed
    pol_poll(&p, &in);
    CHECK(f.reassert_calls == 0 && f.steps == 1);
    in.master = 1.0f;   // someone restored the master volume: set it to 0 again
    pol_poll(&p, &in);
    CHECK(f.reassert_calls == 1);

    // If master volume 0 also silences the recording, auto falls back to mute.
    fake g = {0};
    g.silences_capture[POL_M_MASTER0] = true;
    pol q;
    pol_env eg = env_for(&g);
    pol_init(&q, &eg, POL_SPK_AUTO, true, NULL);
    start_stream(&q, POL_T_SPEAKER);
    for (int i = 0; i < 12 && q.cand != POL_M_MUTE; i++) {
        second(&q, &g, -20.0f);
        poll(&q, true, true, POL_T_SPEAKER, 8, false);
    }
    CHECK(g.verdicts[POL_T_SPEAKER][POL_M_MASTER0] == VERDICT_SILENCED && q.cand == POL_M_MUTE);
    CHECK(f.bad_calls == 0 && g.bad_calls == 0);
}

// ------------------------------------------------------------------ state.ini

static void test_state(void) {
    eo_state s;
    st_init(&s);
    s.have_volume = true;
    s.volume_db = -17.5f;
    snprintf(s.hos, sizeof(s.hos), "22.1.0");
    s.verdict[1][1] = VERDICT_KEPT;
    s.verdict[3][2] = VERDICT_SILENCED;
    char text[512];
    int n = st_format(&s, text, sizeof(text));
    CHECK(n > 0 && strstr(text, "verdict.speaker.mute=kept") && strstr(text, "verdict.tv.volume0=silenced"));
    eo_state t;
    st_init(&t);
    st_parse(&t, text, (size_t)n);
    CHECK(t.have_volume && t.volume_db == -17.5f && strcmp(t.hos, "22.1.0") == 0);
    CHECK(memcmp(t.verdict, s.verdict, sizeof(s.verdict)) == 0);

    const char junk[] = "\r\nvolume_db = -9\r\nvolume_db=+5\nverdict.moon.mute=kept\nverdict.speaker.loud=kept\n"
                        "verdict.speaker.volume0 = silenced # comment\nnonsense\n";
    st_init(&t);
    st_parse(&t, junk, sizeof(junk) - 1);
    CHECK(t.have_volume && t.volume_db == -9.0f);   // +5 dB is out of range and ignored
    CHECK(t.verdict[1][2] == VERDICT_SILENCED && t.verdict[1][1] == VERDICT_UNKNOWN);
    st_forget_verdicts(&t);
    CHECK(t.verdict[1][2] == VERDICT_UNKNOWN);
    CHECK(st_format(&t, text, 20) < 0 || st_format(&t, text, 20) >= 20);   // too small: reported
    CHECK(st_target_key(1) && strcmp(st_target_key(3), "tv") == 0 && !st_target_key(9) && !st_method_key(0));
}

int main(void) {
    test_mute_keeps_recording();
    test_fallback_to_volume0();
    test_nothing_works();
    test_quiet_game_moment();
    test_known_good();
    test_step_aside();
    test_unmuted_by_system();
    test_volume_buttons_unmute();
    test_target_change();
    test_off_and_refused();
    test_stop_mid_check();
    test_invalid_snapshot();
    test_capture_lost();
    test_master0();
    test_state();
    printf("test_policy: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
