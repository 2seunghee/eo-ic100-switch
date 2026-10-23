// Screen preview on a PC: compiles eofm's real main.c and gfx.c against tests/host/switch.h and
// renders draw() for a few states into PPM images (tests/Makefile: make preview).
#define main eofm_main
#include "../source/main.c"
#undef main

u64 g_fake_ms = 100000;
extern const char *g_host_fonts[2];
extern const char *g_host_out;

// ---- stubs for the parts that talk to the console
void fm_poll_device(fm_dev *d) {}
void fm_close(fm_dev *d) {}
Result fm_set(fm_dev *d, u16 cmd, u16 value) { return 0; }
Result fm_get(fm_dev *d, u16 cmd, u8 out[2]) { return out[0] = out[1] = 0; }
Result fm_query(fm_dev *d, u8 out[12], u32 *got) { return *got = 0; }
Result fm_reset(fm_dev *d) { return 0; }
bool spk_start(void) { return false; }
void spk_stop(void) {}
void spk_get(spk_status *out) { memset(out, 0, sizeof(*out)); }
void spk_set_gain(int vol15) {}
void log_init(void) {}
void log_exit(void) {}
const char *log_path(void) { return "-"; }
void log_file(const char *fmt, ...) {}
const char *rc_str(Result rc) { return "OK"; }

static void shot(const char *name) {
    g_host_out = name;
    draw();
}

int main(int argc, char **argv) {
    g_host_fonts[0] = argc > 1 ? argv[1] : "malgun.ttf";
    g_host_fonts[1] = argc > 2 ? argv[2] : NULL;
    char err[64];
    if (!gfx_init(err, sizeof(err))) {
        printf("font: %s\n", err);
        return 1;
    }
    R.freq = 8750;
    R.volume = 6;
    R.rds = true;
    R.tab = TAB_CH;
    rds_reset(&R.rd);

    R.dev.state = FMDEV_OPEN;
    shot("preview_off.ppm");   // like the reference screenshot: radio off, no channels

    R.mode = MODE_EARPHONE;
    R.freq = 8910;
    R.rssi = 41;
    u16 ch[] = {8910, 8970, 9190, 9310, 9510, 9590, 9790, 9930, 10070, 10190, 10370, 10490, 10670};
    memcpy(R.ch, ch, sizeof(ch));
    R.nch = sizeof(ch) / sizeof(ch[0]);
    R.fav[0] = 8910;
    R.fav[1] = 9510;
    R.nfav = 2;
    uint16_t g0[4] = {0x1234, 0x0000, 0, ('K' << 8) | 'B'}, g1[4] = {0x1234, 0x0001, 0, ('S' << 8) | ' '},
             g2[4] = {0x1234, 0x0002, 0, ('F' << 8) | 'M'}, g3[4] = {0x1234, 0x0003, 0, (' ' << 8) | '1'};
    rds_group(&R.rd, g0), rds_group(&R.rd, g1), rds_group(&R.rd, g2), rds_group(&R.rd, g3);
    shot("preview_on.ppm");

    R.tab = TAB_FAV;
    rds_reset(&R.rd);
    R.scanning = true;
    R.nch = 5;
    shot("preview_scan.ppm");
    gfx_exit();
    return 0;
}
