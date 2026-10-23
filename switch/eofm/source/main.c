// eofm: FM radio of the Samsung EO-IC100 USB-C earphone on the Switch (homebrew NRO).
//
// The receiver plays the radio inside the earphone (FM power on); the console's own sound keeps
// going to the earphone as usual (usbhs-logmitm). Speaker output (experimental, RS press): the
// receiver's recording mode sends the radio to IF1, and usbhs-logmitm 0.3.6 (deny_mic=1,
// rewrite=none) plays it on the console in the background (spkctl.c asks it to). 0.3.0-0.3.3 played
// it in the app itself (attic/README.md has the history).
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
#include "spkctl.h"

#define SETTINGS_PATH   APP_DIR "/settings.ini"
#define MAX_LIST        24
#define QUERY_MS        100
#define RSSI_MS         1000
#define SWEEP_DWELL_MS  100    // per 0.1 MHz step: tune, let the receiver settle, read the signal
#define REPEAT_DELAY_MS 400
#define REPEAT_MS       90
#define MSG_MS          4000

typedef enum { MODE_OFF, MODE_EARPHONE, MODE_SPEAKER } radio_mode;
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
    int        region;
    u16        freq;
    u8         volume;        // earphone output, 0..15
    bool       mute, mono, rds;
    u8         rssi;
    bool       seeking;       // the receiver seeks; the app watches its channel
    bool       scanning;      // 채널 찾기: seek up again and again through the band
    int        seek_dir;
    int        seek_phase;    // 0: fresh tune sent, 1: seeking
    u64        seek_since;    // seek phase start, or when the current scan step was tuned
    u64        sweep_next;    // next seek/scan poll
    u8         scan_vals[256];   // strength per scan step from reg()->min
    int        scan_n;
    int        noise, margin; // kept in settings.ini for a later signal-based scan
    u16        rssi_logged;   // last frequency whose signal went to the log
    int        query_logs;    // QUERY answers logged since the last tune
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
    box        b_power, b_scan, b_left, b_right, b_star, b_tab[2], b_chip[MAX_LIST];
    spk_state  spk_state;                // usbhs-logmitm's speaker, read once a second in speaker mode
    char       spk_why[96];
    box        b_dial, b_num;            // drag areas: the dial scale, the big frequency
    int        dial_lo, dial_hi, dial_x0, dial_x1;
    // a frequency drag in progress: 1 = on the dial (absolute), 2 = on the number (relative)
    int        drag, drag_x0;
    u16        drag_f0;
    u64        drag_tuned;
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
    R.rds = true;
    R.tab = TAB_CH;
    R.noise = -1;
    R.margin = 6;   // 0.1.3 on the console: noise ~20, stations 29-38
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
        else if (!strcmp(k, "mono")) R.mono = atoi(v) != 0;
        else if (!strcmp(k, "rds")) R.rds = atoi(v) != 0;
        else if (!strcmp(k, "favorites") || !strcmp(k, "presets")) parse_list(v, R.fav, &R.nfav);
        else if (!strcmp(k, "channels")) parse_list(v, R.ch, &R.nch);
        else if (!strcmp(k, "noise")) R.noise = atoi(v);
        else if (!strcmp(k, "station_margin")) R.margin = atoi(v);
    }
    if (R.volume > 15) R.volume = 15;
    if (R.freq < reg()->min || R.freq > reg()->max) R.freq = reg()->min;
    if (!R.nch && R.nfav) R.tab = TAB_FAV;
}

static void settings_save(void) {
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "; eofm settings. region = kr | eu | us | jp.\n"
               "; Frequencies in 10 kHz units (8910 = 89.1 MHz).\n");
    fprintf(f, "region=%s\nfreq=%u\nvolume=%u\nmono=%d\nrds=%d\n", reg()->name, R.freq, R.volume, R.mono, R.rds);
    fprintf(f, "noise=%d\nstation_margin=%d\n", R.noise, R.margin);
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

