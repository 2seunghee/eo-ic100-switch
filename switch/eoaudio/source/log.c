#include "log.h"

#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define LOG_RING     (64 * 1024)   // power of two: positions are free-running u32 counters
#define LOG_LINE     1024
#define LOG_CHUNK    8192
#define LOG_MAX_FILE (4u << 20)    // continue in <name>_partN.txt after 4 MB
#define LOG_MAX_LIST 256

static struct {
    Mutex         mutex;           // guards ring/head/tail/dropped
    Mutex         sd_mutex;        // serialises drain()
    char          ring[LOG_RING];
    u32           head, tail;
    u32           dropped;
    UEvent        wake;
    Thread        thread;
    bool          thread_ok;
    volatile bool stop;
    volatile u32  on_sd;           // ring position that is known to be on the SD card
    FILE         *f;
    char          path[128];
    char          run_name[40];
    u32           file_bytes;
    int           part;
    bool          clock_ok;
    u32           base_ms_of_day;
    u64           base_tick;
    int           pruned;
    volatile bool debug;
} L;

void log_set_debug(bool on) { L.debug = on; }
bool log_debug_on(void) { return L.debug; }

static void drain(void);

// ------------------------------------------------------------------ helpers

const char *rc_str(Result rc) {
    static char bufs[64][32];
    static u32 idx;
    char *b = bufs[__atomic_fetch_add(&idx, 1, __ATOMIC_RELAXED) & 63];
    if (R_SUCCEEDED(rc)) snprintf(b, 32, "OK");
    else snprintf(b, 32, "0x%X (%04u-%04u)", rc, 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return b;
}

bool save_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    return ok;
}

long read_file(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    return (long)n;
}

