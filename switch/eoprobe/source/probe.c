#include "probe.h"

#include "log.h"
#include "usbio.h"

typedef struct {
    bool open;
    u32  bus_id, device_id;
    u8   ifnum;
    UsbHsClientIfSession s;
} pr_session;

static pr_session     g_sess[PR_MAX_SESS];
static UsbHsInterface g_q_all[PR_QUERY_MAX], g_q_avail[PR_QUERY_MAX];

u16 pr_vid(const pr_device *d) { return d->dev.idVendor; }
u16 pr_pid(const pr_device *d) { return d->dev.idProduct; }

static const char *class_name(u8 cls, u8 sub) {
    switch (cls) {
    case USB_CLASS_AUDIO:
        return sub == 1 ? "AudioControl" : sub == 2 ? "AudioStreaming" : sub == 3 ? "MIDIStreaming" : "Audio";
    case USB_CLASS_HID:          return "HID";
    case USB_CLASS_MASS_STORAGE: return "MassStorage";
    case USB_CLASS_HUB:          return "Hub";
    case USB_CLASS_VIDEO:        return "Video";
    case USB_CLASS_VENDOR_SPEC:  return "Vendor";
    default:                     return "other";
    }
}

// --------------------------------------------------------------------------- scanning

static void filter_all(UsbHsInterfaceFilter *f) {
    // [7.0.0+] Flags must be non-zero. bcdDevice 0..0xFFFF matches every device; before 6.0.0
    // these bits are ignored, which also matches everything.
    memset(f, 0, sizeof(*f));
    f->Flags = UsbHsInterfaceFilterFlags_bcdDevice_Min | UsbHsInterfaceFilterFlags_bcdDevice_Max;
    f->bcdDevice_Min = 0;
    f->bcdDevice_Max = 0xFFFF;
}

static void filter_vid(UsbHsInterfaceFilter *f, u16 vid) {
    memset(f, 0, sizeof(*f));
    f->Flags = UsbHsInterfaceFilterFlags_idVendor;
    f->idVendor = vid;
}

static Result query(bool available, UsbHsInterface *out, s32 *count) {
    UsbHsInterfaceFilter f;
    filter_all(&f);
    memset(out, 0, sizeof(UsbHsInterface) * PR_QUERY_MAX);
    size_t size = sizeof(UsbHsInterface) * PR_QUERY_MAX;
    Result rc = available ? usbHsQueryAvailableInterfaces(&f, out, size, count)
                          : usbHsQueryAllInterfaces(&f, out, size, count);
    if (R_FAILED(rc)) {
        log_file("query(%s) with match-all filter failed %s, retrying with VID filter\n",
                 available ? "available" : "all", rc_str(rc));
        filter_vid(&f, TARGET_VID);
        rc = available ? usbHsQueryAvailableInterfaces(&f, out, size, count)
                       : usbHsQueryAllInterfaces(&f, out, size, count);
    }
    if (R_FAILED(rc) || *count < 0) *count = 0;
    if (*count > PR_QUERY_MAX) *count = PR_QUERY_MAX;
    return rc;
}

static pr_device *device_for(pr_state *st, const UsbHsInterface *i) {
    for (int k = 0; k < st->dev_count; k++)
        if (st->devs[k].bus_id == i->busID && st->devs[k].device_id == i->deviceID) return &st->devs[k];
    if (st->dev_count >= PR_MAX_DEVS) return NULL;
    pr_device *d = &st->devs[st->dev_count++];
    memset(d, 0, sizeof(*d));
    d->bus_id = i->busID;
    d->device_id = i->deviceID;
    memcpy(d->path, i->pathstr, sizeof(d->path));
    d->path[sizeof(d->path) - 1] = 0;
    d->dev = i->device_desc;
    d->cfg_hdr = i->config_desc;
    return d;
}

static pr_iface *iface_for(pr_device *d, u8 num, bool *created) {
    *created = false;
    for (int k = 0; k < d->if_count; k++)
        if (d->ifs[k].info.inf.interface_desc.bInterfaceNumber == num) return &d->ifs[k];
    if (d->if_count >= PR_MAX_IFS) return NULL;
    *created = true;
    pr_iface *f = &d->ifs[d->if_count++];
    memset(f, 0, sizeof(*f));
    return f;
}

