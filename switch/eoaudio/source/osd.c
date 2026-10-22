#include "osd.h"

#include "log.h"

// Exactly Tesla's layer geometry (libtesla Renderer::init): a 448x720 framebuffer on a layer that is
// the left strip of the screen, 672x1080 in 1920x1080 layer coordinates, at 0,0. A smaller layer of
// our own shape was created fine but never appeared on screen while a game ran.
#define FB_W     448
#define FB_H     720
#define LAYER_W  672
#define LAYER_H  1080
#define LAYER_X  0
#define LAYER_Y  0
#define PANEL_X  16      // the bar, in framebuffer pixels (x1.5 on screen): top left, like the console's
#define PANEL_Y  14
#define PANEL_W  288
#define PANEL_H  48
#define SHOW_MS  1500
#define MAX_LEVELS 30
#define MAX_FAILURES 3   // give up on the overlay after this many failed layer creations

// The framebuffers do not come from eoaudio's own heap: they live in the process heap region
// (svcSetHeapSize), taken from the system pool only while the bar is shown and given back after.
// The system pool is shared with every sysmodule and the Tesla overlay loader and has only a few
// MiB free; a fixed 3.5 MiB heap left too little to open the Ultrahand menu.
#define FB_STRIDE  ALIGN_UP(FB_W * 2, 64)   // GOBs are 64 bytes wide
#define FB_HALIGN  ALIGN_UP(FB_H, 128)      // block height 16 GOBs
#define FB_SIZE    (FB_STRIDE * FB_HALIGN)
#define FB_COUNT   2
#define FB_REGION  0x200000                 // svcSetHeapSize granularity: 2 framebuffers + the linear copy
_Static_assert(FB_SIZE * (FB_COUNT + 1) <= FB_REGION, "framebuffers do not fit the heap region");

extern u64 __nx_vi_layer_id;   // libnx: viCreateLayer opens this layer instead of a stray one

static struct {
    Thread      thread;
    UEvent      wake;
    Mutex       mutex;
    bool        pending, muted;
    float       db, lo, hi, step;
    bool        vi_ok, nv_ok, layer_on, given_up, visible, have_vsync;
    Event       vsync;
    int         failures;
    ViDisplay   display;
    ViLayer     layer;
    NWindow     win;
    Framebuffer fb;
    bool        nvmap_ok, fence_ok;
    void       *region;   // FB_REGION bytes of process heap, NULL while given back
    u64         hide_at;
} O;

// framebufferCreate + framebufferMakeLinear, on memory of the heap region (libnx would malloc it).
// framebufferBegin/End work on the result unchanged; fb_destroy replaces framebufferClose.
static Result fb_create(const char **step) {
    Result rc = 0;
    if (!O.region) {
        *step = "svcSetHeapSize";
        void *addr = NULL;
        rc = svcSetHeapSize(&addr, FB_REGION);
        if (R_FAILED(rc)) return rc;
        O.region = addr;
    }
    *step = "nvMapInit";
    if (!O.nvmap_ok) O.nvmap_ok = R_SUCCEEDED(rc = nvMapInit());
    if (R_SUCCEEDED(rc) && !O.fence_ok) {
        *step = "nvFenceInit";
        O.fence_ok = R_SUCCEEDED(rc = nvFenceInit());
    }
    if (R_FAILED(rc)) return rc;
    // As framebufferCreate does: framebufferEnd copies win->width x win->height pixels from the
    // linear buffer, so without this nothing reached the screen (v0.3.0).
    *step = "nwindowSetDimensions";
    rc = nwindowSetDimensions(&O.win, FB_W, FB_H);
    if (R_FAILED(rc)) return rc;
    memset(&O.fb, 0, sizeof(O.fb));
    *step = "nvMapCreate";
    rc = nvMapCreate(&O.fb.map, O.region, FB_SIZE * FB_COUNT, 0x20000, NvKind_Pitch, true);
    if (R_FAILED(rc)) return rc;

    NvGraphicBuffer gb = {0};
    gb.header.num_ints = (sizeof(NvGraphicBuffer) - sizeof(NativeHandle)) / 4;
    gb.unk0 = -1;
    gb.nvmap_id = nvMapGetId(&O.fb.map);
    gb.magic = 0xDAFFCAFF;
    gb.pid = 42;
    gb.usage = GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE;
    gb.format = PIXEL_FORMAT_RGBA_4444;
    gb.ext_format = PIXEL_FORMAT_RGBA_4444;
    gb.stride = FB_STRIDE / 2;   // in pixels
    gb.total_size = FB_SIZE;
    gb.num_planes = 1;
    gb.planes[0].width = FB_W;
    gb.planes[0].height = FB_H;
    gb.planes[0].color_format = NvColorFormat_A4B4G4R4;
    gb.planes[0].layout = NvLayout_BlockLinear;
    gb.planes[0].pitch = FB_STRIDE;
    gb.planes[0].kind = NvKind_Generic_16BX2;
    gb.planes[0].block_height_log2 = 4;
    gb.planes[0].size = FB_SIZE;
    *step = "nwindowConfigureBuffer";
    for (int i = 0; i < FB_COUNT && R_SUCCEEDED(rc); i++) {
        gb.planes[0].offset = (u32)(i * FB_SIZE);
        rc = nwindowConfigureBuffer(&O.win, i, &gb);
    }
    if (R_FAILED(rc)) {
        nwindowReleaseBuffers(&O.win);
        nvMapClose(&O.fb.map);
        return rc;
    }
    O.fb.win = &O.win;
    O.fb.buf = O.region;
    O.fb.buf_linear = (u8 *)O.region + FB_SIZE * FB_COUNT;
    memset(O.fb.buf_linear, 0, FB_SIZE);
    O.fb.stride = FB_STRIDE;
    O.fb.width_aligned = FB_STRIDE / 2;
    O.fb.height_aligned = FB_HALIGN;
    O.fb.num_fbs = FB_COUNT;
    O.fb.fb_size = FB_SIZE;
    O.fb.has_init = true;
    return 0;
}

