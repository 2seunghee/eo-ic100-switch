// eofm-ovl: a Tesla/Ultrahand overlay for the EO-IC100's FM radio. It controls the receiver the
// eofm app left playing (power, volume, frequency, saved channels) without leaving the game.
//
// The overlay talks to the earphone like eofm does (fm.c, IF0 vendor requests). It holds IF0 only
// while it is shown, so the eofm app (and the audio sysmodule at plug-in) can have it otherwise.
// A thread keeps following the earphone while the overlay is hidden, so that showing it does not
// have to wait for a plug-in to settle.

#define TESLA_INIT_IMPL
#include <tesla.hpp>

extern "C" {
#include "fm.h"
#include "log.h"
#include "spkctl.h"
}

#define OVL_VERSION   "0.2.2"
#define SETTINGS_PATH "sdmc:/switch/eofm/settings.ini"
#define MAX_LIST      24

namespace {

struct Region {
    const char *name;
    u8  band, spacing;   // FM_SET_BAND / FM_SET_SPACING values
    u16 min, max, step;  // 10 kHz units
};

// The same table as eofm's main.c.
const Region REGIONS[] = {
    {"kr", 0, 1, 8750, 10800, 10},
    {"eu", 0, 1, 8750, 10800, 10},
    {"us", 0, 0, 8790, 10790, 20},
    {"jp", 1, 1, 7600, 9500, 10},
};
constexpr int N_REGIONS = sizeof(REGIONS) / sizeof(REGIONS[0]);

struct {
    Mutex  lock;     // dev: the thread polls it, the UI sends requests through it
    fm_dev dev;
    Thread thread;
    volatile bool stop;

    // main thread only
    bool synced;     // read the receiver's state since IF0 opened
    bool on;
    bool speaker;    // usbhs-logmitm's output mode: speaker (everything on the console) or earphone
    bool switching;  // the mode changed: waiting for the re-enumerated earphone
    bool pending_on; // and then the radio goes on again
    u16  freq;
    u8   volume;
    bool mono, mute, rds;
    int  region;
    u16  ch[MAX_LIST], fav[MAX_LIST];
    int  nch, nfav;
    bool dirty;      // freq/volume changed: written to settings.ini when the overlay hides
} S;

const Region &reg() { return REGIONS[S.region]; }

std::string fmt_freq(u16 f) {
    char b[16];
    if (reg().step % 10 == 0 && f % 10 == 0) snprintf(b, sizeof(b), "%u.%u MHz", f / 100, f % 100 / 10);
    else snprintf(b, sizeof(b), "%u.%02u MHz", f / 100, f % 100);
    return b;
}

// ------------------------------------------------------------------ settings.ini (shared with eofm)

void parse_list(const char *v, u16 *list, int *n) {
    *n = 0;
    for (char *p = (char *)v; *p && *n < MAX_LIST;) {
        long x = strtol(p, &p, 10);
        if (x > 0) list[(*n)++] = (u16)x;
        while (*p == ',' || *p == ' ') p++;
        if (*p && (*p < '0' || *p > '9')) break;
    }
}

void settings_load() {
    S.region = 0;
    S.freq = 8750;
    S.volume = 6;
    S.rds = true;
    S.nch = S.nfav = 0;
    if (!fsdevGetDeviceFileSystem("sdmc")) return;
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
                if (!strcmp(v, REGIONS[i].name)) S.region = i;
        } else if (!strcmp(k, "freq")) S.freq = (u16)atoi(v);
        else if (!strcmp(k, "volume")) S.volume = (u8)atoi(v);
        else if (!strcmp(k, "mono")) S.mono = atoi(v) != 0;
        else if (!strcmp(k, "rds")) S.rds = atoi(v) != 0;
        else if (!strcmp(k, "favorites") || !strcmp(k, "presets")) parse_list(v, S.fav, &S.nfav);
        else if (!strcmp(k, "channels")) parse_list(v, S.ch, &S.nch);
    }
    if (S.volume > 15) S.volume = 15;
    if (S.freq < reg().min || S.freq > reg().max) S.freq = reg().min;
}

// Rewrites freq= and volume= and keeps every other line as eofm wrote it.
void settings_save() {
    if (!fsdevGetDeviceFileSystem("sdmc")) return;
    std::string out;
    bool had_freq = false, had_vol = false;
    char buf[2048];
    FILE *f = fopen(SETTINGS_PATH, "r");
    size_t n = 0;
    if (f) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[n] = 0;
    char line_freq[24], line_vol[24];
    snprintf(line_freq, sizeof(line_freq), "freq=%u", S.freq);
    snprintf(line_vol, sizeof(line_vol), "volume=%u", S.volume);
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strncmp(line, "freq=", 5)) {
            out += line_freq;
            had_freq = true;
        } else if (!strncmp(line, "volume=", 7)) {
            out += line_vol;
            had_vol = true;
        } else {
            out += line;
        }
        out += "\n";
    }
    if (!had_freq) out += std::string(line_freq) + "\n";
    if (!had_vol) out += std::string(line_vol) + "\n";
    f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}

