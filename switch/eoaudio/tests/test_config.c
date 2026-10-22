// Host-side unit tests for source/config.c and for the descriptor facts the sysmodule relies on
// (source/usbdesc.c against the user's own a05b dump in fixtures/).
//   make -C switch/eoaudio/tests check
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/config.h"
#include "../source/usbdesc.h"

static int g_fail, g_pass;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (cond) g_pass++;                                                           \
        else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }     \
    } while (0)

typedef struct {
    int  count;
    int  lines[16];
    char last[160];
} warns;

static void on_warn(void *ctx, int line, const char *msg) {
    warns *w = ctx;
    if (w->count < 16) w->lines[w->count] = line;
    w->count++;
    snprintf(w->last, sizeof(w->last), "%s", msg);
}

static bool same(const eo_config *a, const eo_config *b) {
    return a->config_version == b->config_version && a->mode == b->mode && a->start_delay_s == b->start_delay_s &&
           a->log_level == b->log_level && a->health_s == b->health_s && a->log_keep == b->log_keep &&
           a->speaker_mute == b->speaker_mute && a->volume_db == b->volume_db &&
           a->volume_step_db == b->volume_step_db && a->volume_min_db == b->volume_min_db &&
           a->volume_max_db == b->volume_max_db && a->volume_follow == b->volume_follow &&
           a->volume_osd == b->volume_osd && a->media_button == b->media_button &&
           a->media_app_count == b->media_app_count && memcmp(a->media_apps, b->media_apps, sizeof(a->media_apps)) == 0 &&
           a->play_button == b->play_button && a->target_ms == b->target_ms && a->rec_frames == b->rec_frames &&
           a->rec_buffers == b->rec_buffers && a->mute_test == b->mute_test && a->acquire == b->acquire &&
           a->tone_hz == b->tone_hz && a->tone_dbfs == b->tone_dbfs && a->pid_count == b->pid_count &&
           memcmp(a->pids, b->pids, sizeof(a->pids)) == 0 && a->settle_ms == b->settle_ms && a->resets == b->resets &&
           a->iso_urbs == b->iso_urbs && a->iso_batches == b->iso_batches && a->iso_lead == b->iso_lead;
}

static void test_default_text(void) {
    // The shipped config.ini must say exactly what the built-in defaults are.
    eo_config def, parsed;
    cfg_defaults(&def);
    CHECK(def.config_version == 0);   // a file without config_version is a v0.1 file
    def.config_version = CFG_VERSION;
    cfg_defaults(&parsed);
    parsed.mode = MODE_OFF;   // make sure the text really sets these
    parsed.rec_frames = 1;
    parsed.pid_count = 0;
    parsed.speaker_mute = SPK_OFF;
    parsed.play_button = PLAY_NONE;
    parsed.log_level = LOGLVL_DEBUG;
    parsed.volume_follow = true;
    parsed.volume_osd = false;
    parsed.media_button = BTN_ZR;
    parsed.media_app_count = 0;
    warns w = {0};
    int problems = cfg_parse(&parsed, cfg_default_text, strlen(cfg_default_text), on_warn, &w);
    if (problems) printf("  default text warning: %s\n", w.last);
    CHECK(problems == 0 && w.count == 0);
    CHECK(same(&def, &parsed));
    CHECK(parsed.config_version == CFG_VERSION);
    CHECK(def.mode == MODE_STREAM);   // v0.2: all audio to the earphone by default
    CHECK(def.speaker_mute == SPK_AUTO && def.play_button == PLAY_MEDIA && !def.volume_follow && def.volume_osd);
    CHECK(def.media_app_count == 1 && cfg_media_app(&def, 0x01003A400C3DA000ULL) && def.media_button == BTN_A);
    // 15 levels from -42 to 0 dB in 3 dB steps, like the console volume.
    CHECK((def.volume_max_db - def.volume_min_db) / def.volume_step_db + 1 == 15);
    CHECK(def.log_level == LOGLVL_INFO && def.health_s == 10);
    CHECK(def.volume_min_db <= def.volume_db && def.volume_db <= def.volume_max_db);
    CHECK(def.iso_urbs * def.iso_batches <= 16);
    char line[640];
    int n = cfg_describe(&def, line, sizeof(line));
    CHECK(n > 0 && (size_t)n < sizeof(line));
    CHECK(strstr(line, "mode=stream") && strstr(line, "speaker_mute=auto") && strstr(line, "pids=a054,a059,a05b"));
}

