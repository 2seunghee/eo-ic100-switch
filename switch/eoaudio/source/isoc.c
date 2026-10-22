#include "isoc.h"

#include "log.h"

#define ISO_MAX_BATCHES   16
#define ISO_MAX_URBS      16
#define ISO_WAIT_NS       20000000ULL   // completion wait per loop
#define ISO_STALL_US      1000000ULL    // no completed batch for this long: give up
#define ISO_FAILED_MAX    250           // consecutive failed batches (~1 s): give up
#define ISO_FAILING_BATCHES 25        // ~0.1 s of failed batches: report failing (speaker back on early)
#define RC_FRAME_WINDOW   0xD28C        // 2140-0105: start frame outside the scheduling window

typedef struct {
    bool busy;      // submitted, waiting for its reports
    bool filled;    // holds audio that has not been submitted yet
    u64  id;        // of the current submission: (sequence << 8) | batch index
    u32  reports, requested, done;
} iso_batch;

static struct {
    UsbHsClientIfSession          *as;
    struct usb_endpoint_descriptor epd;
    iso_config                     cfg;
    iso_fill_fn                    fill;
    iso_tick_fn                    tick;
    void                          *ctx;
    Thread                         thread;
    bool                           thread_ok;
    volatile bool                  stop, exited;
    Result                         why;

    UsbHsClientEpSession ep;
    u8       *region;
    u32       slot, batch_bytes, frames_per_batch;
    u16       upb, nb;
    u32       urbs[ISO_MAX_URBS];
    iso_batch b[ISO_MAX_BATCHES];
    u32       frame_next, frame_mask;
    bool      check_frame;
    u32       failed_in_row;
    u64       seq;
    u64       t_last_done, t_progress, t_tick;

    Mutex     mutex;   // st
    iso_stats st;
} I;

#define STAT(expr)                 \
    do {                           \
        mutexLock(&I.mutex);       \
        expr;                      \
        mutexUnlock(&I.mutex);     \
    } while (0)

static void note_error(Result rc) {
    if (!I.st.first_error) I.st.first_error = rc;
}

static void schedule_from_now(void) {
    u32 cur = 0;
    if (R_SUCCEEDED(usbHsIfGetCurrentFrame(I.as, &cur))) I.frame_next = cur + I.cfg.frame_lead;
}

// After a failed transfer: move the schedule forward if it fell behind the bus.
static void resync_frame(void) {
    u32 cur = 0;
    I.check_frame = false;
    if (R_FAILED(usbHsIfGetCurrentFrame(I.as, &cur))) return;
    u32 ahead = (I.frame_next - cur) & I.frame_mask;
    u32 half = (I.frame_mask >> 1) + 1;
    if (ahead >= half || ahead < 2) {
        I.frame_next = cur + I.cfg.frame_lead;
        STAT(I.st.resyncs++);
    }
}

static void submit(int bi) {
    iso_batch *b = &I.b[bi];
    u8 *buf = I.region + (size_t)bi * I.slot;
    if (!b->filled) {
        I.fill(I.ctx, (int16_t *)buf, I.frames_per_batch);
        armDCacheFlush(buf, I.batch_bytes);
        b->filled = true;
    }
    if (I.check_frame) resync_frame();
    for (int attempt = 0; attempt < 2; attempt++) {
        u64 id = (I.seq++ << 8) | (u64)bi;
        u32 xfer_id = 0;
        Result rc = usbHsEpBatchBufferAsync(&I.ep, buf, I.urbs, I.upb, id, 0, I.frame_next & I.frame_mask, &xfer_id);
        if (R_SUCCEEDED(rc)) {
            I.frame_next += I.upb;
            b->busy = true;
            b->filled = false;
            b->id = id;
            b->reports = b->requested = b->done = 0;
            STAT(I.st.submits++);
            return;
        }
        STAT({
            I.st.submits++;
            I.st.submit_errors++;
            note_error(rc);
        });
        if (rc != RC_FRAME_WINDOW || attempt) break;
        schedule_from_now();   // the start frame was already gone (or too far ahead): restart from the bus
        STAT(I.st.resyncs++);
    }
    b->busy = false;   // retried after the next completion or timeout
}