Result pr_scan(pr_state *st) {
    memset(st, 0, sizeof(*st));
    st->rc_all = query(false, g_q_all, &st->n_all);
    st->rc_avail = query(true, g_q_avail, &st->n_avail);
    save_file(APP_DIR "/usbhs_query_all.bin", g_q_all, sizeof(UsbHsInterface) * st->n_all);
    save_file(APP_DIR "/usbhs_query_avail.bin", g_q_avail, sizeof(UsbHsInterface) * st->n_avail);

    bool created;
    for (s32 i = 0; i < st->n_all; i++) {
        pr_device *d = device_for(st, &g_q_all[i]);
        pr_iface *f = d ? iface_for(d, g_q_all[i].inf.interface_desc.bInterfaceNumber, &created) : NULL;
        if (!f) continue;
        f->info = g_q_all[i];
        f->state = IF_HELD_OTHER;
    }
    for (s32 i = 0; i < st->n_avail; i++) {
        pr_device *d = device_for(st, &g_q_avail[i]);
        pr_iface *f = d ? iface_for(d, g_q_avail[i].inf.interface_desc.bInterfaceNumber, &created) : NULL;
        if (!f) continue;
        if (created) f->info = g_q_avail[i];
        f->avail = g_q_avail[i];
        f->state = IF_AVAILABLE;
    }
    for (int k = 0; k < st->dev_count; k++) {
        pr_device *d = &st->devs[k];
        for (int j = 0; j < d->if_count; j++)
            if (pr_session_get(d, d->ifs[j].info.inf.interface_desc.bInterfaceNumber)) d->ifs[j].state = IF_OURS;
        // sort by interface number
        for (int a = 1; a < d->if_count; a++)
            for (int b = a; b > 0 && d->ifs[b].info.inf.interface_desc.bInterfaceNumber <
                                         d->ifs[b - 1].info.inf.interface_desc.bInterfaceNumber; b--) {
                pr_iface tmp = d->ifs[b];
                d->ifs[b] = d->ifs[b - 1];
                d->ifs[b - 1] = tmp;
            }
    }
    return st->rc_all;
}

const pr_iface *pr_find_iface(const pr_device *d, u8 ifnum) {
    for (int k = 0; k < d->if_count; k++)
        if (d->ifs[k].info.inf.interface_desc.bInterfaceNumber == ifnum) return &d->ifs[k];
    return NULL;
}

static void endpoints_str(const UsbHsInterfaceInfo *inf, char *out, size_t size) {
    size_t n = 0;
    out[0] = 0;
    for (int e = 0; e < 15; e++) {
        const struct usb_endpoint_descriptor *in = &inf->input_endpoint_descs[e];
        const struct usb_endpoint_descriptor *ou = &inf->output_endpoint_descs[e];
        if (in->bLength && n < size) n += snprintf(out + n, size - n, " %02x", in->bEndpointAddress);
        if (ou->bLength && n < size) n += snprintf(out + n, size - n, " %02x", ou->bEndpointAddress);
    }
    if (!out[0]) snprintf(out, size, " -");
}

void pr_print(const pr_state *st, int target) {
    log_both(C_CYAN "== usb:hs ==" C_RESET " QueryAll %d (%s), QueryAvailable %d (%s)\n", st->n_all,
             rc_str(st->rc_all), st->n_avail, rc_str(st->rc_avail));
    if (st->dev_count == 0) {
        log_both(C_YELLOW "No USB device is visible to usb:hs." C_RESET
                 " Enumeration fails below the driver level:\n"
                 "  check the plug, try the dock USB-A port with a C-to-A adapter, compare usb:pm above.\n");
        return;
    }
    for (int k = 0; k < st->dev_count; k++) {
        const pr_device *d = &st->devs[k];
        log_both("%s[%d] %04x:%04x bus %u dev %u %s, USB %x.%02x, config: %u interface(s)\n",
                 k == target ? C_GREEN ">" : " ", k, pr_vid(d), pr_pid(d), d->bus_id, d->device_id, d->path,
                 d->dev.bcdUSB >> 8, d->dev.bcdUSB & 0xFF, d->cfg_hdr.bNumInterfaces);
        if (k == target) log_both(C_RESET);
        for (int j = 0; j < d->if_count; j++) {
            const pr_iface *f = &d->ifs[j];
            const struct usb_interface_descriptor *id = &f->info.inf.interface_desc;
            char eps[64];
            endpoints_str(&f->info.inf, eps, sizeof(eps));
            const char *state = f->state == IF_OURS ? C_CYAN "ours" C_RESET
                              : f->state == IF_AVAILABLE ? C_GREEN "available" C_RESET
                              : C_YELLOW "held by another process" C_RESET;
            log_both("     IF%u alt%u %02x/%02x %-14s ep:%-10s %s\n", id->bInterfaceNumber, id->bAlternateSetting,
                     id->bInterfaceClass, id->bInterfaceSubClass, class_name(id->bInterfaceClass, id->bInterfaceSubClass),
                     eps, state);
        }
        for (u8 n = 0; n < d->cfg_hdr.bNumInterfaces && n < 16; n++)
            if (!pr_find_iface(d, n))
                log_both(C_YELLOW "     IF%u not listed by usb:hs (held by a sysmodule?)\n" C_RESET, n);
    }
}

