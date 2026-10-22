#include "device.h"

#include <math.h>

#include "log.h"
#include "pcm.h"
#include "osd.h"
#include "remote.h"
#include "uac.h"
#include "usbio.h"

#define DEV_MAX_IFS   8
#define DEV_QUERY_MAX 16
#define INV_MAX_IFS   32
#define INV_MAX_DEVS  8
#define INV_EVERY_MS  2000
#define FAST_POLL_MS  10000   // after any USB event, poll QueryAvailable every 2 ms for this long
#define MAX_RESTARTS  3       // audio path restarts per plug-in

typedef struct {
    bool                 open;
    u8                   ifnum, cls, sub;
    UsbHsClientIfSession s;
} dev_if;

typedef struct {
    u32  bus, dev;
    u16  vid, pid;
    u8   n_if;
    char classes[40];
} inv_dev;

static struct {
    const eo_config *cfg;
    bool             stream;   // configure and stream (tone/stream); false = hold + remote only
    dev_hooks        hooks;
    void            *ctx;

    UsbHsInterfaceFilter vid_filter, all_filter;
    Event            avail[3];
    u8               avail_index[3];
    int              n_avail;
    Event           *state_ev;
    bool             usb_ok;

    dev_state        state;
    bool             hold;
    bool             have_dev;
    u32              bus, devid;
    struct usb_device_descriptor devdesc;
    u64              t_seen, t_ready, t_lost, settle_until, fast_until;
    u16              lost_pid;
    u32              lost_devid, instances, reenum;
    int              resets_left, restarts;
    dev_if           ifs[DEV_MAX_IFS];
    bool             audio_on;

    u8               raw_cfg[1024];
    u16              raw_len;
    ud_config        ucfg;
    ud_stream        ps;
    struct usb_endpoint_descriptor epd;
    dev_if          *as, *ac, *hid;
    char             product[64];

    Mutex            ctrl;   // control transfers + volume state (main thread vs remote thread)
    bool             have_vol;
    s16              vmin, vmax, vres;
    float            volume_db, vol_lo, vol_hi;   // vol_lo/hi: effective range (config within device)
    bool             muted, fu_ok, vol_changed;
    u32              gain_q15;

    inv_dev          inv[INV_MAX_DEVS];
    int              inv_n;
    u64              inv_next;
} D;

static UsbHsInterface g_q[DEV_QUERY_MAX];
static UsbHsInterface g_all[INV_MAX_IFS];

// ------------------------------------------------------------------ helpers

const char *dev_state_name(dev_state s) {
    switch (s) {
    case DEV_IDLE:     return "idle";
    case DEV_SETTLING: return "settling";
    case DEV_READY:    return "ready";
    case DEV_HELD:     return "held";
    case DEV_FAILED:   return "failed";
    }
    return "?";
}

static void set_state(dev_state s, const char *why) {
    if (D.state == s) return;
    log_printf("dev: state %s -> %s (%s)", dev_state_name(D.state), dev_state_name(s), why);
    D.state = s;
}

static int open_count(void) {
    int n = 0;
    for (int i = 0; i < DEV_MAX_IFS; i++) n += D.ifs[i].open;
    return n;
}

static dev_if *find_if(u8 num) {
    for (int i = 0; i < DEV_MAX_IFS; i++)
        if (D.ifs[i].open && D.ifs[i].ifnum == num) return &D.ifs[i];
    return NULL;
}

static dev_if *any_if(void) {
    if (D.ac && D.ac->open) return D.ac;
    for (int i = 0; i < DEV_MAX_IFS; i++)
        if (D.ifs[i].open) return &D.ifs[i];
    return NULL;
}

// usb:hs may refuse class requests addressed to an interface the session does not own, so FU
// requests go through the AudioControl session when we hold it.
static UsbHsClientIfSession *ctrl_session(void) {
    if (D.ac && D.ac->open) return &D.ac->s;
    if (D.as && D.as->open) return &D.as->s;
    return NULL;
}

static void close_if(dev_if *f) {
    usbHsIfClose(&f->s);
    f->open = false;
    if (f == D.as) D.as = NULL;
    if (f == D.ac) D.ac = NULL;
    if (f == D.hid) D.hid = NULL;
}

static void leave_ready(bool reset_alt) {
    remote_stop();
    if (D.audio_on) {
        D.hooks.stop(D.ctx);
        D.audio_on = false;
        if (reset_alt && D.as && D.as->open) {
            Result rc = usbHsIfSetInterface(&D.as->s, NULL, 0);
            log_printf("dev: SET_INTERFACE IF%u alt0: %s", D.as->ifnum, rc_str(rc));
        }
    }
}