static void fb_destroy(void) {
    nwindowReleaseBuffers(&O.win);
    nvMapClose(&O.fb.map);
    memset(&O.fb, 0, sizeof(O.fb));
}

// Gives the heap region back once nothing uses it. If the kernel refuses (still mapped for the
// GPU), it is kept and reused next time.
static void release_region(void) {
    if (!O.region) return;
    void *addr = NULL;
    Result rc = svcSetHeapSize(&addr, 0);
    if (R_SUCCEEDED(rc)) {
        O.region = NULL;
    } else {
        static int warned;
        if (warned++ < 3) log_printf("osd: giving back the %u KiB framebuffer memory: %s (kept for the next time)",
                                     FB_REGION / 1024, rc_str(rc));
    }
}

// IManagerDisplayService 6000: AddToLayerStack(u32 stack, u64 layer_id). libnx does not wrap it.
static Result add_to_layer_stack(ViLayerStack stack) {
    const struct {
        u32 stack;
        u32 pad;
        u64 layer_id;
    } in = {stack, 0, O.layer.layer_id};
    return serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, in);
}

static bool open_layer(void) {
    static int attempts;
    if (attempts++ < 3) log_printf("osd: opening the overlay layer (vi:m %s, nvdrv:s %s)", O.vi_ok ? "open" : "not yet",
                                   O.nv_ok ? "open" : "not yet");
    Result rc = 0;
    const char *step = "viInitialize(vi:m)";
    if (!O.vi_ok) {
        rc = viInitialize(ViServiceType_Manager);
        if (R_SUCCEEDED(rc)) {
            step = "viOpenDefaultDisplay";
            rc = viOpenDefaultDisplay(&O.display);
            if (R_FAILED(rc)) viExit();
            else O.have_vsync = R_SUCCEEDED(viGetDisplayVsyncEvent(&O.display, &O.vsync));
        }
        O.vi_ok = R_SUCCEEDED(rc);
    }
    if (R_SUCCEEDED(rc) && !O.nv_ok) {
        step = "nvInitialize";
        rc = nvInitialize();
        O.nv_ok = R_SUCCEEDED(rc);
    }
    memset(&O.layer, 0, sizeof(O.layer));
    if (R_SUCCEEDED(rc)) {
        step = "viCreateManagedLayer";
        rc = viCreateManagedLayer(&O.display, (ViLayerFlags)0, 0, &__nx_vi_layer_id);
    }
    if (R_SUCCEEDED(rc)) {
        step = "viCreateLayer";
        rc = viCreateLayer(&O.display, &O.layer);
        if (R_FAILED(rc)) __nx_vi_layer_id = 0;
    }
    if (R_SUCCEEDED(rc)) {
        viSetLayerScalingMode(&O.layer, ViScalingMode_FitToLayer);
        s32 z = 0;
        if (R_SUCCEEDED(viGetZOrderCountMax(&O.display, &z)) && z > 0) viSetLayerZ(&O.layer, z);
        step = "AddToLayerStack";
        rc = add_to_layer_stack(ViLayerStack_Default);
        // The same stacks Tesla adds (so the bar also shows in screenshots and recordings, as Tesla's do).
        static const ViLayerStack more[] = {ViLayerStack_Screenshot, ViLayerStack_Recording, ViLayerStack_Arbitrary,
                                            ViLayerStack_LastFrame,  ViLayerStack_Null,      ViLayerStack_ApplicationForDebug,
                                            ViLayerStack_Lcd};
        char stacks[96];
        int sn = snprintf(stacks, sizeof(stacks), "Default %s", rc_str(rc));
        for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
            Result rs = add_to_layer_stack(more[i]);
            if (R_FAILED(rs) && sn < (int)sizeof(stacks) - 24)
                sn += snprintf(stacks + sn, sizeof(stacks) - (size_t)sn, ", stack %d %s", more[i], rc_str(rs));
        }
        log_printf("osd: layer stacks: %s (others OK unless listed)", stacks);
        if (R_SUCCEEDED(rc)) {
            viSetLayerSize(&O.layer, LAYER_W, LAYER_H);
            viSetLayerPosition(&O.layer, LAYER_X, LAYER_Y);
            step = "nwindowCreateFromLayer";
            rc = nwindowCreateFromLayer(&O.win, &O.layer);
            if (R_SUCCEEDED(rc)) {
                rc = fb_create(&step);
                if (R_FAILED(rc)) nwindowClose(&O.win);
            }
        }
        if (R_FAILED(rc)) {
            viDestroyManagedLayer(&O.layer);
            __nx_vi_layer_id = 0;
        }
    }
    if (R_FAILED(rc)) release_region();
    if (R_FAILED(rc) && !strcmp(step, "svcSetHeapSize")) {
        // The system pool is full right now: skip this bar, not a reason to give up on the overlay.
        static int warned;
        if (warned++ < 5) log_printf("osd: no memory for the volume bar now (%u KiB, %s): not shown", FB_REGION / 1024,
                                     rc_str(rc));
        return false;
    }
    if (R_FAILED(rc)) {
        O.failures++;
        log_printf("osd: %s: %s%s", step, rc_str(rc),
                   O.failures >= MAX_FAILURES ? " (volume overlay disabled for this run)" : "");
        if (O.failures >= MAX_FAILURES) O.given_up = true;
        return false;
    }
    O.layer_on = true;
    static int opened;
    s32 zmin = 0, zmax = 0;
    viGetZOrderCountMin(&O.display, &zmin);
    viGetZOrderCountMax(&O.display, &zmax);
    if (opened++ < 3)   // the first few at info level: the overlay is not confirmed on hardware yet
        log_printf("osd: layer %llu open (z range %d..%d, fb %dx%d RGBA4444 at %d,%d size %dx%d), all steps OK",
                   (unsigned long long)O.layer.layer_id, zmin, zmax, FB_W, FB_H, LAYER_X, LAYER_Y, LAYER_W, LAYER_H);
    return true;
}