static void retry_idle(void) {
    for (int bi = 0; bi < I.nb && !I.stop; bi++)
        if (!I.b[bi].busy) {
            I.b[bi].filled = false;   // its audio is stale by now
            submit(bi);
        }
}

static void handle_report(const UsbHsXferReport *x, u64 t) {
    bool failed = R_FAILED(x->res);
    STAT({
        I.st.reports++;
        if (failed) {
            I.st.report_errors++;
            note_error(x->res);
        }
    });
    if (failed) I.check_frame = true;
    // One report per urb (seen on HOS 22.1.0); also accept one per batch. Reports of an earlier
    // submission of the same slot (late, after a failure) are counted above and otherwise ignored.
    int bi = (int)(x->id & 0xFF);
    if (bi >= I.nb || !I.b[bi].busy || I.b[bi].id != x->id) return;
    iso_batch *b = &I.b[bi];
    b->reports++;
    b->requested += x->requestedSize;
    b->done += x->transferredSize;
    if (b->requested < I.batch_bytes && b->reports < I.upb) return;

    b->busy = false;
    bool ok = b->done >= I.batch_bytes;
    I.failed_in_row = ok ? 0 : I.failed_in_row + 1;
    if (ok) I.t_progress = t;
    u32 gap = I.t_last_done ? (u32)(t - I.t_last_done) : 0;
    I.t_last_done = t;
    STAT({
        I.st.batches++;
        I.st.frames += b->done / 4;
        if (gap > I.st.max_gap_us) I.st.max_gap_us = gap;
    });
    if (!I.stop) submit(bi);
}

static void iso_thread(void *arg) {
    UsbHsXferReport reps[32];
    Result why = 0;
    I.upb = I.cfg.urbs_per_batch < 1 ? 1 : I.cfg.urbs_per_batch > ISO_MAX_URBS ? ISO_MAX_URBS : I.cfg.urbs_per_batch;
    I.nb = I.cfg.batches < 1 ? 1 : I.cfg.batches > ISO_MAX_BATCHES ? ISO_MAX_BATCHES : I.cfg.batches;
    I.batch_bytes = I.upb * I.cfg.packet_bytes;
    I.frames_per_batch = I.batch_bytes / 4;
    I.slot = ALIGN_UP(I.batch_bytes, 0x1000);
    for (int i = 0; i < ISO_MAX_URBS; i++) I.urbs[i] = I.cfg.packet_bytes;
    memset(I.b, 0, sizeof(I.b));
    I.check_frame = false;
    I.failed_in_row = 0;
    I.seq = 0;
    I.t_last_done = 0;

    memset(&I.ep, 0, sizeof(I.ep));
    Result rc = usbHsIfOpenUsbEp(I.as, &I.ep, I.cfg.max_urb_count, I.epd.wMaxPacketSize, &I.epd);
    if (R_FAILED(rc)) {
        log_printf("iso: OpenUsbEp(EP %02x, maxUrbCount %u, maxXferSize %u): %s", I.epd.bEndpointAddress,
                   I.cfg.max_urb_count, I.epd.wMaxPacketSize, rc_str(rc));
        why = rc;
        goto out;
    }
    I.region = memalign(0x1000, (size_t)I.slot * I.nb);
    if (!I.region) {
        why = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        goto out_ep;
    }
    memset(I.region, 0, (size_t)I.slot * I.nb);

    // 11-bit USB frame numbers wrap at 2048; otherwise treat the counter as free-running.
    u32 f0 = 0, f1 = 0;
    usbHsIfGetCurrentFrame(I.as, &f0);
    svcSleepThread(2000000ULL);
    usbHsIfGetCurrentFrame(I.as, &f1);
    I.frame_mask = (f0 < 2048 && f1 < 2048) ? 0x7FF : 0xFFFFFFFF;
    log_printf("iso: EP %02x open (attr %02x maxpkt %u), %u batches x %u urbs x %u B, frames %u -> %u in 2 ms, "
               "mask 0x%X, lead %u",
               I.epd.bEndpointAddress, I.epd.bmAttributes, I.epd.wMaxPacketSize, I.nb, I.upb, I.cfg.packet_bytes, f0,
               f1, I.frame_mask, I.cfg.frame_lead);

    // Read the frame right before the first submit: anything slow in between would put the first
    // start frame in the past.
    schedule_from_now();
    for (int bi = 0; bi < I.nb && !I.stop; bi++) submit(bi);
    I.t_progress = I.t_tick = now_us();

    while (!I.stop) {
        rc = eventWait(usbHsEpGetXferEvent(&I.ep), ISO_WAIT_NS);
        u64 t = now_us();
        if (R_SUCCEEDED(rc)) {
            eventClear(usbHsEpGetXferEvent(&I.ep));
            u32 count = 0;
            rc = usbHsEpGetXferReport(&I.ep, reps, 32, &count);
            if (R_FAILED(rc)) {
                log_printf("iso: GetXferReport: %s", rc_str(rc));
                why = rc;
                break;
            }
            for (u32 k = 0; k < count && k < 32; k++) handle_report(&reps[k], t);
        } else {
            STAT(I.st.timeouts++);
        }
        retry_idle();   // batches whose submit failed (a no-op while all are in flight)
        if (I.failed_in_row > ISO_FAILED_MAX) {
            why = I.st.first_error ? I.st.first_error : MAKERESULT(Module_Libnx, LibnxError_IoError);
            log_printf("iso: %u batches in a row failed, stopping", I.failed_in_row);
            break;
        }
        if (t - I.t_progress > ISO_STALL_US) {
            why = MAKERESULT(Module_Libnx, LibnxError_Timeout);
            log_printf("iso: no completed batch for %llu ms, stopping", (unsigned long long)((t - I.t_progress) / 1000));
            break;
        }
        if (I.tick && t - I.t_tick >= 1000000ULL) {
            I.t_tick = t;
            I.tick(I.ctx);
        }
    }

out_ep:
    usbHsEpClose(&I.ep);   // also cancels transfers still in flight; only then may the buffers go
    free(I.region);
    I.region = NULL;
out:
    I.why = why;
    I.exited = !I.stop;
    mutexLock(&I.mutex);
    iso_stats s = I.st;
    mutexUnlock(&I.mutex);
    log_printf("iso: stream ended (%s): %llu batches, %llu reports (%llu failed), %llu submits (%llu failed), "
               "%llu resyncs, %llu timeouts, first error %s",
               I.exited ? rc_str(why) : "stopped", (unsigned long long)s.batches, (unsigned long long)s.reports,
               (unsigned long long)s.report_errors, (unsigned long long)s.submits, (unsigned long long)s.submit_errors,
               (unsigned long long)s.resyncs, (unsigned long long)s.timeouts, rc_str(s.first_error));
}

