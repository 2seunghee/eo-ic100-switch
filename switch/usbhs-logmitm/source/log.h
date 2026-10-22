#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <switch.h>

// Logger (derived from eoaudio's). log_printf() formats into a RAM ring and is safe from any thread.
// Lines logged before log_open() stay in the ring: the mitm is installed before the log file exists,
// so that boot-time clients of usb:hs wait as little as possible.
// One file per run: APP_DIR/logs/usbhslog_YYYYMMDD_HHMMSS.txt (usbhslog_runNNNN.txt without a clock).
void log_init_early(void);                      // first thing in main(), before any log_printf()
void log_open(int keep_files, bool clock_ok);   // main thread, after the SD card and the clock
void log_exit(void);
const char *log_path(void);

void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_hex(const char *indent, const void *data, size_t len);
// Blocks (up to ~1 s) until everything logged so far is on the SD card (fsync). No-op before log_open().
void log_sync(void);
// Exception handler only: writes logs/<run>_crash.txt with `text` and the newest log lines.
void log_crash(const char *text, size_t len);

// "OK" or "0x1234 (2xxx-yyyy)". Rotating buffers: any thread, valid for a few dozen calls.
const char *rc_str(Result rc);
long read_file(const char *path, char *buf, size_t size);
