#pragma once
#include <switch.h>

// The EO-IC100's remote (IF3, interrupt EP 0x84, 3-byte reports: 01 01 00 = Vol+, 01 02 00 =
// Vol-, 01 04 00 = middle, 01 00 00 = released). hid never sees IF3/IF4 (mitm.c hides them), so
// this thread takes IF3 and turns the volume buttons into volume steps: the FM radio's volume
// while the earphone's receiver plays (eofm left it on), otherwise the console's output volume.
void remote_start(void);
