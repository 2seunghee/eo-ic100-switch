// eoprobe: USB audio probe for the Samsung EO-IC100 (04e8:a054) on Nintendo Switch.
//
// Steps 2 and 3 of docs/PLAN.md:
//   - which interfaces usb:hs exposes and which ones a sysmodule already holds
//   - full descriptor dump + hypothesis flags (H1..H7)
//   - approach A prototype: stream a test tone over the isochronous OUT endpoint ourselves
// Every run logs to its own file, sdmc:/switch/eoprobe/logs/eoprobe_YYYYMMDD_HHMMSS.txt.
#include "common.h"
#include "hidmon.h"
#include "isoc.h"
#include "log.h"
#include "probe.h"
#include "sysinfo.h"
#include "uac.h"

static PadState   g_pad;
static pr_state   g_pr;
static int        g_target = -1;
static u32        g_sel_bus, g_sel_dev;
static bool       g_sel_valid;
static iso_params g_params;
static iso_stats  g_stats;
static bool       g_usb_ok;
static bool       g_lr_pattern;

static const float k_levels[] = {-48.0f, -40.0f, -30.0f, -24.0f, -18.0f, -12.0f};
static int g_level = 2;   // -30 dBFS

static pr_device *target(void) { return g_target >= 0 && g_target < g_pr.dev_count ? &g_pr.devs[g_target] : NULL; }

static void print_menu(void) {
    const iso_strategy *s = &g_iso_strategies[g_params.strategy];
    printf(C_CYAN "------------------------------------------------------------------------------\n" C_RESET);
    printf(" A rescan+dump  X tone (B stops)  Y sweep  L HID buttons  R race-acquire  ZR reset\n");
    printf(" Up/Down level %.0f dBFS  Left/Right strategy %d \"%s\"  ZL L/R test %s\n", k_levels[g_level],
           g_params.strategy, s->name, g_lr_pattern ? "on" : "off");
    printf(" - next device  + exit      target: ");
    pr_device *d = target();
    if (d) printf("[%d] %04x:%04x %s\n", g_target, pr_vid(d), pr_pid(d), d->product);
    else printf("none\n");
}

static bool wait_confirm(void) {
    consoleUpdate(NULL);
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        u64 k = padGetButtonsDown(&g_pad);
        if (k & HidNpadButton_A) return true;
        if (k & (HidNpadButton_B | HidNpadButton_Plus)) return false;
        consoleUpdate(NULL);
    }
    return false;
}

// --------------------------------------------------------------------------- scan

static void select_target(void) {
    g_target = -1;
    if (g_sel_valid)
        for (int k = 0; k < g_pr.dev_count; k++)
            if (g_pr.devs[k].bus_id == g_sel_bus && g_pr.devs[k].device_id == g_sel_dev) g_target = k;
    if (g_target < 0) g_target = pr_pick_target(&g_pr, TARGET_VID);
}

static void do_scan(void) {
    log_both(C_CYAN "== Scan ==" C_RESET "\n");
    pr_prune_sessions();
    pr_scan(&g_pr);
    select_target();
    pr_print(&g_pr, g_target);
    pr_device *d = target();
    if (!d) return;
    int n = pr_acquire_all(d);
    if (n) log_both("  holding %d more interface(s) of %04x:%04x\n", n, pr_vid(d), pr_pid(d));
    pr_read_descriptors(d);
    pr_print_analysis(d);
    log_sync();
}

static void next_device(void) {
    if (g_pr.dev_count == 0) return;
    g_target = (g_target + 1) % g_pr.dev_count;
    g_sel_bus = g_pr.devs[g_target].bus_id;
    g_sel_dev = g_pr.devs[g_target].device_id;
    g_sel_valid = true;
    pr_device *d = target();
    if (!d->have_cfg && pr_session_any(d) == NULL) pr_acquire_all(d);
    if (!d->have_cfg) pr_read_descriptors(d);
    pr_print_analysis(d);
}

// --------------------------------------------------------------------------- streaming (approach A)