// ------------------------------------------------------------------ the receiver

void poll_thread(void *) {
    while (!S.stop) {
        mutexLock(&S.lock);
        fm_poll_device(&S.dev);
        mutexUnlock(&S.lock);
        for (int i = 0; i < (S.switching ? 2 : 10) && !S.stop; i++) svcSleepThread(100000000ULL);
    }
}

bool dev_open() { return S.dev.state == FMDEV_OPEN; }

// Adopt what the receiver is doing (eofm left it playing, or it is off).
void sync_from_device() {
    u8 p[2], r[2] = {0, 0}, c[2], v[2];
    if (R_FAILED(fm_get(&S.dev, FM_GET_POWER, p))) return;
    fm_get(&S.dev, FM_GET_RECORDING, r);
    bool spk = r[0] && spkctl_status(NULL, 0) == SPK_RUNNING;
    S.on = p[0] != 0 || spk;
    S.speaker = spkctl_mode() == 1;
    if (p[0]) {
        if (R_SUCCEEDED(fm_get(&S.dev, FM_GET_CHANNEL, c))) {
            u16 f = (u16)(c[0] | c[1] << 8);
            if (f >= 6400 && f <= 10800) S.freq = f;
        }
        if (R_SUCCEEDED(fm_get(&S.dev, FM_GET_VOLUME, v)) && v[0] <= 15) S.volume = v[0];
    }
    if (spk && R_SUCCEEDED(fm_get(&S.dev, FM_GET_CHANNEL, c))) {
        u16 f = (u16)(c[0] | c[1] << 8);
        if (f >= 6400 && f <= 10800) S.freq = f;
    }
    log_file("ovl: receiver power %u, recording %u (speaker %d), %u, volume %u\n", p[0], r[0], spk, S.freq, S.volume);
    S.synced = true;
}

// eofm's radio_start: power (or recording, for the speaker), 200 ms, volume, (band, spacing),
// channel - in this order, or noise.
bool radio_on() {
    if (R_FAILED(fm_set(&S.dev, S.speaker ? FM_SET_RECORDING : FM_SET_POWER, 1))) return false;
    svcSleepThread(200000000ULL);
    fm_set(&S.dev, FM_SET_VOLUME, S.volume);
    if (reg().band != 0 || reg().spacing != 1) {
        fm_set(&S.dev, FM_SET_BAND, reg().band);
        fm_set(&S.dev, FM_SET_SPACING, reg().spacing);
    }
    svcSleepThread(50000000ULL);
    fm_set(&S.dev, FM_SET_CHANNEL, S.freq);
    if (S.mono) fm_set(&S.dev, FM_SET_MONO, 1);
    if (S.rds) fm_set(&S.dev, FM_SET_RDS, 1);
    S.on = true;
    if (!S.speaker) return true;
    // The speaker: the receiver records at full level, usbhs-logmitm plays it scaled by the volume.
    fm_set(&S.dev, FM_SET_VOLUME, 15);
    spk_state st = SPK_NO_SERVICE;
    if (spkctl_status(NULL, 0) != SPK_NO_SERVICE && spkctl_request(true, S.volume)) {
        for (int i = 0; i < 20; i++) {
            svcSleepThread(100000000ULL);
            st = spkctl_status(NULL, 0);
            if (st == SPK_RUNNING || st == SPK_ERROR) break;
        }
    }
    log_file("ovl: speaker %d\n", st);
    if (st == SPK_RUNNING) return true;
    spkctl_request(false, 0);   // the driver could not play it: off again
    fm_set(&S.dev, FM_SET_RECORDING, 0);
    S.on = false;
    return false;
}

void radio_off() {
    if (S.speaker) {
        spkctl_request(false, 0);
        fm_set(&S.dev, FM_SET_RECORDING, 0);
    } else {
        fm_set(&S.dev, FM_SET_POWER, 0);
    }
    S.on = false;
}

void tune(u16 f) {
    if (f < reg().min) f = reg().max;
    if (f > reg().max) f = reg().min;
    S.freq = f;
    S.dirty = true;
    if (S.on) fm_set(&S.dev, FM_SET_CHANNEL, f);
}

void set_volume(int delta) {
    int nv = S.volume + delta;
    S.volume = (u8)(nv < 0 ? 0 : nv > 15 ? 15 : nv);
    S.dirty = true;
    if (S.on && S.speaker) spkctl_request(true, S.volume);
    else if (S.on) fm_set(&S.dev, FM_SET_VOLUME, S.volume);
}

