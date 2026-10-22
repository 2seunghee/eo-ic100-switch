#pragma once

// Read-only system state relevant to "is the earphone recognised?":
// firmware, the audio routing the OS reports (auddev / audctl), and the USB-C port role (usb:pm).
void sysinfo_report(void);