static void test_v2_keys(void) {
    eo_config c;
    cfg_defaults(&c);
    const char text[] = "speaker_mute=OFF\nplay_button=none\nlog_level=debug\nvolume_follow=0\nhealth_s=0\n"
                        "volume_min_db=-10\nvolume_max_db=-20\nspeaker_mute=whisper\n";
    warns w = {0};
    int problems = cfg_parse(&c, text, sizeof(text) - 1, on_warn, &w);
    CHECK(c.speaker_mute == SPK_OFF && c.play_button == PLAY_NONE && c.log_level == LOGLVL_DEBUG && !c.volume_follow);
    CHECK(c.health_s == 1);   // clamped
    // min above max: min is pulled down to max, and volume_db into the range.
    CHECK(c.volume_min_db == -20.0f && c.volume_max_db == -20.0f && c.volume_db == -20.0f);
    CHECK(problems == 3);     // health_s range, speaker_mute value, min > max
    CHECK(strstr(w.last, "volume_min_db") != NULL);
    CHECK(strcmp(cfg_speaker_name(SPK_VOLUME0), "volume0") == 0 && strcmp(cfg_play_name(PLAY_MUTE), "mute") == 0);
    CHECK(strcmp(cfg_mode_name(MODE_TONE), "tone") == 0);
}

static void test_sections(void) {
    // Ultrahand's set-ini-val needs a section; [eoaudio] (or any other) lines are skipped, and a key
    // appended at the end of the file overrides the earlier one.
    eo_config c;
    cfg_defaults(&c);
    const char text[] = "enabled=1\nvolume_osd=1\n\n[eoaudio]\nenabled = 0\n  [other]\nvolume_osd=0\n";
    warns w = {0};
    CHECK(cfg_parse(&c, text, sizeof(text) - 1, on_warn, &w) == 0 && w.count == 0);
    CHECK(!c.enabled && !c.volume_osd);
    CHECK(strstr(cfg_default_text, "[eoaudio]\n") != NULL);
}

static void test_media_keys(void) {
    eo_config c;
    cfg_defaults(&c);
    const char text[] = "play_button=media\nmedia_button=ZR\nmedia_apps=0100000000010000, 01003a400c3da000\n";
    CHECK(cfg_parse(&c, text, sizeof(text) - 1, NULL, NULL) == 0);
    CHECK(c.play_button == PLAY_MEDIA && c.media_button == BTN_ZR && strcmp(cfg_button_name(BTN_ZR), "zr") == 0);
    CHECK(c.media_app_count == 2 && cfg_media_app(&c, 0x0100000000010000ULL) && cfg_media_app(&c, 0x01003A400C3DA000ULL));
    CHECK(!cfg_media_app(&c, 0x0100000000010001ULL));
    CHECK(cfg_parse(&c, "media_apps=\n", 12, NULL, NULL) == 0 && c.media_app_count == 0);   // none
    CHECK(cfg_parse(&c, "media_apps=xyz\n", 15, NULL, NULL) == 1);
    CHECK(cfg_parse(&c, "media_button=home\n", 18, NULL, NULL) == 1 && c.media_button == BTN_ZR);
}

static void test_edits(void) {
    const char text[] =
        "\xEF\xBB\xBF"                     // UTF-8 BOM (Windows Notepad)
        "# 주석\r\n"
        "  MODE = Stream   # 2단계\r\n"
        "volume_db=-35.5\r\n"
        "speaker_mute = volume0 ; trailing comment\r\n"
        "pids = A05B, 0xa054\r\n"
        "acquire=off\r\n"
        "target_ms=500\r\n"                // out of range: clamped to 150
        "no_such_key=1\r\n"                // unknown
        "rec_frames=abc\r\n"               // not a number: ignored
        "this line has no equals sign\r\n"
        "iso_urbs=8\r\n"
        "iso_batches=4\r\n"                // 32 urbs in flight: reduced to 8 x 2
        "\r\n";
    eo_config c;
    cfg_defaults(&c);
    warns w = {0};
    int problems = cfg_parse(&c, text, sizeof(text) - 1, on_warn, &w);
    CHECK(c.mode == MODE_STREAM);
    CHECK(c.volume_db == -35.5f);
    CHECK(c.speaker_mute == SPK_VOLUME0);
    CHECK(c.pid_count == 2 && c.pids[0] == 0xA05B && c.pids[1] == 0xA054);
    CHECK(cfg_pid_ok(&c, 0xA054) && !cfg_pid_ok(&c, 0xA059));
    CHECK(c.acquire == false);
    CHECK(c.target_ms == 150);
    CHECK(c.rec_frames == 240);
    CHECK(c.iso_urbs == 8 && c.iso_batches == 2);
    CHECK(problems == 5 && w.count == 5);
    CHECK(w.lines[0] == 7 && w.lines[1] == 8 && w.lines[2] == 9 && w.lines[3] == 10 && w.lines[4] == 0);
}

