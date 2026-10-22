#include "isoc.h"

#include "log.h"

#define ISO_MAX_BATCHES 16
#define ISO_MAX_URBS    16
#define ISO_LOG_REPORTS 48
#define ISO_LOG_SUBMITS 8

// Confirmed on HOS 22.1.0 with the EO-IC100 (2026-10-20 sweep): unk1 = 0 and unk2 = the absolute
// 11-bit start frame (GetCurrentFrame + lead) streams at exactly 192 kB/s with no errors. With 0/0 the
// batch waits for frame 0; a frame outside the scheduling window fails at once with 0xD28C (2140-0105).
const iso_strategy g_iso_strategies[] = {
    {"0/frame (OK)",  false, ISO_ARG_CONST, ISO_ARG_FRAME, 0, 0},
    {"batch 1/frame", false, ISO_ARG_CONST, ISO_ARG_FRAME, 1, 0},
    {"batch 0/0",     false, ISO_ARG_CONST, ISO_ARG_CONST, 0, 0},   // what PostBufferAsync passes internally
    {"post 1 urb",    true,  ISO_ARG_CONST, ISO_ARG_CONST, 0, 0},
    {"batch 1/0",     false, ISO_ARG_CONST, ISO_ARG_CONST, 1, 0},
    {"batch 2/0",     false, ISO_ARG_CONST, ISO_ARG_CONST, 2, 0},
    {"batch frame/0", false, ISO_ARG_FRAME, ISO_ARG_CONST, 0, 0},
    {"batch frame/1", false, ISO_ARG_FRAME, ISO_ARG_CONST, 0, 1},
};
const int g_iso_strategy_count = sizeof(g_iso_strategies) / sizeof(g_iso_strategies[0]);

typedef struct {
    bool busy;
    u32  reports;
    u32  requested;   // sum of requestedSize over this batch's reports
    u32  done;        // sum of transferredSize
} iso_batch;

// Kept in RAM while streaming and written to the log afterwards: an SD write in the hot loop
// would stall the stream for milliseconds and distort the timing being measured.
typedef struct {
    u64    t_us, id;
    u32    unk1, unk2, xfer_id;
    Result rc;
} iso_submit_rec;

typedef struct {
    u64    t_us, id;
    u32    xfer_id, requested, done;
    Result res;
} iso_report_rec;

// One stream at a time; everything below is owned by the streaming thread while it runs.
static struct {
    UsbHsClientIfSession          *as;
    struct usb_endpoint_descriptor epd;
    iso_params                     p;
    iso_stats                     *st;
    const iso_strategy            *strat;
    volatile bool                  stop;
    Thread                         thread;
    bool                           thread_active;

    UsbHsClientEpSession ep;
    u8       *region;
    u32       slot, batch_bytes, frames_per_batch;
    u16       upb, nb;
    u32       urbs[ISO_MAX_URBS];
    iso_batch b[ISO_MAX_BATCHES];
    tone_gen  tone;
    u32       frame_next, frame_mask, first_frame;
    Result    frame_rc;
    bool      gave_up;         // no completion for 1 s
    bool      check_frame;     // a transfer failed: make sure frame_next isn't in the past
    u32       failed_in_row;   // consecutive failed batches, for backoff
    u64       seq;
    u32       n_submit_recs, n_report_recs;
    iso_submit_rec submit_recs[ISO_LOG_SUBMITS];
    iso_report_rec report_recs[ISO_LOG_REPORTS];
} g;

void iso_default_params(iso_params *p) {
    memset(p, 0, sizeof(*p));
    p->strategy = 0;
    p->max_urb_count = 16;
    p->urbs_per_batch = 4;
    p->batches = 4;
    p->packet_bytes = 192;
    p->frame_lead = 16;
    p->use_smmu = false;
    p->duration_ms = 5000;
    p->tone_hz = 1000.0f;
    p->amplitude_dbfs = -30.0f;
    p->pattern = TONE_BOTH;
}

// After a missed or rejected transfer, move the schedule forward if it fell behind the bus.
static void resync_frame(void) {
    u32 cur = 0;
    g.check_frame = false;
    if (R_FAILED(usbHsIfGetCurrentFrame(g.as, &cur))) return;
    u32 ahead = (g.frame_next - cur) & g.frame_mask;
    u32 half = (g.frame_mask >> 1) + 1;
    if (ahead >= half || ahead < 2) {
        g.frame_next = cur + g.p.frame_lead;
        g.st->resyncs++;
    }
}

