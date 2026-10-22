// What eoaudio does with the console's own output while the earphone plays. No libnx dependency:
// unit-tested on the host (tests/test_policy.c). The main loop feeds it
//   pol_poll()      every 250 ms: audctl snapshot + earphone stream state
//   pol_interval()  every recorder interval (1 s): capture level
// and it acts through callbacks:
//   - silences the active target (speaker or TV) once the earphone stream has run for a second.
//     A method nobody has verified yet is applied only during audible game sound, and the capture
//     level is compared before and after: if the recording goes silent, the method is undone and
//     the level checked again. Only a recording that comes back twice marks the method "silenced"
//     (remembered per target and firmware); then the next method is tried (auto: mute, then volume 0,
//     then leave the speaker on);
//   - steps aside while another output is active (3.5 mm jack, Bluetooth, a USB headset Nintendo's
//     driver recognised): nothing silenced, earphone stream held, resumed when speaker/TV is back;
//   - while silenced with mute, follows the console volume buttons/slider as relative volume steps;
//   - re-applies silencing the system undid (mute: at most 3 times per session).
// Nothing is ever left silenced when the stream stops (pol_stop / pol_poll with streaming off) or
// when the recorder stops delivering (the earphone would be silent too).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "state.h"

enum { POL_T_INVALID = 0, POL_T_SPEAKER = 1, POL_T_HEADPHONE = 2, POL_T_TV = 3, POL_T_USB = 4, POL_T_BLUETOOTH = 5 };
enum { POL_M_NONE = 0, POL_M_MUTE = 1, POL_M_VOLUME0 = 2, POL_M_MASTER0 = 3 };
enum { POL_SPK_OFF = 0, POL_SPK_AUTO = 1, POL_SPK_MUTE = 2, POL_SPK_VOLUME0 = 3, POL_SPK_MASTER0 = 4 };   // = eo_speaker_mode

typedef struct {
    bool (*silence)(void *ctx, int target, int method);   // save the current state, then apply
    void (*reassert)(void *ctx);                           // apply the same method again (state kept)
    void (*restore)(void *ctx);                            // put back what silence() saved
    void (*hold)(void *ctx, bool hold, const char *why);   // stop / allow the earphone stream
    void (*volume_steps)(void *ctx, int steps, const char *why);
    void (*verdict)(void *ctx, int target, int method, eo_verdict v);   // remember in state.ini
    void (*log)(void *ctx, const char *msg);
    void *ctx;
} pol_env;

typedef struct {
    bool    valid;        // audctl answered; nothing changes otherwise
    bool    streaming;    // the earphone stream is running
    bool    stream_ok;    // ... and has delivered audio for about a second
    bool    capture_ok;   // the recorder delivered frames within the last ~2.5 s (silence counts)
    int     target;       // active output target (POL_T_*)
    int32_t volume;       // of that target
    int32_t volume_min;
    bool    mute;         // of that target
    bool    have_master;
    float   master;       // system output master volume
} pol_input;

typedef enum {
    PH_OFF = 0,      // earphone not streaming
    PH_ASIDE,        // another output is active
    PH_WAIT,         // streaming; waiting for a healthy stream / audible sound to apply a method
    PH_CHECK_SKIP,   // method applied; this interval straddles the change
    PH_CHECK,        // measuring the recording with the method applied
    PH_UNDO_SKIP,    // undone after the recording went quiet
    PH_UNDO_CHECK,   // measuring the recording after the undo
    PH_ACTIVE,       // silenced
    PH_NONE,         // speaker stays on for this session
} pol_phase;

typedef struct {
    pol_env    env;
    int        speaker_mode;
    bool       follow;
    eo_verdict verdict[ST_TARGETS][ST_METHODS];

    pol_phase  phase;
    int        target;          // target of the current session
    int        cand;            // method being tried
    int        applied;         // method currently applied by us (POL_M_NONE if none)
    float      before_db, after_db;
    int        strikes[ST_METHODS];   // recordings that came back after undoing the method (session)
    unsigned   failed_mask;     // methods audctl refused this session
    int        reasserts, inconclusive;
    bool       have_volume;
    int32_t    last_volume;
    bool       none_logged;
    bool       capture_ok;      // from the newest pol_poll
} pol;

void pol_init(pol *p, const pol_env *env, int speaker_mode, bool follow, const eo_state *st);
void pol_poll(pol *p, const pol_input *in);
void pol_interval(pol *p, float rms_db, bool digital_silence);
void pol_stop(pol *p, const char *why);   // earphone stream stopped: put everything back now
const char *pol_phase_name(pol_phase ph);
const char *pol_target_name(int target);
const char *pol_method_name(int method);
