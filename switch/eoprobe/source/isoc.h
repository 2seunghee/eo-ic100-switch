#pragma once
#include "common.h"
#include "tone.h"

// BatchBufferAsync's unk1/unk2 are undocumented (switchbrew: PostBufferAsync passes 0/0). The sweep
// on HOS 22.1.0 showed that for an isochronous endpoint unk1 = 0 with unk2 = absolute 11-bit start
// frame works (strategy 0). The other entries are kept so the sweep can be re-run on other firmware.
typedef enum { ISO_ARG_CONST = 0, ISO_ARG_FRAME } iso_arg_mode;

typedef struct {
    const char  *name;
    bool         single_post;   // one PostBufferAsync per 1 ms packet instead of BatchBufferAsync
    iso_arg_mode unk1_mode, unk2_mode;
    u32          unk1, unk2;    // used when the mode is ISO_ARG_CONST
} iso_strategy;

extern const iso_strategy g_iso_strategies[];
extern const int g_iso_strategy_count;

typedef struct {
    int   strategy;
    u16   max_urb_count;    // usbHsIfOpenUsbEp maxUrbCount, 1..16 (audio sysmodule uses 3 or 6)
    u16   urbs_per_batch;   // packets per BatchBufferAsync (one packet per 1 ms full-speed frame)
    u16   batches;          // batches kept in flight
    u32   packet_bytes;     // 192 = 48 kHz * 2 ch * 2 bytes per ms
    u32   frame_lead;       // frames ahead of GetCurrentFrame for frame-scheduled strategies
    bool  use_smmu;         // usbHsEpCreateSmmuSpace on the transfer buffers [4.0.0+]
    u32   duration_ms;
    float tone_hz, amplitude_dbfs;
    tone_pattern pattern;
} iso_params;

typedef struct {
    volatile bool   done;
    volatile Result open_rc, smmu_rc, first_submit_rc, first_report_rc, final_rc;
    volatile u32    submitted, submit_errors, completed, reports, report_errors, timeouts, resyncs;
    volatile u32    report_mode;          // 0 unknown, 1 one report per batch, 2 one per urb
    volatile u64    bytes_requested, bytes_done, bytes_at_first_done;
    volatile u64    t_start, t_first_done, t_last_done, max_gap_us;   // microseconds
    volatile u32    frame0, frame1;       // GetCurrentFrame 10 ms apart
} iso_stats;

void   iso_default_params(iso_params *p);
// Streams a tone on an interface that is already set to the right alt setting.
Result iso_start(UsbHsClientIfSession *as, const struct usb_endpoint_descriptor *ep, const iso_params *p,
                 iso_stats *st);
bool   iso_running(void);
void   iso_wait(void);   // join; the thread ends by itself after duration_ms
void   iso_stop(void);   // request stop, then join
// One-line verdict, e.g. "PACED 191.9 kB/s (expect 192.0): isochronous timing looks right".
void   iso_summary(const iso_params *p, const iso_stats *st, char *out, size_t size);