static void log_signal(void) {
    u8 c[2] = {0, 0}, s[2] = {0, 0};
    Result rc = fm_get(&R.dev, FM_GET_CHANNEL, c);
    Result rs = fm_get(&R.dev, FM_GET_RSSI, s);
    log_file("signal: set %u, receiver at %u (%s), strength %u (rssi %02x %02x %s)\n", R.freq, c[0] | c[1] << 8, rc_str(rc),
             R.rssi, s[0], s[1], rc_str(rs));
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
    if (R.seeking && R.seek_phase == 1 && R.mode != MODE_OFF) fm_set(&R.dev, FM_SET_SEEK_STOP, 0);
    if (R.scanning && R.mode != MODE_OFF) fm_set(&R.dev, FM_SET_MUTE, R.mute);   // the scan ran muted
    R.seeking = R.scanning = false;
}

static void radio_off(void) {
    stop_seek();
    if (R.mode == MODE_SPEAKER) {
        spkctl_request(false, 0);
        fm_set(&R.dev, FM_SET_RECORDING, 0);
    } else if (R.mode == MODE_EARPHONE) {
        fm_set(&R.dev, FM_SET_POWER, 0);
    }
    R.mode = MODE_OFF;
}

// The receiver plays the radio inside the earphone (its "power" mode), or sends it to the console
// as the USB microphone (its "recording" mode) for usbhs-logmitm's speaker. The receiver is set up the same way
// in both.
static void radio_start(radio_mode m) {
    if (R.dev.state != FMDEV_OPEN) {
        say(R.dev.state == FMDEV_NONE ? "이어폰을 연결하세요." : "이어폰을 준비하는 중입니다. 잠시 후 다시 누르세요.");
        return;
    }
    radio_off();
    Result rc = fm_set(&R.dev, m == MODE_SPEAKER ? FM_SET_RECORDING : FM_SET_POWER, 1);
    if (R_FAILED(rc)) {
        say("라디오를 켜지 못했습니다 (%s).", rc_str(rc));
        return;
    }
    // The same order and pause as kjy00302's working tool (and Samsung's framework): power, 200 ms,
    // volume, channel. Band and spacing are left at the receiver's defaults (87.5-108 MHz) unless
    // the region needs others: sent right after power-on (0.1.0), they left the receiver on noise.
    svcSleepThread(200000000ULL);
    fm_set(&R.dev, FM_SET_VOLUME, R.volume);
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
    if (m != MODE_SPEAKER) return;
    // Speaker: the receiver records at full level, the app's volume scales the console output.
    fm_set(&R.dev, FM_SET_VOLUME, 15);
    spk_state st = spkctl_status(NULL, 0);
    if (st == SPK_NO_SERVICE || !spkctl_request(true, R.volume)) {
        say("스피커로 듣기: usbhs-logmitm 0.3.6 이상과 deny_mic=1이 필요합니다.");
        radio_start(MODE_EARPHONE);
        return;
    }
    char why[96] = "";
    for (int i = 0; i < 20; i++) {   // the sysmodule looks every 0.3 s
        svcSleepThread(100000000ULL);
        st = spkctl_status(why, sizeof(why));
        if (st == SPK_RUNNING || st == SPK_ERROR) break;
    }
    log_file("speaker: usbhs-logmitm says %d %s\n", st, why);
    if (st != SPK_RUNNING) {
        spkctl_request(false, 0);
        say("스피커로 듣기 실패: %s", st == SPK_ERROR ? why : "응답 없음");
        radio_start(MODE_EARPHONE);
        return;
    }
    say("스피커로 듣는 중. +로 나가도 계속 나옵니다.");
}

static void radio_on(void) { radio_start(MODE_EARPHONE); }

// RS press: the radio between the earphone and the console speaker.
static void toggle_speaker(void) {
    if (R.mode == MODE_OFF) {
        say("라디오를 켜세요.");
        return;
    }
    radio_start(R.mode == MODE_SPEAKER ? MODE_EARPHONE : MODE_SPEAKER);
}

static void set_volume(int delta) {
    int nv = R.volume + delta;
    R.volume = (u8)(nv < 0 ? 0 : nv > 15 ? 15 : nv);
    if (R.mode == MODE_SPEAKER) spkctl_request(true, R.volume);
    else if (R.mode != MODE_OFF) fm_set(&R.dev, FM_SET_VOLUME, R.volume);
    say("볼륨 %u / 15", R.volume);
}