static int stamp(char *out, size_t size) {
    u64 ms = armTicksToNs(armGetSystemTick() - L.base_tick) / 1000000;
    if (!L.clock_ok)
        return snprintf(out, size, "+%05llu.%03llu ", (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
    u32 t = (u32)((L.base_ms_of_day + ms) % 86400000ULL);
    return snprintf(out, size, "%02u:%02u:%02u.%03u ", t / 3600000, t / 60000 % 60, t / 1000 % 60, t % 1000);
}

// ------------------------------------------------------------------ ring

static void ring_put(const char *s, u32 n) {
    u32 pos = L.head % LOG_RING;
    u32 first = n < LOG_RING - pos ? n : LOG_RING - pos;
    memcpy(L.ring + pos, s, first);
    memcpy(L.ring, s + first, n - first);
    L.head += n;
}

static void push(const char *s, u32 n) {
    mutexLock(&L.mutex);
    u32 used = L.head - L.tail;
    if (L.dropped && used + n + 64 <= LOG_RING) {
        char note[64];
        int k = snprintf(note, sizeof(note), "[log] %u record(s) lost, the ring was full\n", L.dropped);
        ring_put(note, (u32)k);
        used += (u32)k;
        L.dropped = 0;
    }
    if (used + n > LOG_RING) L.dropped++;
    else ring_put(s, n);
    bool wake = L.head - L.tail > LOG_RING / 2;
    mutexUnlock(&L.mutex);
    if (!L.thread_ok) drain();   // no writer thread (it failed to start): write synchronously
    else if (wake) ueventSignal(&L.wake);
}

void log_printf(const char *fmt, ...) {
    char line[LOG_LINE];
    int n = stamp(line, sizeof(line));
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    size_t len = (size_t)n + (size_t)(k < 0 ? 0 : k);
    if (len > sizeof(line) - 2) len = sizeof(line) - 2;   // truncated; keep room for '\n'
    if (line[len - 1] != '\n') line[len++] = '\n';
    push(line, (u32)len);
}

void log_hex(const char *label, const void *data, size_t len) {
    const u8 *p = data;
    log_printf("%s (%u bytes):", label, (unsigned)len);
    for (size_t i = 0; i < len; i += 16) {
        char hex[64];
        int n = 0;
        for (size_t k = i; k < i + 16 && k < len; k++) n += snprintf(hex + n, sizeof(hex) - (size_t)n, "%02x ", p[k]);
        log_printf("  %04x: %s", (unsigned)i, hex);
    }
}

// ------------------------------------------------------------------ SD side

static void open_file(void) {
    L.f = fopen(L.path, "a");
    L.file_bytes = 0;
    if (L.f) {
        fseek(L.f, 0, SEEK_END);
        long pos = ftell(L.f);
        L.file_bytes = pos > 0 ? (u32)pos : 0;
    }
}

static void file_write(const char *s, u32 n) {
    if (!L.f) return;
    if (L.file_bytes + n > LOG_MAX_FILE) {
        fclose(L.f);
        L.part++;
        snprintf(L.path, sizeof(L.path), APP_DIR "/logs/%s_part%d.txt", L.run_name, L.part);
        open_file();
        if (!L.f) return;
        char head[96];
        int k = snprintf(head, sizeof(head), "===== %s continued (part %d) =====\n", L.run_name, L.part);
        fwrite(head, 1, (size_t)k, L.f);
        L.file_bytes += (u32)k;
    }
    fwrite(s, 1, n, L.f);
    L.file_bytes += n;
}

static void drain(void) {
    static char chunk[LOG_CHUNK];
    bool any = false;
    u32 done;
    mutexLock(&L.sd_mutex);
    for (;;) {
        mutexLock(&L.mutex);
        u32 n = L.head - L.tail;
        if (n > LOG_CHUNK) n = LOG_CHUNK;
        u32 pos = L.tail % LOG_RING;
        u32 first = n < LOG_RING - pos ? n : LOG_RING - pos;
        memcpy(chunk, L.ring + pos, first);
        memcpy(chunk + first, L.ring, n - first);
        L.tail += n;
        done = L.tail;
        mutexUnlock(&L.mutex);
        if (!n) break;
        file_write(chunk, n);
        any = true;
    }
    if (any && L.f) {
        fflush(L.f);
        fsync(fileno(L.f));
    }
    __atomic_store_n(&L.on_sd, done, __ATOMIC_RELEASE);
    mutexUnlock(&L.sd_mutex);
}

static void writer_thread(void *arg) {
    while (!L.stop) {
        waitSingle(waiterForUEvent(&L.wake), 500000000ULL);
        drain();
    }
    drain();
}

void log_sync(void) {
    if (!L.thread_ok) return;
    mutexLock(&L.mutex);
    u32 target = L.head;
    mutexUnlock(&L.mutex);
    ueventSignal(&L.wake);
    for (int i = 0; i < 500 && (s32)(__atomic_load_n(&L.on_sd, __ATOMIC_ACQUIRE) - target) < 0; i++)
        svcSleepThread(2000000ULL);
}

void log_crash(const char *text, size_t len) {
    char path[128];
    snprintf(path, sizeof(path), APP_DIR "/logs/%s_crash.txt", L.run_name[0] ? L.run_name : "eoaudio");
    FILE *f = fopen(path, "w");
    if (!f) return;
    fwrite(text, 1, len, f);
    static const char sep[] = "---- newest log lines (some may not be in the main log yet) ----\n";
    fwrite(sep, 1, sizeof(sep) - 1, f);
    u32 head = L.head;
    u32 n = head < 16384 ? head : 16384;
    u32 pos = (head - n) % LOG_RING;
    u32 first = n < LOG_RING - pos ? n : LOG_RING - pos;
    fwrite(L.ring + pos, 1, first, f);
    fwrite(L.ring, 1, n - first, f);
    fclose(f);
}

static int cmp_names(const void *a, const void *b) { return strcmp(a, b); }

// Keeps the newest keep-1 logs so that, with the file about to be created, `keep` remain.
static void prune_old(int keep) {
    static char names[LOG_MAX_LIST][48];
    if (keep <= 0) return;
    DIR *d = opendir(APP_DIR "/logs");
    if (!d) return;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < LOG_MAX_LIST) {
        size_t len = strlen(e->d_name);
        if (len < 12 || len >= sizeof(names[0]) || strncmp(e->d_name, "eoaudio_", 8) != 0 ||
            strcmp(e->d_name + len - 4, ".txt") != 0)
            continue;
        memcpy(names[n++], e->d_name, len + 1);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_names);
    for (int i = 0; i < n - (keep - 1); i++) {
        char p[128];
        snprintf(p, sizeof(p), APP_DIR "/logs/%.47s", names[i]);
        if (unlink(p) == 0) L.pruned++;
    }
}

void log_init(int keep_files) {
    mutexInit(&L.mutex);
    mutexInit(&L.sd_mutex);
    ueventCreate(&L.wake, true);
    L.base_tick = armGetSystemTick();
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    mkdir(APP_DIR "/logs", 0777);

    char started[32] = "unknown time";
    time_t t = time(NULL);
    struct tm tmv;
    L.clock_ok = localtime_r(&t, &tmv) && tmv.tm_year + 1900 >= 2020;
    if (L.clock_ok) {
        L.base_ms_of_day = (u32)(tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec) * 1000;
        strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", &tmv);
        strftime(L.run_name, sizeof(L.run_name), "eoaudio_%Y%m%d_%H%M%S", &tmv);
    } else {
        for (int n = 1; n < 10000; n++) {
            snprintf(L.run_name, sizeof(L.run_name), "eoaudio_run%04d", n);
            snprintf(L.path, sizeof(L.path), APP_DIR "/logs/%s.txt", L.run_name);
            struct stat st;
            if (stat(L.path, &st) != 0) break;
        }
    }
    prune_old(keep_files);
    snprintf(L.path, sizeof(L.path), APP_DIR "/logs/%s.txt", L.run_name);
    open_file();

    u32 v = hosversionGet();
    log_printf("===== eoaudio %s  started %s  HOS %u.%u.%u =====", EOAUDIO_VERSION, started, HOSVER_MAJOR(v),
               HOSVER_MINOR(v), HOSVER_MICRO(v));
    if (L.pruned) log_printf("log: removed %d old log file(s), keeping %d", L.pruned, keep_files);

    L.stop = false;
    L.thread_ok = R_SUCCEEDED(thread_spawn(&L.thread, writer_thread, NULL, 0x4000, PRIO_LOG));
    if (!L.thread_ok) drain();
}

const char *log_path(void) { return L.f ? L.path : "(log file could not be opened)"; }

void log_exit(void) {
    if (L.thread_ok) {
        L.stop = true;
        ueventSignal(&L.wake);
        threadWaitForExit(&L.thread);
        threadClose(&L.thread);
        L.thread_ok = false;
    }
    drain();
    if (L.f) fclose(L.f);
    L.f = NULL;
}
