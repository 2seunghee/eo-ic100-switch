#pragma once
#include "common.h"

// Control transfer through an acquired interface session. Handles the 0x1000-aligned bounce
// buffer and cache maintenance; `data` may be any pointer. For IN requests the reply is
// copied to `data` (up to wLength bytes).
Result usbio_ctrl(UsbHsClientIfSession *s, u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex,
                  u16 wLength, void *data, u32 *transferred);

// GET_DESCRIPTOR. recipient: 0 = device, 1 = interface (HID report descriptors).
Result usbio_get_descriptor(UsbHsClientIfSession *s, u8 recipient, u8 type, u8 index, u16 wIndex,
                            void *out, u16 len, u32 *got);

// String descriptor as ASCII (non-ASCII -> '?'). Empty string on failure.
Result usbio_get_string(UsbHsClientIfSession *s, u8 index, u16 langid, char *out, size_t out_size);
