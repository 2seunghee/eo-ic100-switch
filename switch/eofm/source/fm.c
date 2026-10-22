#include "fm.h"

#include "log.h"
#include "usbio.h"

#define SETTLE_MS 6000   // a plug-in re-enumerates ~3 times within ~5 s, and audio reads IF0 each time

static bool is_eo_ic100(u16 pid) { return pid == 0xA054 || pid == 0xA059 || pid == 0xA05B; }

static UsbHsInterface g_list[16];

static int query(bool available) {
    UsbHsInterfaceFilter f;
    memset(&f, 0, sizeof(f));
    f.Flags = UsbHsInterfaceFilterFlags_idVendor;
    f.idVendor = TARGET_VID;
    s32 n = 0;
    Result rc = available ? usbHsQueryAvailableInterfaces(&f, g_list, sizeof(g_list), &n)
                          : usbHsQueryAllInterfaces(&f, g_list, sizeof(g_list), &n);
    return R_SUCCEEDED(rc) ? n : 0;
}

// IF0 of an EO-IC100 in g_list, or -1.
static int find_if0(int n) {
    for (int i = 0; i < n; i++) {
        UsbHsInterface *it = &g_list[i];
        if (is_eo_ic100(it->device_desc.idProduct) && it->inf.interface_desc.bInterfaceNumber == 0) return i;
    }
    return -1;
}

void fm_close(fm_dev *d) {
    if (d->state == FMDEV_OPEN) {
        usbHsIfClose(&d->s);
        log_file("fm: released IF0 (id %d)\n", d->if_id);
    }
    d->state = FMDEV_NONE;
    d->failures = 0;
}

void fm_poll_device(fm_dev *d) {
    int n = query(false);
    int k = find_if0(n);
    if (k < 0) {   // gone
        if (d->state != FMDEV_NONE) log_file("fm: earphone gone\n");
        fm_close(d);
        return;
    }
    s32 id = g_list[k].inf.ID;
    if (d->state == FMDEV_OPEN) {
        if (id == d->if_id && d->failures < 3) return;
        log_file("fm: device changed or not answering (id %d -> %d, %u failures)\n", d->if_id, id, d->failures);
        fm_close(d);
    }
    if (d->state == FMDEV_NONE || id != d->if_id) {   // a new instance: let the plug-in settle first
        d->state = FMDEV_SETTLING;
        d->if_id = id;
        d->pid = g_list[k].device_desc.idProduct;
        d->seen_ms = now_ms();
        log_file("fm: earphone 04e8:%04x seen (IF0 id %d), waiting %d ms\n", d->pid, id, SETTLE_MS);
        return;
    }
    if (now_ms() - d->seen_ms < SETTLE_MS) return;
    n = query(true);
    k = find_if0(n);
    if (k < 0) {
        if (d->state != FMDEV_BUSY) log_file("fm: IF0 is held by another process\n");
        d->state = FMDEV_BUSY;
        return;
    }
    Result rc = usbHsAcquireUsbIf(&d->s, &g_list[k]);
    log_file("fm: acquire IF0 (id %d): %s\n", g_list[k].inf.ID, rc_str(rc));
    if (R_SUCCEEDED(rc)) {
        d->state = FMDEV_OPEN;
        d->if_id = g_list[k].inf.ID;
        d->failures = 0;
    } else {
        d->state = FMDEV_BUSY;
    }
}

static Result ctrl(fm_dev *d, u8 req, u16 value, u16 index, u16 len, void *out, u32 *got) {
    if (d->state != FMDEV_OPEN) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    Result rc = usbio_ctrl(&d->s, 0xC0, req, value, index, len, out, got);
    d->failures = R_SUCCEEDED(rc) ? 0 : d->failures + 1;
    return rc;
}

Result fm_set(fm_dev *d, u16 cmd, u16 value) {
    u8 b[1] = {0};
    Result rc = ctrl(d, 0xA1, cmd, value, 1, b, NULL);
    log_file("fm: SET %u = %u: %s\n", cmd, value, rc_str(rc));
    return rc;
}

Result fm_get(fm_dev *d, u16 cmd, u8 out[2]) {
    out[0] = out[1] = 0;
    return ctrl(d, 0xA2, cmd, 0, 2, out, NULL);
}

Result fm_query(fm_dev *d, u8 out[12], u32 *got) {
    memset(out, 0, 12);
    return ctrl(d, 0xA3, 0, 0, 12, out, got);
}