static void test_bad_values(void) {
    eo_config c;
    cfg_defaults(&c);
    const char text[] = "mode=loud\npids=xyz\nmute_test=maybe\nspeaker_mute=sometimes\nvolume_max_db=-30\n";
    warns w = {0};
    int problems = cfg_parse(&c, text, sizeof(text) - 1, on_warn, &w);
    CHECK(problems == 4);
    CHECK(c.mode == MODE_STREAM && c.pid_count == 3 && c.mute_test && c.speaker_mute == SPK_AUTO);
    // volume_db (-20) above the new maximum is pulled down to it.
    CHECK(c.volume_max_db == -30.0f && c.volume_db == -30.0f);
    CHECK(cfg_parse(&c, "", 0, NULL, NULL) == 0);
    CHECK(cfg_parse(&c, "pids=,,\n", 8, NULL, NULL) == 1);
    CHECK(cfg_parse(&c, "pids=1,2,3,4,5,6,7,8,9\n", 23, NULL, NULL) == 1);   // too many
    CHECK(cfg_parse(&c, "mode=off", 8, NULL, NULL) == 0 && c.mode == MODE_OFF);   // no final newline
}

// ------------------------------------------------------------------ descriptors (user's a05b unit)

static size_t load_hex(const char *path, uint8_t *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    size_t n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        for (char *tok = strtok(line, " \t\r\n,"); tok; tok = strtok(NULL, " \t\r\n,")) {
            if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) tok += 2;
            for (size_t i = 0; tok[i] && tok[i + 1] && n < cap; i += 2) {
                unsigned v;
                if (sscanf(tok + i, "%2x", &v) != 1) { printf("bad hex token %s\n", tok); exit(2); }
                out[n++] = (uint8_t)v;
            }
        }
    }
    fclose(f);
    return n;
}

static void test_a05b(const char *fixtures) {
    char path[512];
    snprintf(path, sizeof(path), "%s/eo-ic100_04e8_a05b_config.hex", fixtures);
    static uint8_t buf[1024];
    size_t len = load_hex(path, buf, sizeof(buf));
    static ud_config c;
    CHECK(len == 345);
    CHECK(ud_parse_config(buf, len, &c) && c.warnings == 0);
    CHECK(c.has_ac && c.ac_iface == 0);
    // What device.c configures: IF2 alt1, EP 0x03 at 192 B/ms, SET_CUR 48000, FU5 volume + mute.
    ud_stream p;
    CHECK(ud_find_stream(&c, true, 48000, 16, 2, &p));
    CHECK(p.alt && p.alt->number == 2 && p.alt->alt == 1);
    CHECK(p.ep && p.ep->address == 0x03 && p.bytes_per_ms == 192);
    CHECK(p.has_rate_ctrl);
    CHECK(p.fu_id == 5 && (p.fu_controls & UD_FU_VOLUME) && (p.fu_controls & UD_FU_MUTE));
    CHECK(p.terminal_id == 6 && p.terminal_type == 0x0402);
    // The remote is the first HID interface (IF3, interrupt IN 0x84).
    const ud_alt *hid = NULL;
    for (int i = 0; i < c.alt_count && !hid; i++)
        if (c.alts[i].cls == 3) hid = &c.alts[i];
    CHECK(hid && hid->number == 3 && hid->ep_count == 1 && hid->eps[0].address == 0x84);
    CHECK(hid && ud_ep_xfer(&hid->eps[0]) == 3);
}

int main(int argc, char **argv) {
    test_default_text();
    test_v2_keys();
    test_media_keys();
    test_sections();
    test_edits();
    test_bad_values();
    test_a05b(argc > 1 ? argv[1] : "../../../fixtures");
    printf("test_config: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
