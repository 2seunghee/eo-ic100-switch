#include "remote.h"

#include "log.h"

#define REPEAT_DELAY_US 500000ULL
#define REPEAT_EVERY_US 150000ULL
#define MAX_ERRORS      20

static struct {
    UsbHsClientIfSession *s;
    remote_fn             cb;
    void                 *ctx;
    Thread                thread;
    bool                  thread_ok;
    volatile bool         stop;
} M;

static void remote_thread(void *arg) {
    struct usb_endpoint_descriptor *epd = NULL;
    for (int e = 0; e < 15 && !epd; e++) {
        struct usb_endpoint_descriptor *x = &M.s->inf.inf.input_endpoint_descs[e];
        if (x->bLength && (x->bmAttributes & USB_TRANSFER_TYPE_MASK) == USB_TRANSFER_TYPE_INTERRUPT) epd = x;
    }
    u8 ifnum = M.s->inf.inf.interface_desc.bInterfaceNumber;
    if (!epd) {
        log_printf("remote: IF%u has no interrupt IN endpoint", ifnum);
        return;
    }
    UsbHsClientEpSession ep;
    memset(&ep, 0, sizeof(ep));
    Result rc = usbHsIfOpenUsbEp(M.s, &ep, 1, epd->wMaxPacketSize, epd);
    log_printf("remote: IF%u EP %02x (%u-byte reports): open %s", ifnum, epd->bEndpointAddress, epd->wMaxPacketSize,
               rc_str(rc));
    if (R_FAILED(rc)) return;
    u8 *buf = memalign(0x1000, 0x1000);
    if (!buf) {
        usbHsEpClose(&ep);
        return;
    }

    bool pending = false;
    u32 errors = 0, n_reports = 0;
    u8 state = 0;
    u64 next_repeat = 0;
    while (!M.stop) {
        if (!pending) {
            memset(buf, 0, 0x1000);
            armDCacheFlush(buf, 0x1000);
            u32 xfer_id = 0;
            rc = usbHsEpPostBufferAsync(&ep, buf, epd->wMaxPacketSize, n_reports, &xfer_id);
            if (R_FAILED(rc)) {
                if (errors < 3) log_printf("remote: PostBufferAsync: %s", rc_str(rc));
                if (++errors > MAX_ERRORS) break;
                svcSleepThread(50000000ULL);
                continue;
            }
            pending = true;
        }
        u64 now = now_us();
        u64 wait_us = 100000;
        if (next_repeat) wait_us = next_repeat > now ? (next_repeat - now < wait_us ? next_repeat - now : wait_us) : 0;
        rc = eventWait(usbHsEpGetXferEvent(&ep), wait_us * 1000);
        now = now_us();
        if (R_SUCCEEDED(rc)) {
            eventClear(usbHsEpGetXferEvent(&ep));
            UsbHsXferReport rep;
            u32 count = 0;
            memset(&rep, 0, sizeof(rep));
            rc = usbHsEpGetXferReport(&ep, &rep, 1, &count);
            if (R_FAILED(rc)) {
                log_printf("remote: GetXferReport: %s", rc_str(rc));
                break;
            }
            if (count == 0) continue;
            pending = false;
            n_reports++;
            if (R_FAILED(rep.res)) {
                if (errors < 3) log_printf("remote: report failed: %s", rc_str(rep.res));
                if (++errors > MAX_ERRORS) break;
                svcSleepThread(10000000ULL);
                continue;
            }
            errors = 0;
            armDCacheFlush(buf, 0x1000);
            u32 len = rep.transferredSize < 16 ? rep.transferredSize : 16;
            char hex[64] = "";
            for (u32 k = 0; k < len; k++) snprintf(hex + 3 * k, sizeof(hex) - 3 * k, "%02x ", buf[k]);
            LOGD("remote: report %s", len ? hex : "(empty)");
            u8 now_state = state;
            if (len >= 2 && buf[0] == 0x01) now_state = buf[1];
            else if (len == 1) now_state = buf[0];
            u8 pressed = (u8)(now_state & ~state);
            state = now_state;
            for (unsigned bit = 1; bit < 0x100; bit <<= 1)
                if (pressed & bit) M.cb(M.ctx, (u8)bit, false);
            next_repeat = (state & (REMOTE_VOL_UP | REMOTE_VOL_DOWN)) ? now + REPEAT_DELAY_US : 0;
        }
        if (next_repeat && now >= next_repeat) {
            if (state & REMOTE_VOL_UP) M.cb(M.ctx, REMOTE_VOL_UP, true);
            if (state & REMOTE_VOL_DOWN) M.cb(M.ctx, REMOTE_VOL_DOWN, true);
            next_repeat = now + REPEAT_EVERY_US;
        }
    }
    usbHsEpClose(&ep);   // cancels the pending interrupt transfer
    free(buf);
    log_printf("remote: stopped after %u report(s)%s", n_reports, M.stop ? "" : " (endpoint errors)");
}

Result remote_start(UsbHsClientIfSession *hid, remote_fn cb, void *ctx) {
    if (M.thread_ok) return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    M.s = hid;
    M.cb = cb;
    M.ctx = ctx;
    M.stop = false;
    Result rc = thread_spawn(&M.thread, remote_thread, NULL, 0x3000, PRIO_REMOTE);
    M.thread_ok = R_SUCCEEDED(rc);
    return rc;
}

void remote_stop(void) {
    if (!M.thread_ok) return;
    M.stop = true;
    threadWaitForExit(&M.thread);
    threadClose(&M.thread);
    M.thread_ok = false;
}

bool remote_active(void) { return M.thread_ok; }
