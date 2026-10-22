// eofm: FM radio of the Samsung EO-IC100 USB-C earphone on the Switch (homebrew NRO).
//
// Earphone output: the receiver plays the radio inside the earphone (FM power on); the console's
// own sound keeps going to the earphone as usual (usbhs-logmitm).
// Speaker output: the receiver runs in its recording mode, which sends the radio to the console as
// the earphone's USB microphone; speaker.c plays that through the console speaker.
//
// The screen follows Samsung's radio app (Korean, dark): power button, channel scan, the frequency
// with a favourite star, a dial scale, and favourites / channels tabs. Touch and buttons both work.
#include <math.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "common.h"
#include "fm.h"
#include "gfx.h"
#include "log.h"
#include "rds.h"
#include "speaker.h"

#define SETTINGS_PATH   APP_DIR "/settings.ini"
#define MAX_LIST        24
#define QUERY_MS        100
#define RSSI_MS         1000
#define SWEEP_DWELL_MS  100    // per 0.1 MHz step: tune, let the receiver settle, read the signal
#define REPEAT_DELAY_MS 400
#define REPEAT_MS       90
#define MSG_MS          4000

typedef enum { MODE_OFF, MODE_EARPHONE, MODE_SPEAKER } radio_mode;
enum { OUT_EARPHONE = 0, OUT_SPEAKER = 1 };
enum { TAB_FAV = 0, TAB_CH = 1 };

typedef struct {
    const char *name;
    u8  band, spacing;      // FM_SET_BAND / FM_SET_SPACING values
    u16 min, max, step;     // 10 kHz units
} region;

// Bands and spacings from the receiver's enums: band 0 = 87-108 MHz, 1 = 76-107, 2 = 76-91;
// spacing 0 = 200 kHz, 1 = 100 kHz, 2 = 50 kHz.
static const region REGIONS[] = {
    {"kr", 0, 1, 8750, 10800, 10},
    {"eu", 0, 1, 8750, 10800, 10},
    {"us", 0, 0, 8790, 10790, 20},
    {"jp", 1, 1, 7600, 9500, 10},
};
#define N_REGIONS (int)(sizeof(REGIONS) / sizeof(REGIONS[0]))

typedef struct {
    int x, y, w, h;
} box;

static struct {
    fm_dev     dev;
    radio_mode mode;
    int        output;        // OUT_*: what the power button switches on
    int        region;
    u16        freq;
    u8         volume;        // earphone output, 0..15
    u8         spk_volume;    // receiver volume in speaker output (capture level), 0..15
    bool       mute, mono, rds;
    u8         rssi;
    bool       seeking;       // the receiver seeks; the app watches its channel
    bool       scanning;      // 채널 찾기: seek up again and again through the band
    int        seek_dir;
    u64        seek_since;
    u16        seek_last;     // channel at the last poll
    bool       seek_moved;
    int        seek_stable;   // polls on the same channel since it last moved
    u64        sweep_next;    // next seek poll
    u16        scan_last;     // highest station found so far by 채널 찾기
    int        noise, margin; // kept in settings.ini for a later signal-based scan
    u16        rssi_logged;   // last frequency whose signal went to the log
    int        query_logs;    // QUERY answers logged since the last tune
    bool       enum_mic_only; // this plug-in was enumerated as a microphone only (speaker output)
    bool       pending_on;    // switch the radio on once the earphone is back (after an output switch)
    u16        fav[MAX_LIST];
    int        nfav;
    u16        ch[MAX_LIST];
    int        nch;
    int        tab;
    rds_state  rd;
    char       msg[120];
    u64        msg_ms;
    u64        last_query, last_rssi, last_dev;
    // touch targets from the last frame
    box        b_power, b_scan, b_left, b_right, b_star, b_tab[2], b_chip[MAX_LIST], b_out;
    int        nchips;
} R;

static PadState g_pad;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(R.msg, sizeof(R.msg), fmt, ap);
    va_end(ap);
    R.msg_ms = now_ms();
    log_file("ui: %s\n", R.msg);
}

static const region *reg(void) { return &REGIONS[R.region]; }

static void fmt_freq(char *out, size_t size, u16 f) {
    if (reg()->step % 10 == 0 && f % 10 == 0) snprintf(out, size, "%u.%u", f / 100, f % 100 / 10);
    else snprintf(out, size, "%u.%02u", f / 100, f % 100);
}

// ------------------------------------------------------------------ settings

static void parse_list(const char *v, u16 *list, int *n) {
    *n = 0;
    for (char *p = (char *)v; *p && *n < MAX_LIST;) {
        long x = strtol(p, &p, 10);
        if (x > 0) list[(*n)++] = (u16)x;
        while (*p == ',' || *p == ' ') p++;
        if (*p && (*p < '0' || *p > '9')) break;
    }
}