static void submit(int bi) {
    iso_stats *st = g.st;
    u8 *buf = g.region + (size_t)bi * g.slot;
    tone_fill(&g.tone, (int16_t *)buf, g.frames_per_batch);
    armDCacheFlush(buf, g.batch_bytes);

    bool frame_mode = g.strat->unk1_mode == ISO_ARG_FRAME || g.strat->unk2_mode == ISO_ARG_FRAME;
    if (frame_mode && g.check_frame) resync_frame();
    if (g.failed_in_row > 64) svcSleepThread(1000000ULL);   // don't spin on a strategy that always fails

    u32 unk1 = g.strat->unk1, unk2 = g.strat->unk2;
    if (g.strat->unk1_mode == ISO_ARG_FRAME) unk1 = g.frame_next & g.frame_mask;
    if (g.strat->unk2_mode == ISO_ARG_FRAME) unk2 = g.frame_next & g.frame_mask;
    g.frame_next += g.upb;

    u64 id = (g.seq++ << 8) | (u64)bi;
    u32 xfer_id = 0;
    Result rc = g.strat->single_post ? usbHsEpPostBufferAsync(&g.ep, buf, g.batch_bytes, id, &xfer_id)
                                     : usbHsEpBatchBufferAsync(&g.ep, buf, g.urbs, g.upb, id, unk1, unk2, &xfer_id);
    st->submitted++;
    if (g.n_submit_recs < ISO_LOG_SUBMITS)
        g.submit_recs[g.n_submit_recs++] = (iso_submit_rec){now_us() - st->t_start, id, unk1, unk2, xfer_id, rc};
    if (R_FAILED(rc)) {
        st->submit_errors++;
        if (!st->first_submit_rc) st->first_submit_rc = rc;
        g.b[bi].busy = false;
        return;
    }
    g.b[bi].busy = true;
    g.b[bi].reports = 0;
    g.b[bi].requested = 0;
    g.b[bi].done = 0;
    st->bytes_requested += g.batch_bytes;
}

static bool any_busy(void) {
    for (int i = 0; i < g.nb; i++)
        if (g.b[i].busy) return true;
    return false;
}

static void handle_report(const UsbHsXferReport *x, u64 t, u64 deadline) {
    iso_stats *st = g.st;
    st->reports++;
    if (g.n_report_recs < ISO_LOG_REPORTS)
        g.report_recs[g.n_report_recs++] =
            (iso_report_rec){t - st->t_start, x->id, x->xferId, x->requestedSize, x->transferredSize, x->res};
    if (R_FAILED(x->res)) {
        st->report_errors++;
        if (!st->first_report_rc) st->first_report_rc = x->res;
        g.check_frame = true;
    }
    st->bytes_done += x->transferredSize;
    if (st->report_mode == 0) st->report_mode = (g.upb == 1 || x->requestedSize >= g.batch_bytes) ? 1 : 2;

    // Whether usb:hs reports once per batch or once per urb is unknown: a batch is complete when
    // its reports cover the requested bytes, or when there is one report per urb.
    int bi = (int)(x->id & 0xFF);
    if (bi >= g.nb || !g.b[bi].busy) return;
    g.b[bi].reports++;
    g.b[bi].requested += x->requestedSize;
    g.b[bi].done += x->transferredSize;
    if (g.b[bi].requested < g.batch_bytes && g.b[bi].reports < g.upb) return;

    g.b[bi].busy = false;
    st->completed++;
    g.failed_in_row = g.b[bi].done < g.batch_bytes ? g.failed_in_row + 1 : 0;
    if (!st->t_first_done) {
        st->t_first_done = t;
        st->bytes_at_first_done = st->bytes_done;
    } else if (t - st->t_last_done > st->max_gap_us) {
        st->max_gap_us = t - st->t_last_done;
    }
    st->t_last_done = t;
    if (!g.stop && t < deadline) submit(bi);
}

static void probe_frame_counter(void) {
    u32 f0 = 0, f1 = 0;
    Result r0 = usbHsIfGetCurrentFrame(g.as, &f0);
    svcSleepThread(10 * 1000000ULL);
    Result r1 = usbHsIfGetCurrentFrame(g.as, &f1);
    g.st->frame0 = f0;
    g.st->frame1 = f1;
    // 11-bit USB frame numbers wrap at 2048; otherwise treat it as a free-running counter.
    g.frame_mask = (f0 < 2048 && f1 < 2048) ? 0x7FF : 0xFFFFFFFF;
    g.frame_next = f1 + g.p.frame_lead;
    g.frame_rc = R_FAILED(r0) ? r0 : r1;
}