int pr_pick_target(const pr_state *st, u16 vid) {
    int any_audio = -1;
    for (int k = 0; k < st->dev_count; k++) {
        const pr_device *d = &st->devs[k];
        bool audio = false;
        for (int j = 0; j < d->if_count; j++) audio |= d->ifs[j].info.inf.interface_desc.bInterfaceClass == USB_CLASS_AUDIO;
        if (pr_vid(d) == vid) return k;
        if (audio && any_audio < 0) any_audio = k;
    }
    if (any_audio >= 0) return any_audio;
    return st->dev_count ? 0 : -1;
}

// --------------------------------------------------------------------------- sessions

UsbHsClientIfSession *pr_session_get(const pr_device *d, u8 ifnum) {
    for (int i = 0; i < PR_MAX_SESS; i++) {
        pr_session *p = &g_sess[i];
        if (p->open && p->bus_id == d->bus_id && p->device_id == d->device_id && p->ifnum == ifnum) return &p->s;
    }
    return NULL;
}

UsbHsClientIfSession *pr_session_any(const pr_device *d) {
    for (int i = 0; i < PR_MAX_SESS; i++) {
        pr_session *p = &g_sess[i];
        if (p->open && p->bus_id == d->bus_id && p->device_id == d->device_id) return &p->s;
    }
    return NULL;
}

Result pr_session_acquire(UsbHsInterface *avail_if) {
    pr_session *slot = NULL;
    for (int i = 0; i < PR_MAX_SESS && !slot; i++)
        if (!g_sess[i].open) slot = &g_sess[i];
    if (!slot) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    Result rc = usbHsAcquireUsbIf(&slot->s, avail_if);
    if (R_SUCCEEDED(rc)) {
        slot->open = true;
        slot->bus_id = avail_if->busID;
        slot->device_id = avail_if->deviceID;
        slot->ifnum = avail_if->inf.interface_desc.bInterfaceNumber;
    }
    return rc;
}

int pr_acquire_all(pr_device *d) {
    int n = 0;
    for (int j = 0; j < d->if_count; j++) {
        pr_iface *f = &d->ifs[j];
        u8 num = f->info.inf.interface_desc.bInterfaceNumber;
        if (f->state != IF_AVAILABLE || pr_session_get(d, num)) continue;
        Result rc = pr_session_acquire(&f->avail);
        log_both("  acquire IF%u: %s\n", num, rc_str(rc));
        if (R_SUCCEEDED(rc)) {
            f->state = IF_OURS;
            n++;
        }
    }
    return n;
}

static void close_slot(pr_session *p) {
    usbHsIfClose(&p->s);
    p->open = false;
}

void pr_release_device(const pr_device *d) {
    for (int i = 0; i < PR_MAX_SESS; i++)
        if (g_sess[i].open && g_sess[i].bus_id == d->bus_id && g_sess[i].device_id == d->device_id)
            close_slot(&g_sess[i]);
}

void pr_release_all(void) {
    for (int i = 0; i < PR_MAX_SESS; i++)
        if (g_sess[i].open) close_slot(&g_sess[i]);
}

int pr_session_count(void) {
    int n = 0;
    for (int i = 0; i < PR_MAX_SESS; i++) n += g_sess[i].open;
    return n;
}

void pr_prune_sessions(void) {
    static UsbHsInterface acquired[PR_MAX_SESS];
    s32 n = 0;
    memset(acquired, 0, sizeof(acquired));
    Result rc = usbHsQueryAcquiredInterfaces(acquired, sizeof(acquired), &n);
    if (R_FAILED(rc)) {
        log_both("QueryAcquiredInterfaces %s\n", rc_str(rc));
        return;
    }
    for (int i = 0; i < PR_MAX_SESS; i++) {
        if (!g_sess[i].open) continue;
        bool found = false;
        for (s32 k = 0; k < n && k < PR_MAX_SESS; k++) found |= acquired[k].inf.ID == usbHsIfGetID(&g_sess[i].s);
        if (!found) {
            log_both(C_YELLOW "  session IF%u (bus %u dev %u) is gone, closing\n" C_RESET, g_sess[i].ifnum,
                     g_sess[i].bus_id, g_sess[i].device_id);
            close_slot(&g_sess[i]);
        }
    }
}

