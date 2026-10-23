#pragma once
#include <stdbool.h>
#include <stddef.h>

// The radio on the console speaker is played by usbhs-logmitm (0.3.6, deny_mic=1), so it keeps
// playing after the app exits. The app (or the overlay) puts the receiver in recording mode and asks
// for it through a file; the sysmodule answers through another one.

typedef enum { SPK_NO_SERVICE, SPK_OFF, SPK_RUNNING, SPK_ERROR } spk_state;

bool spkctl_request(bool on, int gain);   // gain = the app's volume, 0..15
spk_state spkctl_status(char *why, size_t size);
