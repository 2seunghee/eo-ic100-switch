#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <switch.h>

// Asynchronous logger. log_printf() only formats into a RAM ring, so it is cheap and safe from any
// thread, the streaming threads included; a low-priority thread writes the ring to SD about twice a
// second. Nothing on the audio path ever waits for the SD card (an SD write once took ~30 ms and
// made eoprobe miss its isochronous start frame).
// One file per run: APP_DIR/logs/eoaudio_YYYYMMDD_HHMMSS.txt (eoaudio_runNNNN.txt without a clock).
void log_init(int keep_files);   // main thread, before other threads start
void log_exit(void);             // flush everything, stop the writer
const char *log_path(void);

// One record per call; a missing final '\n' is added. Lines are prefixed with the local time.
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// log_level=debug in config.ini: LOGD lines are written too.
void log_set_debug(bool on);
bool log_debug_on(void);
#define LOGD(...)                               \
    do {                                        \
        if (log_debug_on()) log_printf(__VA_ARGS__); \
    } while (0)
void log_hex(const char *label, const void *data, size_t len);
// Blocks (up to ~1 s) until everything logged so far is on the SD card. Not for streaming threads.
void log_sync(void);
// Exception handler only: writes logs/<run>_crash.txt with `text` and the newest 16 KiB of the log
// ring, including lines the writer has not put on the SD card yet. Takes no locks.
void log_crash(const char *text, size_t len);

// "OK" or "0x1234 (2xxx-yyyy)". Rotating buffers: any thread, valid for a few dozen calls.
const char *rc_str(Result rc);

bool save_file(const char *path, const void *data, size_t len);
// Reads a small file into buf and NUL-terminates it. Returns the length, or -1 if it can't be read.
long read_file(const char *path, char *buf, size_t size);