// --------------------------------------------------------------------------- descriptors

static void dump_alt_settings(pr_device *d) {
    // What usb:hs itself reports for every alt setting of the AS interfaces we hold.
    for (int j = 0; j < d->if_count; j++) {
        u8 num = d->ifs[j].info.inf.interface_desc.bInterfaceNumber;
        UsbHsClientIfSession *s = pr_session_get(d, num);
        if (!s || d->ifs[j].info.inf.interface_desc.bInterfaceClass != USB_CLASS_AUDIO) continue;
        for (u8 alt = 0; alt < 8; alt++) {
            static UsbHsInterfaceInfo info;
            memset(&info, 0, sizeof(info));
            Result rc = usbHsIfGetAlternateInterface(s, &info, alt);
            if (R_FAILED(rc)) {
                log_file("  usb:hs GetAlternateInterface(IF%u, alt%u): %s\n", num, alt, rc_str(rc));
                break;
            }
            char eps[64];
            endpoints_str(&info, eps, sizeof(eps));
            log_file("  usb:hs view IF%u alt%u: class %02x/%02x, %u ep:%s\n", num, alt,
                     info.interface_desc.bInterfaceClass, info.interface_desc.bInterfaceSubClass,
                     info.interface_desc.bNumEndpoints, eps);
        }
    }
}

