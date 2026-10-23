#pragma once
#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USBHSLOG_VERSION "0.3.9"
#define APP_DIR          "sdmc:/switch/usbhs-logmitm"

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define PROGRAM_HID   0x0100000000000013ULL
#define PROGRAM_AUDIO 0x0100000000000014ULL

// Thread priorities (lower = more urgent; usbhslog.json allows 24..63). The mitm thread is above the
// rest: every intercepted client is blocked until it answers.
#define PRIO_MITM 0x26
#define PRIO_MAIN 0x2C   // main_thread_priority in usbhslog.json (44)
#define PRIO_LOG  0x3B
#define CORE_SYS  3

static inline u64 now_ms(void) { return armTicksToNs(armGetSystemTick()) / 1000000; }

static inline Result thread_spawn(Thread *t, ThreadFunc fn, void *arg, size_t stack, int prio) {
    Result rc = threadCreate(t, fn, arg, NULL, stack, prio, CORE_SYS);
    if (R_FAILED(rc)) return rc;
    rc = threadStart(t);
    if (R_FAILED(rc)) threadClose(t);
    return rc;
}