static void fill(u16 *px, u32 stride, int x0, int y0, int x1, int y1, u16 c) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FB_W) x1 = FB_W;
    if (y1 > FB_H) y1 = FB_H;
    for (int y = y0; y < y1; y++) {
        u16 *row = (u16 *)((u8 *)px + (size_t)y * stride);
        for (int x = x0; x < x1; x++) row[x] = c;
    }
}

// Like Tesla: wait for vsync, then queue the frame (at most 50 ms, so a missing vsync can't stall us).
static void present(void) {
    if (O.have_vsync) eventWait(&O.vsync, 50000000ULL);
    framebufferEnd(&O.fb);
}

static void draw(float db, float lo, float hi, float step, bool muted) {
    u32 stride = 0;
    u16 *px = framebufferBegin(&O.fb, &stride);
    const u16 clear = RGBA4(0, 0, 0, 0), panel = RGBA4(2, 2, 2, 13), on = RGBA4_MAXALPHA(15, 15, 15),
              off = RGBA4_MAXALPHA(6, 6, 6), dim = RGBA4_MAXALPHA(9, 9, 9), red = RGBA4_MAXALPHA(15, 4, 4);
    fill(px, stride, 0, 0, FB_W, FB_H, clear);
    // Rounded panel (radius 10).
    const int r = 10, OX = PANEL_X, OY = PANEL_Y;
    for (int y = 0; y < PANEL_H; y++) {
        int dy = y < r ? r - y : y >= PANEL_H - r ? y - (PANEL_H - r - 1) : 0, inset = 0;
        while (inset < r && (r - inset) * (r - inset) + dy * dy > r * r) inset++;
        fill(px, stride, OX + (dy ? inset : 0), OY + y, OX + PANEL_W - (dy ? inset : 0), OY + y + 1, panel);
    }

    // Levels like the console: level 0 = silent, then one block per step up to the maximum.
    if (step <= 0.0f) step = 3.0f;
    int n = (int)((hi - lo) / step + 0.5f) + 1;
    if (n < 1) n = 1;
    if (n > MAX_LEVELS) n = MAX_LEVELS;
    int lit = db < lo - 0.01f ? 0 : (int)((db - lo) / step + 0.5f) + 1;
    if (lit > n) lit = n;
    const int x0 = OX + 56, x1 = OX + PANEL_W - 14, y0 = OY + 17, y1 = OY + PANEL_H - 17, gap = 3;
    int w = (x1 - x0 + gap) / n;
    for (int i = 0; i < n; i++)
        fill(px, stride, x0 + i * w, y0, x0 + (i + 1) * w - gap, y1, i < lit ? (muted ? dim : on) : off);

    // Earphone icon (headband with two ear cups) where the console draws its speaker.
    u16 ic = muted || lit == 0 ? dim : on;
    const int ix = OX + 12, iy = OY + 10;
    fill(px, stride, ix + 6, iy, ix + 26, iy + 3, ic);
    fill(px, stride, ix + 3, iy + 2, ix + 6, iy + 6, ic);
    fill(px, stride, ix + 26, iy + 2, ix + 29, iy + 6, ic);
    fill(px, stride, ix + 1, iy + 5, ix + 4, iy + 18, ic);
    fill(px, stride, ix + 28, iy + 5, ix + 31, iy + 18, ic);
    fill(px, stride, ix, iy + 15, ix + 9, iy + 28, ic);
    fill(px, stride, ix + 23, iy + 15, ix + 32, iy + 28, ic);
    if (muted || lit == 0)   // red stroke across the icon: paused or level 0
        for (int k = 0; k < 30; k++) fill(px, stride, ix + 1 + k, iy + 28 - k, ix + 4 + k, iy + 31 - k, red);
    present();
}

