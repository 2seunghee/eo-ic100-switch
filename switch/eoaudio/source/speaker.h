#pragma once
#include "common.h"

// Console output state through audctl, and silencing one output target (speaker or TV) while the
// earphone plays. Before anything is changed the previous state is saved to
// APP_DIR/speaker_restore.txt: spk_init() puts it back after a crash or a pm kill, and the crash
// handler calls spk_emergency_restore(), so the console is never left silenced.

typedef struct {
    bool        ok;
    AudioTarget target;        // active output target (speaker in handheld mode)
    s32         volume;        // of that target
    bool        mute;
    bool        have_master;
    float       master;        // system output master volume
} spk_state;

typedef enum {
    SPK_M_NONE = 0,
    SPK_M_TARGET_MUTE,         // SetTargetMute(target, true)          (= POL_M_MUTE)
    SPK_M_TARGET_VOLUME0,      // SetTargetVolume(target, min)         (= POL_M_VOLUME0)
    SPK_M_MASTER0,             // SetSystemOutputMasterVolume(0), capture-mode test only
} spk_method;

void        spk_init(void);
void        spk_exit(void);
bool        spk_read(spk_state *s);                       // the active target and its volume/mute
void        spk_volume_range(s32 *min, s32 *max);
Result      spk_silence(AudioTarget target, spk_method m);   // saves that target's state, then applies m
Result      spk_reassert(void);                           // applies the current method again
Result      spk_step_volume(AudioTarget target, int dir, s32 *out);   // console volume +-1 level
void        spk_restore(void);                            // no-op if nothing is silenced
void        spk_emergency_restore(void);                  // exception handler: no logging, no locks of ours
spk_method  spk_silenced(void);
AudioTarget spk_silenced_target(void);
const char *spk_method_name(spk_method m);
const char *spk_target_name(AudioTarget t);
