#pragma once
#include <switch.h>

// The EO-IC100's remote (IF3, interrupt EP 0x84, 3-byte reports: 01 01 00 = Vol+, 01 02 00 =
// Vol-, 01 04 00 = middle, 01 00 00 = released).
//
// hid keeps IF3: with IF3/IF4 hidden from hid (0.3.0/0.3.1), the audio sysmodule stopped taking the
// earphone as its output. So mitm.c only watches hid's own reports on that endpoint and passes the
// keys here; a thread of this file turns Vol+/- into volume steps: the FM radio's volume while the
// earphone's receiver plays, otherwise the console's output volume.
void remote_start(void);
// From the mitm thread: the report's key byte. Never blocks.
void remote_key(u8 keys);
