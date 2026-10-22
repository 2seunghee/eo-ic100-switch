#include "log.h"

#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

static FILE *g_log;
static Mutex g_log_mutex;
static char  g_log_path[128];

static void write_stripped(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '\x1b') {
            while (*p && *p != 'm') p++;
            if (!*p) break;
            continue;
        }
        fputc(*p, g_log);
    }
    fflush(g_log);
}

// One file per run: APP_DIR/logs/eoprobe_YYYYMMDD_HHMMSS.txt (or eoprobe_runNNNN.txt without a valid clock).
void log_init(void) {
    mutexInit(&g_log_mutex);
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    mkdir(APP_DIR "/logs", 0777);

    char stamp[32] = "?";
    time_t t = time(NULL);
    struct tm tmv;
    bool clock_ok = localtime_r(&t, &tmv) && tmv.tm_year + 1900 >= 2020;
    if (clock_ok) {
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
        char name[32];
        strftime(name, sizeof(name), "%Y%m%d_%H%M%S", &tmv);
        snprintf(g_log_path, sizeof(g_log_path), APP_DIR "/logs/eoprobe_%s.txt", name);
    }
    for (int n = 1; !clock_ok && n < 10000; n++) {
        snprintf(g_log_path, sizeof(g_log_path), APP_DIR "/logs/eoprobe_run%04d.txt", n);
        FILE *probe = fopen(g_log_path, "r");
        if (!probe) break;
        fclose(probe);
    }
    g_log = fopen(g_log_path, "a");

    u32 v = hosversionGet();
    log_file("===== eoprobe %s  %s  HOS %u.%u.%u =====\n", EOPROBE_VERSION, stamp, HOSVER_MAJOR(v),
             HOSVER_MINOR(v), HOSVER_MICRO(v));
}

const char *log_path(void) { return g_log ? g_log_path : "(log file could not be opened)"; }

void log_exit(void) {
    if (g_log) fclose(g_log);
    g_log = NULL;
}

void log_sync(void) {
    mutexLock(&g_log_mutex);
    if (g_log) {
        fflush(g_log);
        fsync(fileno(g_log));
    }
    mutexUnlock(&g_log_mutex);
}

void log_both(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    mutexLock(&g_log_mutex);
    if (g_log) write_stripped(buf);
    mutexUnlock(&g_log_mutex);
}

void log_file(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mutexLock(&g_log_mutex);
    if (g_log) write_stripped(buf);
    mutexUnlock(&g_log_mutex);
}

void log_hex(const char *label, const void *data, size_t len, size_t console_bytes) {
    const u8 *p = data;
    log_file("%s (%u bytes):\n", label, (unsigned)len);
    for (size_t i = 0; i < len; i += 16) {
        char line[80];
        int n = 0;
        for (size_t k = i; k < i + 16 && k < len; k++) n += snprintf(line + n, sizeof(line) - n, "%02x ", p[k]);
        log_file("  %04x: %s\n", (unsigned)i, line);
    }
    if (console_bytes) {
        printf("  %s:", label);
        for (size_t k = 0; k < len && k < console_bytes; k++) printf(" %02x", p[k]);
        printf("%s\n", len > console_bytes ? " ..." : "");
    }
}

const char *rc_str(Result rc) {
    static char bufs[4][40];
    static unsigned idx;
    char *b = bufs[idx++ & 3];
    if (R_SUCCEEDED(rc)) snprintf(b, 40, "OK");
    else snprintf(b, 40, "0x%X (%04u-%04u)", rc, 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return b;
}

bool save_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}