// Called right before the first submit: anything slow between reading the frame counter and
// submitting (an SD write took ~30 ms once) would put the first start frame in the past.
static void schedule_from_now(void) {
    u32 cur = 0;
    if (R_SUCCEEDED(usbHsIfGetCurrentFrame(g.as, &cur))) g.frame_next = cur + g.p.frame_lead;
    g.first_frame = g.frame_next;
}

static void iso_thread(void *arg) {
    (void)arg;
    iso_stats *st = g.st;
    const iso_params *p = &g.p;
    UsbHsXferReport reps[32];

    g.upb = g.strat->single_post ? 1 : p->urbs_per_batch;
    g.nb = g.strat->single_post ? p->max_urb_count : p->batches;
    if (g.upb < 1) g.upb = 1;
    if (g.upb > ISO_MAX_URBS) g.upb = ISO_MAX_URBS;
    if (g.nb < 1) g.nb = 1;
    if (g.nb > ISO_MAX_BATCHES) g.nb = ISO_MAX_BATCHES;
    g.batch_bytes = g.upb * p->packet_bytes;
    g.frames_per_batch = g.batch_bytes / 4;
    g.slot = ALIGN_UP(g.batch_bytes, 0x1000);
    for (int i = 0; i < ISO_MAX_URBS; i++) g.urbs[i] = p->packet_bytes;
    memset(g.b, 0, sizeof(g.b));
    g.check_frame = false;
    g.gave_up = false;
    g.failed_in_row = 0;
    g.seq = 0;
    g.n_report_recs = g.n_submit_recs = 0;
    tone_init(&g.tone, p->tone_hz, 48000, p->amplitude_dbfs, p->pattern);

    // No SD writes from here until the stream ends: everything is logged at `out:`.
    st->t_start = now_us();
    memset(&g.ep, 0, sizeof(g.ep));
    g.frame_rc = 0;
    g.first_frame = 0;
    Result rc = usbHsIfOpenUsbEp(g.as, &g.ep, p->max_urb_count, g.epd.wMaxPacketSize, &g.epd);
    st->open_rc = rc;
    if (R_FAILED(rc)) goto out;

    g.region = memalign(0x1000, (size_t)g.slot * g.nb);
    if (!g.region) {
        rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        goto out_ep;
    }
    memset(g.region, 0, (size_t)g.slot * g.nb);
    if (p->use_smmu && hosversionAtLeast(4, 0, 0)) st->smmu_rc = usbHsEpCreateSmmuSpace(&g.ep, g.region, g.slot * g.nb);
    probe_frame_counter();
    schedule_from_now();

    u64 deadline = now_us() + (u64)p->duration_ms * 1000;
    for (int bi = 0; bi < g.nb && !g.stop; bi++) {
        submit(bi);
        if (bi == 0 && !g.b[0].busy) break;   // the first submit already failed: don't hammer the sysmodule
    }
    int idle = 0;
    while (any_busy() && !g.stop && now_us() < deadline) {
        rc = eventWait(usbHsEpGetXferEvent(&g.ep), 50 * 1000000ULL);
        if (R_FAILED(rc)) {
            st->timeouts++;
            if (++idle >= 20) {
                g.gave_up = true;   // no completion for 1 s
                break;
            }
            continue;
        }
        idle = 0;
        eventClear(usbHsEpGetXferEvent(&g.ep));
        u32 count = 0;
        rc = usbHsEpGetXferReport(&g.ep, reps, 32, &count);
        if (R_FAILED(rc)) {
            st->final_rc = rc;
            break;
        }
        u64 t = now_us();
        for (u32 k = 0; k < count && k < 32; k++) handle_report(&reps[k], t, deadline);
    }
    rc = 0;

out_ep:
    usbHsEpClose(&g.ep);   // also cancels transfers still in flight
    free(g.region);
    g.region = NULL;
out:
    if (!st->final_rc) st->final_rc = rc;
    log_file("iso: [%s] OpenUsbEp(addr 0x%02x attr 0x%02x maxpkt %u interval %u, maxUrbCount %u) -> 0x%X\n",
             g.strat->name, g.epd.bEndpointAddress, g.epd.bmAttributes, g.epd.wMaxPacketSize, g.epd.bInterval,
             p->max_urb_count, st->open_rc);
    if (p->use_smmu) log_file("iso: CreateSmmuSpace(%u bytes) -> 0x%X\n", g.slot * g.nb, st->smmu_rc);
    log_file("iso: GetCurrentFrame %u -> %u over 10 ms (0x%X), mask 0x%X, first start frame %u\n", st->frame0,
             st->frame1, g.frame_rc, g.frame_mask, g.first_frame);
    if (g.gave_up) log_file("iso: no completion for 1 s, gave up\n");
    if (R_FAILED(st->final_rc)) log_file("iso: final result 0x%X\n", st->final_rc);
    for (u32 i = 0; i < g.n_submit_recs; i++) {
        const iso_submit_rec *r = &g.submit_recs[i];
        log_file("iso: submit t=+%llu us id 0x%llx unk1 %u unk2 %u -> 0x%X xferId %u\n", (unsigned long long)r->t_us,
                 (unsigned long long)r->id, r->unk1, r->unk2, r->rc, r->xfer_id);
    }
    for (u32 i = 0; i < g.n_report_recs; i++) {
        const iso_report_rec *r = &g.report_recs[i];
        log_file("iso: report t=+%llu us xferId %u res 0x%X req %u done %u id 0x%llx\n", (unsigned long long)r->t_us,
                 r->xfer_id, r->res, r->requested, r->done, (unsigned long long)r->id);
    }
    log_file("iso: done: submitted %u (errors %u), completed %u, reports %u (errors %u), bytes %llu/%llu, "
             "timeouts %u, resyncs %u, report mode %u, max gap %llu us\n",
             st->submitted, st->submit_errors, st->completed, st->reports, st->report_errors,
             (unsigned long long)st->bytes_done, (unsigned long long)st->bytes_requested, st->timeouts,
             st->resyncs, st->report_mode, (unsigned long long)st->max_gap_us);
    st->done = true;
}