static void settings_load(void) {
    R.region = 0;
    R.freq = 8750;
    R.volume = 6;
    R.spk_volume = 15;
    R.rds = true;
    R.tab = TAB_CH;
    R.noise = -1;
    R.margin = 8;
    char buf[2048];
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '#') continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "region")) {
            for (int i = 0; i < N_REGIONS; i++)
                if (!strcmp(v, REGIONS[i].name)) R.region = i;
        } else if (!strcmp(k, "freq")) R.freq = (u16)atoi(v);
        else if (!strcmp(k, "volume")) R.volume = (u8)atoi(v);
        else if (!strcmp(k, "speaker_volume")) R.spk_volume = (u8)atoi(v);
        else if (!strcmp(k, "output")) R.output = !strcmp(v, "speaker") ? OUT_SPEAKER : OUT_EARPHONE;
        else if (!strcmp(k, "mono")) R.mono = atoi(v) != 0;
        else if (!strcmp(k, "rds")) R.rds = atoi(v) != 0;
        else if (!strcmp(k, "favorites") || !strcmp(k, "presets")) parse_list(v, R.fav, &R.nfav);
        else if (!strcmp(k, "channels")) parse_list(v, R.ch, &R.nch);
        else if (!strcmp(k, "noise")) R.noise = atoi(v);
        else if (!strcmp(k, "scan_margin")) R.margin = atoi(v);
    }
    if (R.volume > 15) R.volume = 15;
    if (R.spk_volume > 15) R.spk_volume = 15;
    if (R.freq < reg()->min || R.freq > reg()->max) R.freq = reg()->min;
    if (!R.nch && R.nfav) R.tab = TAB_FAV;
}

static void settings_save(void) {
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "; eofm settings. region = kr | eu | us | jp, output = earphone | speaker.\n"
               "; Frequencies in 10 kHz units (8910 = 89.1 MHz).\n");
    fprintf(f, "region=%s\noutput=%s\nfreq=%u\nvolume=%u\nspeaker_volume=%u\nmono=%d\nrds=%d\n", reg()->name,
            R.output == OUT_SPEAKER ? "speaker" : "earphone", R.freq, R.volume, R.spk_volume, R.mono, R.rds);
    fprintf(f, "noise=%d\nscan_margin=%d\n", R.noise, R.margin);
    fprintf(f, "favorites=");
    for (int i = 0; i < R.nfav; i++) fprintf(f, "%s%u", i ? "," : "", R.fav[i]);
    fprintf(f, "\nchannels=");
    for (int i = 0; i < R.nch; i++) fprintf(f, "%s%u", i ? "," : "", R.ch[i]);
    fprintf(f, "\n");
    fclose(f);
}

static int list_find(const u16 *l, int n, u16 f) {
    for (int i = 0; i < n; i++)
        if (l[i] == f) return i;
    return -1;
}

static void list_insert(u16 *l, int *n, u16 f) {
    if (list_find(l, *n, f) >= 0 || *n == MAX_LIST) return;
    int k = (*n)++;
    while (k > 0 && l[k - 1] > f) {
        l[k] = l[k - 1];
        k--;
    }
    l[k] = f;
}

// ------------------------------------------------------------------ radio

// Speaker output needs the console to not take the earphone as its audio output, or the system
// keeps playing to USB whatever audctl is told (eofm 0.1.2 on the console: "SetOutputTarget OK ->
// active USB"). usbhs-logmitm reads this file when the earphone enumerates and then rewrites only
// the microphone's terminal type: the earphone comes back as a USB microphone only, and the
// console plays on its speaker. The earphone is re-enumerated (ResetDevice) to apply it.
#define OVERRIDE_PATH "sdmc:/switch/usbhs-logmitm/rewrite_override.txt"