Result pr_read_descriptors(pr_device *d) {
    UsbHsClientIfSession *s = pr_session_any(d);
    d->have_cfg = false;
    if (!s) {
        log_both(C_YELLOW "  No interface of %04x:%04x is ours, so no control requests can be sent.\n"
                 "  Press R (race) and replug the earphone, or free it from the sysmodule first.\n" C_RESET,
                 pr_vid(d), pr_pid(d));
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    char base[96];
    snprintf(base, sizeof(base), APP_DIR "/%04x_%04x", pr_vid(d), pr_pid(d));
    char path[128];

    u8 dd[18];
    u32 got = 0;
    Result rc = usbio_get_descriptor(s, USB_RECIPIENT_DEVICE, USB_DT_DEVICE, 0, 0, dd, sizeof(dd), &got);
    log_both("  GET_DESCRIPTOR(DEVICE): %s, %u bytes\n", rc_str(rc), got);
    if (R_SUCCEEDED(rc) && got == sizeof(dd)) {
        snprintf(path, sizeof(path), "%s_device.bin", base);
        save_file(path, dd, sizeof(dd));
    }

    u8 head[9];
    rc = usbio_get_descriptor(s, USB_RECIPIENT_DEVICE, USB_DT_CONFIG, 0, 0, head, sizeof(head), &got);
    if (R_FAILED(rc) || got < 4) {
        log_both(C_RED "  GET_DESCRIPTOR(CONFIG, 9): %s, %u bytes\n" C_RESET, rc_str(rc), got);
        return R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    u16 total = (u16)(head[2] | (head[3] << 8));
    if (total > sizeof(d->raw_cfg)) total = sizeof(d->raw_cfg);
    rc = usbio_get_descriptor(s, USB_RECIPIENT_DEVICE, USB_DT_CONFIG, 0, 0, d->raw_cfg, total, &got);
    log_both("  GET_DESCRIPTOR(CONFIG): %s, %u of %u bytes\n", rc_str(rc), got, total);
    if (R_FAILED(rc)) return rc;
    d->raw_cfg_len = (u16)got;
    snprintf(path, sizeof(path), "%s_config.bin", base);
    log_both("  saved %s %s\n", path, save_file(path, d->raw_cfg, got) ? "" : C_RED "(write failed)" C_RESET);
    log_hex("config descriptor", d->raw_cfg, got, 0);
    d->have_cfg = ud_parse_config(d->raw_cfg, got, &d->cfg);

    // Strings (manufacturer/product only; the serial number is not read).
    u8 langs[4] = {0};
    u16 lang = 0x0409;
    if (R_SUCCEEDED(usbio_get_descriptor(s, USB_RECIPIENT_DEVICE, USB_DT_STRING, 0, 0, langs, sizeof(langs), &got)) &&
        got >= 4)
        lang = (u16)(langs[2] | (langs[3] << 8));
    if (d->dev.iManufacturer) usbio_get_string(s, d->dev.iManufacturer, lang, d->manufacturer, sizeof(d->manufacturer));
    if (d->dev.iProduct) usbio_get_string(s, d->dev.iProduct, lang, d->product, sizeof(d->product));
    log_both("  strings: \"%s\" / \"%s\"\n", d->manufacturer, d->product);
    snprintf(path, sizeof(path), "%s_strings.txt", base);
    char text[192];
    int n = snprintf(text, sizeof(text), "manufacturer=%s\nproduct=%s\nlangid=0x%04x\npath=%s\n", d->manufacturer,
                     d->product, lang, d->path);
    save_file(path, text, (size_t)n);

    // HID report descriptors, through the HID interface's own session.
    if (d->have_cfg) {
        for (int i = 0; i < d->cfg.alt_count; i++) {
            const ud_alt *a = &d->cfg.alts[i];
            if (!a->has_hid || a->alt != 0 || !a->hid_report_len) continue;
            UsbHsClientIfSession *hs = pr_session_get(d, a->number);
            if (!hs) {
                log_both("  HID IF%u report descriptor: interface not ours, skipped\n", a->number);
                continue;
            }
            u8 rep[512];
            u16 len = a->hid_report_len < sizeof(rep) ? a->hid_report_len : sizeof(rep);
            rc = usbio_get_descriptor(hs, USB_RECIPIENT_INTERFACE, UD_DT_HID_REPORT, 0, a->number, rep, len, &got);
            log_both("  HID IF%u report descriptor: %s, %u bytes\n", a->number, rc_str(rc), got);
            if (R_SUCCEEDED(rc)) {
                snprintf(path, sizeof(path), "%s_hid_if%u.bin", base, a->number);
                save_file(path, rep, got);
                log_hex("HID report descriptor", rep, got, 0);
            }
        }
    }
    dump_alt_settings(d);
    return 0;
}

void pr_print_analysis(const pr_device *d) {
    if (!d->have_cfg) return;
    const ud_config *c = &d->cfg;
    log_both(C_CYAN "== Descriptor analysis ==" C_RESET " %u bytes, %u interfaces, bcdADC %x.%02x, AC = IF%u\n",
             c->total_length, c->num_interfaces, c->bcd_adc >> 8, c->bcd_adc & 0xFF, c->ac_iface);
    if (c->warnings) log_both(C_YELLOW "  parser: %d warning(s), first: %s\n" C_RESET, c->warnings, c->first_warning);

    log_both("  AS order:");
    u8 seen[16] = {0};
    for (int i = 0; i < c->alt_count; i++) {
        const ud_alt *a = &c->alts[i];
        if (a->cls != 1 || a->subcls != 2 || a->number >= 16 || seen[a->number]) continue;
        const char *dir = "?";
        for (int k = 0; k < c->alt_count; k++) {
            const ud_endpoint *ep = c->alts[k].number == a->number ? ud_iso_data_ep(&c->alts[k]) : NULL;
            if (ep) { dir = ud_ep_is_in(ep) ? "capture" : "playback"; break; }
        }
        seen[a->number] = 1;
        log_both(" IF%u=%s", a->number, dir);
    }
    log_both("\n");

    ud_stream p;
    if (ud_find_stream(c, true, 48000, 16, 2, &p)) {
        log_both("  playback 48k/16/2ch: IF%u alt%u EP %02x %s maxpkt %u -> %u B/ms, rate ctrl %s\n", p.alt->number,
                 p.alt->alt, p.ep->address, ud_sync_name(ud_ep_sync(p.ep)), ud_ep_packet(p.ep), p.bytes_per_ms,
                 p.has_rate_ctrl ? (p.needs_rate_set ? "yes (required)" : "yes") : "no");
        log_both("  path: FU id%u (%s%s), output terminal id%u 0x%04x %s\n", p.fu_id,
                 (p.fu_controls & UD_FU_MUTE) ? "mute " : "", (p.fu_controls & UD_FU_VOLUME) ? "volume" : "",
                 p.terminal_id, p.terminal_type, ud_terminal_name(p.terminal_type));
    } else {
        log_both(C_RED "  no playback alt setting with PCM 48000 Hz / 16-bit / stereo\n" C_RESET);
    }
    uint32_t flags = ud_check(c);
    log_both("  flags:");
    for (int b = 0; b < UD_FLAG_COUNT; b++)
        if (flags & (1u << b)) log_both(" [%s]", ud_flag_name(1u << b));
    log_both(flags ? "\n" : " none\n");
}