// ---- what the receiver reports. QUERY answers its latest event and repeats it until there is a
// new one (eofm 0.1.3 on the console): type 0 = seek result, 1 = tune result, each
// {type, success, freq lo, freq hi, strength}; type 2 = an RDS group {2, errors, strength, 8 bytes}.
// The strength there is real (about 20 on noise, 30+ on a station here); GET_RSSI always reads 0.
// So a seek is only finished by a type-0 answer that arrives after a fresh tune result, and the
// scan tunes step by step and takes the strength from each step's own tune result.

#define SEEK_POLL_MS    40
#define SEEK_TIMEOUT_MS 12000
#define SCAN_STEP_MAX_MS 400    // no tune result for a step by then: count it as nothing

typedef struct {
    u8  type, ok, strength;
    u16 freq;
} fm_event;

static bool read_event(fm_event *e) {
    u8 q[12];
    u32 got = 0;
    if (R_FAILED(fm_query(&R.dev, q, &got)) || got < 5) return false;
    e->type = q[0];
    e->ok = q[1];
    e->freq = (u16)(q[2] | q[3] << 8);
    e->strength = q[0] == 2 ? q[2] : q[4];
    if (q[0] == 2 && R.rds && got >= 11 && q[1] == 0) {   // an RDS group without block errors
        uint16_t blk[4];
        rds_blocks_from_query(q + 3, blk);
        if (blk[0] || blk[1]) rds_group(&R.rd, blk);
    }
    return true;
}

static void start_seek(int dir) {
    // Phase 0: tune one step away, so that the receiver's latest event is a tune result for a new
    // frequency (an old one with the same frequency would pass for fresh) and the seek does not
    // stop on the station it starts from (0.1.4: "found 10470 after 33 ms", over and over);
    // phase 1: seek, and the first seek result after that is this seek's.
    int f = (int)R.freq + dir * reg()->step;
    R.freq = (u16)(f < reg()->min ? reg()->max : f > reg()->max ? reg()->min : f);
    fm_set(&R.dev, FM_SET_CHANNEL, R.freq);
    R.seeking = true;
    R.seek_dir = dir;
    R.seek_phase = 0;
    R.seek_since = now_ms();
    R.sweep_next = now_ms() + SEEK_POLL_MS;
    rds_reset(&R.rd);
}

static void seek(int dir) {
    if (R.mode == MODE_OFF) {
        say("라디오를 켜세요.");
        return;
    }
    if (R.seeking || R.scanning) {
        stop_seek();
        return;
    }
    // With a channel list from 채널 찾기, go to its next station at once; seek only without one.
    if (R.nch) {
        int k = -1;
        if (dir > 0) {
            for (int i = 0; i < R.nch && k < 0; i++)
                if (R.ch[i] > R.freq) k = i;
            if (k < 0) k = 0;
        } else {
            for (int i = R.nch - 1; i >= 0 && k < 0; i--)
                if (R.ch[i] < R.freq) k = i;
            if (k < 0) k = R.nch - 1;
        }
        tune(R.ch[k]);
        return;
    }
    start_seek(dir);
}

static void seek_poll(void) {
    fm_event e;
    bool have = read_event(&e);
    u64 t = now_ms() - R.seek_since;
    if (R.seek_phase == 0) {
        if ((have && e.type == 1 && e.freq == R.freq) || t > SCAN_STEP_MAX_MS) {
            fm_set(&R.dev, FM_SET_SEEK_START, R.seek_dir > 0 ? FM_SEEK_UP : FM_SEEK_DOWN);
            R.seek_phase = 1;
            R.seek_since = now_ms();
        }
        return;
    }
    if (have && e.type == 0) {
        R.seeking = false;
        log_file("seek: %s %u, strength %u, after %llu ms\n", e.ok ? "found" : "nothing, at", e.freq, e.strength,
                 (unsigned long long)t);
        if (e.freq >= 6400 && e.freq <= 10800) R.freq = e.freq;
        R.rssi = e.strength;
        // Tune there once more: it ends the receiver's seek state, which distorted the sound in
        // 0.1.4 when seeks followed one another.
        fm_set(&R.dev, FM_SET_CHANNEL, R.freq);
        R.rssi_logged = R.freq;
        if (!e.ok) say("방송을 찾지 못했습니다.");
        return;
    }
    u8 c[2];   // the dial follows the receiver while it seeks
    if (R_SUCCEEDED(fm_get(&R.dev, FM_GET_CHANNEL, c))) {
        u16 f = (u16)(c[0] | c[1] << 8);
        if (f >= 6400 && f <= 10800) R.freq = f;
    }
    if (t > SEEK_TIMEOUT_MS) {
        stop_seek();
        say("방송을 찾지 못했습니다.");
    }
}