Result iso_start(UsbHsClientIfSession *as, const struct usb_endpoint_descriptor *ep, const iso_params *p,
                 iso_stats *st) {
    if (g.thread_active) return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    if (p->strategy < 0 || p->strategy >= g_iso_strategy_count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memset(st, 0, sizeof(*st));
    g.as = as;
    g.epd = *ep;
    g.p = *p;
    g.st = st;
    g.strat = &g_iso_strategies[p->strategy];
    g.stop = false;

    // Above the UI thread (0x2C) so console rendering can't starve the stream.
    Result rc = threadCreate(&g.thread, iso_thread, NULL, NULL, 0x8000, 0x24, -2);
    if (R_FAILED(rc)) rc = threadCreate(&g.thread, iso_thread, NULL, NULL, 0x8000, 0x2C, -2);
    if (R_FAILED(rc)) return rc;
    rc = threadStart(&g.thread);
    if (R_FAILED(rc)) {
        threadClose(&g.thread);
        return rc;
    }
    g.thread_active = true;
    return 0;
}

bool iso_running(void) { return g.thread_active && !g.st->done; }

void iso_wait(void) {
    if (!g.thread_active) return;
    threadWaitForExit(&g.thread);
    threadClose(&g.thread);
    g.thread_active = false;
}

void iso_stop(void) {
    g.stop = true;
    iso_wait();
}

void iso_summary(const iso_params *p, const iso_stats *st, char *out, size_t size) {
    double expect = p->packet_bytes * 1000.0;   // bytes per second
    double rate = 0.0;
    if (st->completed >= 2 && st->t_last_done > st->t_first_done)
        rate = (double)(st->bytes_done - st->bytes_at_first_done) * 1e6 / (double)(st->t_last_done - st->t_first_done);

    if (R_FAILED(st->open_rc))
        snprintf(out, size, "OpenUsbEp failed 0x%X", st->open_rc);
    else if (st->completed == 0 && st->submit_errors)
        snprintf(out, size, "submit rejected 0x%X", st->first_submit_rc);
    else if (st->completed == 0)
        snprintf(out, size, "no completions (%u timeouts)", st->timeouts);
    else if (st->bytes_done == 0)
        snprintf(out, size, "all transfers failed, res 0x%X", st->first_report_rc);
    else if (rate > 0 && rate < expect * 1.05 && rate > expect * 0.95)
        snprintf(out, size, "PACED %.1f kB/s (expect %.1f), err %u, resync %u, gap %llu ms", rate / 1000,
                 expect / 1000, st->report_errors, st->resyncs, (unsigned long long)(st->max_gap_us / 1000));
    else if (rate >= expect * 1.05)
        snprintf(out, size, "TOO FAST %.1f kB/s (expect %.1f): not paced like isochronous", rate / 1000, expect / 1000);
    else
        snprintf(out, size, "SLOW %.1f kB/s (expect %.1f), err %u, gap %llu ms", rate / 1000, expect / 1000,
                 st->report_errors, (unsigned long long)(st->max_gap_us / 1000));
}
