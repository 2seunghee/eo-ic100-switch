// usbhs-logmitm: approaches C-0 and C of the EO-IC100 project (docs/PLAN.md §5).
//
// A sysmodule that puts itself between the audio sysmodule and usb:hs (Atmosphère mitm), forwards
// every request unchanged and logs it, so that the log shows where Nintendo's USB audio driver gives
// up on the EO-IC100. The one change (approach C, rewrite= in config.ini) is to the EO-IC100's
// configuration descriptor as the audio sysmodule receives it (mitm.c rewrite_ctrl_data).
//
// Start-up order matters: with sd_extra/mitm.lst in the program folder, boot2 declares a future mitm
// for usb:hs, and every client of usb:hs (audio, hid, bsdsockets) waits in GetServiceHandle until
// this process has installed the mitm and cleared the declaration. So the mitm goes up first, before
// the clock and the log file.
#include "common.h"
#include "config.h"
#include "log.h"
#include "mitm.h"
#include "remote.h"
#include "rewrite.h"

#define INNER_HEAP_SIZE 0x60000   // thread stacks (2 x 16 KiB), stdio, the config text
#define CONFIG_PATH     APP_DIR "/config.ini"
#define STATUS_MS       60000ULL

// ------------------------------------------------------------------ libnx sysmodule boilerplate

u32 __nx_applet_type = AppletType_None;   // sysmodules must not use applet services
u32 __nx_fs_num_sessions = 1;
void __libnx_init_time(void);              // libnx: sets the local time zone for localtime()

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

static bool g_fs_ok, g_sd_ok, g_time_ok;

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    // No abort without the SD card: the mitm still has to come up (or clear the mitm.lst
    // declaration), or every usb:hs client waits forever.
    g_fs_ok = R_SUCCEEDED(fsInitialize());
    g_sd_ok = g_fs_ok && R_SUCCEEDED(fsdevMountSdmc());
}

void __appExit(void) {
    if (g_sd_ok) fsdevUnmountAll();
    if (g_fs_ok) fsExit();
    if (g_time_ok) timeExit();
    smExit();
}

// ------------------------------------------------------------------ main

static void config_warn(const char *msg) { log_printf("%s", msg); }

static void log_config(const lm_config *c) {
    char progs[200];
    int n = snprintf(progs, sizeof(progs), "%s%s", c->all ? "all " : "", c->homebrew ? "homebrew " : "");
    for (int i = 0; i < c->n_programs && n < (int)sizeof(progs) - 20; i++)
        n += snprintf(progs + n, sizeof(progs) - (size_t)n, "%016llx ", (unsigned long long)c->programs[i]);
    char svcs[32];
    snprintf(svcs, sizeof(svcs), "%s%s%s", c->services[0], c->n_services > 1 ? "," : "", c->n_services > 1 ? c->services[1] : "");
    char rw[80];
    rewrite_names(c->rewrite, rw, sizeof(rw));
    log_printf("config: enabled=%d services=%s programs=%sread_ctrl_data=%d sync=%d hot_detail=%d buf_dump=%d log_keep=%d "
               "rewrite=%s remote=%d",
               c->enabled, svcs, progs, c->read_ctrl_data, c->sync, c->hot_detail, c->buf_dump, c->log_keep, rw, c->remote);
}

int main(int argc, char **argv) {
    static lm_config cfg;
    static char text[4096];
    log_init_early();

    u32 v = hosversionGet();
    log_printf("usbhs-logmitm %s: HOS %u.%u.%u, SD %s", USBHSLOG_VERSION, HOSVER_MAJOR(v), HOSVER_MINOR(v),
               HOSVER_MICRO(v), g_sd_ok ? "OK" : "NOT MOUNTED");
    // An override is an app's request for this boot only (eofm's speaker output). One left by a crash
    // kept the earphone away from the audio sysmodule after the reboot, so it goes now.
    if (g_sd_ok && remove(APP_DIR "/rewrite_override.txt") == 0) log_printf("removed a leftover rewrite_override.txt");
    if (g_sd_ok && read_file(CONFIG_PATH, text, sizeof(text)) >= 0) {
        int problems = config_parse(&cfg, text, config_warn);
        if (problems) log_printf("config: %d problem(s), see above", problems);
    } else {
        config_defaults(&cfg);
        log_printf("config: %s not readable, using defaults", CONFIG_PATH);
    }

    int installed = 0;
    if (cfg.enabled) installed = mitm_start(&cfg);
    else log_printf("enabled=0 in config.ini: no mitm, the process only idles");

    if (R_SUCCEEDED(timeInitialize())) {
        __libnx_init_time();
        g_time_ok = true;
    }
    if (g_sd_ok) log_open(cfg.log_keep, g_time_ok);
    log_config(&cfg);
    log_printf("%d of %d service(s) intercepted. Log: %s", installed, cfg.n_services, log_path());
    log_sync();
    if (cfg.enabled && cfg.remote) remote_start();

    // The mitm thread does the work. This process never exits on its own: an intercepted client's
    // sessions end with it, and the audio sysmodule would not survive that.
    u64 next = now_ms() + STATUS_MS;
    for (;;) {
        svcSleepThread(1000000000ULL);
        if (now_ms() >= next) {
            next = now_ms() + STATUS_MS;   // not += : after sleep mode it caught up once a second
            char line[256];
            mitm_status(line, sizeof(line));
            log_printf("%s", line);
        }
    }
    return 0;
}