// ---- 채널 찾기: tune every step of the band (muted), keep each step's strength, then pick the
// stations: local maxima that stand scan_margin above the band's median (the noise).

static void scan_tune(u16 f) {
    R.freq = f;
    fm_set(&R.dev, FM_SET_CHANNEL, f);
    R.seek_since = now_ms();
}

static void scan_toggle(void) {
    if (R.scanning) {
        stop_seek();
        say("채널 찾기를 멈췄습니다.");
        return;
    }
    if (R.mode == MODE_OFF) {
        say("라디오를 켜세요.");
        return;
    }
    stop_seek();
    R.scanning = true;
    R.scan_n = 0;
    R.tab = TAB_CH;
    R.sweep_next = now_ms() + SEEK_POLL_MS;
    fm_set(&R.dev, FM_SET_MUTE, 1);
    rds_reset(&R.rd);
    scan_tune(reg()->min);
}

static int median(const u8 *v, int n) {
    u8 tmp[256];
    if (n <= 0) return -1;
    memcpy(tmp, v, (size_t)n);
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0 && tmp[k - 1] > tmp[k]; k--) {
            u8 x = tmp[k];
            tmp[k] = tmp[k - 1];
            tmp[k - 1] = x;
        }
    return tmp[n / 2];
}

static void scan_finish(void) {
    int n = R.scan_n;
    R.noise = median(R.scan_vals, n);
    int thr = R.noise + R.margin;
    for (int i = 0; i < n; i += 10) {   // the whole table in the log: 10 steps per line
        char line[96];
        int k = 0;
        for (int j = i; j < n && j < i + 10; j++) k += snprintf(line + k, sizeof(line) - (size_t)k, " %3u", R.scan_vals[j]);
        log_file("scan %u:%s\n", reg()->min + i * reg()->step, line);
    }
    R.nch = 0;
    u8 best = 0;
    u16 best_f = 0;
    for (int i = 0; i < n && R.nch < MAX_LIST; i++) {
        u8 v = R.scan_vals[i];
        if (v < thr || (i > 0 && v < R.scan_vals[i - 1]) || (i < n - 1 && v <= R.scan_vals[i + 1])) continue;
        u16 f = (u16)(reg()->min + i * reg()->step);
        if (R.nch && f - R.ch[R.nch - 1] <= 2 * reg()->step) {   // the same station's shoulder: keep the stronger
            int prev = (R.ch[R.nch - 1] - reg()->min) / reg()->step;
            if (v > R.scan_vals[prev]) R.ch[R.nch - 1] = f;
            continue;
        }
        R.ch[R.nch++] = f;
        if (v > best) best = v, best_f = f;
    }
    log_file("scan: noise %d, threshold %d, %d station(s)\n", R.noise, thr, R.nch);
    stop_seek();
    settings_save();
    tune(R.nch ? best_f : reg()->min);
    say(R.nch ? "채널 %d개를 찾았습니다." : "채널을 찾지 못했습니다. 이어폰 줄을 펴 보세요.", R.nch);
}

static void scan_poll(void) {
    fm_event e;
    bool have = read_event(&e);
    bool done = have && e.type == 1 && e.freq == R.freq;
    if (!done && now_ms() - R.seek_since < SCAN_STEP_MAX_MS) return;
    if (R.scan_n < (int)sizeof(R.scan_vals)) R.scan_vals[R.scan_n++] = done ? e.strength : 0;
    R.rssi = done ? e.strength : 0;
    if (R.freq + reg()->step > reg()->max) scan_finish();
    else scan_tune((u16)(R.freq + reg()->step));
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
    if (p[0]) {
        R.mode = MODE_EARPHONE;
        read_channel();
        if (R_SUCCEEDED(fm_get(&R.dev, FM_GET_VOLUME, v)) && v[0] <= 15) R.volume = v[0];
    } else if (r[0] && spkctl_status(NULL, 0) == SPK_RUNNING) {   // on the speaker in the background
        R.mode = MODE_SPEAKER;
        read_channel();
    } else if (r[0]) {   // left in recording mode with nobody playing it: switch it off
        fm_set(&R.dev, FM_SET_RECORDING, 0);
    }
}