static void dev_lost(const char *why) {
    leave_ready(false);
    int n = 0;
    for (int i = 0; i < DEV_MAX_IFS; i++)
        if (D.ifs[i].open) {
            close_if(&D.ifs[i]);
            n++;
        }
    u64 now = now_ms();
    if (D.have_dev) {
        log_printf("dev: %04x:%04x bus %u dev %u released (%s): closed %d session(s), %llu ms after it appeared",
                   D.devdesc.idVendor, D.devdesc.idProduct, D.bus, D.devid, why, n,
                   (unsigned long long)(now - D.t_seen));
        D.lost_pid = D.devdesc.idProduct;
        D.lost_devid = D.devid;
        D.t_lost = now;
    }
    D.have_dev = false;
    D.as = D.ac = D.hid = NULL;
    set_state(DEV_IDLE, why);
}

// ------------------------------------------------------------------ volume (D.ctrl held)

static void volume_apply_locked(const char *why) {
    float lo = D.cfg->volume_min_db, hi = D.cfg->volume_max_db;
    if (D.have_vol) {
        if (D.vmin / 256.0f > lo) lo = D.vmin / 256.0f;
        if (D.vmax / 256.0f < hi) hi = D.vmax / 256.0f;
    }
    if (hi < lo) hi = lo;
    D.vol_lo = lo;
    D.vol_hi = hi;
    // Like the console volume: one step below the minimum is level 0 = silent.
    float floor_db = lo - D.cfg->volume_step_db;
    if (D.volume_db < lo - 0.01f) D.volume_db = floor_db;
    if (D.volume_db > hi) D.volume_db = hi;
    bool silent = D.volume_db < lo - 0.01f;
    bool mute = D.muted || silent;

    UsbHsClientIfSession *s = ctrl_session();
    Result rv = MAKERESULT(Module_Libnx, LibnxError_NotFound), rm = rv;
    float set_db = D.volume_db;
    if (s && D.have_vol) {
        s32 raw = (s32)lrintf((silent ? lo : D.volume_db) * 256.0f);
        raw = D.vmin + (raw - D.vmin + D.vres / 2) / D.vres * D.vres;   // onto the device's step grid
        if (raw > D.vmax) raw = D.vmax;
        if (raw < D.vmin) raw = D.vmin;
        set_db = raw / 256.0f;
        u8 ac_if = D.ucfg.ac_iface;
        rv = uac_fu_set_volume(s, D.ps.fu_id, ac_if, 0, (s16)raw);
        if (D.ps.fu_controls & UD_FU_MUTE) rm = uac_fu_set_mute(s, D.ps.fu_id, ac_if, 0, mute);
    }
    D.fu_ok = R_SUCCEEDED(rv);
    // Digital gain carries the volume where FU5 cannot, and always the mute (instant and certain).
    u32 gain = mute ? 0 : D.fu_ok ? PCM_UNITY_Q15 : pcm_gain_q15_from_db(D.volume_db);
    __atomic_store_n(&D.gain_q15, gain, __ATOMIC_RELAXED);
    const char *state = D.muted ? ", PAUSED (muted)" : silent ? " = level 0 (silent)" : "";
    if (!s)
        log_printf("dev: %s: earphone volume %.1f dB%s (applied when the earphone is configured)", why,
                   (double)D.volume_db, state);
    else
        log_printf("dev: %s: earphone volume %.1f dB%s via %s (FU5 volume %s, mute %s)", why, (double)set_db, state,
                   D.fu_ok ? "FU5" : "digital gain", rc_str(rv), rc_str(rm));
}

static void read_volume_range(void) {
    UsbHsClientIfSession *s = ctrl_session();
    D.have_vol = false;
    if (!s || !D.ps.fu_id || !(D.ps.fu_controls & UD_FU_VOLUME)) {
        log_printf("dev: no feature-unit volume control on the playback path (FU id %u): using digital gain",
                   D.ps.fu_id);
        return;
    }
    s16 vmin = 0, vmax = 0, vres = 0, vcur = 0;
    bool mute = false;
    u8 fu = D.ps.fu_id, ac_if = D.ucfg.ac_iface;
    Result r1 = uac_fu_get_volume(s, UAC_GET_MIN, fu, ac_if, 0, &vmin);
    Result r2 = uac_fu_get_volume(s, UAC_GET_MAX, fu, ac_if, 0, &vmax);
    uac_fu_get_volume(s, UAC_GET_RES, fu, ac_if, 0, &vres);
    Result r4 = uac_fu_get_volume(s, UAC_GET_CUR, fu, ac_if, 0, &vcur);
    Result r5 = uac_fu_get_mute(s, fu, ac_if, 0, &mute);
    log_printf("dev: FU%u via %s session: volume %.1f dB (%s) range %.1f..%.1f step %.2f, mute %d (%s)", fu,
               D.ac && s == &D.ac->s ? "AC" : "AS", vcur / 256.0, rc_str(r4), vmin / 256.0, vmax / 256.0,
               vres / 256.0, mute, rc_str(r5));
    D.have_vol = R_SUCCEEDED(r1) && R_SUCCEEDED(r2) && vmax > vmin;
    D.vmin = vmin;
    D.vmax = vmax;
    D.vres = vres > 0 ? vres : 1;
}

