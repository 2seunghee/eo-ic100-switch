#include "uac.h"

#include "usbio.h"

#define RT_CLASS_EP_OUT  (USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_ENDPOINT)   // 0x22
#define RT_CLASS_EP_IN   (USB_ENDPOINT_IN | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_ENDPOINT)    // 0xA2
#define RT_CLASS_IF_OUT  (USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE)  // 0x21
#define RT_CLASS_IF_IN   (USB_ENDPOINT_IN | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE)   // 0xA1

Result uac_set_rate(UsbHsClientIfSession *s, u8 ep_addr, u32 rate) {
    u8 data[3] = {(u8)rate, (u8)(rate >> 8), (u8)(rate >> 16)};
    u32 got;
    return usbio_ctrl(s, RT_CLASS_EP_OUT, UAC_SET_CUR, UAC_EP_SAMPLING_FREQ_CTL << 8, ep_addr, sizeof(data), data, &got);
}

Result uac_get_rate(UsbHsClientIfSession *s, u8 ep_addr, u32 *rate) {
    u8 data[3] = {0};
    u32 got = 0;
    Result rc = usbio_ctrl(s, RT_CLASS_EP_IN, UAC_GET_CUR, UAC_EP_SAMPLING_FREQ_CTL << 8, ep_addr, sizeof(data), data, &got);
    *rate = (u32)data[0] | ((u32)data[1] << 8) | ((u32)data[2] << 16);
    return rc;
}

Result uac_fu_get_volume(UsbHsClientIfSession *s, u8 request, u8 unit, u8 ac_if, u8 channel, s16 *value) {
    u8 data[2] = {0};
    u32 got = 0;
    Result rc = usbio_ctrl(s, RT_CLASS_IF_IN, request, (u16)(UAC_FU_VOLUME_CONTROL << 8 | channel),
                           (u16)(unit << 8 | ac_if), sizeof(data), data, &got);
    *value = (s16)(data[0] | (data[1] << 8));
    return rc;
}

Result uac_fu_set_volume(UsbHsClientIfSession *s, u8 unit, u8 ac_if, u8 channel, s16 value) {
    u8 data[2] = {(u8)value, (u8)((u16)value >> 8)};
    u32 got;
    return usbio_ctrl(s, RT_CLASS_IF_OUT, UAC_SET_CUR, (u16)(UAC_FU_VOLUME_CONTROL << 8 | channel),
                      (u16)(unit << 8 | ac_if), sizeof(data), data, &got);
}

Result uac_fu_get_mute(UsbHsClientIfSession *s, u8 unit, u8 ac_if, u8 channel, bool *mute) {
    u8 data[1] = {0};
    u32 got = 0;
    Result rc = usbio_ctrl(s, RT_CLASS_IF_IN, UAC_GET_CUR, (u16)(UAC_FU_MUTE_CONTROL << 8 | channel),
                           (u16)(unit << 8 | ac_if), sizeof(data), data, &got);
    *mute = data[0] != 0;
    return rc;
}

Result uac_fu_set_mute(UsbHsClientIfSession *s, u8 unit, u8 ac_if, u8 channel, bool mute) {
    u8 data[1] = {mute ? 1 : 0};
    u32 got;
    return usbio_ctrl(s, RT_CLASS_IF_OUT, UAC_SET_CUR, (u16)(UAC_FU_MUTE_CONTROL << 8 | channel),
                      (u16)(unit << 8 | ac_if), sizeof(data), data, &got);
}