// The next or previous station of the channel list from 채널 찾기, or of the favourites without one.
void step_station(int dir) {
    const u16 *l = S.nch ? S.ch : S.fav;
    int n = S.nch ? S.nch : S.nfav;
    if (!n) return;
    int k = -1;
    if (dir > 0) {
        for (int i = 0; i < n && k < 0; i++)
            if (l[i] > S.freq) k = i;
        if (k < 0) k = 0;
    } else {
        for (int i = n - 1; i >= 0 && k < 0; i--)
            if (l[i] < S.freq) k = i;
        if (k < 0) k = n - 1;
    }
    tune(l[k]);
}

// ------------------------------------------------------------------ UI

const char *status_text() {
    if (S.switching) return "출력 모드를 바꾸는 중";
    switch (S.dev.state) {
    case FMDEV_NONE:     return "이어폰 없음";
    case FMDEV_SETTLING: return "준비 중";
    case FMDEV_BUSY:     return "라디오 앱이 사용 중";
    case FMDEV_OPEN:     return !S.synced ? "읽는 중" : S.on ? "켜짐" : "꺼짐";
    default:             break;
    }
    return "";
}

// Runs fn with the receiver locked if IF0 is open; false otherwise.
template <typename F> bool with_dev(F fn) {
    mutexLock(&S.lock);
    bool ok = dev_open() && S.synced;
    if (ok) fn();
    mutexUnlock(&S.lock);
    return ok;
}

class RadioGui : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame("EO 라디오", "eofm-ovl " OVL_VERSION);
        auto list = new tsl::elm::List();

        m_status = new tsl::elm::ListItem("상태");
        list->addItem(m_status);

        m_power = new tsl::elm::ListItem("라디오");
        m_power->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            with_dev([] {
                if (S.on) radio_off();
                else radio_on();
            });
            return true;
        });
        list->addItem(m_power);

        // Earphone mode <-> speaker mode, as eofm's RS press: usbhs-logmitm takes the new mode and
        // the earphone is re-enumerated, so that the console reads it again under that mode.
        m_output = new tsl::elm::ListItem("출력 모드");
        m_output->setClickListener([](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            with_dev([] {
                int cur = spkctl_mode();
                if (cur < 0 || S.switching) return;
                bool to_speaker = cur == 0;
                bool was_on = S.on;
                if (S.on) radio_off();
                spkctl_set_mode(to_speaker);
                int now = cur;
                for (int i = 0; i < 20 && now != (int)to_speaker; i++) {
                    svcSleepThread(100000000ULL);
                    now = spkctl_mode();
                }
                if (now != (int)to_speaker) return;
                S.speaker = to_speaker;
                S.switching = true;
                S.pending_on = was_on;
                // one re-enumeration, read again by audio within 0.5 s: quick_settle is enough
                Result rc = fm_reset(&S.dev);
                log_file("ovl: mode %s, earphone reset %s\n", to_speaker ? "speaker" : "earphone", rc_str(rc));
            });
            return true;
        });
        list->addItem(m_output);

        m_volume = new tsl::elm::ListItem("볼륨");
        m_volume->setClickListener([](u64 keys) {
            int d = (keys & HidNpadButton_AnyLeft) ? -1 : (keys & (HidNpadButton_AnyRight | HidNpadButton_A)) ? 1 : 0;
            if (!d) return false;
            with_dev([d] { set_volume(d); });
            return true;
        });
        list->addItem(m_volume);

        m_freq = new tsl::elm::ListItem("주파수");
        m_freq->setClickListener([](u64 keys) {
            int d = (keys & HidNpadButton_AnyLeft) ? -1 : (keys & HidNpadButton_AnyRight) ? 1 : 0;
            if (keys & (HidNpadButton_L | HidNpadButton_R)) d = (keys & HidNpadButton_L) ? -10 : 10;
            if (!d) return false;
            with_dev([d] { tune((u16)(S.freq + d * reg().step)); });
            return true;
        });
        list->addItem(m_freq);

        m_station = new tsl::elm::ListItem("방송 이동");
        m_station->setClickListener([](u64 keys) {
            int d = (keys & HidNpadButton_AnyLeft) ? -1 : (keys & (HidNpadButton_AnyRight | HidNpadButton_A)) ? 1 : 0;
            if (!d) return false;
            with_dev([d] { step_station(d); });
            return true;
        });
        list->addItem(m_station);

        add_stations(list, "찾은 채널", S.ch, S.nch);
        add_stations(list, "즐겨찾기", S.fav, S.nfav);
        if (!S.nch && !S.nfav)
            list->addItem(new tsl::elm::CategoryHeader("채널 목록은 라디오 앱의 '채널 찾기'로 만듭니다"));

        frame->setContent(list);
        refresh();
        return frame;
    }

    void update() override {
        mutexLock(&S.lock);
        if (!dev_open()) S.synced = false;
        else if (!S.synced) {
            sync_from_device();
            if (S.synced && S.switching) {   // back after a mode switch
                S.switching = false;
                S.dev.quick_settle = true;
                if (S.pending_on) radio_on();
                S.pending_on = false;
            }
        }
        mutexUnlock(&S.lock);
        if (++m_tick % 10 == 0) refresh();   // the labels are cheap, but there is no need every frame
    }

    bool handleInput(u64 keysDown, u64, const HidTouchState &, HidAnalogStickState, HidAnalogStickState) override {
        // ZL / ZR anywhere: previous / next station, like in the app
        int d = (keysDown & HidNpadButton_ZL) ? -1 : (keysDown & HidNpadButton_ZR) ? 1 : 0;
        if (!d) return false;
        with_dev([d] { step_station(d); });
        refresh();
        return true;
    }

