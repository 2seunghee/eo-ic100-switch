#pragma once
#include "common.h"
#include "config.h"

// usb:hs side of the earphone, driven from the main loop.
//   idle      -> an interface of 04e8 with a PID from pids= becomes available: acquire everything at
//                once (streaming interfaces first). The audio/hid sysmodules only claim the earphone at
//                arrival while a game runs, so the first to acquire keeps it until it is unplugged.
//                Any other device is only listed in the log, never acquired.
//   settling  -> plug-in re-enumerates ~3 times within ~5 s: every lost instance is raced again;
//                configuration waits until nothing has changed for settle_ms
//   ready     -> descriptors read, IF2 alt1 + SET_CUR 48000 + FU volume (stream/tone), audio path
//                started through the hooks, remote thread running
//   held      -> configured but not streaming: another output is active (see dev_set_hold)
//   failed    -> e.g. the system holds the playback interface and ResetDevice did not help:
//                wait for the next plug-in
typedef enum { DEV_IDLE = 0, DEV_SETTLING, DEV_READY, DEV_HELD, DEV_FAILED } dev_state;

typedef struct {
    // Start the audio path on `as` (already on the streaming alt setting). Return false on failure.
    bool (*start)(void *ctx, UsbHsClientIfSession *as, const struct usb_endpoint_descriptor *epd, u32 packet_bytes);
    void (*stop)(void *ctx);                 // before the interface sessions are closed
    bool (*alive)(void *ctx, Result *why);   // false once the audio path ended by itself
    // Remote Vol+/Vol- (remote thread). Return true if handled (console volume); false = FU steps.
    bool (*volume_key)(void *ctx, int dir, const char *why);
    // Middle button with play_button=media (remote thread).
    void (*play_key)(void *ctx);
} dev_hooks;

void        dev_init(const eo_config *cfg, bool stream, float volume_db, const dev_hooks *hooks, void *ctx);
void        dev_exit(void);
int         dev_waiters(Waiter *out, int max);   // events for the main loop's waitObjects()
u64         dev_timeout_ns(void);                 // longest the main loop may sleep
void        dev_step(bool usb_event);             // after every wake-up; usb_event: one of our waiters fired
dev_state   dev_get_state(void);
const char *dev_state_name(dev_state s);
bool        dev_streaming(void);                  // the audio path is running
void        dev_set_hold(bool hold, const char *why);   // main thread: no stream while held
void        dev_status(char *out, size_t size);   // one line for the periodic log

// Earphone output volume: FU5 on the device, digital gain if FU5 is missing. Any thread.
void        dev_volume_steps(int steps, const char *why);   // +-volume_step_db per step
void        dev_volume_set_db(float db, const char *why);
void        dev_volume_show(void);   // the earphone volume bar on screen, for the current volume
void        dev_mute_toggle(const char *why);
void        dev_volume_refresh(const char *why);   // after volume_min/max/step changed
bool        dev_take_volume_change(float *db);   // true once after a change (main saves it to state.ini)
u32         dev_output_gain_q15(void);           // applied to the stream: unity unless muted / no FU5
