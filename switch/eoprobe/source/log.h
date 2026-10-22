#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <switch.h>

// Each run writes its own file, sdmc:/switch/eoprobe/logs/eoprobe_YYYYMMDD_HHMMSS.txt (ANSI colours stripped).
void log_init(void);
const char *log_path(void);
void log_exit(void);
void log_sync(void);   // flush + fsync; call before anything that might crash the console

// Console + file. Main thread only (the console is not thread-safe).
void log_both(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// File only. Safe from any thread.
void log_file(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Hex dump: file always, console only the first `console_bytes` bytes (0 = none).
void log_hex(const char *label, const void *data, size_t len, size_t console_bytes);

// "OK" or "0x1234 (2xxx-yyyy)". Rotating static buffers: main thread only.
const char *rc_str(Result rc);

bool save_file(const char *path, const void *data, size_t len);
