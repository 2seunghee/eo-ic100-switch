#pragma once
#include "common.h"

// Speaker mode. In the earphone's recording mode the radio arrives at the console as the USB
// microphone (IF1). The audio sysmodule owns that interface, so the sound is taken from the system
// audio input (audin) and played again with audout, after the system output has been moved to the
// console speaker (audctl). Everything that could go wrong is logged: whether audin lists the USB
// microphone, and whether audctl may move the output while a USB output device is attached.

typedef struct {
    bool     running;
    bool     moved_output;   // audctl moved the system output to the speaker
    AudioTarget before;      // active output target before the move
    AudioTarget now;         // active output target after the move attempt
    u32      in_rate, in_channels;
    u64      frames;         // captured and played
    u32      dropped;        // captured buffers with no free output buffer
    int      peak_db;        // last second's peak level, dBFS (-99 = silence)
    char     error[96];      // why speaker mode is not running, or ""
} spk_status;

bool spk_start(void);
void spk_stop(void);
void spk_get(spk_status *out);
