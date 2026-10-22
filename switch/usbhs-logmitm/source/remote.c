#include "remote.h"

#include <malloc.h>

#include "common.h"
#include "log.h"

static struct {
    Thread       thread;
    UEvent       wake;
    volatile u32 pending;   // key bytes not handled yet: bit 0 Vol+, bit 1 Vol- (as in the report)
    u8          *buf;       // 0x1000-aligned, for control transfers
    bool         audctl_ok;
} RM;

static bool is_eo_ic100(u16 pid) { return pid == 0xA054 || pid == 0xA059 || pid == 0xA05B; }

// Vendor request to the FM receiver (the same as eofm's fm.c): SET 0xA1 / GET 0xA2, IN.
static Result fm_ctrl(UsbHsClientIfSession *s, u8 req, u16 value, u16 index, u16 len, u8 *out) {
    memset(RM.buf, 0, 0x1000);
    armDCacheFlush(RM.buf, 0x1000);
    u32 got = 0;
    Result rc = usbHsIfCtrlXfer(s, 0xC0, req, value, index, len, RM.buf, &got);
    armDCacheFlush(RM.buf, 0x1000);
    if (R_SUCCEEDED(rc) && out) memcpy(out, RM.buf, len);
    return rc;
}

// The FM receiver is reached through IF0, which the audio sysmodule only borrows at plug-in. While
// eofm runs it holds IF0; then this returns false and the console volume is stepped instead.
static bool fm_volume_step(int dir) {
    static UsbHsInterface list[8];
    UsbHsInterfaceFilter f;
    memset(&f, 0, sizeof(f));
    f.Flags = UsbHsInterfaceFilterFlags_idVendor;
    f.idVendor = 0x04E8;
    s32 n = 0;
    if (R_FAILED(usbHsQueryAvailableInterfaces(&f, list, sizeof(list), &n))) return false;
    int k = -1;
    for (s32 i = 0; i < n && k < 0; i++)
        if (is_eo_ic100(list[i].device_desc.idProduct) && list[i].inf.interface_desc.bInterfaceNumber == 0) k = (int)i;
    if (k < 0) return false;
    UsbHsClientIfSession s;
    if (R_FAILED(usbHsAcquireUsbIf(&s, &list[k]))) return false;
    bool done = false;
    u8 p[2] = {0, 0}, v[2] = {0, 0};
    if (R_SUCCEEDED(fm_ctrl(&s, 0xA2, 2, 0, 2, p)) && p[0] && R_SUCCEEDED(fm_ctrl(&s, 0xA2, 8, 0, 2, v))) {
        int nv = v[0] + dir;
        nv = nv < 0 ? 0 : nv > 15 ? 15 : nv;
        Result rc = fm_ctrl(&s, 0xA1, 5, (u16)nv, 1, NULL);
        log_printf("remote: radio volume %u -> %d (%s)", v[0], nv, rc_str(rc));
        done = true;
    }
    usbHsIfClose(&s);
    return done;
}

static void console_volume_step(int dir) {
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

static void remote_thread(void *arg) {
    Result rc = usbHsInitialize();
    log_printf("remote: usb:hs %s", rc_str(rc));
    RM.audctl_ok = R_SUCCEEDED(audctlInitialize());
    for (;;) {
        waitSingle(waiterForUEvent(&RM.wake), UINT64_MAX);
        u32 keys = __atomic_exchange_n(&RM.pending, 0, __ATOMIC_ACQ_REL);
        int dir = (keys & 0x01) ? 1 : (keys & 0x02) ? -1 : 0;
        if (!dir) continue;
        if (R_FAILED(rc) || !fm_volume_step(dir)) console_volume_step(dir);
    }
}

void remote_key(u8 keys) {
    if (!(keys & 0x03)) return;   // release, middle button
    __atomic_or_fetch(&RM.pending, keys & 0x03, __ATOMIC_ACQ_REL);
    ueventSignal(&RM.wake);
}

void remote_start(void) {
    RM.buf = memalign(0x1000, 0x1000);
    if (!RM.buf) return;
    ueventCreate(&RM.wake, true);
    Result rc = thread_spawn(&RM.thread, remote_thread, NULL, 0x4000, PRIO_MAIN);
    log_printf("remote: thread %s", rc_str(rc));
}
