#pragma once
#include "probe.h"

// Prints interrupt-IN reports from every HID interface of `d` we hold, until B is pressed.
// On the EO-IC100: IF3 (EP 0x84, 3-byte reports) = remote buttons, IF4 (EP 0x85) = FM radio events.
void hidmon_run(pr_device *d, PadState *pad);
