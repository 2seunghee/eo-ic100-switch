#pragma once
#include <stddef.h>
#include <stdint.h>

// Names for what crosses a usb:hs session. Pure functions: also built into the host tests.

enum { OBJ_ROOT = 1, OBJ_IF = 2, OBJ_EP = 3 };

// Command names for [2.0.0+] (libnx usbhs.c / switchbrew). NULL if unknown.
const char *usbhs_cmd_name(int kind, uint32_t cmd);
// Control commands (CMIF type 5/7): ConvertCurrentObjectToDomain, CloneCurrentObject, ...
const char *cmif_control_name(uint32_t cmd);

// Commands a streaming client sends many times a second: logged in detail only for the first ones.
int usbhs_cmd_is_hot(int kind, uint32_t cmd);

// "IN  std  dev GET_DESCRIPTOR CONFIG idx 0 len 9" and the like, from an 8-byte setup packet.
// Class requests of USB audio (UAC1) are decoded with entity, control selector and channel.
void usb_setup_str(char *out, size_t size, const uint8_t setup[8]);

// One line for a standard descriptor header at p (type and the fields that matter for audio).
void usb_desc_str(char *out, size_t size, const uint8_t *p, size_t len);
