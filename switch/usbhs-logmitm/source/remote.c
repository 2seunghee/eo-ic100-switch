#include "remote.h"

#include <malloc.h>

#include "common.h"
#include "log.h"

#define SETTLE_MS  4000   // after the earphone appears: let it finish its re-enumerations
#define WAIT_NS    1000000000ULL

static struct {
    Thread thread;
    u8    *buf;           // 0x1000-aligned: interrupt reports and control transfers
    bool   audctl_ok;
} RM;

static bool is_eo_ic100(u16 pid) { return pid == 0xA054 || pid == 0xA059 || pid == 0xA05B; }

// IF3 of an EO-IC100 that nobody holds.
static bool find_if3(UsbHsInterface *out) {
    static UsbHsInterface list[8];
    UsbHsInterfaceFilter f;
    memset(&f, 0, sizeof(f));
    f.Flags = UsbHsInterfaceFilterFlags_idVendor;
    f.idVendor = 0x04E8;
    s32 n = 0;
    if (R_FAILED(usbHsQueryAvailableInterfaces(&f, list, sizeof(list), &n))) return false;
    for (s32 i = 0; i < n; i++)
        if (is_eo_ic100(list[i].device_desc.idProduct) && list[i].inf.interface_desc.bInterfaceNumber == 3) {
            *out = list[i];
            return true;
        }
    return false;
}

// Vendor request to the FM receiver (the same as eofm's fm.c): SET 0xA1 / GET 0xA2, IN, through
// any acquired interface of the device.
static Result fm_ctrl(UsbHsClientIfSession *s, u8 req, u16 value, u16 index, u16 len, u8 *out) {
    memset(RM.buf, 0, 0x1000);
    armDCacheFlush(RM.buf, 0x1000);
    u32 got = 0;
    Result rc = usbHsIfCtrlXfer(s, 0xC0, req, value, index, len, RM.buf, &got);
    armDCacheFlush(RM.buf, 0x1000);
    if (R_SUCCEEDED(rc) && out) memcpy(out, RM.buf, len);
    return rc;
}

static void volume_step(UsbHsClientIfSession *s, int dir) {
    u8 p[2] = {0, 0}, v[2] = {0, 0};
    // FM radio playing in the earphone (power mode): its own volume, 0..15
    if (R_SUCCEEDED(fm_ctrl(s, 0xA2, 2, 0, 2, p)) && p[0] && R_SUCCEEDED(fm_ctrl(s, 0xA2, 8, 0, 2, v))) {
        int nv = v[0] + dir;
        nv = nv < 0 ? 0 : nv > 15 ? 15 : nv;
        Result rc = fm_ctrl(s, 0xA1, 5, (u16)nv, 1, NULL);
        log_printf("remote: radio volume %u -> %d (%s)", v[0], nv, rc_str(rc));
        return;
    }
    if (!RM.audctl_ok) return;
    AudioTarget t = AudioTarget_Invalid;
    s32 cur = 0, lo = 0, hi = 15;
    audctlGetActiveOutputTarget(&t);
    audctlGetTargetVolumeMin(&lo);
    audctlGetTargetVolumeMax(&hi);
    if (R_FAILED(audctlGetTargetVolume(&cur, t))) return;
    s32 nv = cur + dir;
    nv = nv < lo ? lo : nv > hi ? hi : nv;
    Result rc = audctlSetTargetVolume(t, nv);
    log_printf("remote: console volume (target %d) %d -> %d (%s)", t, cur, nv, rc_str(rc));
}

