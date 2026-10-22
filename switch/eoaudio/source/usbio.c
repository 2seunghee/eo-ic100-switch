#include "usbio.h"

Result usbio_ctrl(UsbHsClientIfSession *s, u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex,
                  u16 wLength, void *data, u32 *transferred) {
    size_t size = ALIGN_UP((size_t)(wLength ? wLength : 1), 0x1000);
    u8 *buf = memalign(0x1000, size);
    if (!buf) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    memset(buf, 0, size);
    bool in = (bmRequestType & USB_ENDPOINT_IN) != 0;
    if (!in && wLength && data) memcpy(buf, data, wLength);
    armDCacheFlush(buf, size);

    u32 got = 0;
    Result rc = usbHsIfCtrlXfer(s, bmRequestType, bRequest, wValue, wIndex, wLength, buf, &got);
    if (in) {
        armDCacheFlush(buf, size);
        if (R_SUCCEEDED(rc) && data) memcpy(data, buf, got < wLength ? got : wLength);
    }
    if (transferred) *transferred = got;
    free(buf);
    return rc;
}

Result usbio_get_descriptor(UsbHsClientIfSession *s, u8 recipient, u8 type, u8 index, u16 wIndex,
                            void *out, u16 len, u32 *got) {
    return usbio_ctrl(s, USB_ENDPOINT_IN | USB_REQUEST_TYPE_STANDARD | recipient, USB_REQUEST_GET_DESCRIPTOR,
                      (u16)((type << 8) | index), wIndex, len, out, got);
}

Result usbio_get_string(UsbHsClientIfSession *s, u8 index, u16 langid, char *out, size_t out_size) {
    u8 raw[256];
    u32 got = 0;
    out[0] = 0;
    Result rc = usbio_get_descriptor(s, USB_RECIPIENT_DEVICE, USB_DT_STRING, index, langid, raw, sizeof(raw) - 1, &got);
    if (R_FAILED(rc)) return rc;
    size_t blen = raw[0] < got ? raw[0] : got;
    if (blen < 2 || raw[1] != USB_DT_STRING) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    size_t n = 0, chars = (blen - 2) / 2;
    for (size_t i = 0; i < chars && n + 1 < out_size; i++) {
        u16 ch = (u16)(raw[2 + 2 * i] | (raw[3 + 2 * i] << 8));
        out[n++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
    }
    out[n] = 0;
    return 0;
}