static void setup_volume(pr_device *d, const ud_stream *ps, UsbHsClientIfSession *as) {
    if (!ps->fu_id) return;
    u8 ac_if = d->cfg.ac_iface;
    UsbHsClientIfSession *ac = pr_session_get(d, ac_if);
    const char *via = "AC session";
    if (!ac) {
        ac = as;   // usb:hs may refuse requests addressed to an interface the session doesn't own
        via = "AS session";
    }
    s16 vmin = 0, vmax = 0, vres = 0, vcur = 0;
    bool mute = false;
    Result r_min = uac_fu_get_volume(ac, UAC_GET_MIN, ps->fu_id, ac_if, 0, &vmin);
    Result r_max = uac_fu_get_volume(ac, UAC_GET_MAX, ps->fu_id, ac_if, 0, &vmax);
    uac_fu_get_volume(ac, UAC_GET_RES, ps->fu_id, ac_if, 0, &vres);
    Result r_cur = uac_fu_get_volume(ac, UAC_GET_CUR, ps->fu_id, ac_if, 0, &vcur);
    Result r_mute = uac_fu_get_mute(ac, ps->fu_id, ac_if, 0, &mute);
    log_both("  FU id%u via %s: volume %.1f dB [%.1f..%.1f step %.2f] (%s), mute %d (%s)\n", ps->fu_id, via,
             vcur / 256.0, vmin / 256.0, vmax / 256.0, vres / 256.0, rc_str(r_cur), mute, rc_str(r_mute));
    if (R_SUCCEEDED(r_min) && R_SUCCEEDED(r_max) && vmax > vmin && (ps->fu_controls & UD_FU_VOLUME)) {
        int want = vmax - 20 * 256;   // 20 dB below the device maximum: audible, not loud
        if (want < vmin) want = vmin;
        Result rc = uac_fu_set_volume(ac, ps->fu_id, ac_if, 0, (s16)want);
        log_both("  FU volume -> %.1f dB: %s\n", want / 256.0, rc_str(rc));
    }
    if (ps->fu_controls & UD_FU_MUTE) {
        Result rc = uac_fu_set_mute(ac, ps->fu_id, ac_if, 0, false);
        log_both("  FU unmute: %s\n", rc_str(rc));
    }
}

static bool prepare_playback(pr_device *d, ud_stream *ps, UsbHsClientIfSession **as_out,
                             struct usb_endpoint_descriptor *epd) {
    if (!d || !d->have_cfg) {
        log_both(C_YELLOW "No descriptors for the target yet: press A (or R if its interfaces are held).\n" C_RESET);
        return false;
    }
    if (!ud_find_stream(&d->cfg, true, 48000, 16, 2, ps)) {
        log_both(C_RED "No playback alt setting with PCM 48 kHz / 16-bit / stereo.\n" C_RESET);
        return false;
    }
    UsbHsClientIfSession *as = pr_session_get(d, ps->alt->number);
    if (!as) {
        log_both(C_YELLOW "Playback IF%u is not ours (another process holds it). Try R (race-acquire).\n" C_RESET,
                 ps->alt->number);
        return false;
    }
    Result rc = usbHsIfSetInterface(as, NULL, ps->alt->alt);
    log_both("  SET_INTERFACE IF%u alt%u: %s\n", ps->alt->number, ps->alt->alt, rc_str(rc));
    if (R_FAILED(rc)) return false;

    bool found = false;
    for (int e = 0; e < 15 && !found; e++) {
        const struct usb_endpoint_descriptor *x = &as->inf.inf.output_endpoint_descs[e];
        if (x->bLength && x->bEndpointAddress == ps->ep->address) {
            *epd = *x;
            found = true;
        }
    }
    if (!found) {
        log_both(C_RED "  EP %02x missing from the usb:hs interface info after SET_INTERFACE\n" C_RESET,
                 ps->ep->address);
        log_hex("interface info", &as->inf.inf, sizeof(as->inf.inf), 0);
        return false;
    }
    log_both("  usb:hs EP %02x: attr %02x maxpkt %u interval %u\n", epd->bEndpointAddress, epd->bmAttributes,
             epd->wMaxPacketSize, epd->bInterval);

    if (ps->has_rate_ctrl) {
        u32 cur = 0;
        rc = uac_set_rate(as, ps->ep->address, 48000);
        Result rc2 = uac_get_rate(as, ps->ep->address, &cur);
        log_both("  SET_CUR sampling freq 48000: %s, GET_CUR -> %u (%s)\n", rc_str(rc), cur, rc_str(rc2));
    }
    setup_volume(d, ps, as);
    *as_out = as;
    return true;
}

