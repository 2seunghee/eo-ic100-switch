// The parts of eofm's log.h that fm.c uses. One file, opened and closed per line so it can be
// copied over MTP while the overlay runs; started afresh when it grows past 64 KiB.
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

#include "log.h"

#define LOG_PATH "sdmc:/switch/eofm/logs/eofm-ovl.txt"

void log_file(const char *fmt, ...) {
    if (!fsdevGetDeviceFileSystem("sdmc")) return;   // not mounted: stdio on sdmc: crashes here
    static bool checked;
    if (!checked) {   // needs sdmc: mounted (initServices does it first)
        checked = true;
        mkdir("sdmc:/switch/eofm", 0777);
        mkdir("sdmc:/switch/eofm/logs", 0777);
        struct stat st;
        if (stat(LOG_PATH, &st) == 0 && st.st_size > 64 * 1024) remove(LOG_PATH);
    }
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    u64 ms = armTicksToNs(armGetSystemTick()) / 1000000;
    fprintf(f, "%llu.%03llu ", (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

const char *rc_str(Result rc) {
    static char bufs[4][32];
    static int k;
    char *b = bufs[k++ & 3];
    if (R_SUCCEEDED(rc)) return "OK";
    snprintf(b, 32, "0x%x (%04u-%04u)", rc, 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return b;
}