static void clear_frame(void) {
    u32 stride = 0;
    u16 *px = framebufferBegin(&O.fb, &stride);
    fill(px, stride, 0, 0, FB_W, FB_H, RGBA4(0, 0, 0, 0));
    present();
}

// Everything goes once the bar is hidden: a layer (and vi:m / nvdrv:s sessions) kept open next to
// the Tesla overlay menu left that menu blank until a reboot. Reopening takes ~5 ms.
static void close_all(void) {
    if (O.layer_on) {
        fb_destroy();
        nwindowClose(&O.win);
        viDestroyManagedLayer(&O.layer);
        __nx_vi_layer_id = 0;
        O.layer_on = false;
    }
    if (O.fence_ok) {
        nvFenceExit();
        O.fence_ok = false;
    }
    if (O.nvmap_ok) {
        nvMapExit();
        O.nvmap_ok = false;
    }
    if (O.nv_ok) {
        nvExit();
        O.nv_ok = false;
    }
    if (O.vi_ok) {
        if (O.have_vsync) eventClose(&O.vsync);
        O.have_vsync = false;
        viCloseDisplay(&O.display);
        viExit();
        O.vi_ok = false;
    }
    release_region();
    LOGD("osd: overlay closed%s", O.region ? " (framebuffer memory kept)" : "");
}

// The layer exists only while the bar is shown; hiding draws a transparent frame, then closes it.
static void osd_thread(void *arg) {
    for (;;) {
        u64 now = now_ms();
        u64 wait_ms = O.visible ? (O.hide_at > now ? O.hide_at - now : 0) : 60000;
        waitSingle(waiterForUEvent(&O.wake), wait_ms * 1000000ULL);
        mutexLock(&O.mutex);
        bool pending = O.pending, muted = O.muted;
        float db = O.db, lo = O.lo, hi = O.hi, step = O.step;
        O.pending = false;
        mutexUnlock(&O.mutex);
        if (pending && !O.given_up && (O.layer_on || open_layer())) {
            draw(db, lo, hi, step, muted);
            O.visible = true;
            O.hide_at = now_ms() + SHOW_MS;
            static int drawn;
            if (drawn++ < 3) log_printf("osd: drew volume %.1f dB (%.1f..%.1f)%s", (double)db, (double)lo, (double)hi,
                                        muted ? " paused" : "");
        } else if (pending && O.given_up) {
            LOGD("osd: volume change not shown (overlay disabled after failures)");
        } else if (O.visible && now_ms() >= O.hide_at) {
            clear_frame();
            close_all();
            O.visible = false;
        }
    }
}

void osd_init(void) {
    static bool started;
    if (started) return;
    started = true;
    mutexInit(&O.mutex);
    ueventCreate(&O.wake, true);
    Result rc = thread_spawn(&O.thread, osd_thread, NULL, 0x4000, PRIO_LOG - 2);
    log_printf("osd: volume overlay thread: %s", rc_str(rc));
}

void osd_show_volume(float db, float min_db, float max_db, float step_db, bool muted) {
    mutexLock(&O.mutex);
    O.db = db;
    O.lo = min_db;
    O.hi = max_db;
    O.step = step_db;
    O.muted = muted;
    O.pending = true;
    mutexUnlock(&O.mutex);
    ueventSignal(&O.wake);
}