// The earphone's own volume bar on screen (D.ctrl held). Not for dev_volume_set_db: that is the
// console-volume follow mode, where the console buttons show the system's own volume bar (the
// remote's buttons there call dev_volume_show: setting the volume through audctl shows nothing).
static void show_osd(void) {
    if (D.cfg->volume_osd) osd_show_volume(D.volume_db, D.vol_lo, D.vol_hi, D.cfg->volume_step_db, D.muted);
}

void dev_volume_steps(int steps, const char *why) {
    if (!steps) return;
    mutexLock(&D.ctrl);
    float before = D.volume_db;
    D.volume_db += (float)steps * D.cfg->volume_step_db;
    if (steps > 0) D.muted = false;   // Vol+ also ends a pause
    volume_apply_locked(why);
    show_osd();
    if (D.volume_db != before) D.vol_changed = true;   // saved to state.ini by the main loop
    mutexUnlock(&D.ctrl);
}

void dev_volume_set_db(float db, const char *why) {
    mutexLock(&D.ctrl);
    float before = D.volume_db;
    D.volume_db = db;
    if (db > before) D.muted = false;
    volume_apply_locked(why);
    if (D.volume_db != before) D.vol_changed = true;
    mutexUnlock(&D.ctrl);
}

void dev_volume_show(void) {
    mutexLock(&D.ctrl);
    show_osd();
    mutexUnlock(&D.ctrl);
}

void dev_volume_refresh(const char *why) {
    mutexLock(&D.ctrl);
    float before = D.volume_db;
    volume_apply_locked(why);   // re-clamps to a changed range
    if (D.volume_db != before) D.vol_changed = true;
    mutexUnlock(&D.ctrl);
}

void dev_mute_toggle(const char *why) {
    mutexLock(&D.ctrl);
    D.muted = !D.muted;
    volume_apply_locked(why);
    show_osd();
    mutexUnlock(&D.ctrl);
}

bool dev_take_volume_change(float *db) {
    mutexLock(&D.ctrl);
    bool changed = D.vol_changed;
    D.vol_changed = false;
    *db = D.volume_db;
    mutexUnlock(&D.ctrl);
    return changed;
}

u32 dev_output_gain_q15(void) { return __atomic_load_n(&D.gain_q15, __ATOMIC_RELAXED); }

static void on_remote(void *ctx, u8 bit, bool repeat) {
    if (bit == REMOTE_VOL_UP || bit == REMOTE_VOL_DOWN) {
        const char *why = bit == REMOTE_VOL_UP ? (repeat ? "remote Vol+ (held)" : "remote Vol+")
                                               : (repeat ? "remote Vol- (held)" : "remote Vol-");
        int dir = bit == REMOTE_VOL_UP ? 1 : -1;
        // While the console volume drives the earphone, the remote moves the console volume
        // (the HOME slider follows), and the earphone follows that.
        if (!(D.hooks.volume_key && D.hooks.volume_key(D.ctx, dir, why))) dev_volume_steps(dir, why);
    } else if (repeat) {
        // only Vol+/Vol- repeat
    } else if (bit == REMOTE_PLAY && D.cfg->play_button == PLAY_MUTE) {
        dev_mute_toggle("remote middle button (play_button=mute)");
    } else if (bit == REMOTE_PLAY && D.cfg->play_button == PLAY_MEDIA) {
        if (D.hooks.play_key) D.hooks.play_key(D.ctx);
    } else if (bit == REMOTE_PLAY) {
        log_printf("remote: middle button: no action (play_button=none)");
    } else {
        log_printf("remote: button bit 0x%02x pressed: no action", bit);
    }
}

// ------------------------------------------------------------------ inventory (log only)

