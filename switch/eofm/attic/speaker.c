#include "speaker.h"

#include <math.h>

#include "log.h"
#include "usbio.h"

// Speaker output, third design (0.3.0). The first went through the system audio input (audin) and
// crashed the audio sysmodule (eofm 0.1.6); the second had usbhs-logmitm hide the earphone from it,
// which crashed it too (0.2.0). Now usbhs-logmitm (0.3.5, deny_mic=1, rewrite=none) only lets the
// audio sysmodule's AcquireUsbIf of the microphone fail, so:
//  - the console has no USB output (no terminal rewrite) and plays on its speaker;
//  - the earphone's microphone interface (IF1) is free: this file takes it, sets 48 kHz and reads
//    the isochronous IN endpoint 0x83 itself, the same way the audio sysmodule does it (batches of
//    5 urbs x 192 bytes = 1 ms each, scheduled by absolute USB frame numbers: approach A, read side);
//  - the PCM goes to audout through a small ring, scaled by the app's volume (spk_set_gain).

#define PKT        192      // 1 ms of 48 kHz 16-bit stereo
#define UPB        8        // urbs per batch
#define NB         8        // batches in flight (64 ms). 0.3.1 had 3 x 5 and got only ~83% of the
                            // stream (4.1 s per 5 s): the batches ran dry between completions.
#define MAX_URBS   16       // usbHsIfOpenUsbEp maxUrbCount, 1..16 (0.3.0 asked for 20: 2140-0101). It
                            // counts transfers (batches): the audio sysmodule streams 5-urb batches with 3.
#define SLOT       0x1000
#define FRAME_LEAD 16
#define RING       (48000 / 2)   // frames (0.5 s)
#define OUT_FRAMES 960           // 20 ms per audout buffer
#define OUT_SIZE   0x1000
#define N_OUT      6
#define TARGET_FILL (48000 * 60 / 1000)   // keep ~60 ms queued in the ring
#define RC_FRAME_WINDOW 0xD28C

typedef struct {
    bool busy;
    u64  id;
    u32  reports;
    u32  got[UPB];
} batch;

static struct {
    Mutex         lock;
    spk_status    st;
    // USB side
    UsbHsClientIfSession ifs;
    bool          if_ok, ep_ok;
    UsbHsClientEpSession ep;
    struct usb_endpoint_descriptor epd;
    u8           *region;
    batch         b[NB];
    u32           urbs[UPB];
    u32           frame_next;
    u64           seq;
    // ring of stereo frames between the capture and the output
    int16_t       ring[RING * 2];
    u32           wpos, rpos;   // free-running frame counters
    // output side
    bool          audout_ok, audctl_ok;
    AudioOutBuffer out[N_OUT];
    bool          out_busy[N_OUT];
    void         *out_mem[N_OUT];
    // threads
    Thread        cap_thread, play_thread;
    bool          cap_ok, play_ok;
    volatile bool stop;
    int16_t       peak;
    u64           peak_since;
    volatile int  gain;   // output scale, 0..256 (256 = as captured)
    // diagnostics, logged every 5 s
    u32           restarts, submit_fail, short_urbs, bad_urbs, underruns;
} S;

void spk_set_gain(int vol15) { S.gain = vol15 <= 0 ? 0 : vol15 >= 15 ? 256 : vol15 * vol15 * 256 / 225; }

static void set_error(const char *msg, Result rc) {
    mutexLock(&S.lock);
    snprintf(S.st.error, sizeof(S.st.error), "%s%s%s", msg, rc ? ": " : "", rc ? rc_str(rc) : "");
    mutexUnlock(&S.lock);
    log_file("speaker: %s\n", S.st.error);
}

static void track_peak(const int16_t *p, size_t samples) {
    for (size_t i = 0; i < samples; i++) {
        int16_t v = p[i] < 0 ? (int16_t)(p[i] == -32768 ? 32767 : -p[i]) : p[i];
        if (v > S.peak) S.peak = v;
    }
    u64 t = now_ms();
    if (t - S.peak_since >= 1000) {   // the caller holds S.lock
        S.st.peak_db = S.peak ? (int)lrintf(20.0f * log10f(S.peak / 32768.0f)) : -99;
        S.peak = 0;
        S.peak_since = t;
    }
}

// ------------------------------------------------------------------ USB capture

