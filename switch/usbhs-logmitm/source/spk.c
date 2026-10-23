#include "spk.h"

#include <malloc.h>
#include <sys/stat.h>

#include "common.h"
#include "log.h"

// The EO-IC100's FM radio on the console speaker, in the background (deny_mic=1). eofm and its
// overlay put the receiver in recording mode, which sends the radio to the USB microphone (IF1),
// and write SPK_REQUEST_PATH ("on <gain 0..15>"). A control thread here sees that file and starts
// the capture: IF1 alt 1, SET_CUR 48 kHz, batches of isochronous IN transfers on EP 0x83 into a
// ring, and audout plays the ring on the console. deny_mic keeps the audio sysmodule off IF1.
// eofm 0.3.0-0.3.3 did this in the app itself, so it stopped with the app.
//
// Memory is taken only while the speaker runs: this process starts at boot, before the audio
// sysmodule, and the system pool is nearly full then.

#define REQ_PATH    APP_DIR "/speaker.txt"          // written by eofm / eofm-ovl
#define STATUS_PATH APP_DIR "/speaker_status.txt"   // written here, read by them
#define MODE_PATH   APP_DIR "/mode.txt"             // "speaker" or "earphone", written by eofm / eofm-ovl
#define MODE_STATUS APP_DIR "/mode_status.txt"      // the mode in force, written here

// The output mode. Earphone: the descriptor rewrite (config rewrite=) makes the earphone the USB
// output, so everything plays there, the radio inside the earphone. Speaker: no rewrite, and the
// audio sysmodule may not take the microphone (deny_mic), so everything plays on the console and
// the radio comes from IF1 (this file). The mitm thread only reads this variable: the file is read
// here, never between a forward and its reply (0.2.2-0.3.3). A new mode applies when the audio
// sysmodule reads the earphone again: eofm re-enumerates it after asking.
static volatile bool g_speaker_mode;

bool spk_speaker_mode(void) { return g_speaker_mode; }

void spk_init_mode(bool speaker) { g_speaker_mode = speaker; }
#define POLL_MS     300

#define PKT        192      // 1 ms of 48 kHz 16-bit stereo
#define UPB        8        // urbs per batch
#define NB         8        // batches in flight (64 ms; eofm 0.3.1 lost ~17% with 3 x 5)
#define MAX_URBS   16       // usbHsIfOpenUsbEp maxUrbCount, 1..16; it counts batches
#define SLOT       0x1000
#define FRAME_LEAD 16
#define RING       12000    // frames (250 ms)
#define OUT_FRAMES 960      // 20 ms per audout buffer
#define OUT_SIZE   0x1000
#define N_OUT      4
#define TARGET_FILL (48000 * 60 / 1000)   // keep ~60 ms queued in the ring
#define RC_FRAME_WINDOW 0xD28C

typedef struct {
    bool busy;
    u64  id;
    u32  reports;
    u32  got[UPB];
} batch;

static struct {
    Thread        ctl_thread, cap_thread, play_thread;
    bool          usb_init, running, cap_ok, play_ok, audout_ok, if_ok, ep_ok;
    volatile bool stop;
    volatile int  gain;          // 0..256
    Mutex         lock;
    UsbHsClientIfSession ifs;
    UsbHsClientEpSession ep;
    struct usb_endpoint_descriptor epd;
    u8           *region;        // NB slots of SLOT bytes
    batch         b[NB];
    u32           urbs[UPB];
    u32           frame_next;
    u64           seq;
    int16_t      *ring;          // RING stereo frames
    u32           wpos, rpos;
    AudioOutBuffer out[N_OUT];
    bool          out_busy[N_OUT];
    void         *out_mem[N_OUT];
    u64           frames;
    int16_t       peak;
    u32           restarts, submit_fail, short_urbs, bad_urbs, underruns;
    char          error[96];
    volatile bool failed;        // the capture or the output gave up: stop, and say so
} P;

static void set_gain(int vol15) { P.gain = vol15 <= 0 ? 0 : vol15 >= 15 ? 256 : vol15 * vol15 * 256 / 225; }

