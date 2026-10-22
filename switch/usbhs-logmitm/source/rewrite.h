#pragma once
#include <stddef.h>
#include <stdint.h>

// Approach C: rewrites of the EO-IC100 configuration descriptor, made in place before the audio
// sysmodule parses it. Every rule keeps every descriptor's length, so wTotalLength, the interface
// layout and all offsets stay as the device reports them. Pure function: also in the host tests.

enum {
    RW_OT_HEADPHONES = 1 << 0,   // H2: output terminal 0x04xx (bidirectional, "Headset") -> 0x0302 Headphones
    RW_IT_MIC        = 1 << 1,   // H2: input terminal 0x04xx -> 0x0201 Microphone
    RW_SINGLE_RATE   = 1 << 2,   // H3: a rate list that holds 48 kHz becomes 48 kHz in every slot
    RW_ALT1_ONLY     = 1 << 3,   // audio streaming alt settings >= 2 (24/32-bit) turn into vendor class
    RW_MPS48         = 1 << 4,   // iso endpoints of AS alt settings: wMaxPacketSize = 1 ms at 48 kHz
};
#define RW_TERMINAL (RW_OT_HEADPHONES | RW_IT_MIC)

#define RW_MAX_NOTES 16

typedef struct {
    int  n;
    char text[RW_MAX_NOTES][96];
} rw_notes;

// Parses "terminal,single_rate,..." ("none" = 0). Returns -1 and points *bad at an unknown word.
int rewrite_parse(const char *list, unsigned *rules, const char **bad);
void rewrite_names(unsigned rules, char *out, size_t size);
// Applies `rules` to the config descriptor d[0..n). Returns the number of changed fields.
int rewrite_config(uint8_t *d, size_t n, unsigned rules, rw_notes *notes);