static bool find_if1(UsbHsInterface *out) {
    static UsbHsInterface list[8];
    UsbHsInterfaceFilter f;
    memset(&f, 0, sizeof(f));
    f.Flags = UsbHsInterfaceFilterFlags_idVendor;
    f.idVendor = TARGET_VID;
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
        u64 id = (S.seq++ << 8) | (u64)bi;
        u32 xfer = 0;
        u8 *buf = S.region + (size_t)bi * SLOT;
        armDCacheFlush(buf, SLOT);
        Result rc = usbHsEpBatchBufferAsync(&S.ep, buf, S.urbs, UPB, id, 0, S.frame_next & 0x7FF, &xfer);
        if (R_SUCCEEDED(rc)) {
            S.frame_next += UPB;
            S.b[bi] = (batch){.busy = true, .id = id};
            return;
        }
        if (rc != RC_FRAME_WINDOW || attempt) {
            if (S.submit_fail++ < 5) log_file("speaker: BatchBufferAsync: %s\n", rc_str(rc));
            break;
        }
        S.restarts++;
        u32 cur = 0;   // the start frame is gone: restart from the bus
        usbHsIfGetCurrentFrame(&S.ifs, &cur);
        S.frame_next = cur + FRAME_LEAD;
    }
    S.b[bi].busy = false;
}

static void ring_put(const u8 *data, u32 bytes) {
    u32 frames = bytes / 4;
    const int16_t *p = (const int16_t *)data;
    track_peak(p, frames * 2);
    for (u32 i = 0; i < frames; i++) {
        if (S.wpos - S.rpos >= RING) S.rpos++;   // full: drop the oldest
        u32 k = S.wpos % RING;
        S.ring[2 * k] = p[2 * i];
        S.ring[2 * k + 1] = p[2 * i + 1];
        S.wpos++;
    }
}

// One urb report. When the batch has all its reports, its data goes to the ring and it is sent again.
static void on_report(const UsbHsXferReport *r, u64 *last_ok) {
    int bi = (int)(r->id & 0xFF);
    if (bi >= NB || !S.b[bi].busy || S.b[bi].id != r->id) return;
    batch *b = &S.b[bi];
    u32 got = R_SUCCEEDED(r->res) ? r->transferredSize : 0;
    if (R_FAILED(r->res)) S.bad_urbs++;
    else if (got < PKT) S.short_urbs++;
    if (b->reports < UPB) b->got[b->reports] = got;
    if (++b->reports < UPB) return;
    // the batch is complete: urb i's data is at i * PKT
    u8 *buf = S.region + (size_t)bi * SLOT;
    armDCacheFlush(buf, SLOT);
    u32 total = 0;
    mutexLock(&S.lock);
    for (int i = 0; i < UPB; i++) {
        u32 n = b->got[i] > PKT ? PKT : b->got[i];
        ring_put(buf + i * PKT, n);
        total += n;
    }
    S.st.frames += total / 4;
    mutexUnlock(&S.lock);
    if (total) *last_ok = now_ms();
    b->busy = false;
    if (!S.stop) submit(bi);
}

static void cap_loop(void *arg) {
    UsbHsXferReport reps[32];
    u64 last_ok = now_ms();
    u32 cur = 0;
    usbHsIfGetCurrentFrame(&S.ifs, &cur);
    S.frame_next = cur + FRAME_LEAD;
    for (int bi = 0; bi < NB; bi++) submit(bi);
    while (!S.stop) {
        if (R_FAILED(eventWait(usbHsEpGetXferEvent(&S.ep), 20000000ULL))) {
            if (now_ms() - last_ok > 2000 && !S.st.error[0]) set_error("2초 동안 이어폰 소리가 오지 않음", 0);
            for (int bi = 0; bi < NB; bi++)
                if (!S.b[bi].busy) submit(bi);
            continue;
        }
        eventClear(usbHsEpGetXferEvent(&S.ep));
        // Up to NB * UPB urb reports can be waiting: read until a short read, since the event is
        // already cleared for the ones a single read leaves behind.
        u32 count = 32;
        while (count == 32 && !S.stop) {
            count = 0;
            if (R_FAILED(usbHsEpGetXferReport(&S.ep, reps, 32, &count))) return;
            for (u32 k = 0; k < count && k < 32; k++) on_report(&reps[k], &last_ok);
        }
    }
}