private:
    tsl::elm::ListItem *m_output = nullptr;
    tsl::elm::ListItem *m_status = nullptr, *m_power = nullptr, *m_volume = nullptr, *m_freq = nullptr,
                       *m_station = nullptr;
    u32 m_tick = 0;

    static void add_stations(tsl::elm::List *list, const char *title, const u16 *l, int n) {
        if (!n) return;
        list->addItem(new tsl::elm::CategoryHeader(title));
        for (int i = 0; i < n; i++) {
            u16 f = l[i];
            auto item = new tsl::elm::ListItem(fmt_freq(f));
            item->setClickListener([f](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                with_dev([f] { tune(f); });
                return true;
            });
            list->addItem(item);
        }
    }

    void refresh() {
        bool ready = dev_open() && S.synced;
        char b[32];
        m_status->setValue(status_text(), !ready);
        m_power->setValue(S.on ? "켜짐 (A: 끄기)" : "꺼짐 (A: 켜기)", !ready);
        if (S.switching) m_output->setValue("바꾸는 중... (몇 초)", true);
        else m_output->setValue(S.speaker ? "스피커 (A: 이어폰으로)" : "이어폰 (A: 스피커로)", !ready);
        snprintf(b, sizeof(b), "< %u / 15 >", S.volume);
        m_volume->setValue(b, !ready);
        m_freq->setValue("< " + fmt_freq(S.freq) + " >", !ready);
        m_station->setValue((S.nch || S.nfav) ? "< ZL    ZR >" : "목록 없음", !ready);
    }
};

class RadioOverlay : public tsl::Overlay {
public:
    // libtesla opens fs but mounts no sdmc: (0.1.0 crashed in its first mkdir), and closes its sm
    // session after its own services.
    void initServices() override {
        m_sdmc = R_SUCCEEDED(fsdevMountSdmc());
        log_file("ovl: eofm-ovl " OVL_VERSION " start\n");
        mutexInit(&S.lock);
        settings_load();
        S.dev.passive = true;
        S.dev.quick_settle = true;
        Result rc = 0;
        tsl::hlp::doWithSmSession([&rc] { rc = usbHsInitialize(); });
        log_file("ovl: usb:hs: %s\n", rc_str(rc));
        if (R_FAILED(rc)) return;
        S.stop = false;
        if (R_SUCCEEDED(threadCreate(&S.thread, poll_thread, NULL, NULL, 0x4000, 0x2C, -2))) {
            if (R_SUCCEEDED(threadStart(&S.thread))) m_thread = true;
            else threadClose(&S.thread);
        }
    }

    void exitServices() override {
        S.stop = true;
        if (m_thread) {
            threadWaitForExit(&S.thread);
            threadClose(&S.thread);
        }
        if (S.dirty) settings_save();
        fm_close(&S.dev);
        usbHsExit();
        log_file("ovl: exit\n");
        if (m_sdmc) fsdevUnmountDevice("sdmc");
    }

    void onShow() override {
        mutexLock(&S.lock);
        S.dev.passive = false;
        fm_poll_device(&S.dev);   // take IF0 now rather than within the thread's next second
        mutexUnlock(&S.lock);
    }

    void onHide() override {
        mutexLock(&S.lock);
        S.dev.passive = true;
        fm_poll_device(&S.dev);   // gives IF0 back (the eofm app may want it)
        S.synced = false;
        mutexUnlock(&S.lock);
        if (S.dirty) {
            settings_save();
            S.dirty = false;
        }
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<RadioGui>(); }

private:
    bool m_thread = false, m_sdmc = false;
};

}  // namespace

int main(int argc, char **argv) { return tsl::loop<RadioOverlay>(argc, argv); }