Result iso_start(UsbHsClientIfSession *as, const struct usb_endpoint_descriptor *epd, const iso_config *cfg,
                 iso_fill_fn fill, iso_tick_fn tick, void *ctx) {
    if (I.thread_ok) return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    I.as = as;
    I.epd = *epd;
    I.cfg = *cfg;
    I.fill = fill;
    I.tick = tick;
    I.ctx = ctx;
    I.stop = false;
    I.exited = false;
    I.why = 0;
    mutexInit(&I.mutex);
    memset(&I.st, 0, sizeof(I.st));
    Result rc = thread_spawn(&I.thread, iso_thread, NULL, 0x4000, PRIO_ISO);
    I.thread_ok = R_SUCCEEDED(rc);
    return rc;
}

void iso_stop(void) {
    if (!I.thread_ok) return;
    I.stop = true;
    threadWaitForExit(&I.thread);
    threadClose(&I.thread);
    I.thread_ok = false;
}

bool iso_active(void) { return I.thread_ok; }

bool iso_exited(Result *why) {
    if (!I.thread_ok || !I.exited) return false;
    if (why) *why = I.why;
    return true;
}

void iso_take(iso_stats *out) {
    mutexLock(&I.mutex);
    *out = I.st;
    I.st.max_gap_us = 0;
    mutexUnlock(&I.mutex);
}

bool iso_failing(void) { return I.thread_ok && !I.exited && I.failed_in_row >= ISO_FAILING_BATCHES; }

u64 iso_batches(void) {
    mutexLock(&I.mutex);
    u64 n = I.st.batches;
    mutexUnlock(&I.mutex);
    return n;
}