static bool override_present(void) {
    FILE *f = fopen(OVERRIDE_PATH, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void set_override(bool mic_only) {
    if (mic_only) {
        FILE *f = fopen(OVERRIDE_PATH, "wb");
        if (f) {
            fputs("it_mic\n", f);
            fclose(f);
        }
    } else {
        remove(OVERRIDE_PATH);
    }
    log_file("override: %s\n", mic_only ? "microphone only (speaker output)" : "removed (earphone is the output)");
}

// Re-enumerates the earphone so that the audio sysmodule reads its descriptors again. The receiver
// loses its state with it; R.pending_on switches the radio on again once IF0 is back.
static void reenumerate(void) {
    Result rc = fm_reset(&R.dev);
    log_file("fm: reset the earphone: %s\n", rc_str(rc));
}

static void log_signal(void) {
    u8 c[2] = {0, 0}, s[2] = {0, 0};
    Result rc = fm_get(&R.dev, FM_GET_CHANNEL, c);
    Result rs = fm_get(&R.dev, FM_GET_RSSI, s);
    if (R_SUCCEEDED(rs)) R.rssi = s[0];
    log_file("signal: set %u, receiver at %u (%s), rssi %02x %02x (%s)\n", R.freq, c[0] | c[1] << 8, rc_str(rc), s[0], s[1],
             rc_str(rs));
    R.rssi_logged = R.freq;
}

static void tune(u16 f) {
    if (f < reg()->min) f = reg()->max;
    if (f > reg()->max) f = reg()->min;
    R.freq = f;
    rds_reset(&R.rd);
    if (R.mode != MODE_OFF) fm_set(&R.dev, FM_SET_CHANNEL, f);
    R.last_rssi = now_ms();   // the signal is read (and logged) a moment after the tune settles
    R.query_logs = 0;
}

static void stop_seek(void) {
    if (R.seeking && R.mode != MODE_OFF) fm_set(&R.dev, FM_SET_SEEK_STOP, 0);
    R.seeking = R.scanning = false;
}

static void radio_off(void) {
    stop_seek();
    if (R.mode == MODE_SPEAKER) {
        spk_stop();
        fm_set(&R.dev, FM_SET_RECORDING, 0);
    } else if (R.mode == MODE_EARPHONE) {
        fm_set(&R.dev, FM_SET_POWER, 0);
    }
    R.mode = MODE_OFF;
}

static void radio_on(void) {
    if (R.dev.state != FMDEV_OPEN) {
        say(R.dev.state == FMDEV_NONE ? "이어폰을 연결하세요." : "이어폰을 준비하는 중입니다. 잠시 후 다시 누르세요.");
        return;
    }
    radio_mode m = R.output == OUT_SPEAKER ? MODE_SPEAKER : MODE_EARPHONE;
    // Speaker output needs the earphone enumerated as a microphone only (see set_override).
    if ((m == MODE_SPEAKER) != R.enum_mic_only) {
        set_override(m == MODE_SPEAKER);
        R.enum_mic_only = m == MODE_SPEAKER;
        R.pending_on = true;
        radio_off();
        say(m == MODE_SPEAKER ? "스피커로 바꾸는 중... 이어폰이 다시 연결됩니다." : "이어폰으로 바꾸는 중... 이어폰이 다시 연결됩니다.");
        reenumerate();
        return;
    }
    radio_off();
    // The receiver has two exclusive modes: power (sound in the earphone) and recording (sound to USB).
    Result rc = fm_set(&R.dev, m == MODE_EARPHONE ? FM_SET_POWER : FM_SET_RECORDING, 1);
    if (R_FAILED(rc)) {
        say("라디오를 켜지 못했습니다 (%s).", rc_str(rc));
        return;
    }
    // The same order and pause as kjy00302's working tool (and Samsung's framework): power, 200 ms,
    // volume, channel. Band and spacing are left at the receiver's defaults (87.5-108 MHz) unless
    // the region needs others: sent right after power-on (0.1.0), they left the receiver on noise.
    svcSleepThread(200000000ULL);
    fm_set(&R.dev, FM_SET_VOLUME, m == MODE_EARPHONE ? R.volume : R.spk_volume);
    if (reg()->band != 0 || reg()->spacing != 1) {
        fm_set(&R.dev, FM_SET_BAND, reg()->band);
        fm_set(&R.dev, FM_SET_SPACING, reg()->spacing);
    }
    svcSleepThread(50000000ULL);
    fm_set(&R.dev, FM_SET_CHANNEL, R.freq);
    if (R.mono) fm_set(&R.dev, FM_SET_MONO, 1);
    if (R.mute) fm_set(&R.dev, FM_SET_MUTE, 1);
    if (R.rds) fm_set(&R.dev, FM_SET_RDS, 1);
    rds_reset(&R.rd);
    R.mode = m;
    R.last_rssi = now_ms();
    R.rssi_logged = 0;
    R.query_logs = 0;
    if (m == MODE_SPEAKER && !spk_start()) {
        spk_status st;
        spk_get(&st);
        say("스피커로 듣기를 시작하지 못했습니다: %s", st.error);
        radio_off();
    }
}

static void set_output(int out) {
    R.output = out;
    if (R.mode != MODE_OFF || out == OUT_SPEAKER || R.enum_mic_only) radio_on();   // re-enumerates if needed
    else say("이어폰으로 듣기");
}

static void set_volume(int delta) {
    u8 *v = R.mode == MODE_SPEAKER ? &R.spk_volume : &R.volume;
    int nv = *v + delta;
    *v = (u8)(nv < 0 ? 0 : nv > 15 ? 15 : nv);
    if (R.mode != MODE_OFF) fm_set(&R.dev, FM_SET_VOLUME, *v);
    say(R.mode == MODE_SPEAKER ? "녹음 레벨 %u / 15 (스피커 음량은 본체 볼륨으로)" : "볼륨 %u / 15", *v);
}

// ---- seeking with the receiver's own seek. It reports its end on IF4 (held by the hid sysmodule)
// and QUERY answers zeros meanwhile, and the signal strength reads 0 on this firmware (0.1.2 on the
// console), so the app watches the channel instead: the receiver moves while it seeks and stops
// on a station.

#define SEEK_POLL_MS   100
#define SEEK_STABLE    6        // 600 ms on the same channel after moving: found
#define SEEK_STILL_MS  3000     // never moved: nothing found
#define SEEK_TIMEOUT_MS 15000

static void start_seek(int dir) {
    fm_set(&R.dev, FM_SET_SEEK_START, dir > 0 ? FM_SEEK_UP : FM_SEEK_DOWN);
    R.seeking = true;
    R.seek_dir = dir;
    R.seek_since = now_ms();
    R.seek_last = R.freq;
    R.seek_moved = false;
    R.seek_stable = 0;
    R.sweep_next = now_ms() + SEEK_POLL_MS;
    rds_reset(&R.rd);
}

static void seek(int dir) {
    if (R.mode == MODE_OFF) {
        say("라디오를 켜세요.");
        return;
    }
    if (R.seeking) {
        stop_seek();
        return;
    }
    start_seek(dir);
}

static void scan_toggle(void) {
    if (R.scanning) {
        stop_seek();
        settings_save();
        say("채널 찾기를 멈췄습니다. %d개 찾음.", R.nch);
        return;
    }
    if (R.mode == MODE_OFF) {
        say("라디오를 켜세요.");
        return;
    }
    stop_seek();
    R.nch = 0;
    R.tab = TAB_CH;
    R.scan_last = 0;
    tune(reg()->min);
    svcSleepThread(100000000ULL);
    start_seek(1);
    R.scanning = true;
}

static void seek_done(bool found, u16 f) {
    bool scanning = R.scanning;
    R.seeking = false;
    log_file("seek: %s at %u after %llu ms\n", found ? "stopped" : "nothing", f,
             (unsigned long long)(now_ms() - R.seek_since));
    if (f >= 6400 && f <= 10800) R.freq = f;
    if (!scanning) {
        if (!found) say("방송을 찾지 못했습니다.");
        R.last_rssi = now_ms();
        return;
    }
    // 채널 찾기: seek up again until the receiver wraps around or gives up.
    if (found && f > R.scan_last) {
        list_insert(R.ch, &R.nch, f);
        R.scan_last = f;
        if (f + reg()->step <= reg()->max && R.nch < MAX_LIST) {
            start_seek(1);
            R.scanning = true;
            return;
        }
    }
    R.scanning = false;
    settings_save();
    tune(R.nch ? R.ch[0] : reg()->min);
    say(R.nch ? "채널 %d개를 찾았습니다." : "채널을 찾지 못했습니다. 이어폰 줄을 펴 보세요.", R.nch);
}

static void seek_poll(void) {
    u8 c[2] = {0, 0};
    if (R_FAILED(fm_get(&R.dev, FM_GET_CHANNEL, c))) return;
    u16 f = (u16)(c[0] | c[1] << 8);
    u8 q[12];
    u32 got = 0;
    if (R_SUCCEEDED(fm_query(&R.dev, q, &got)) && got >= 5 && (q[0] || q[1] || q[2] || q[3])) {
        log_file("seek: query %02x %02x %02x %02x %02x\n", q[0], q[1], q[2], q[3], q[4]);
        u16 qf = (u16)(q[2] | q[3] << 8);
        if (q[0] == 0 && qf >= 6400 && qf <= 10800) {   // a real seek result
            R.rssi = q[4];
            seek_done(q[1] != 0, qf);
            return;
        }
    }
    if (f < 6400 || f > 10800) return;
    if (f != R.seek_last) {
        R.seek_moved = true;
        R.seek_stable = 0;
        R.seek_last = f;
        R.freq = f;   // the dial follows the receiver
    } else if (R.seek_moved && ++R.seek_stable >= SEEK_STABLE) {
        seek_done(true, f);
        return;
    }
    u64 t = now_ms() - R.seek_since;
    if ((!R.seek_moved && t > SEEK_STILL_MS) || t > SEEK_TIMEOUT_MS) {
        fm_set(&R.dev, FM_SET_SEEK_STOP, 0);
        seek_done(false, f);
    }
}

static void read_channel(void) {
    u8 b[2];
    if (R_SUCCEEDED(fm_get(&R.dev, FM_GET_CHANNEL, b))) {
        u16 f = (u16)(b[0] | b[1] << 8);
        if (f >= 6400 && f <= 10800) R.freq = f;
    }
}

// After IF0 opens: adopt what the receiver is already doing (the radio keeps playing in the
// earphone after the app exits), or finish an output switch.
static void sync_from_device(void) {
    u8 p[2], r[2], v[2], proto[2];
    fm_get(&R.dev, FM_GET_POWER, p);
    fm_get(&R.dev, FM_GET_RECORDING, r);
    fm_get(&R.dev, FM_GET_PROTOCOL, proto);
    log_file("fm: power %u, recording %u, protocol %u.%u\n", p[0], r[0], proto[0], proto[1]);
    if (R.pending_on) {
        R.pending_on = false;
        radio_on();
        return;
    }
    if (p[0]) {
        R.mode = MODE_EARPHONE;
        R.output = OUT_EARPHONE;
        read_channel();
        if (R_SUCCEEDED(fm_get(&R.dev, FM_GET_VOLUME, v)) && v[0] <= 15) R.volume = v[0];
    } else if (r[0]) {   // left in recording mode by a crash: nobody plays it, switch it off
        fm_set(&R.dev, FM_SET_RECORDING, 0);
    }
}

static void poll_radio(void) {
    u64 t = now_ms();
    if (R.mode == MODE_OFF || R.dev.state != FMDEV_OPEN) return;
    if (R.seeking) {
        if (t >= R.sweep_next) {
            R.sweep_next = t + SEEK_POLL_MS;
            seek_poll();
        }
        return;
    }
    if (t - R.last_query >= QUERY_MS) {
        R.last_query = t;
        u8 q[12];
        u32 got = 0;
        if (R_SUCCEEDED(fm_query(&R.dev, q, &got)) && got >= 5) {
            bool any = false;
            for (u32 i = 0; i < got && i < 12; i++) any |= q[i] != 0;
            if (any && R.query_logs < 12) {   // what the receiver reports after a tune: where the strength may be
                R.query_logs++;
                log_file("query: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", q[0], q[1], q[2], q[3], q[4],
                         q[5], q[6], q[7], q[8], q[9], q[10], q[11]);
            }
            if (q[0] == 1 && (q[2] || q[3])) R.rssi = q[4];   // tune result: strength
            else if (R.rds && q[0] == 2 && got >= 11 && q[1] == 0) {
                uint16_t blk[4];
                rds_blocks_from_query(q + 3, blk);
                if (blk[0] || blk[1]) rds_group(&R.rd, blk);
            }
        }
    }
    if (R.freq != R.rssi_logged && t - R.last_rssi >= 300) log_signal();
}

static void toggle_fav(void) {
    int i = list_find(R.fav, R.nfav, R.freq);
    char f[16];
    fmt_freq(f, sizeof(f), R.freq);
    if (i >= 0) {
        memmove(&R.fav[i], &R.fav[i + 1], (size_t)(R.nfav - i - 1) * sizeof(u16));
        R.nfav--;
        say("즐겨찾기에서 %s를 뺐습니다.", f);
    } else if (R.nfav == MAX_LIST) {
        say("즐겨찾기가 가득 찼습니다.");
        return;
    } else {
        list_insert(R.fav, &R.nfav, R.freq);
        say("즐겨찾기에 %s를 추가했습니다.", f);
    }
    settings_save();
}

static void step_list(int dir) {
    const u16 *l = R.tab == TAB_FAV ? R.fav : R.ch;
    int n = R.tab == TAB_FAV ? R.nfav : R.nch;
    if (!n) return;
    int k = -1;
    if (dir > 0) {
        for (int i = 0; i < n && k < 0; i++)
            if (l[i] > R.freq) k = i;
        if (k < 0) k = 0;
    } else {
        for (int i = n - 1; i >= 0 && k < 0; i--)
            if (l[i] < R.freq) k = i;
        if (k < 0) k = n - 1;
    }
    stop_seek();
    tune(l[k]);
}

// ------------------------------------------------------------------ screen

#define C_BG      RGB(0, 0, 0)
#define C_CARD    RGB(30, 30, 32)
#define C_WHITE   RGB(250, 250, 250)
#define C_TEXT2   RGB(160, 160, 165)
#define C_TEXT3   RGB(105, 105, 110)
#define C_OFFNUM  RGB(110, 110, 116)
#define C_LINE    RGB(80, 80, 84)
#define C_ACCENT  RGB(56, 125, 255)
#define C_TABBG   RGB(38, 38, 40)
#define C_TABON   RGB(66, 66, 70)

static box mkbox(int x, int y, int w, int h) { return (box){x, y, w, h}; }

static void draw_power(int cx, int cy, bool on) {
    gfx_circle(cx, cy, 40, on ? C_ACCENT : RGB(8, 8, 8));
    gfx_ring(cx, cy, 40, 1.5f, on ? C_ACCENT : RGB(95, 95, 100));
    gfx_arc(cx, cy, 15, 3.0f, 40, 320, C_WHITE);
    gfx_line(cx, cy - 20, cx, cy - 4, 3.0f, C_WHITE);
}

static void draw_chevron(int cx, int cy, int dir, u32 c) {
    gfx_line(cx - dir * 12, cy - 24, cx + dir * 12, cy, 2.5f, c);   // dir -1: "<", 1: ">"
    gfx_line(cx + dir * 12, cy, cx - dir * 12, cy + 24, 2.5f, c);
}

// 5-point star outline (or filled with a ring of lines) centred at cx, cy.
static void draw_star(float cx, float cy, float r, bool filled, u32 c) {
    float px[10], py[10];
    for (int i = 0; i < 10; i++) {
        float a = (float)M_PI / 5 * i - (float)M_PI / 2, rr = i % 2 ? r * 0.45f : r;
        px[i] = cx + rr * cosf(a);
        py[i] = cy + rr * sinf(a);
    }
    if (filled)
        for (float s = 0.15f; s < 1.0f; s += 0.12f)
            for (int i = 0; i < 10; i++) {
                int j = (i + 1) % 10;
                gfx_line(cx + (px[i] - cx) * s, cy + (py[i] - cy) * s, cx + (px[j] - cx) * s, cy + (py[j] - cy) * s,
                         2.2f, c);
            }
    for (int i = 0; i < 10; i++) gfx_line(px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], 2.0f, c);
}

static void draw_antenna(int cx, int cy, u32 c) {
    gfx_arc(cx, cy - 4, 14, 2.0f, 300, 60, c);
    gfx_arc(cx, cy - 4, 7, 2.0f, 300, 60, c);
    gfx_line(cx, cy - 4, cx - 8, cy + 14, 2.0f, c);
    gfx_line(cx, cy - 4, cx + 8, cy + 14, 2.0f, c);
    gfx_circle(cx, cy - 4, 2.5f, c);
}

static void draw(void) {
    bool on = R.mode != MODE_OFF;
    gfx_begin(C_BG);

    // ---- title and status line
    char title[80], sub[140];
    if (R.dev.state == FMDEV_NONE) snprintf(title, sizeof(title), "이어폰을 연결하세요.");
    else if (R.dev.state != FMDEV_OPEN) snprintf(title, sizeof(title), "이어폰을 준비하는 중...");
    else if (!on) snprintf(title, sizeof(title), "라디오를 켜세요.");
    else if (R.rds && R.rd.ps_seen == 0xF) {
        snprintf(title, sizeof(title), "%s", R.rd.ps);
        for (int i = (int)strlen(title) - 1; i >= 0 && title[i] == ' '; i--) title[i] = 0;
    } else {
        char f[16];
        fmt_freq(f, sizeof(f), R.freq);
        snprintf(title, sizeof(title), "FM %s", f);
    }
    gfx_text(SCREEN_W / 2, 34, 50, C_WHITE, ALIGN_CENTER, true, title);
    sub[0] = 0;
    if (R.msg[0] && now_ms() - R.msg_ms < MSG_MS) snprintf(sub, sizeof(sub), "%s", R.msg);
    else if (R.scanning || R.seeking) {
        char fs[16];
        fmt_freq(fs, sizeof(fs), R.freq);
        snprintf(sub, sizeof(sub), "%s %s MHz", R.scanning ? "채널을 찾는 중..." : "방송을 찾는 중...", fs);
    }
    else if (R.dev.state == FMDEV_BUSY) snprintf(sub, sizeof(sub), "다른 프로그램이 이어폰을 쓰고 있습니다 (eoaudio 등).");
    else if (on && R.rds && R.rd.rt_any) snprintf(sub, sizeof(sub), "%.64s", R.rd.rt);
    else if (on) snprintf(sub, sizeof(sub), R.mode == MODE_SPEAKER ? "스피커로 듣는 중" : "이어폰으로 듣는 중");
    else if (R.dev.state == FMDEV_OPEN) snprintf(sub, sizeof(sub), "이어폰 줄이 안테나 역할을 합니다.");
    gfx_text(SCREEN_W / 2, 100, 22, C_TEXT2, ALIGN_CENTER, false, sub);

    // ---- card
    const int cx0 = 140, cy0 = 150, cw = 1000, ch = 370;
    gfx_rrect(cx0, cy0, cw, ch, 44, C_CARD);

    // power, output selector and scan button
    draw_power(cx0 + 76, cy0 + 72, on);
    R.b_power = mkbox(cx0 + 30, cy0 + 26, 92, 92);
    const char *out = R.output == OUT_SPEAKER ? "스피커" : "이어폰";
    int ow = gfx_text_width(22, false, out) + 48;
    gfx_rrect(cx0 + 140, cy0 + 50, ow, 44, 22, RGB(44, 44, 47));
    gfx_text(cx0 + 140 + ow / 2, cy0 + 59, 22, R.output == OUT_SPEAKER ? C_ACCENT : C_TEXT2, ALIGN_CENTER, false, out);
    R.b_out = mkbox(cx0 + 140, cy0 + 50, ow, 44);
    const char *scan = R.scanning ? "찾기 중지" : "채널 찾기";
    int sw = gfx_text_width(28, true, scan) + 64;
    int sx = cx0 + cw - 34 - sw;
    gfx_rrect(sx, cy0 + 40, sw, 66, 33, RGB(8, 8, 8));
    gfx_rrect_outline(sx, cy0 + 40, sw, 66, 33, 1.5f, RGB(95, 95, 100));
    gfx_text(sx + sw / 2, cy0 + 58, 28, on ? C_WHITE : C_TEXT2, ALIGN_CENTER, true, scan);
    R.b_scan = mkbox(sx, cy0 + 40, sw, 66);

    // signal and volume, small, under the scan button
    char info[64];
    u8 vol = R.mode == MODE_SPEAKER ? R.spk_volume : R.volume;
    if (on && R.rssi) snprintf(info, sizeof(info), "신호 %u   볼륨 %u%s%s", R.rssi, vol, R.mono ? "   모노" : "", R.mute ? "   음소거" : "");
    else if (on) snprintf(info, sizeof(info), "볼륨 %u%s%s", vol, R.mono ? "   모노" : "", R.mute ? "   음소거" : "");
    else snprintf(info, sizeof(info), "볼륨 %u", vol);
    gfx_text(cx0 + cw - 40, cy0 + 118, 18, C_TEXT3, ALIGN_RIGHT, false, info);

    // frequency, star, chevrons
    char f[16];
    fmt_freq(f, sizeof(f), R.freq);
    int fy = cy0 + 128;
    int fw = gfx_text(SCREEN_W / 2, fy, 140, on ? C_WHITE : C_OFFNUM, ALIGN_CENTER, true, f);
    bool fav = list_find(R.fav, R.nfav, R.freq) >= 0;
    float stx = SCREEN_W / 2 + fw / 2 + 32, sty = fy + 26;
    draw_star(stx, sty, 17, fav, fav ? RGB(255, 196, 0) : C_TEXT3);
    R.b_star = mkbox((int)stx - 30, (int)sty - 30, 60, 60);
    draw_chevron(cx0 + 70, fy + 70, -1, on ? C_TEXT2 : C_TEXT3);
    draw_chevron(cx0 + cw - 70, fy + 70, 1, on ? C_TEXT2 : C_TEXT3);
    R.b_left = mkbox(cx0 + 20, fy + 10, 110, 120);
    R.b_right = mkbox(cx0 + cw - 130, fy + 10, 110, 120);

    // dial scale: labels every 3 MHz, ticks every 0.5 MHz, a marker at the frequency
    int lo = reg()->min < 8750 ? 7600 : 8600, hi = reg()->min < 8750 ? 9600 : 11000;
    int step_lbl = reg()->min < 8750 ? 200 : 300;
    int sx0 = cx0 + 60, sx1 = cx0 + cw - 60, ly = cy0 + 284, ty = cy0 + 318;
    for (int v = lo; v <= hi; v += step_lbl) {
        char l[12];
        snprintf(l, sizeof(l), "%d", v / 100);
        int x = sx0 + (sx1 - sx0) * (v - lo) / (hi - lo);
        gfx_text(x, ly, 22, C_TEXT3, ALIGN_CENTER, false, l);
    }
    for (int v = lo; v <= hi; v += 50) {
        int x = sx0 + (sx1 - sx0) * (v - lo) / (hi - lo);
        bool major = (v - lo) % step_lbl == 0;
        gfx_line(x, ty + (major ? 0 : 6), x, ty + 28, major ? 2.0f : 1.4f, major ? RGB(96, 96, 100) : C_LINE);
    }
    float mx = sx0 + (sx1 - sx0) * (float)(R.freq - lo) / (float)(hi - lo);
    gfx_line(mx, ty - 8, mx, ty + 34, 3.0f, on ? C_ACCENT : C_TEXT3);

    // ---- list: favourites or channels
    const u16 *l = R.tab == TAB_FAV ? R.fav : R.ch;
    int n = R.tab == TAB_FAV ? R.nfav : R.nch;
    R.nchips = 0;
    if (!n) {
        gfx_text(SCREEN_W / 2, 548, 26, RGB(215, 215, 220), ALIGN_CENTER, false,
                 R.tab == TAB_FAV ? "즐겨찾기가 없습니다" : "채널이 없습니다");
        gfx_text(SCREEN_W / 2, 588, 20, C_TEXT3, ALIGN_CENTER, false,
                 R.tab == TAB_FAV ? "주파수 옆의 별(Y)을 누르면 여기에 나타납니다." : "채널을 검색하면 여기에 나타납니다.");
    } else {
        // chips in rows centred under the card
        int x = 0, row = 0, widths[MAX_LIST], total[3] = {0, 0, 0}, rows_of[MAX_LIST];
        for (int i = 0; i < n; i++) {
            char t[16];
            fmt_freq(t, sizeof(t), l[i]);
            widths[i] = gfx_text_width(22, false, t) + 36;
            if (x + widths[i] > 1000 && row < 2) row++, x = 0;
            rows_of[i] = row;
            total[row] += widths[i] + 10;
            x += widths[i] + 10;
        }
        int xs[3];
        for (int r = 0; r < 3; r++) xs[r] = SCREEN_W / 2 - (total[r] - 10) / 2;
        for (int i = 0; i < n; i++) {
            int r = rows_of[i], y = 538 + r * 48;
            if (y > 640) break;
            bool cur = l[i] == R.freq;
            char t[16];
            fmt_freq(t, sizeof(t), l[i]);
            gfx_rrect(xs[r], y, widths[i], 40, 20, cur ? C_ACCENT : RGB(34, 34, 36));
            gfx_text(xs[r] + widths[i] / 2, y + 8, 22, cur ? C_WHITE : RGB(210, 210, 214), ALIGN_CENTER, false, t);
            R.b_chip[R.nchips++] = mkbox(xs[r], y, widths[i], 40);
            xs[r] += widths[i] + 10;
        }
    }

    // ---- tab bar
    const int tx = 470, tyb = 648, tw = 340, th = 64;
    gfx_rrect(tx, tyb, tw, th, 32, C_TABBG);
    gfx_rrect(tx + 5 + (R.tab == TAB_CH ? tw / 2 : 0), tyb + 5, tw / 2 - 10, th - 10, 27, C_TABON);
    draw_star(tx + 52, tyb + 32, 12, false, R.tab == TAB_FAV ? C_WHITE : C_TEXT2);
    gfx_text(tx + 72, tyb + 20, 22, R.tab == TAB_FAV ? C_WHITE : C_TEXT2, ALIGN_LEFT, R.tab == TAB_FAV, "즐겨찾기");
    draw_antenna(tx + tw / 2 + 52, tyb + 32, R.tab == TAB_CH ? C_WHITE : C_TEXT2);
    gfx_text(tx + tw / 2 + 76, tyb + 20, 22, R.tab == TAB_CH ? C_WHITE : C_TEXT2, ALIGN_LEFT, R.tab == TAB_CH, "채널");
    R.b_tab[0] = mkbox(tx, tyb, tw / 2, th);
    R.b_tab[1] = mkbox(tx + tw / 2, tyb, tw / 2, th);

    // ---- button help, both sides of the tab bar
    static const char *help_l[] = {"A  전원", "X  채널 찾기", "Y  즐겨찾기", "RS 누름  이어폰/스피커"};
    static const char *help_r[] = {"←→  0.1MHz   L R  1MHz", "ZL ZR  방송 탐색   ↑↓  볼륨", "RS ←→  목록 이동   -  탭",
                                   "+  나가기   B  끄고 나가기"};
    for (int i = 0; i < 4; i++) {
        gfx_text(40, 626 + i * 22, 16, C_TEXT3, ALIGN_LEFT, false, help_l[i]);
        gfx_text(SCREEN_W - 40, 626 + i * 22, 16, C_TEXT3, ALIGN_RIGHT, false, help_r[i]);
    }

    // speaker output status, when it is the one in use
    if (R.mode == MODE_SPEAKER) {
        spk_status st;
        spk_get(&st);
        char s[120];
        if (st.error[0]) snprintf(s, sizeof(s), "스피커: %s", st.error);
        else
            snprintf(s, sizeof(s), "스피커 %s · 입력 %d dBFS · %.0f초", st.now == AudioTarget_Speaker ? "출력 중" : "전환 실패(이어폰으로 나감)",
                     st.peak_db, st.frames / 48000.0);
        gfx_text(SCREEN_W / 2, 126, 16, st.error[0] || st.now != AudioTarget_Speaker ? RGB(255, 170, 60) : C_TEXT3, ALIGN_CENTER,
                 false, s);
    }
    gfx_end();
}

static bool hit(box b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static void on_touch(int x, int y) {
    if (hit(R.b_power, x, y)) {
        if (R.mode != MODE_OFF) radio_off();
        else radio_on();
    } else if (hit(R.b_scan, x, y)) scan_toggle();
    else if (hit(R.b_out, x, y)) set_output(R.output == OUT_SPEAKER ? OUT_EARPHONE : OUT_SPEAKER);
    else if (hit(R.b_star, x, y)) toggle_fav();
    else if (hit(R.b_left, x, y)) stop_seek(), tune((u16)(R.freq - reg()->step));
    else if (hit(R.b_right, x, y)) stop_seek(), tune((u16)(R.freq + reg()->step));
    else if (hit(R.b_tab[0], x, y)) R.tab = TAB_FAV;
    else if (hit(R.b_tab[1], x, y)) R.tab = TAB_CH;
    else {
        const u16 *l = R.tab == TAB_FAV ? R.fav : R.ch;
        for (int i = 0; i < R.nchips; i++)
            if (hit(R.b_chip[i], x, y)) {
                stop_seek();
                tune(l[i]);
            }
    }
}

// ------------------------------------------------------------------ main

int main(int argc, char **argv) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    log_init();
    char err[96];
    if (!gfx_init(err, sizeof(err))) {   // no screen: fall back to the text console to say why
        log_file("gfx: %s\n", err);
        consoleInit(NULL);
        printf("eofm: graphics failed: %s\nPress + to exit.\n", err);
        while (appletMainLoop()) {
            padUpdate(&g_pad);
            if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
        log_exit();
        return 0;
    }
    settings_load();
    rds_reset(&R.rd);
    // A leftover override (a crash in speaker output) keeps the earphone a microphone-only device:
    // treat it as such, so that switching on in earphone output undoes it.
    R.enum_mic_only = override_present();

    Result rc = usbHsInitialize();
    if (R_FAILED(rc)) say("usb:hs를 열 수 없습니다 (%s).", rc_str(rc));
    log_file("usbHsInitialize: %s, region %s, freq %u\n", rc_str(rc), reg()->name, R.freq);

    bool quit_off = false, touching = false;
    int held_dir = 0;
    u64 held_since = 0, last_repeat = 0;
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        u64 down = padGetButtonsDown(&g_pad), held = padGetButtons(&g_pad);
        u64 t = now_ms();

        if (R_SUCCEEDED(rc) && t - R.last_dev >= 1000) {
            R.last_dev = t;
            fmdev_state before = R.dev.state;
            fm_poll_device(&R.dev);
            if (R.dev.state == FMDEV_OPEN && before != FMDEV_OPEN) {
                sync_from_device();
            } else if (R.dev.state != FMDEV_OPEN && before == FMDEV_OPEN) {
                if (R.mode == MODE_SPEAKER) spk_stop();
                R.mode = MODE_OFF;
                R.seeking = R.scanning = false;
                say("이어폰 연결이 끊어졌습니다.");
            }
        }

        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_B) {
            quit_off = true;
            break;
        }
        if (down & HidNpadButton_A) {
            if (R.mode != MODE_OFF) radio_off();
            else radio_on();
        }
        if (down & HidNpadButton_X) scan_toggle();
        if (down & HidNpadButton_Y) toggle_fav();
        if (down & HidNpadButton_StickR) set_output(R.output == OUT_SPEAKER ? OUT_EARPHONE : OUT_SPEAKER);
        int dir = (held & HidNpadButton_Right) ? 1 : (held & HidNpadButton_Left) ? -1 : 0;
        if (dir && (down & (HidNpadButton_Right | HidNpadButton_Left))) {
            stop_seek();
            held_dir = dir;
            held_since = last_repeat = t;
            tune((u16)(R.freq + dir * reg()->step));
        } else if (dir && dir == held_dir && t - held_since > REPEAT_DELAY_MS && t - last_repeat > REPEAT_MS) {
            last_repeat = t;
            tune((u16)(R.freq + dir * reg()->step));
        } else if (!dir) {
            held_dir = 0;
        }
        if (down & (HidNpadButton_R | HidNpadButton_L)) {
            stop_seek();
            int d = (down & HidNpadButton_R) ? 1 : -1;
            int f = (int)R.freq + d * 100;   // 1 MHz, clamped to the band (no wrap)
            tune((u16)(f < reg()->min ? reg()->min : f > reg()->max ? reg()->max : f));
        }
        if (down & HidNpadButton_ZR) seek(1);
        if (down & HidNpadButton_ZL) seek(-1);
        if (down & HidNpadButton_Up) set_volume(1);
        if (down & HidNpadButton_Down) set_volume(-1);
        if (down & HidNpadButton_StickRRight) step_list(1);
        if (down & HidNpadButton_StickRLeft) step_list(-1);
        if (down & HidNpadButton_Minus) R.tab = R.tab == TAB_FAV ? TAB_CH : TAB_FAV;
        if (down & HidNpadButton_StickL) {
            R.mono = !R.mono;
            if (R.mode != MODE_OFF) fm_set(&R.dev, FM_SET_MONO, R.mono);
            say(R.mono ? "모노" : "스테레오");
        }

        HidTouchScreenState ts = {0};
        if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
            if (!touching) on_touch((int)ts.touches[0].x, (int)ts.touches[0].y);
            touching = true;
        } else {
            touching = false;
        }

        poll_radio();
        draw();
    }

    if (R.mode == MODE_SPEAKER || quit_off) radio_off();   // speaker output cannot outlive the app
    if (R.enum_mic_only) {   // give the console its USB output back
        set_override(false);
        if (R.dev.state == FMDEV_OPEN) reenumerate();
    }
    log_file("exit: radio %s\n", R.mode == MODE_EARPHONE ? "left on in the earphone" : "off");
    settings_save();
    fm_close(&R.dev);
    if (R_SUCCEEDED(rc)) usbHsExit();
    gfx_exit();
    log_exit();
    return 0;
}