static void inventory(u64 now, bool force) {
    if (!force && now < D.inv_next) return;
    D.inv_next = now + INV_EVERY_MS;
    s32 n = 0;
    if (R_FAILED(usbHsQueryAllInterfaces(&D.all_filter, g_all, sizeof(g_all), &n)) || n < 0) return;
    if (n > INV_MAX_IFS) n = INV_MAX_IFS;
    inv_dev cur[INV_MAX_DEVS];
    int nc = 0;
    for (s32 i = 0; i < n; i++) {
        const UsbHsInterface *q = &g_all[i];
        int k = 0;
        while (k < nc && !(cur[k].bus == q->busID && cur[k].dev == q->deviceID)) k++;
        if (k == nc) {
            if (nc == INV_MAX_DEVS) continue;
            memset(&cur[nc], 0, sizeof(cur[nc]));
            cur[nc].bus = q->busID;
            cur[nc].dev = q->deviceID;
            cur[nc].vid = q->device_desc.idVendor;
            cur[nc].pid = q->device_desc.idProduct;
            nc++;
        }
        inv_dev *d = &cur[k];
        size_t len = strlen(d->classes);
        if (len + 7 < sizeof(d->classes))
            snprintf(d->classes + len, sizeof(d->classes) - len, "%s%02x/%02x", len ? " " : "",
                     q->inf.interface_desc.bInterfaceClass, q->inf.interface_desc.bInterfaceSubClass);
        d->n_if++;
    }
    for (int k = 0; k < nc; k++) {
        bool known = false;
        for (int j = 0; j < D.inv_n && !known; j++) known = D.inv[j].bus == cur[k].bus && D.inv[j].dev == cur[k].dev;
        if (known) continue;
        const char *decision = cur[k].vid == TARGET_VID && cfg_pid_ok(D.cfg, cur[k].pid)
                                   ? "EO-IC100: eoaudio claims it"
                               : cur[k].vid == TARGET_VID ? "Samsung but not in pids=: left alone"
                                                          : "not an EO-IC100: left alone for the system";
        log_printf("usb: + %04x:%04x bus %u dev %u, %u interface(s) [%s] -> %s", cur[k].vid, cur[k].pid, cur[k].bus,
                   cur[k].dev, cur[k].n_if, cur[k].classes, decision);
    }
    for (int j = 0; j < D.inv_n; j++) {
        bool still = false;
        for (int k = 0; k < nc && !still; k++) still = D.inv[j].bus == cur[k].bus && D.inv[j].dev == cur[k].dev;
        if (!still)
            log_printf("usb: - %04x:%04x bus %u dev %u removed", D.inv[j].vid, D.inv[j].pid, D.inv[j].bus, D.inv[j].dev);
    }
    memcpy(D.inv, cur, sizeof(inv_dev) * (size_t)nc);
    D.inv_n = nc;
}

// ------------------------------------------------------------------ racing

// Drops sessions usb:hs no longer lists (unplugged, reset or re-enumerated).
static void prune(void) {
    static UsbHsInterface acq[DEV_MAX_IFS * 2];
    s32 n = 0;
    Result rc = usbHsQueryAcquiredInterfaces(acq, sizeof(acq), &n);
    if (R_FAILED(rc)) {
        log_printf("dev: QueryAcquiredInterfaces: %s", rc_str(rc));
        return;
    }
    bool dead[DEV_MAX_IFS] = {false};
    int n_dead = 0;
    for (int i = 0; i < DEV_MAX_IFS; i++) {
        if (!D.ifs[i].open) continue;
        bool found = false;
        for (s32 k = 0; k < n && k < DEV_MAX_IFS * 2 && !found; k++) found = acq[k].inf.ID == usbHsIfGetID(&D.ifs[i].s);
        if (!found) {
            dead[i] = true;
            n_dead++;
        }
    }
    if (!n_dead) return;
    leave_ready(false);   // stop the threads before their sessions go
    for (int i = 0; i < DEV_MAX_IFS; i++)
        if (dead[i]) {
            log_printf("dev: IF%u of bus %u dev %u is gone", D.ifs[i].ifnum, D.bus, D.devid);
            close_if(&D.ifs[i]);
        }
    if (open_count() == 0) {
        dev_lost("unplugged or re-enumerated");
    } else if (D.state == DEV_READY || D.state == DEV_FAILED || D.state == DEV_HELD) {
        D.settle_until = now_ms() + (u64)D.cfg->settle_ms;
        set_state(DEV_SETTLING, "an interface went away");
    }
}

static dev_if *free_slot(void) {
    for (int i = 0; i < DEV_MAX_IFS; i++)
        if (!D.ifs[i].open) return &D.ifs[i];
    return NULL;
}

