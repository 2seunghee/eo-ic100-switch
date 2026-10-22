#include "hidmon.h"

#include "log.h"

#define HM_MAX 4

typedef struct {
    UsbHsClientEpSession ep;
    u8  *buf;
    u16  len;
    u8   ifnum, addr;
    bool pending;
    u32  count;
} hm_ep;

static bool post(hm_ep *m) {
    u32 xfer_id = 0;
    memset(m->buf, 0, 0x1000);
    armDCacheFlush(m->buf, 0x1000);
    Result rc = usbHsEpPostBufferAsync(&m->ep, m->buf, m->len, m->count, &xfer_id);
    m->pending = R_SUCCEEDED(rc);
    if (R_FAILED(rc)) log_both("  IF%u EP %02x PostBufferAsync: %s\n", m->ifnum, m->addr, rc_str(rc));
    return m->pending;
}

void hidmon_run(pr_device *d, PadState *pad) {
    static hm_ep mons[HM_MAX];
    int n = 0;
    if (!d) return;
    log_both(C_CYAN "== HID monitor ==" C_RESET "\n");
    for (int j = 0; j < d->if_count && n < HM_MAX; j++) {
        const struct usb_interface_descriptor *id = &d->ifs[j].info.inf.interface_desc;
        UsbHsClientIfSession *s = pr_session_get(d, id->bInterfaceNumber);
        if (id->bInterfaceClass != USB_CLASS_HID) continue;
        if (!s) {
            log_both("  IF%u: not ours (state %d), skipped\n", id->bInterfaceNumber, d->ifs[j].state);
            continue;
        }
        struct usb_endpoint_descriptor *epd = NULL;
        for (int e = 0; e < 15 && !epd; e++) {
            struct usb_endpoint_descriptor *x = &s->inf.inf.input_endpoint_descs[e];
            if (x->bLength && (x->bmAttributes & USB_TRANSFER_TYPE_MASK) == USB_TRANSFER_TYPE_INTERRUPT) epd = x;
        }
        if (!epd) continue;
        hm_ep *m = &mons[n];
        memset(m, 0, sizeof(*m));
        Result rc = usbHsIfOpenUsbEp(s, &m->ep, 1, epd->wMaxPacketSize, epd);
        log_both("  IF%u EP %02x (%u bytes): open %s\n", id->bInterfaceNumber, epd->bEndpointAddress,
                 epd->wMaxPacketSize, rc_str(rc));
        if (R_FAILED(rc)) continue;
        m->buf = memalign(0x1000, 0x1000);
        if (!m->buf) {
            usbHsEpClose(&m->ep);
            continue;
        }
        m->len = epd->wMaxPacketSize;
        m->ifnum = id->bInterfaceNumber;
        m->addr = epd->bEndpointAddress;
        if (post(m)) n++;
        else {
            usbHsEpClose(&m->ep);
            free(m->buf);
        }
    }
    if (n == 0) {
        log_both(C_YELLOW "  No HID interface of this device is ours (A to acquire, R to race).\n" C_RESET);
        return;
    }
    log_both("  Press the earphone's buttons. B = stop.\n");
    consoleUpdate(NULL);

    while (appletMainLoop()) {
        padUpdate(pad);
        if (padGetButtonsDown(pad) & HidNpadButton_B) break;
        for (int i = 0; i < n; i++) {
            hm_ep *m = &mons[i];
            if (!m->pending || R_FAILED(eventWait(usbHsEpGetXferEvent(&m->ep), 0))) continue;
            eventClear(usbHsEpGetXferEvent(&m->ep));
            UsbHsXferReport rep;
            u32 count = 0;
            memset(&rep, 0, sizeof(rep));
            Result rc = usbHsEpGetXferReport(&m->ep, &rep, 1, &count);
            if (R_FAILED(rc) || count == 0) continue;
            armDCacheFlush(m->buf, 0x1000);
            char hex[64] = "";
            size_t len = rep.transferredSize < 16 ? rep.transferredSize : 16;
            for (size_t k = 0; k < len; k++) snprintf(hex + 3 * k, sizeof(hex) - 3 * k, "%02x ", m->buf[k]);
            log_both("  [%llu ms] IF%u EP %02x #%u res %s: %s\n", (unsigned long long)(now_us() / 1000), m->ifnum,
                     m->addr, m->count, rc_str(rep.res), hex);
            m->count++;
            post(m);
        }
        consoleUpdate(NULL);
    }
    for (int i = 0; i < n; i++) {
        usbHsEpClose(&mons[i].ep);   // cancels the pending interrupt transfer
        free(mons[i].buf);
    }
    log_both("  HID monitor stopped.\n");
}