// Holds IF3 and reads reports until the earphone goes away.
static void serve(UsbHsInterface *it) {
    UsbHsClientIfSession s;
    Result rc = usbHsAcquireUsbIf(&s, it);
    log_printf("remote: acquire IF3 (id %d): %s", it->inf.ID, rc_str(rc));
    if (R_FAILED(rc)) return;
    struct usb_endpoint_descriptor *epd = NULL;
    for (int e = 0; e < 15 && !epd; e++) {
        struct usb_endpoint_descriptor *x = &s.inf.inf.input_endpoint_descs[e];
        if (x->bLength && (x->bmAttributes & USB_TRANSFER_TYPE_MASK) == USB_TRANSFER_TYPE_INTERRUPT) epd = x;
    }
    UsbHsClientEpSession ep;
    rc = epd ? usbHsIfOpenUsbEp(&s, &ep, 1, epd->wMaxPacketSize, epd) : MAKERESULT(Module_Libnx, LibnxError_NotFound);
    log_printf("remote: open EP %02x: %s", epd ? epd->bEndpointAddress : 0, rc_str(rc));
    if (R_FAILED(rc)) {
        usbHsIfClose(&s);
        return;
    }
    u16 len = epd->wMaxPacketSize;
    u8 *rep = RM.buf + 0x800;   // the report half of the buffer; control transfers use the start
    u32 seq = 0, failures = 0;
    for (;;) {
        memset(rep, 0, len);
        armDCacheFlush(RM.buf, 0x1000);
        u32 xfer = 0;
        rc = usbHsEpPostBufferAsync(&ep, rep, len, seq++, &xfer);
        if (R_FAILED(rc)) break;
        // Wait for a report; every second check that the earphone is still there.
        for (;;) {
            if (R_SUCCEEDED(eventWait(usbHsEpGetXferEvent(&ep), WAIT_NS))) break;
            static UsbHsInterface held[8];   // what this process holds: IF3 drops out when unplugged
            s32 n = 0;
            bool present = false;
            if (R_SUCCEEDED(usbHsQueryAcquiredInterfaces(held, sizeof(held), &n)))
                for (s32 i = 0; i < n; i++) present |= held[i].inf.ID == it->inf.ID;
            if (!present) goto gone;
        }
        eventClear(usbHsEpGetXferEvent(&ep));
        UsbHsXferReport r;
        u32 count = 0;
        memset(&r, 0, sizeof(r));
        if (R_FAILED(usbHsEpGetXferReport(&ep, &r, 1, &count)) || !count) continue;
        if (R_FAILED(r.res)) {
            if (++failures > 5) break;   // unplugged: transfers fail
            continue;
        }
        failures = 0;
        armDCacheFlush(RM.buf, 0x1000);
        if (r.transferredSize < 2) continue;
        u8 keys = rep[1];
        log_printf("remote: report %02x %02x %02x", rep[0], rep[1], len > 2 ? rep[2] : 0);
        if (keys & 0x01) volume_step(&s, +1);
        else if (keys & 0x02) volume_step(&s, -1);
    }
gone:
    log_printf("remote: earphone gone (%s)", rc_str(rc));
    usbHsEpClose(&ep);
    usbHsIfClose(&s);
}

static void remote_thread(void *arg) {
    Result rc = usbHsInitialize();
    log_printf("remote: usb:hs %s", rc_str(rc));
    if (R_FAILED(rc)) return;
    RM.audctl_ok = R_SUCCEEDED(audctlInitialize());
    u64 seen = 0;
    for (;;) {
        UsbHsInterface it;
        if (!find_if3(&it)) {
            seen = 0;
            svcSleepThread(1000000000ULL);
            continue;
        }
        if (!seen) seen = now_ms();
        if (now_ms() - seen < SETTLE_MS) {
            svcSleepThread(500000000ULL);
            continue;
        }
        serve(&it);
        seen = 0;
        svcSleepThread(1000000000ULL);
    }
}

void remote_start(void) {
    RM.buf = memalign(0x1000, 0x1000);
    if (!RM.buf) return;
    Result rc = thread_spawn(&RM.thread, remote_thread, NULL, 0x4000, PRIO_MAIN);
    log_printf("remote: thread %s", rc_str(rc));
}