static void new_instance(const UsbHsInterface *q, u64 now) {
    D.have_dev = true;
    D.bus = q->busID;
    D.devid = q->deviceID;
    D.devdesc = q->device_desc;
    D.t_seen = now;
    D.instances++;
    char path[0x41];
    memcpy(path, q->pathstr, 0x40);
    path[0x40] = 0;
    if (D.t_lost && now - D.t_lost < FAST_POLL_MS && q->device_desc.idProduct == D.lost_pid) {
        D.reenum++;
        log_printf("dev: re-enumeration #%u: %04x:%04x dev %u -> dev %u, %llu ms after the previous instance went "
                   "away (%s)",
                   D.reenum, q->device_desc.idVendor, q->device_desc.idProduct, D.lost_devid, q->deviceID,
                   (unsigned long long)(now - D.t_lost), path);
        return;
    }
    // A new plug-in, not a re-enumeration.
    D.reenum = 0;
    D.resets_left = D.cfg->resets;
    D.restarts = 0;
    mutexLock(&D.ctrl);
    if (D.muted) {
        D.muted = false;
        __atomic_store_n(&D.gain_q15, D.fu_ok ? PCM_UNITY_Q15 : pcm_gain_q15_from_db(D.volume_db), __ATOMIC_RELAXED);
        log_printf("dev: new plug-in: the earphone pause (mute) is cleared");
    }
    mutexUnlock(&D.ctrl);
    log_printf("dev: %04x:%04x plugged in: bus %u dev %u, %s, instance #%u, usb:hs timestamp %llu",
               q->device_desc.idVendor, q->device_desc.idProduct, q->busID, q->deviceID, path, D.instances,
               (unsigned long long)q->timestamp);
}

static void acquire_available(u64 now) {
    s32 n = 0;
    Result rc = usbHsQueryAvailableInterfaces(&D.vid_filter, g_q, sizeof(g_q), &n);
    if (R_FAILED(rc) || n <= 0) return;
    if (n > DEV_QUERY_MAX) n = DEV_QUERY_MAX;

    bool other = false;
    for (s32 i = 0; i < n; i++) {
        const UsbHsInterface *q = &g_q[i];
        if (!cfg_pid_ok(D.cfg, q->device_desc.idProduct)) continue;   // logged by the inventory
        if (D.have_dev && (q->busID != D.bus || q->deviceID != D.devid)) other = true;
    }
    // A new instance of a wanted device while we still hold an old one: the old one is probably the
    // instance that just re-enumerated, before its state-change event was handled.
    if (other) prune();

    // Streaming interfaces first, then AudioControl, then the rest (HID).
    u64 base = D.have_dev ? D.t_seen : now;
    for (int pass = 0; pass < 3; pass++) {
        for (s32 i = 0; i < n; i++) {
            UsbHsInterface *q = &g_q[i];
            const struct usb_interface_descriptor *id = &q->inf.interface_desc;
            // Only 04e8 (the query filter) with a PID from pids=, checked before anything is acquired.
            if (q->device_desc.idVendor != TARGET_VID || !cfg_pid_ok(D.cfg, q->device_desc.idProduct)) continue;
            if (D.have_dev && (q->busID != D.bus || q->deviceID != D.devid)) continue;
            int prio = id->bInterfaceClass == USB_CLASS_AUDIO ? (id->bInterfaceSubClass == 2 ? 0 : 1) : 2;
            if (prio != pass || (D.have_dev && find_if(id->bInterfaceNumber))) continue;
            dev_if *slot = free_slot();
            if (!slot) return;
            rc = usbHsAcquireUsbIf(&slot->s, q);
            if (R_SUCCEEDED(rc) && !D.have_dev) new_instance(q, now);
            log_printf("dev: +%llu ms acquire IF%u (%02x/%02x) of %04x:%04x: %s", (unsigned long long)(now - base),
                       id->bInterfaceNumber, id->bInterfaceClass, id->bInterfaceSubClass, q->device_desc.idVendor,
                       q->device_desc.idProduct, rc_str(rc));
            if (R_FAILED(rc)) continue;
            slot->open = true;
            slot->ifnum = id->bInterfaceNumber;
            slot->cls = id->bInterfaceClass;
            slot->sub = id->bInterfaceSubClass;
            D.settle_until = now + (u64)D.cfg->settle_ms;
            if (D.state == DEV_IDLE) set_state(DEV_SETTLING, "earphone interfaces acquired");
        }
    }
}

// ------------------------------------------------------------------ configuration

static void log_held(void) {
    s32 n = 0;
    Result rc = usbHsQueryAllInterfaces(&D.vid_filter, g_q, sizeof(g_q), &n);
    if (R_FAILED(rc)) {
        log_printf("dev: QueryAllInterfaces: %s", rc_str(rc));
        return;
    }
    char line[192];
    int len = snprintf(line, sizeof(line), "dev: interfaces of bus %u dev %u:", D.bus, D.devid);
    u32 listed = 0;
    u8 count = 0;
    for (s32 i = 0; i < n && i < DEV_QUERY_MAX; i++) {
        const UsbHsInterface *q = &g_q[i];
        if (q->busID != D.bus || q->deviceID != D.devid) continue;
        u8 num = q->inf.interface_desc.bInterfaceNumber;
        count = q->config_desc.bNumInterfaces;
        if (num < 32) listed |= 1u << num;
        if (len < (int)sizeof(line) - 24)
            len += snprintf(line + len, sizeof(line) - (size_t)len, " IF%u=%s", num,
                            find_if(num) ? "ours" : "held by another process");
    }
    // An interface usb:hs does not list at all is also taken by someone else.
    for (u8 num = 0; num < count && num < 32; num++)
        if (!(listed & (1u << num)) && len < (int)sizeof(line) - 24)
            len += snprintf(line + len, sizeof(line) - (size_t)len, " IF%u=unlisted", num);
    log_printf("%s", line);
}