static bool usb_open(void) {
    UsbHsInterface it;
    if (!find_if1(&it)) {
        set_error("마이크 통로가 비어 있지 않음 (드라이버 deny_mic=1 필요)", 0);
        return false;
    }
    Result rc = usbHsAcquireUsbIf(&S.ifs, &it);
    log_file("speaker: acquire IF1: %s\n", rc_str(rc));
    if (R_FAILED(rc)) {
        set_error("마이크 통로를 잡지 못함", rc);
        return false;
    }
    S.if_ok = true;
    rc = usbHsIfSetInterface(&S.ifs, NULL, 1);   // alt 1: 48 kHz 16-bit stereo, EP 0x83
    log_file("speaker: SetInterface(alt 1): %s\n", rc_str(rc));
    if (R_FAILED(rc)) {
        set_error("마이크 48 kHz 설정 실패", rc);
        return false;
    }
    struct usb_endpoint_descriptor *epd = NULL;
    for (int e = 0; e < 15 && !epd; e++) {
        struct usb_endpoint_descriptor *x = &S.ifs.inf.inf.input_endpoint_descs[e];
        if (x->bLength && (x->bmAttributes & USB_TRANSFER_TYPE_MASK) == USB_TRANSFER_TYPE_ISOCHRONOUS) epd = x;
    }
    if (!epd) {
        set_error("마이크 엔드포인트 없음", 0);
        return false;
    }
    S.epd = *epd;
    u8 rate[3] = {0x80, 0xBB, 0x00};   // SET_CUR sampling frequency 48000 on the endpoint
    rc = usbio_ctrl(&S.ifs, 0x22, 0x01, 0x0100, S.epd.bEndpointAddress, 3, rate, NULL);
    log_file("speaker: SET_CUR 48000 on EP %02x: %s\n", S.epd.bEndpointAddress, rc_str(rc));
    rc = usbHsIfOpenUsbEp(&S.ifs, &S.ep, MAX_URBS, S.epd.wMaxPacketSize, &S.epd);
    log_file("speaker: open EP %02x (mps %u): %s\n", S.epd.bEndpointAddress, S.epd.wMaxPacketSize, rc_str(rc));
    if (R_FAILED(rc)) {
        set_error("마이크 엔드포인트 열기 실패", rc);
        return false;
    }
    S.ep_ok = true;
    S.region = memalign(0x1000, (size_t)SLOT * NB);
    if (!S.region) {
        set_error("메모리 부족", 0);
        return false;
    }
    memset(S.region, 0, (size_t)SLOT * NB);
    for (int i = 0; i < UPB; i++) S.urbs[i] = PKT;
    return true;
}

static void usb_close(void) {
    if (S.ep_ok) usbHsEpClose(&S.ep);   // cancels the transfers in flight; only then may the buffers go
    S.ep_ok = false;
    free(S.region);
    S.region = NULL;
    if (S.if_ok) {
        usbHsIfSetInterface(&S.ifs, NULL, 0);
        usbHsIfClose(&S.ifs);
    }
    S.if_ok = false;
}

// ------------------------------------------------------------------ output

static void play_loop(void *arg) {
    u64 last_report = now_ms();
    bool primed = false;
    while (!S.stop) {
        AudioOutBuffer *rel = NULL;
        u32 n = 0;
        while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&rel, &n)) && n && rel) {
            for (int k = 0; k < N_OUT; k++)
                if (&S.out[k] == rel) S.out_busy[k] = false;
            rel = NULL;
        }
        int queued = 0, free_k = -1;
        for (int k = 0; k < N_OUT; k++) {
            if (S.out_busy[k]) queued++;
            else if (free_k < 0) free_k = k;
        }
        mutexLock(&S.lock);
        u32 fill = S.wpos - S.rpos;
        if (fill > TARGET_FILL * 3) S.rpos = S.wpos - TARGET_FILL;   // too far behind the radio: catch up
        fill = S.wpos - S.rpos;
        // Start (and restart after running dry) only with TARGET_FILL in the ring, so that the
        // output does not stop and go every 20 ms on a thin ring.
        if (primed && !queued && fill < OUT_FRAMES) {
            primed = false;
            S.underruns++;
        }
        if (!primed && fill >= TARGET_FILL) primed = true;
        bool take = primed && free_k >= 0 && queued < 3 && fill >= OUT_FRAMES;
        if (take) {
            int16_t *o = S.out_mem[free_k];
            int g = S.gain;
            for (u32 i = 0; i < OUT_FRAMES; i++) {
                u32 k = (S.rpos + i) % RING;
                o[2 * i] = (int16_t)(S.ring[2 * k] * g / 256);
                o[2 * i + 1] = (int16_t)(S.ring[2 * k + 1] * g / 256);
            }
            S.rpos += OUT_FRAMES;
        }
        mutexUnlock(&S.lock);
        if (take) {
            S.out[free_k].data_size = OUT_FRAMES * 4;
            S.out_busy[free_k] = true;
            audoutAppendAudioOutBuffer(&S.out[free_k]);
        } else {
            svcSleepThread(4000000ULL);
        }
        if (now_ms() - last_report >= 5000) {
            last_report = now_ms();
            log_file("speaker: %.1f s captured, peak %d dBFS, ring %u frames; restarts %u, failed submits %u, short urbs %u, "
                     "failed urbs %u, output underruns %u\n", S.st.frames / 48000.0, S.st.peak_db, fill, S.restarts,
                     S.submit_fail, S.short_urbs, S.bad_urbs, S.underruns);
        }
    }
}