static void write_status(const char *text) {
    FILE *f = fopen(STATUS_PATH, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static void fail(const char *what, Result rc) {
    snprintf(P.error, sizeof(P.error), "%s%s%s", what, rc ? " " : "", rc ? rc_str(rc) : "");
    log_printf("speaker: %s", P.error);
    P.failed = true;
}

// ------------------------------------------------------------------ USB capture

static bool find_if1(UsbHsInterface *out) {
    static UsbHsInterface list[8];
    UsbHsInterfaceFilter f;
    memset(&f, 0, sizeof(f));
    f.Flags = UsbHsInterfaceFilterFlags_idVendor;
    f.idVendor = 0x04E8;
    s32 n = 0;
    if (R_FAILED(usbHsQueryAvailableInterfaces(&f, list, sizeof(list), &n))) return false;
    for (s32 i = 0; i < n; i++) {
        u16 pid = list[i].device_desc.idProduct;
        if ((pid == 0xA054 || pid == 0xA059 || pid == 0xA05B) && list[i].inf.interface_desc.bInterfaceNumber == 1) {
            *out = list[i];
            return true;
        }
    }
    return false;
}

static void submit(int bi) {
    for (int attempt = 0; attempt < 2; attempt++) {
        u64 id = (P.seq++ << 8) | (u64)bi;
        u32 xfer = 0;
        u8 *buf = P.region + (size_t)bi * SLOT;
        armDCacheFlush(buf, SLOT);
        Result rc = usbHsEpBatchBufferAsync(&P.ep, buf, P.urbs, UPB, id, 0, P.frame_next & 0x7FF, &xfer);
        if (R_SUCCEEDED(rc)) {
            P.frame_next += UPB;
            P.b[bi] = (batch){.busy = true, .id = id};
            return;
        }
        if (rc != RC_FRAME_WINDOW || attempt) {
            if (P.submit_fail++ < 5) log_printf("speaker: BatchBufferAsync: %s", rc_str(rc));
            break;
        }
        P.restarts++;
        u32 cur = 0;   // the start frame is gone: restart from the bus
        usbHsIfGetCurrentFrame(&P.ifs, &cur);
        P.frame_next = cur + FRAME_LEAD;
    }
    P.b[bi].busy = false;
}

static void ring_put(const u8 *data, u32 bytes) {
    u32 n = bytes / 4;
    const int16_t *p = (const int16_t *)data;
    for (u32 i = 0; i < n; i++) {
        int16_t a = p[2 * i] < 0 ? (int16_t)-(p[2 * i] + 1) : p[2 * i];
        if (a > P.peak) P.peak = a;
        if (P.wpos - P.rpos >= RING) P.rpos++;   // full: drop the oldest
        u32 k = P.wpos % RING;
        P.ring[2 * k] = p[2 * i];
        P.ring[2 * k + 1] = p[2 * i + 1];
        P.wpos++;
    }
}

static void on_report(const UsbHsXferReport *r, u64 *last_ok) {
    int bi = (int)(r->id & 0xFF);
    if (bi >= NB || !P.b[bi].busy || P.b[bi].id != r->id) return;
    batch *b = &P.b[bi];
    u32 got = R_SUCCEEDED(r->res) ? r->transferredSize : 0;
    if (R_FAILED(r->res)) P.bad_urbs++;
    else if (got < PKT) P.short_urbs++;
    if (b->reports < UPB) b->got[b->reports] = got;
    if (++b->reports < UPB) return;
    u8 *buf = P.region + (size_t)bi * SLOT;   // urb i's data is at i * PKT
    armDCacheFlush(buf, SLOT);
    u32 total = 0;
    mutexLock(&P.lock);
    for (int i = 0; i < UPB; i++) {
        u32 n = b->got[i] > PKT ? PKT : b->got[i];
        ring_put(buf + i * PKT, n);
        total += n;
    }
    P.frames += total / 4;
    mutexUnlock(&P.lock);
    if (total) *last_ok = now_ms();
    b->busy = false;
    if (!P.stop) submit(bi);
}

static void cap_loop(void *arg) {
    UsbHsXferReport reps[32];
    u64 last_ok = now_ms();
    u32 cur = 0;
    usbHsIfGetCurrentFrame(&P.ifs, &cur);
    P.frame_next = cur + FRAME_LEAD;
    for (int bi = 0; bi < NB; bi++) submit(bi);
    while (!P.stop) {
        if (R_FAILED(eventWait(usbHsEpGetXferEvent(&P.ep), 20000000ULL))) {
            if (now_ms() - last_ok > 3000) {
                fail("no sound from the earphone for 3 s (unplugged?)", 0);
                return;
            }
            for (int bi = 0; bi < NB; bi++)
                if (!P.b[bi].busy) submit(bi);
            continue;
        }
        eventClear(usbHsEpGetXferEvent(&P.ep));
        // Up to NB * UPB urb reports can be waiting: read until a short read (the event is cleared).
        u32 count = 32;
        while (count == 32 && !P.stop) {
            count = 0;
            Result rc = usbHsEpGetXferReport(&P.ep, reps, 32, &count);
            if (R_FAILED(rc)) {
                fail("GetXferReport", rc);
                return;
            }
            for (u32 k = 0; k < count && k < 32; k++) on_report(&reps[k], &last_ok);
        }
    }
}

static bool usb_open(void) {
    UsbHsInterface it;
    if (!find_if1(&it)) {
        fail("the microphone interface (IF1) is not free: is deny_mic=1 set and the earphone in?", 0);
        return false;
    }
    Result rc = usbHsAcquireUsbIf(&P.ifs, &it);
    if (R_FAILED(rc)) {
        fail("acquire IF1", rc);
        return false;
    }
    P.if_ok = true;
    rc = usbHsIfSetInterface(&P.ifs, NULL, 1);   // alt 1: 48 kHz 16-bit stereo, EP 0x83
    if (R_FAILED(rc)) {
        fail("SetInterface(alt 1)", rc);
        return false;
    }
    struct usb_endpoint_descriptor *epd = NULL;
    for (int e = 0; e < 15 && !epd; e++) {
        struct usb_endpoint_descriptor *x = &P.ifs.inf.inf.input_endpoint_descs[e];
        if (x->bLength && (x->bmAttributes & USB_TRANSFER_TYPE_MASK) == USB_TRANSFER_TYPE_ISOCHRONOUS) epd = x;
    }
    if (!epd) {
        fail("no isochronous IN endpoint on IF1", 0);
        return false;
    }
    P.epd = *epd;
    // SET_CUR sampling frequency 48000 on the endpoint (class request, OUT, endpoint recipient)
    u8 *cb = memalign(0x1000, 0x1000);
    if (cb) {
        memset(cb, 0, 0x1000);
        cb[0] = 0x80;
        cb[1] = 0xBB;
        armDCacheFlush(cb, 0x1000);
        u32 got = 0;
        rc = usbHsIfCtrlXfer(&P.ifs, 0x22, 0x01, 0x0100, P.epd.bEndpointAddress, 3, cb, &got);
        log_printf("speaker: SET_CUR 48000 on EP %02x: %s", P.epd.bEndpointAddress, rc_str(rc));
        free(cb);
    }
    rc = usbHsIfOpenUsbEp(&P.ifs, &P.ep, MAX_URBS, P.epd.wMaxPacketSize, &P.epd);
    if (R_FAILED(rc)) {
        fail("open EP 0x83", rc);
        return false;
    }
    P.ep_ok = true;
    for (int i = 0; i < UPB; i++) P.urbs[i] = PKT;
    return true;
}

static void usb_close(void) {
    if (P.ep_ok) usbHsEpClose(&P.ep);   // cancels the transfers in flight; only then may the buffers go
    P.ep_ok = false;
    if (P.if_ok) {
        usbHsIfSetInterface(&P.ifs, NULL, 0);
        usbHsIfClose(&P.ifs);
    }
    P.if_ok = false;
}

// ------------------------------------------------------------------ output

static void play_loop(void *arg) {
    u64 last_report = now_ms();
    bool primed = false;
    while (!P.stop) {
        AudioOutBuffer *rel = NULL;
        u32 n = 0;
        while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&rel, &n)) && n && rel) {
            for (int k = 0; k < N_OUT; k++)
                if (&P.out[k] == rel) P.out_busy[k] = false;
            rel = NULL;
        }
        int queued = 0, free_k = -1;
        for (int k = 0; k < N_OUT; k++) {
            if (P.out_busy[k]) queued++;
            else if (free_k < 0) free_k = k;
        }
        mutexLock(&P.lock);
        u32 fill = P.wpos - P.rpos;
        if (fill > TARGET_FILL * 3) P.rpos = P.wpos - TARGET_FILL;   // too far behind the radio: catch up
        fill = P.wpos - P.rpos;
        if (primed && !queued && fill < OUT_FRAMES) {   // ran dry: refill before playing again
            primed = false;
            P.underruns++;
        }
        if (!primed && fill >= TARGET_FILL) primed = true;
        bool take = primed && free_k >= 0 && queued < 3 && fill >= OUT_FRAMES;
        if (take) {
            int16_t *o = P.out_mem[free_k];
            int g = P.gain;
            for (u32 i = 0; i < OUT_FRAMES; i++) {
                u32 k = (P.rpos + i) % RING;
                o[2 * i] = (int16_t)(P.ring[2 * k] * g / 256);
                o[2 * i + 1] = (int16_t)(P.ring[2 * k + 1] * g / 256);
            }
            P.rpos += OUT_FRAMES;
        }
        int peak = P.peak;
        if (now_ms() - last_report >= 10000) P.peak = 0;
        mutexUnlock(&P.lock);
        if (take) {
            P.out[free_k].data_size = OUT_FRAMES * 4;
            P.out_busy[free_k] = true;
            audoutAppendAudioOutBuffer(&P.out[free_k]);
        } else {
            svcSleepThread(4000000ULL);
        }
        if (now_ms() - last_report >= 10000) {
            last_report = now_ms();
            log_printf("speaker: %llu s played, peak %d, ring %u; restarts %u, failed submits %u, short urbs %u, failed urbs %u, "
                       "underruns %u", (unsigned long long)(P.frames / 48000), peak, fill, P.restarts, P.submit_fail,
                       P.short_urbs, P.bad_urbs, P.underruns);
        }
    }
}