static bool read_config(void) {
    dev_if *f = any_if();
    if (!f) return false;
    u8 head[9];
    u32 got = 0;
    Result rc = usbio_get_descriptor(&f->s, USB_RECIPIENT_DEVICE, USB_DT_CONFIG, 0, 0, head, sizeof(head), &got);
    if (R_FAILED(rc) || got < 4) {
        log_printf("dev: GET_DESCRIPTOR(CONFIG, 9) via IF%u: %s, %u bytes", f->ifnum, rc_str(rc), got);
        return false;
    }
    u16 total = (u16)(head[2] | (head[3] << 8));
    if (total > sizeof(D.raw_cfg)) total = sizeof(D.raw_cfg);
    rc = usbio_get_descriptor(&f->s, USB_RECIPIENT_DEVICE, USB_DT_CONFIG, 0, 0, D.raw_cfg, total, &got);
    if (R_FAILED(rc)) {
        log_printf("dev: GET_DESCRIPTOR(CONFIG, %u): %s", total, rc_str(rc));
        return false;
    }
    D.raw_len = (u16)got;
    bool ok = ud_parse_config(D.raw_cfg, got, &D.ucfg);

    D.product[0] = 0;
    if (D.devdesc.iProduct) {
        u8 langs[4] = {0};
        u16 lang = 0x0409;
        u32 lgot = 0;
        if (R_SUCCEEDED(usbio_get_descriptor(&f->s, USB_RECIPIENT_DEVICE, USB_DT_STRING, 0, 0, langs, sizeof(langs),
                                             &lgot)) &&
            lgot >= 4)
            lang = (u16)(langs[2] | (langs[3] << 8));
        usbio_get_string(&f->s, D.devdesc.iProduct, lang, D.product, sizeof(D.product));
    }
    log_printf("dev: config descriptor %u bytes%s, %u interfaces, AC = IF%u, product \"%s\"", got,
               ok ? "" : " (not a config descriptor)", D.ucfg.num_interfaces, D.ucfg.ac_iface, D.product);
    if (D.ucfg.warnings) log_printf("dev: parser: %d warning(s), first: %s", D.ucfg.warnings, D.ucfg.first_warning);
    if (log_debug_on()) log_hex("dev: config descriptor", D.raw_cfg, D.raw_len);
    return ok;
}

static bool configure_stream(void) {
    UsbHsClientIfSession *as = &D.as->s;
    Result rc = usbHsIfSetInterface(as, NULL, D.ps.alt->alt);
    log_printf("dev: SET_INTERFACE IF%u alt%u: %s", D.ps.alt->number, D.ps.alt->alt, rc_str(rc));
    if (R_FAILED(rc)) return false;
    bool found = false;
    for (int e = 0; e < 15 && !found; e++) {
        const struct usb_endpoint_descriptor *x = &as->inf.inf.output_endpoint_descs[e];
        if (x->bLength && x->bEndpointAddress == D.ps.ep->address) {
            D.epd = *x;
            found = true;
        }
    }
    if (!found) {
        log_printf("dev: EP %02x missing from the usb:hs interface info after SET_INTERFACE", D.ps.ep->address);
        log_hex("interface info", &as->inf.inf, sizeof(as->inf.inf));
        return false;
    }
    if (D.ps.has_rate_ctrl) {
        u32 cur = 0;
        rc = uac_set_rate(as, D.ps.ep->address, 48000);
        Result rc2 = uac_get_rate(as, D.ps.ep->address, &cur);
        log_printf("dev: SET_CUR sampling freq 48000: %s, GET_CUR -> %u (%s)", rc_str(rc), cur, rc_str(rc2));
    }
    return true;
}

