#pragma once
#include "common.h"
#include "config.h"

// Presses one controller button for ~80 ms through a hid:dbg virtual device (HDLS). The device is a
// right Joy-Con merged onto the attached rail (npadInterfaceType Rail), so the press shows up as if
// it came from the console's own Joy-Con and no extra controller appears; it is attached only for
// the press and detached again. Blocking (~200 ms): call it from the remote thread.
Result media_press(eo_button button);
