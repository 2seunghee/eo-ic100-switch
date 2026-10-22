#pragma once
#include "common.h"

// UAC1 class requests (USB Audio Class 1.0, section 5.2).
#define UAC_SET_CUR 0x01
#define UAC_GET_CUR 0x81
#define UAC_GET_MIN 0x82
#define UAC_GET_MAX 0x83
#define UAC_GET_RES 0x84

#define UAC_FU_MUTE_CONTROL      0x01
#define UAC_FU_VOLUME_CONTROL    0x02
#define UAC_EP_SAMPLING_FREQ_CTL 0x01

// Endpoint sampling frequency (3-byte little endian), addressed to the endpoint.
Result uac_set_rate(UsbHsClientIfSession *s, u8 ep_addr, u32 rate);
Result uac_get_rate(UsbHsClientIfSession *s, u8 ep_addr, u32 *rate);

// Feature unit controls, addressed to the AudioControl interface (wIndex = unit << 8 | ac_if).
Result uac_fu_get_volume(UsbHsClientIfSession *s, u8 request, u8 unit, u8 ac_if, u8 channel, s16 *value);
Result uac_fu_set_volume(UsbHsClientIfSession *s, u8 unit, u8 ac_if, u8 channel, s16 value);
Result uac_fu_get_mute(UsbHsClientIfSession *s, u8 unit, u8 ac_if, u8 channel, bool *mute);
Result uac_fu_set_mute(UsbHsClientIfSession *s, u8 unit, u8 ac_if, u8 channel, bool mute);