static void settle_done(u64 now) {
    log_held();
    if (!read_config()) {
        set_state(DEV_FAILED, "configuration descriptor unreadable");
        return;
    }
    if (!ud_find_stream(&D.ucfg, true, 48000, 16, 2, &D.ps)) {
        set_state(DEV_FAILED, "no playback alt setting with PCM 48 kHz / 16-bit / stereo");
        return;
    }
    log_printf("dev: playback IF%u alt%u EP %02x (%s, maxpkt %u) %u B/ms, rate control %s, FU id%u, OT id%u 0x%04x",
               D.ps.alt->number, D.ps.alt->alt, D.ps.ep->address, ud_sync_name(ud_ep_sync(D.ps.ep)),
               ud_ep_packet(D.ps.ep), D.ps.bytes_per_ms, D.ps.has_rate_ctrl ? "yes" : "no", D.ps.fu_id,
               D.ps.terminal_id, D.ps.terminal_type);
    D.as = find_if(D.ps.alt->number);
    D.ac = D.ucfg.has_ac ? find_if(D.ucfg.ac_iface) : NULL;
    D.hid = NULL;
    for (int i = 0; i < DEV_MAX_IFS; i++)
        if (D.ifs[i].open && D.ifs[i].cls == USB_CLASS_HID && (!D.hid || D.ifs[i].ifnum < D.hid->ifnum)) D.hid = &D.ifs[i];

    if (D.stream && !D.as) {
        log_printf("dev: playback IF%u is held by another process: the system claimed it first", D.ps.alt->number);
        dev_if *f = any_if();
        if (D.resets_left > 0 && f) {
            D.resets_left--;
            Result rc = usbHsIfResetDevice(&f->s);
            log_printf("dev: ResetDevice via IF%u (software replug, %d left): %s", f->ifnum, D.resets_left, rc_str(rc));
            dev_lost("reset to race again");
            D.fast_until = now + FAST_POLL_MS;
            return;
        }
        set_state(DEV_FAILED, "no resets left: unplug the earphone and plug it back in");
        return;
    }
    if (D.stream && D.hold) {
        set_state(DEV_HELD, "another output is active: not streaming");
        return;
    }

    mutexLock(&D.ctrl);
    read_volume_range();
    bool ok = !D.stream || configure_stream();
    if (ok) volume_apply_locked("configured");
    mutexUnlock(&D.ctrl);
    if (!ok) {
        set_state(DEV_FAILED, "streaming interface could not be configured");
        return;
    }
    if (D.stream) {
        D.audio_on = D.hooks.start(D.ctx, &D.as->s, &D.epd, D.ps.bytes_per_ms);
        if (!D.audio_on) {
            set_state(DEV_FAILED, "the audio path did not start");
            return;
        }
    } else {
        log_printf("dev: capture mode: holding the earphone without streaming (remote buttons are logged)");
    }
    if (D.hid) {
        Result rc = remote_start(&D.hid->s, on_remote, NULL);
        if (R_FAILED(rc)) log_printf("dev: remote thread: %s", rc_str(rc));
    } else {
        log_printf("dev: no HID interface held: remote buttons unavailable");
    }
    D.t_ready = now;
    char why[64];
    snprintf(why, sizeof(why), "%llu ms after the earphone appeared", (unsigned long long)(now - D.t_seen));
    set_state(DEV_READY, why);
}

// ------------------------------------------------------------------ public

void dev_init(const eo_config *cfg, bool stream, float volume_db, const dev_hooks *hooks, void *ctx) {
    memset(&D, 0, sizeof(D));
    D.cfg = cfg;
    D.stream = stream;
    D.hooks = *hooks;
    D.ctx = ctx;
    D.volume_db = volume_db;
    D.gain_q15 = PCM_UNITY_Q15;
    D.resets_left = cfg->resets;
    D.usb_ok = true;
    mutexInit(&D.ctrl);

    D.vid_filter.Flags = UsbHsInterfaceFilterFlags_idVendor;
    D.vid_filter.idVendor = TARGET_VID;
    // [7.0.0+] Flags must be non-zero; bcdDevice 0..0xFFFF matches every device (inventory only).
    D.all_filter.Flags = UsbHsInterfaceFilterFlags_bcdDevice_Min | UsbHsInterfaceFilterFlags_bcdDevice_Max;
    D.all_filter.bcdDevice_Min = 0;
    D.all_filter.bcdDevice_Max = 0xFFFF;
    Result rc = usbHsCreateInterfaceAvailableEvent(&D.avail[0], true, 0, &D.vid_filter);
    log_printf("dev: InterfaceAvailableEvent(VID %04x): %s", TARGET_VID, rc_str(rc));
    if (R_SUCCEEDED(rc)) {
        D.avail_index[0] = 0;
        D.n_avail = 1;
    } else {
        // [7.0.0+] filters must be unique system-wide: fall back to one filter per product ID.
        for (int i = 0; i < cfg->pid_count && D.n_avail < 3; i++) {
            UsbHsInterfaceFilter f;
            memset(&f, 0, sizeof(f));
            f.Flags = UsbHsInterfaceFilterFlags_idVendor | UsbHsInterfaceFilterFlags_idProduct;
            f.idVendor = TARGET_VID;
            f.idProduct = cfg->pids[i];
            rc = usbHsCreateInterfaceAvailableEvent(&D.avail[D.n_avail], true, (u8)D.n_avail, &f);
            log_printf("dev: InterfaceAvailableEvent(%04x:%04x): %s", TARGET_VID, cfg->pids[i], rc_str(rc));
            if (R_SUCCEEDED(rc)) {
                D.avail_index[D.n_avail] = (u8)D.n_avail;
                D.n_avail++;
            }
        }
    }
    D.state_ev = usbHsGetInterfaceStateChangeEvent();
    D.fast_until = now_ms() + FAST_POLL_MS;
    log_printf("dev: waiting for the earphone (%s, settle %d ms, up to %d reset(s), volume %.1f dB)",
               stream ? "configure + stream" : "hold only", cfg->settle_ms, cfg->resets, (double)volume_db);
    inventory(now_ms(), true);
}

