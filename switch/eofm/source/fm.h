#pragma once
#include "common.h"

// The EO-IC100's FM receiver (Bestechnic "BesFM"), driven by vendor control requests on EP0.
// Protocol from kjy00302/eo-ic100_radio (taken from Samsung's decompiled framework):
//   SET   bmRequestType 0xC0, bRequest 0xA1, wValue = command, wIndex = value, 1 byte back
//   GET   bmRequestType 0xC0, bRequest 0xA2, wValue = command, wIndex = 0,     2 bytes back
//   QUERY bmRequestType 0xC0, bRequest 0xA3, 12 bytes back: seek/tune results and RDS groups
//
// usb:hs only allows control transfers through an acquired interface. The audio sysmodule only
// borrows IF0 (AudioControl) while it reads the descriptors at plug-in, so this app takes IF0
// once the earphone has been plugged in for a few seconds and gives it back when it leaves.

enum {
    FM_SET_POWER = 0, FM_SET_BAND = 1, FM_SET_RSSI_TH = 2, FM_SET_SPACING = 3, FM_SET_MUTE = 4,
    FM_SET_VOLUME = 5, FM_SET_MONO = 6, FM_SET_SEEK_START = 7, FM_SET_SEEK_STOP = 8,
    FM_SET_CHANNEL = 9, FM_SET_RDS = 10, FM_SET_RECORDING = 14,
};
enum {
    FM_GET_POWER = 2, FM_GET_BAND = 3, FM_GET_RSSI = 4, FM_GET_SPACING = 5, FM_GET_MUTE = 6,
    FM_GET_MONO = 7, FM_GET_VOLUME = 8, FM_GET_RDS = 10, FM_GET_CHANNEL = 13, FM_GET_RECORDING = 17,
    FM_GET_PROTOCOL = 18,
};
enum { FM_SEEK_UP = 1, FM_SEEK_DOWN = 2 };

typedef enum { FMDEV_NONE, FMDEV_SETTLING, FMDEV_BUSY, FMDEV_OPEN } fmdev_state;

typedef struct {
    fmdev_state state;
    u16  pid;
    s32  if_id;          // usb:hs ID of IF0 (from the available list; QueryAll reports -1 once it is held)
    u32  device_id;      // which plug-in this is: usb:hs device number + insertion timestamp
    u64  inserted;
    u64  seen_ms;        // when this device instance was first seen
    UsbHsClientIfSession s;
    u32  failures;       // control transfers that failed in a row
} fm_dev;

// Called about once a second: notices the earphone coming and going, takes IF0 when it can.
void fm_poll_device(fm_dev *d);
void fm_close(fm_dev *d);

Result fm_set(fm_dev *d, u16 cmd, u16 value);
Result fm_get(fm_dev *d, u16 cmd, u8 out[2]);
Result fm_query(fm_dev *d, u8 out[12], u32 *got);
// Re-enumerates the earphone (usbHsIfResetDevice): it goes away and comes back as a new plug-in.
Result fm_reset(fm_dev *d);