static void output_target(void) {
    S.audctl_ok = R_SUCCEEDED(audctlInitialize());
    AudioTarget t = AudioTarget_Invalid;
    if (S.audctl_ok) audctlGetActiveOutputTarget(&t);
    S.st.before = S.st.now = t;
    S.st.moved_output = false;
    log_file("speaker: active output target %d\n", t);
}

// ------------------------------------------------------------------ public

bool spk_start(void) {
    if (S.st.running) return true;
    mutexLock(&S.lock);
    memset(&S.st, 0, sizeof(S.st));
    S.st.peak_db = -99;
    mutexUnlock(&S.lock);
    S.stop = false;
    S.wpos = S.rpos = 0;
    S.seq = 0;
    memset(S.b, 0, sizeof(S.b));
    S.restarts = S.submit_fail = S.short_urbs = S.bad_urbs = S.underruns = 0;
    output_target();

    if (!usb_open()) {
        spk_stop();
        return false;
    }
    Result rc = audoutInitialize();
    S.audout_ok = R_SUCCEEDED(rc);
    if (!S.audout_ok) {
        set_error("audout 사용 불가", rc);
        spk_stop();
        return false;
    }
    for (int k = 0; k < N_OUT; k++) {
        S.out_mem[k] = memalign(0x1000, OUT_SIZE);
        if (!S.out_mem[k]) {
            set_error("메모리 부족", 0);
            spk_stop();
            return false;
        }
        memset(S.out_mem[k], 0, OUT_SIZE);
        S.out[k] = (AudioOutBuffer){.next = NULL, .buffer = S.out_mem[k], .buffer_size = OUT_SIZE, .data_size = 0};
        S.out_busy[k] = false;
    }
    rc = audoutStartAudioOut();
    log_file("speaker: audout start %s\n", rc_str(rc));
    S.peak_since = now_ms();
    rc = threadCreate(&S.cap_thread, cap_loop, NULL, NULL, 0x4000, 0x2A, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&S.cap_thread);
    S.cap_ok = R_SUCCEEDED(rc);
    if (S.cap_ok) {
        rc = threadCreate(&S.play_thread, play_loop, NULL, NULL, 0x4000, 0x2B, -2);
        if (R_SUCCEEDED(rc)) rc = threadStart(&S.play_thread);
        S.play_ok = R_SUCCEEDED(rc);
    }
    if (!S.cap_ok || !S.play_ok) {
        set_error("오디오 스레드 실패", rc);
        spk_stop();
        return false;
    }
    S.st.running = true;
    return true;
}

void spk_stop(void) {
    S.stop = true;
    if (S.cap_ok) {
        threadWaitForExit(&S.cap_thread);
        threadClose(&S.cap_thread);
        S.cap_ok = false;
    }
    if (S.play_ok) {
        threadWaitForExit(&S.play_thread);
        threadClose(&S.play_thread);
        S.play_ok = false;
    }
    usb_close();
    if (S.audout_ok) {
        audoutStopAudioOut();
        audoutExit();
        S.audout_ok = false;
    }
    for (int k = 0; k < N_OUT; k++) {
        free(S.out_mem[k]);
        S.out_mem[k] = NULL;
    }
    if (S.audctl_ok) audctlExit();
    S.audctl_ok = false;
    if (S.st.running) log_file("speaker: stopped after %.1f s\n", S.st.frames / 48000.0);
    S.st.running = false;
}

void spk_get(spk_status *out) {
    mutexLock(&S.lock);
    *out = S.st;
    mutexUnlock(&S.lock);
}