void dev_exit(void) {
    if (!D.usb_ok) return;
    dev_lost("exit");
    for (int i = 0; i < D.n_avail; i++) usbHsDestroyInterfaceAvailableEvent(&D.avail[i], D.avail_index[i]);
    D.n_avail = 0;
    D.usb_ok = false;
}

int dev_waiters(Waiter *out, int max) {
    int n = 0;
    if (!D.usb_ok) return 0;
    for (int i = 0; i < D.n_avail && n < max; i++) out[n++] = waiterForEvent(&D.avail[i]);
    if (n < max) out[n++] = waiterForEvent(D.state_ev);
    return n;
}

u64 dev_timeout_ns(void) {
    if (!D.usb_ok) return 1000000000ULL;
    u64 now = now_ms(), t = 1000;
    if (now < D.fast_until) t = 2;
    if (D.state == DEV_SETTLING) {
        u64 left = D.settle_until > now ? D.settle_until - now : 0;
        if (left < t) t = left;
    }
    if (D.state == DEV_READY && t > 100) t = 100;
    return t * 1000000ULL;
}

void dev_step(bool usb_event) {
    if (!D.usb_ok) return;
    u64 now = now_ms();
    if (usb_event) D.fast_until = now + FAST_POLL_MS;
    if (R_SUCCEEDED(eventWait(D.state_ev, 0))) {
        eventClear(D.state_ev);
        D.fast_until = now + FAST_POLL_MS;
        prune();
        inventory(now, true);
    }
    if (D.state != DEV_READY || now < D.fast_until) acquire_available(now);
    inventory(now, usb_event);

    switch (D.state) {
    case DEV_SETTLING:
        if (now >= D.settle_until) settle_done(now);
        break;
    case DEV_READY: {
        Result why = 0;
        if (D.audio_on && !D.hooks.alive(D.ctx, &why)) {
            log_printf("dev: the audio path ended by itself (%s)", rc_str(why));
            leave_ready(true);
            if (++D.restarts > MAX_RESTARTS) {
                set_state(DEV_FAILED, "audio path keeps failing: unplug the earphone and plug it back in");
            } else {
                D.settle_until = now + 1000;
                set_state(DEV_SETTLING, "restarting the audio path");
            }
        }
        break;
    }
    default:
        break;
    }
}

dev_state dev_get_state(void) { return D.state; }

bool dev_streaming(void) { return D.audio_on; }

void dev_set_hold(bool hold, const char *why) {
    if (D.hold == hold) return;
    D.hold = hold;
    log_printf("dev: %s (%s)", hold ? "earphone stream held" : "earphone stream allowed", why);
    if (!D.usb_ok || !D.stream) return;
    if (hold && D.state == DEV_READY) {
        leave_ready(true);
        set_state(DEV_HELD, why);
    } else if (!hold && D.state == DEV_HELD) {
        D.settle_until = now_ms();
        set_state(DEV_SETTLING, why);
    }
}

void dev_status(char *out, size_t size) {
    if (!D.usb_ok) {
        snprintf(out, size, "usb:hs unavailable");
        return;
    }
    mutexLock(&D.ctrl);
    float vol = D.volume_db;
    bool muted = D.muted, fu = D.fu_ok;
    mutexUnlock(&D.ctrl);
    if (!D.have_dev) {
        snprintf(out, size, "no earphone (%u plug-in instance(s) so far%s), volume %.1f dB", D.instances,
                 D.hold ? ", held" : "", (double)vol);
        return;
    }
    snprintf(out, size, "%s %04x:%04x, %d interface(s), volume %.1f dB %s%s, remote %s, up %llu s",
             dev_state_name(D.state), D.devdesc.idVendor, D.devdesc.idProduct, open_count(), (double)vol,
             fu ? "(FU5)" : "(digital)", muted ? " MUTED" : "", remote_active() ? "on" : "off",
             (unsigned long long)((now_ms() - (D.state == DEV_READY ? D.t_ready : D.t_seen)) / 1000));
}