// ------------------------------------------------------------------ start / stop

static void spk_stop(void) {
    if (!P.running && !P.if_ok && !P.audout_ok) return;
    P.stop = true;
    if (P.cap_ok) {
        threadWaitForExit(&P.cap_thread);
        threadClose(&P.cap_thread);
        P.cap_ok = false;
    }
    if (P.play_ok) {
        threadWaitForExit(&P.play_thread);
        threadClose(&P.play_thread);
        P.play_ok = false;
    }
    usb_close();
    if (P.audout_ok) {
        audoutStopAudioOut();
        audoutExit();
        P.audout_ok = false;
    }
    for (int k = 0; k < N_OUT; k++) {
        free(P.out_mem[k]);
        P.out_mem[k] = NULL;
    }
    free(P.region);
    P.region = NULL;
    free(P.ring);
    P.ring = NULL;
    if (P.running) log_printf("speaker: stopped after %llu s", (unsigned long long)(P.frames / 48000));
    P.running = false;
}

static bool spk_start(void) {
    P.stop = P.failed = false;
    P.error[0] = 0;
    P.wpos = P.rpos = 0;
    P.seq = P.frames = 0;
    P.peak = 0;
    P.restarts = P.submit_fail = P.short_urbs = P.bad_urbs = P.underruns = 0;
    memset(P.b, 0, sizeof(P.b));
    P.ring = malloc((size_t)RING * 4);
    P.region = memalign(0x1000, (size_t)SLOT * NB);
    bool mem = P.ring && P.region;
    for (int k = 0; k < N_OUT && mem; k++) {
        P.out_mem[k] = memalign(0x1000, OUT_SIZE);
        if (!P.out_mem[k]) mem = false;
        else {
            memset(P.out_mem[k], 0, OUT_SIZE);
            P.out[k] = (AudioOutBuffer){.next = NULL, .buffer = P.out_mem[k], .buffer_size = OUT_SIZE, .data_size = 0};
            P.out_busy[k] = false;
        }
    }
    if (!mem) {
        fail("out of memory", 0);
        spk_stop();
        return false;
    }
    memset(P.region, 0, (size_t)SLOT * NB);
    if (!usb_open()) {
        spk_stop();
        return false;
    }
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        fail("audout", rc);
        spk_stop();
        return false;
    }
    P.audout_ok = true;
    rc = audoutStartAudioOut();
    log_printf("speaker: audout %u Hz, %u ch, start %s", audoutGetSampleRate(), audoutGetChannelCount(), rc_str(rc));
    rc = thread_spawn(&P.cap_thread, cap_loop, NULL, 0x2000, PRIO_MITM + 1);
    P.cap_ok = R_SUCCEEDED(rc);
    if (P.cap_ok) {
        rc = thread_spawn(&P.play_thread, play_loop, NULL, 0x2000, PRIO_MITM + 2);
        P.play_ok = R_SUCCEEDED(rc);
    }
    if (!P.cap_ok || !P.play_ok) {
        fail("threads", rc);
        spk_stop();
        return false;
    }
    P.running = true;
    log_printf("speaker: running");
    return true;
}