static void poll_radio(void) {
    u64 t = now_ms();
    if (R.mode == MODE_OFF || R.dev.state != FMDEV_OPEN) return;
    if (R.seeking || R.scanning) {
        if (t >= R.sweep_next) {
            R.sweep_next = t + SEEK_POLL_MS;
            if (R.scanning) scan_poll();
            else seek_poll();
        }
        return;
    }
    if (t - R.last_query >= QUERY_MS) {
        R.last_query = t;
        fm_event e;
        if (read_event(&e)) {
            if (e.freq == R.freq && (e.type == 0 || e.type == 1)) R.rssi = e.strength;
            else if (e.type == 2 && e.strength) R.rssi = e.strength;
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
    else if (R.mode == MODE_SPEAKER) {
        if (R.spk_state == SPK_ERROR) snprintf(sub, sizeof(sub), "스피커: %s", R.spk_why);
        else snprintf(sub, sizeof(sub), R.spk_state == SPK_RUNNING ? "스피커로 듣는 중 (+로 나가도 계속)" : "스피커가 멈췄습니다");
    }
    else if (on) snprintf(sub, sizeof(sub), "이어폰으로 듣는 중");
    else if (R.dev.state == FMDEV_OPEN) snprintf(sub, sizeof(sub), "이어폰 줄이 안테나 역할을 합니다.");
    gfx_text(SCREEN_W / 2, 100, 22, C_TEXT2, ALIGN_CENTER, false, sub);

    // ---- card
    const int cx0 = 140, cy0 = 150, cw = 1000, ch = 370;
    gfx_rrect(cx0, cy0, cw, ch, 44, C_CARD);

    // power and scan buttons
    draw_power(cx0 + 76, cy0 + 72, on);
    R.b_power = mkbox(cx0 + 30, cy0 + 26, 92, 92);
    const char *scan = R.scanning ? "찾기 중지" : "채널 찾기";
    int sw = gfx_text_width(28, true, scan) + 64;
    int sx = cx0 + cw - 34 - sw;
    gfx_rrect(sx, cy0 + 40, sw, 66, 33, RGB(8, 8, 8));
    gfx_rrect_outline(sx, cy0 + 40, sw, 66, 33, 1.5f, RGB(95, 95, 100));
    gfx_text(sx + sw / 2, cy0 + 58, 28, on ? C_WHITE : C_TEXT2, ALIGN_CENTER, true, scan);
    R.b_scan = mkbox(sx, cy0 + 40, sw, 66);

    // signal and volume, small, under the scan button
    char info[64];
    u8 vol = R.volume;
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
    R.b_num = mkbox(cx0 + 130, fy, cw - 260, 150);

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
    R.b_dial = mkbox(sx0 - 20, ly - 10, sx1 - sx0 + 40, ty + 50 - (ly - 10));
    R.dial_lo = lo;
    R.dial_hi = hi;
    R.dial_x0 = sx0;
    R.dial_x1 = sx1;
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
    static const char *help_l[] = {"A  전원", "X  채널 찾기", "Y  즐겨찾기", "LS 누름  모노   RS 누름  스피커/이어폰"};
    static const char *help_r[] = {"←→  0.1MHz   L R  1MHz", "ZL ZR  방송 탐색   ↑↓  볼륨", "RS ←→  목록 이동   -  탭",
                                   "+  나가기   B  끄고 나가기"};
    for (int i = 0; i < 4; i++) {
        gfx_text(40, 626 + i * 22, 16, C_TEXT3, ALIGN_LEFT, false, help_l[i]);
        gfx_text(SCREEN_W - 40, 626 + i * 22, 16, C_TEXT3, ALIGN_RIGHT, false, help_r[i]);
    }

    gfx_end();
}

static bool hit(box b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

// Returns false when the touch hit no button (it may start a drag).
static bool on_touch(int x, int y) {
    if (hit(R.b_power, x, y)) {
        if (R.mode != MODE_OFF) radio_off();
        else radio_on();
    } else if (hit(R.b_scan, x, y)) scan_toggle();
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
                return true;
            }
        return false;
    }
    return true;
}

// ---- frequency drag: on the dial the marker follows the finger; on the big number every
// DRAG_PX_PER_STEP pixels sideways is one step, moved like a tuning dial: dragging left raises the
// frequency (0.3.2 had it the other way round). The receiver is retuned at most every
// DRAG_TUNE_MS while the finger moves, and once more where it lets go.
#define DRAG_PX_PER_STEP 12
#define DRAG_TUNE_MS     80

static u16 drag_target(int x) {
    int f;
    if (R.drag == 1) f = R.dial_lo + (x - R.dial_x0) * (R.dial_hi - R.dial_lo) / (R.dial_x1 - R.dial_x0);
    else f = R.drag_f0 - (x - R.drag_x0) / DRAG_PX_PER_STEP * reg()->step;
    int step = reg()->step;
    f = reg()->min + (f - reg()->min + step / 2) / step * step;
    return (u16)(f < reg()->min ? reg()->min : f > reg()->max ? reg()->max : f);
}

static void drag_begin(int x, int y) {
    R.drag = hit(R.b_dial, x, y) ? 1 : hit(R.b_num, x, y) ? 2 : 0;
    R.drag_x0 = x;
    R.drag_f0 = R.freq;
    R.drag_tuned = 0;
    if (R.drag) stop_seek();
}

static void drag_move(int x, bool last) {
    if (!R.drag) return;
    u16 f = drag_target(x);
    u64 t = now_ms();
    if (f != R.freq && (last || t - R.drag_tuned >= DRAG_TUNE_MS)) {
        tune(f);
        R.drag_tuned = t;
    }
    if (last) R.drag = 0;
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
    // Use the earphone at once when the audio sysmodule is done with this plug-in (it holds IF2);
    // the 6 s wait is only for an earphone plugged in just now.
    R.dev.quick_settle = true;
    rds_reset(&R.rd);
    // A leftover override (a crash in speaker output) keeps the earphone a microphone-only device:
    // treat it as such, so that switching on in earphone output undoes it.

    Result rc = usbHsInitialize();
    if (R_FAILED(rc)) say("usb:hs를 열 수 없습니다 (%s).", rc_str(rc));
    log_file("usbHsInitialize: %s, region %s, freq %u\n", rc_str(rc), reg()->name, R.freq);

    bool quit_off = false, touching = false;
    int touch_x = 0;
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
            if (R.mode == MODE_SPEAKER) {
                spk_state was = R.spk_state;
                R.spk_state = spkctl_status(R.spk_why, sizeof(R.spk_why));
                if (R.spk_state != was) log_file("speaker: status %d %s\n", R.spk_state, R.spk_why);
            }
            if (R.dev.state == FMDEV_OPEN && before != FMDEV_OPEN) {
                sync_from_device();
            } else if (R.dev.state != FMDEV_OPEN && before == FMDEV_OPEN) {
                if (R.mode == MODE_SPEAKER) spkctl_request(false, 0);
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
        if (down & HidNpadButton_StickR) toggle_speaker();
        if (down & HidNpadButton_StickL) {
            R.mono = !R.mono;
            if (R.mode != MODE_OFF) fm_set(&R.dev, FM_SET_MONO, R.mono);
            say(R.mono ? "모노" : "스테레오");
        }

        HidTouchScreenState ts = {0};
        if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
            int x = (int)ts.touches[0].x, y = (int)ts.touches[0].y;
            if (!touching && !on_touch(x, y)) drag_begin(x, y);
            drag_move(x, false);
            touch_x = x;
            touching = true;
        } else {
            if (touching) drag_move(touch_x, true);
            touching = false;
        }

        poll_radio();
        draw();
    }

    if (quit_off) radio_off();   // + leaves the radio playing (in the earphone, or on the speaker by usbhs-logmitm)
    log_file("exit: radio %s\n", R.mode == MODE_EARPHONE ? "left on in the earphone" : R.mode == MODE_SPEAKER ? "left on the speaker" : "off");
    settings_save();
    fm_close(&R.dev);
    if (R_SUCCEEDED(rc)) usbHsExit();
    gfx_exit();
    log_exit();
    return 0;
}
