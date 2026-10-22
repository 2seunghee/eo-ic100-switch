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

// The same plug-in keeps its device number and insertion timestamp; a re-enumeration gets new ones.
// (The interface ID is no good for this: QueryAll reports -1 for an interface someone holds, ours
// included - v0.1.0 dropped the earphone right after taking it.)
static bool same_instance(const fm_dev *d, const UsbHsInterface *it) {
    return it->deviceID == d->device_id && it->timestamp == d->inserted;
}

void fm_poll_device(fm_dev *d) {
    int n = query(false);
    int k = find_if0(n);
    if (k < 0) {   // gone
        if (d->state != FMDEV_NONE) log_file("fm: earphone gone\n");
        fm_close(d);
        return;
    }
    if (d->state == FMDEV_OPEN) {
        if (same_instance(d, &g_list[k]) && d->failures < 8) return;
        log_file("fm: earphone re-enumerated or not answering (device %u -> %u, %u failures)\n", d->device_id,
                 g_list[k].deviceID, d->failures);
        fm_close(d);
    }
    if (d->state == FMDEV_NONE || !same_instance(d, &g_list[k])) {   // a new plug-in: let it settle first
        d->state = FMDEV_SETTLING;
        d->device_id = g_list[k].deviceID;
        d->inserted = g_list[k].timestamp;
        d->pid = g_list[k].device_desc.idProduct;
        d->seen_ms = now_ms();
        log_file("fm: earphone 04e8:%04x seen (device %u), waiting %d ms\n", d->pid, d->device_id, SETTLE_MS);
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
    log_file("fm: acquire IF0 (id %d, device %u): %s\n", g_list[k].inf.ID, g_list[k].deviceID, rc_str(rc));
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