// Returns false if the user aborted with B.
static bool run_stream(pr_device *d, int strategy, u32 duration_ms, char *verdict, size_t vsize) {
    ud_stream ps;
    UsbHsClientIfSession *as = NULL;
    struct usb_endpoint_descriptor epd;
    snprintf(verdict, vsize, "setup failed");
    if (!prepare_playback(d, &ps, &as, &epd)) return true;

    iso_params p = g_params;
    p.strategy = strategy;
    p.duration_ms = duration_ms;
    p.packet_bytes = ps.bytes_per_ms;
    p.amplitude_dbfs = k_levels[g_level];
    p.pattern = g_lr_pattern ? TONE_LR_CYCLE : TONE_BOTH;
    log_both("  streaming \"%s\": %u urbs x %u batches, %u B/packet, %.0f dBFS, %u ms (B = stop)\n",
             g_iso_strategies[strategy].name, p.urbs_per_batch, p.batches, p.packet_bytes, p.amplitude_dbfs,
             duration_ms);
    log_sync();

    Result rc = iso_start(as, &epd, &p, &g_stats);
    if (R_FAILED(rc)) {
        log_both(C_RED "  stream thread: %s\n" C_RESET, rc_str(rc));
        return true;
    }
    bool aborted = false;
    u64 last = now_us();
    while (iso_running()) {
        if (!appletMainLoop()) {
            aborted = true;
            break;
        }
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_B) {
            aborted = true;
            break;
        }
        if (now_us() - last >= 1000000) {
            last = now_us();
            printf("  ... %u batches done, %u reports, %u errors\n", g_stats.completed, g_stats.reports,
                   g_stats.report_errors);
        }
        consoleUpdate(NULL);
    }
    if (aborted) iso_stop();
    else iso_wait();

    iso_summary(&p, &g_stats, verdict, vsize);
    bool good = strncmp(verdict, "PACED", 5) == 0;
    log_both("  %s%s" C_RESET "  (GetCurrentFrame %u -> %u in 10 ms)\n", good ? C_GREEN : C_YELLOW, verdict,
             g_stats.frame0, g_stats.frame1);
    rc = usbHsIfSetInterface(as, NULL, 0);
    log_file("  SET_INTERFACE alt0: %s\n", rc_str(rc));
    log_sync();
    return !aborted;
}

static void do_tone(void) {
    log_both(C_CYAN "== Tone test ==" C_RESET " up to 60 s, B stops. Earphones a little away from your ears first\n");
    char verdict[128];
    run_stream(target(), g_params.strategy, 60000, verdict, sizeof(verdict));
    log_both("  Did you hear a %s 1 kHz tone without clicks? Note it in your report.\n",
             g_lr_pattern ? "left / right / both" : "steady");
}

static void do_sweep(void) {
    pr_device *d = target();
    log_both(C_CYAN "== Strategy sweep ==" C_RESET " %d ways to call BatchBufferAsync, 1.5 s each\n",
             g_iso_strategy_count);
    log_both(C_YELLOW "  An untested argument combination may crash the usb sysmodule (fatal error ->\n"
             "  reboot). Close games first. Earphones away from your ears.  A = start, B = cancel\n" C_RESET);
    if (!wait_confirm()) {
        log_both("  cancelled\n");
        return;
    }
    static char verdicts[16][128];
    int ran = 0;
    for (int s = 0; s < g_iso_strategy_count && s < 16; s++) {
        log_both(C_CYAN "-- [%d] %s --\n" C_RESET, s, g_iso_strategies[s].name);
        ran++;
        if (!run_stream(d, s, 1500, verdicts[s], sizeof(verdicts[s]))) break;
        svcSleepThread(300 * 1000000ULL);
    }
    log_both(C_CYAN "== Sweep summary ==" C_RESET "\n");
    for (int s = 0; s < ran; s++) log_both("  [%d] %-14s %s\n", s, g_iso_strategies[s].name, verdicts[s]);
    log_both("  Use Left/Right to pick the best one, then X for a listening test.\n");
    log_sync();
}

// --------------------------------------------------------------------------- race / reset

