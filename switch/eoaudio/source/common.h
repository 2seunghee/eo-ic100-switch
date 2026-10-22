#pragma once
#include <switch.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbdesc.h"

#define EOAUDIO_VERSION "0.5.0"
#define APP_DIR         "sdmc:/switch/eoaudio"
#define TARGET_VID      0x04E8   // Samsung; EO-IC100 PIDs seen so far: a054, a059, a05b

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

// Thread priorities (lower = more urgent; eoaudio.json allows 24..63). Everything runs on core 3
// next to the other system services, so a game busy on cores 0-2 never competes with the stream.
#define PRIO_ISO    0x24
#define PRIO_REC    0x26
#define PRIO_MAIN   0x28   // main_thread_priority in eoaudio.json (40): races for the earphone
#define PRIO_REMOTE 0x2E
#define PRIO_LOG    0x3B
#define CORE_SYS    3

static inline u64 ticks_to_us(u64 t) { return armTicksToNs(t) / 1000; }
static inline u64 now_us(void) { return ticks_to_us(armGetSystemTick()); }
static inline u64 now_ms(void) { return now_us() / 1000; }

static inline Result thread_spawn(Thread *t, ThreadFunc fn, void *arg, size_t stack, int prio) {
    Result rc = threadCreate(t, fn, arg, NULL, stack, prio, CORE_SYS);
    if (R_FAILED(rc)) return rc;
    rc = threadStart(t);
    if (R_FAILED(rc)) threadClose(t);
    return rc;
}

// Process ID of the running application, 0 if none (refreshed by the main loop every 2 s).
u64 app_pid(void);
