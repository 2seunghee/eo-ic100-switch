#pragma once
#include "common.h"

// Speaker mode: in the receiver's recording mode the radio arrives at the console as the USB
// microphone (IF1). speaker.c reads it there itself and plays it with audout; usbhs-logmitm's
// deny_mic keeps the audio sysmodule off that interface (see speaker.c).

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
void spk_set_gain(int vol15);   // the app's volume 0..15 scales the speaker output