static void do_race(void) {
    pr_device *d = target();
    u16 vid = d ? pr_vid(d) : TARGET_VID;
    log_both(C_CYAN "== Race-acquire ==" C_RESET " grab vendor %04x's interfaces as soon as they appear\n", vid);
    pr_release_all();   // so that every session held afterwards comes from this race

    UsbHsInterfaceFilter f;
    memset(&f, 0, sizeof(f));
    f.Flags = UsbHsInterfaceFilterFlags_idVendor;
    f.idVendor = vid;
    Event ev;
    Result rc = usbHsCreateInterfaceAvailableEvent(&ev, true, 0, &f);
    bool have_ev = R_SUCCEEDED(rc);
    log_file("  CreateInterfaceAvailableEvent: %s\n", rc_str(rc));

    static UsbHsInterface av[PR_QUERY_MAX];
    s32 n = 0;
    memset(av, 0, sizeof(av));
    bool reset = false;
    if (R_SUCCEEDED(usbHsQueryAvailableInterfaces(&f, av, sizeof(av), &n)) && n > 0) {
        // Software replug: reset the device through any interface we can open right now.
        UsbHsClientIfSession tmp;
        if (R_SUCCEEDED(usbHsAcquireUsbIf(&tmp, &av[0]))) {
            rc = usbHsIfResetDevice(&tmp);
            usbHsIfClose(&tmp);
            reset = R_SUCCEEDED(rc);
            log_both("  ResetDevice via IF%u: %s\n", av[0].inf.interface_desc.bInterfaceNumber, rc_str(rc));
        }
    }
    if (!reset) log_both(C_YELLOW "  Unplug the earphone, then plug it back in now (30 s, B = cancel).\n" C_RESET);
    if (have_ev) eventClear(&ev);   // ignore the "available" signal caused by our own release
    consoleUpdate(NULL);

    // Results are kept in RAM and printed afterwards: an SD write per acquire would hand the
    // remaining interfaces to whoever else is racing for them.
    struct { u64 ms; u8 ifnum, cls, sub; Result rc; } recs[PR_QUERY_MAX];   // ifnum 0xFF: re-enumeration
    int n_recs = 0;
    u64 t0 = now_us(), t_first = 0, last_ui = t0, held_since = 0;
    int got = 0;
    Event *gone = usbHsGetInterfaceStateChangeEvent();
    eventClear(gone);
    while (appletMainLoop() && now_us() - t0 < 30000000ULL) {
        if (now_us() - last_ui > 200000) {   // keep the UI cheap: every console flush delays us
            padUpdate(&g_pad);
            if (padGetButtonsDown(&g_pad) & HidNpadButton_B) break;
            consoleUpdate(NULL);
            last_ui = now_us();
        }
        // Right after plugging in, the device may re-enumerate (seen on 2026-10-20, likely the USB-C
        // role swap). That silently drops what we hold, so keep racing for the new instance.
        if (R_SUCCEEDED(eventWait(gone, 0))) {
            eventClear(gone);
            int before = pr_session_count();
            pr_prune_sessions();
            if (pr_session_count() < before) {
                held_since = 0;
                if (n_recs < PR_QUERY_MAX) {
                    recs[n_recs].ms = t_first ? (now_us() - t_first) / 1000 : 0;
                    recs[n_recs].ifnum = 0xFF;
                    n_recs++;
                }
            }
        }
        if (have_ev) eventWait(&ev, 2000000ULL);
        else svcSleepThread(2000000ULL);
        n = 0;
        if (R_FAILED(usbHsQueryAvailableInterfaces(&f, av, sizeof(av), &n)) || n <= 0) n = 0;
        if (n > PR_QUERY_MAX) n = PR_QUERY_MAX;
        if (n && !t_first) t_first = now_us();
        // Streaming interfaces first, then AudioControl, then the rest.
        for (int pass = 0; pass < 3; pass++) {
            for (s32 i = 0; i < n; i++) {
                const struct usb_interface_descriptor *id = &av[i].inf.interface_desc;
                int prio = id->bInterfaceClass == USB_CLASS_AUDIO ? (id->bInterfaceSubClass == 2 ? 0 : 1) : 2;
                if (prio != pass) continue;
                rc = pr_session_acquire(&av[i]);
                if (R_SUCCEEDED(rc)) got++;
                if (n_recs < PR_QUERY_MAX) {
                    recs[n_recs].ms = (now_us() - t_first) / 1000;
                    recs[n_recs].ifnum = id->bInterfaceNumber;
                    recs[n_recs].cls = id->bInterfaceClass;
                    recs[n_recs].sub = id->bInterfaceSubClass;
                    recs[n_recs].rc = rc;
                    n_recs++;
                }
            }
        }
        // Done once we have held something for 6 s without the device re-enumerating. A plug-in was
        // seen to enumerate ~3 times, with up to ~3.8 s between instances.
        if (pr_session_count() > 0 && !held_since) held_since = now_us();
        if (held_since && now_us() - held_since > 6000000ULL) break;
    }
    if (have_ev) usbHsDestroyInterfaceAvailableEvent(&ev, 0);
    for (int i = 0; i < n_recs; i++) {
        if (recs[i].ifnum == 0xFF)
            log_both(C_YELLOW "  +%llu ms device re-enumerated, racing again\n" C_RESET, (unsigned long long)recs[i].ms);
        else
            log_both("  +%llu ms IF%u (%02x/%02x): %s\n", (unsigned long long)recs[i].ms, recs[i].ifnum, recs[i].cls,
                     recs[i].sub, rc_str(recs[i].rc));
    }
    log_both("  acquired %d interface(s), holding %d%s\n", got, pr_session_count(), t_first ? "" : " - nothing appeared");
    do_scan();
}

