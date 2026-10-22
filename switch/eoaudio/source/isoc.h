#pragma once
#include "common.h"

// Continuous isochronous OUT stream, built on what eoprobe confirmed on HOS 22.1.0:
// usbHsEpBatchBufferAsync(unk1 = 0, unk2 = absolute 11-bit start frame), first start frame
// GetCurrentFrame + lead, then + urbs per batch; a frame outside the scheduling window fails at
// once with 0xD28C (2140-0105) and the schedule is moved forward from GetCurrentFrame.
// The audio for each batch is pulled from `fill` (stream: jitter buffer, tone: test tone) right
// before it is submitted. Nothing in the streaming thread touches the SD card.

typedef void (*iso_fill_fn)(void *ctx, int16_t *stereo, u32 frames);
typedef void (*iso_tick_fn)(void *ctx);   // about once per second, on the streaming thread

typedef struct {
    u16 max_urb_count;    // usbHsIfOpenUsbEp maxUrbCount (16)
    u16 urbs_per_batch;   // 1 ms packets per BatchBufferAsync (4)
    u16 batches;          // batches in flight (4)
    u32 packet_bytes;     // 192 = 48 kHz x 2 ch x 2 bytes per 1 ms frame
    u32 frame_lead;       // frames ahead of GetCurrentFrame for the first batch (16)
} iso_config;

typedef struct {   // cumulative since iso_start
    u64    batches, reports, report_errors, submits, submit_errors, resyncs, timeouts, frames;
    u32    max_gap_us;    // longest time between completions since the previous iso_take
    Result first_error;
} iso_stats;

// `as` must already be on the streaming alt setting. `tick` may be NULL.
Result iso_start(UsbHsClientIfSession *as, const struct usb_endpoint_descriptor *epd, const iso_config *cfg,
                 iso_fill_fn fill, iso_tick_fn tick, void *ctx);
void   iso_stop(void);             // request stop and join
bool   iso_active(void);           // thread started and not yet joined
bool   iso_exited(Result *why);    // the thread ended by itself (device gone, transfers failing)
void   iso_take(iso_stats *out);   // snapshot; resets max_gap_us
u64    iso_batches(void);          // completed batches since iso_start
bool   iso_failing(void);          // the last ISO_FAILING_BATCHES batches all failed (unplugged, most likely)
