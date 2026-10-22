#pragma once
#include "common.h"

// The earphone's remote: interrupt-IN reports on the first HID interface (EO-IC100: IF3, EP 0x84).
// Seen on the user's a05b: 01 01 00 = Vol+, 01 02 00 = Vol-, 01 00 00 = released.
#define REMOTE_VOL_UP   0x01
#define REMOTE_VOL_DOWN 0x02
#define REMOTE_PLAY     0x04   // expected for the middle button, not seen yet

// Called on the remote thread for every press, and again every 150 ms while Vol+/Vol- is held
// (after 500 ms). `bit` is one REMOTE_* bit.
typedef void (*remote_fn)(void *ctx, u8 bit, bool repeat);

Result remote_start(UsbHsClientIfSession *hid, remote_fn cb, void *ctx);
void   remote_stop(void);
bool   remote_active(void);