static void do_reset(void) {
    pr_device *d = target();
    UsbHsClientIfSession *s = d ? pr_session_any(d) : NULL;
    if (!s) {
        log_both(C_YELLOW "Need one interface of the target first (A or R).\n" C_RESET);
        return;
    }
    Result rc = usbHsIfResetDevice(s);
    log_both("ResetDevice: %s\n", rc_str(rc));
    pr_release_device(d);
    svcSleepThread(1500 * 1000000ULL);
    do_scan();
}

// --------------------------------------------------------------------------- main

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    log_init();
    iso_default_params(&g_params);

    printf(C_CYAN "eoprobe %s" C_RESET " - USB audio probe for the Samsung EO-IC100 on Switch\n", EOPROBE_VERSION);
    printf("Log: %s\nDumps: %s/\n", log_path(), APP_DIR);
    sysinfo_report();
    Result rc = usbHsInitialize();
    g_usb_ok = R_SUCCEEDED(rc);
    if (!g_usb_ok) log_both(C_RED "usbHsInitialize: %s\n" C_RESET, rc_str(rc));
    else do_scan();
    print_menu();

    while (appletMainLoop()) {
        padUpdate(&g_pad);
        u64 k = padGetButtonsDown(&g_pad);
        if (k & HidNpadButton_Plus) break;
        if (g_usb_ok && k) {
            bool acted = true;
            if (k & HidNpadButton_A) do_scan();
            else if (k & HidNpadButton_X) do_tone();
            else if (k & HidNpadButton_Y) do_sweep();
            else if (k & HidNpadButton_L) hidmon_run(target(), &g_pad);
            else if (k & HidNpadButton_R) do_race();
            else if (k & HidNpadButton_ZR) do_reset();
            else if (k & HidNpadButton_ZL) g_lr_pattern = !g_lr_pattern;
            else if (k & HidNpadButton_Minus) next_device();
            else if (k & HidNpadButton_Up) g_level = g_level + 1 < (int)(sizeof(k_levels) / sizeof(k_levels[0])) ? g_level + 1 : g_level;
            else if (k & HidNpadButton_Down) g_level = g_level > 0 ? g_level - 1 : 0;
            else if (k & HidNpadButton_Right) g_params.strategy = (g_params.strategy + 1) % g_iso_strategy_count;
            else if (k & HidNpadButton_Left)
                g_params.strategy = (g_params.strategy + g_iso_strategy_count - 1) % g_iso_strategy_count;
            else acted = false;
            if (acted) print_menu();
        }
        if (g_usb_ok && R_SUCCEEDED(eventWait(usbHsGetInterfaceStateChangeEvent(), 0))) {
            eventClear(usbHsGetInterfaceStateChangeEvent());
            log_both(C_YELLOW "usb:hs: a device was removed or reset. Press A to rescan.\n" C_RESET);
            pr_prune_sessions();
        }
        consoleUpdate(NULL);
    }

    iso_stop();
    pr_release_all();
    if (g_usb_ok) usbHsExit();
    log_file("exit\n");
    log_exit();
    consoleExit(NULL);
    return 0;
}