// ------------------------------------------------------------------ the request file

// "on <gain>" -> true and the gain; anything else (or no file) -> false.
static bool read_request(int *gain) {
    char buf[32] = {0};
    FILE *f = fopen(REQ_PATH, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (strncmp(buf, "on", 2)) return false;
    *gain = buf[2] ? atoi(buf + 2) : 15;
    return true;
}

static bool read_mode(bool fallback) {
    char buf[16] = {0};
    FILE *f = fopen(MODE_PATH, "r");
    if (!f) return fallback;
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return !strncmp(buf, "speaker", 7) ? true : !strncmp(buf, "earphone", 8) ? false : fallback;
}

static void write_mode_status(bool speaker) {
    FILE *f = fopen(MODE_STATUS, "w");
    if (!f) return;
    fputs(speaker ? "speaker\n" : "earphone\n", f);
    fclose(f);
}

static void ctl_loop(void *arg) {
    bool boot_speaker = (bool)(uintptr_t)arg;
    Result rc = usbHsInitialize();
    P.usb_init = R_SUCCEEDED(rc);
    log_printf("speaker: service ready (usb:hs %s); requests in %s", rc_str(rc), REQ_PATH);
    remove(REQ_PATH);   // a request from before this boot is stale: the receiver is off now
    remove(MODE_PATH);   // every boot starts in the configured mode (earphone unless deny_mic=1)
    write_status("off\n");
    write_mode_status(g_speaker_mode);
    int last_gain = -1;
    for (;;) {
        svcSleepThread(POLL_MS * 1000000ULL);
        bool m = read_mode(boot_speaker);
        if (m != g_speaker_mode) {
            g_speaker_mode = m;
            log_printf("mode: %s (applies when the earphone is read again)", m ? "speaker" : "earphone");
            if (!m && P.running) {
                spk_stop();
                remove(REQ_PATH);
                write_status("off\n");
            }
            write_mode_status(m);
        }
        int gain = 15;
        bool want = P.usb_init && read_request(&gain);
        if (want && gain != last_gain) {
            set_gain(gain);
            last_gain = gain;
        }
        if (P.failed) {   // the capture gave up (unplugged...): off until asked again
            spk_stop();
            remove(REQ_PATH);
            char s[128];
            snprintf(s, sizeof(s), "error %s\n", P.error);
            write_status(s);
            P.failed = false;
            continue;
        }
        if (want && !P.running) {
            log_printf("speaker: requested (gain %d)", gain);
            if (spk_start()) {
                write_status("running\n");
            } else {
                char s[128];
                snprintf(s, sizeof(s), "error %s\n", P.error);
                write_status(s);
                remove(REQ_PATH);
                P.failed = false;
            }
        } else if (!want && P.running) {
            spk_stop();
            write_status("off\n");
        }
    }
}

void spk_service_start(bool boot_speaker) {
    mutexInit(&P.lock);
    Result rc = thread_spawn(&P.ctl_thread, ctl_loop, (void *)(uintptr_t)boot_speaker, 0x2000, PRIO_MAIN);
    log_printf("speaker: control thread %s", rc_str(rc));
}
