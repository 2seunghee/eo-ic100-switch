// Host tests for the pure parts of usbhs-logmitm: setup-packet decoding, descriptor lines, config.
#include <stdio.h>
#include <string.h>

#include "../source/config.h"
#include "../source/decode.h"
#include "../source/rewrite.h"

static int checks, failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

static void check_setup(const unsigned char s[8], const char *want) {
    char out[200];
    usb_setup_str(out, sizeof(out), s);
    checks++;
    if (strcmp(out, want)) {
        failures++;
        printf("FAIL setup\n  got  \"%s\"\n  want \"%s\"\n", out, want);
    }
}

static int warned;
static void warn(const char *m) {
    warned++;
    printf("  (warning) %s\n", m);
}

int main(void) {
    // Standard requests the driver sends while it enumerates.
    check_setup((const unsigned char[]){0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0x09, 0x00},
                "IN  std    dev GET_DESCRIPTOR CONFIG idx 0 lang/if 0x0 len 9");
    check_setup((const unsigned char[]){0x80, 0x06, 0x02, 0x03, 0x09, 0x04, 0xFF, 0x00},
                "IN  std    dev GET_DESCRIPTOR STRING idx 2 lang/if 0x409 len 255");
    check_setup((const unsigned char[]){0x81, 0x06, 0x00, 0x22, 0x04, 0x00, 0x40, 0x00},
                "IN  std    if  GET_DESCRIPTOR HID_REPORT idx 0 lang/if 0x4 len 64");
    check_setup((const unsigned char[]){0x01, 0x0B, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00},
                "OUT std    if  SET_INTERFACE if 2 alt 1");
    // UAC1: sampling frequency on the endpoint, volume/mute on feature unit 5.
    check_setup((const unsigned char[]){0x22, 0x01, 0x00, 0x01, 0x03, 0x00, 0x03, 0x00},
                "OUT class  ep  UAC SET_CUR ep 0x03 sel 0x01(SAMPLING_FREQ) len 3");
    check_setup((const unsigned char[]){0xA2, 0x81, 0x00, 0x01, 0x03, 0x00, 0x03, 0x00},
                "IN  class  ep  UAC GET_CUR ep 0x03 sel 0x01(SAMPLING_FREQ) len 3");
    check_setup((const unsigned char[]){0xA1, 0x82, 0x01, 0x02, 0x00, 0x05, 0x02, 0x00},
                "IN  class  if  UAC GET_MIN entity 5 if 0 sel 0x02(VOLUME) ch 1 len 2");
    check_setup((const unsigned char[]){0x21, 0x01, 0x00, 0x01, 0x00, 0x05, 0x01, 0x00},
                "OUT class  if  UAC SET_CUR entity 5 if 0 sel 0x01(MUTE) ch 0 len 1");
    check_setup((const unsigned char[]){0x21, 0x0A, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00},
                "OUT class  if  HID SET_IDLE wValue 0x0000 if 3 len 0");
    check_setup((const unsigned char[]){0xC0, 0xA1, 0x34, 0x12, 0x00, 0x00, 0x04, 0x00},
                "IN  vendor dev req 0xa1 wValue 0x1234 wIndex 0x0000 len 4");

    char out[200];
    // Descriptors of the a05b (docs/PLAN.md §0).
    const unsigned char dev[18] = {18, 1, 0x00, 0x02, 0, 0, 0, 64, 0xE8, 0x04, 0x5B, 0xA0, 0x00, 0x01, 1, 2, 3, 1};
    usb_desc_str(out, sizeof(out), dev, sizeof(dev));
    CHECK(!strcmp(out, "DEVICE usb 2.00 class 00/00/00 mps0 64 vid:pid 04e8:a05b bcd 0100 cfgs 1"));
    const unsigned char cfg[9] = {9, 2, 0x59, 0x01, 5, 1, 0, 0x80, 50};
    usb_desc_str(out, sizeof(out), cfg, sizeof(cfg));
    CHECK(!strcmp(out, "CONFIG total 345 ifs 5 value 1 attr 0x80 power 100 mA"));
    const unsigned char ep[9] = {9, 5, 0x03, 0x09, 0xC0, 0x00, 1, 0, 0};
    usb_desc_str(out, sizeof(out), ep, sizeof(ep));
    CHECK(!strcmp(out, "ENDPOINT 0x03 attr 0x09 mps 192 interval 1"));
    const unsigned char cs[3] = {10, 0x24, 1};
    usb_desc_str(out, sizeof(out), cs, sizeof(cs));
    CHECK(!strcmp(out, "CS_INTERFACE len 10"));
    usb_desc_str(out, sizeof(out), cs, 1);
    CHECK(!strcmp(out, "(short)"));

    CHECK(!strcmp(usbhs_cmd_name(OBJ_ROOT, 7), "AcquireUsbIf"));
    CHECK(!strcmp(usbhs_cmd_name(OBJ_IF, 5), "CtrlXferAsync"));
    CHECK(!strcmp(usbhs_cmd_name(OBJ_EP, 6), "BatchBufferAsync"));
    CHECK(usbhs_cmd_name(OBJ_EP, 99) == NULL);
    CHECK(usbhs_cmd_is_hot(OBJ_EP, 6) && !usbhs_cmd_is_hot(OBJ_IF, 5) && !usbhs_cmd_is_hot(OBJ_ROOT, 4));

    // Config.
    lm_config c;
    config_defaults(&c);
    CHECK(c.enabled && c.homebrew && !c.all && c.n_programs == 1 && c.programs[0] == 0x0100000000000014ULL);
    CHECK(config_wants(&c, 0x0100000000000014ULL, 0));
    CHECK(!config_wants(&c, 0x0100000000000013ULL, 0));
    CHECK(config_wants(&c, 0x010000000000100DULL, 1));   // hbmenu NRO in the album applet
    CHECK(!config_wants(&c, 0x010000000000100DULL, 0));

    warned = 0;
    int p = config_parse(&c,
                         "; comment\n[usbhslog]\nenabled = 1\nservices=usb:hs, usb:hs:a ,bogus\n"
                         "programs=hid, 0100000000001000 ,xyz\nread_ctrl_data=0\nsync=off\nhot_detail=7\n"
                         "buf_dump=9999\nfoo=1\nnoequals\n",
                         warn);
    CHECK(p == 5 && warned == 5);   // bogus service, xyz, buf_dump range, foo, noequals
    CHECK(c.n_services == 2 && !strcmp(c.services[0], "usb:hs") && !strcmp(c.services[1], "usb:hs:a"));
    CHECK(c.n_programs == 2 && c.programs[0] == 0x0100000000000013ULL && c.programs[1] == 0x0100000000001000ULL);
    CHECK(!c.homebrew && !c.all && !c.read_ctrl_data && !c.sync && c.hot_detail == 7 && c.buf_dump == 64);
    CHECK(!config_wants(&c, 0x0100000000000014ULL, 1));

    p = config_parse(&c, "services=\r\nprograms=all\r\nenabled=0\r\n", warn);
    CHECK(p == 0 && c.n_services == 1 && !strcmp(c.services[0], "usb:hs") && c.all && !c.enabled);
    CHECK(config_wants(&c, 0x0100000000000099ULL, 0));

    // Rewrites on the a05b's config descriptor as audio read it on the console (C-0 log 2026-10-22).
    unsigned char orig[512], d[512];
    FILE *f = fopen("eo_a05b_config.bin", "rb");
    size_t n = f ? fread(orig, 1, sizeof(orig), f) : 0;
    if (f) fclose(f);
    CHECK(n == 345);
    rw_notes notes;
    memcpy(d, orig, n);
    CHECK(rewrite_config(d, n, 0, &notes) == 0 && !memcmp(d, orig, n));
    memcpy(d, orig, n);
    CHECK(rewrite_config(d, n, RW_TERMINAL, &notes) == 2 && notes.n == 2);
    for (int k = 0; k < notes.n; k++) printf("  %s\n", notes.text[k]);
    int diff = 0;
    for (size_t i = 0; i < n; i++) diff += d[i] != orig[i];
    CHECK(diff == 3);   // 0x0402 -> 0x0201 changes 2 bytes, 0x0402 -> 0x0302 one byte
    CHECK(d[28 + 4] == 0x01 && d[28 + 5] == 0x02);   // IT1 at @028
    CHECK(d[87 + 4] == 0x02 && d[87 + 5] == 0x03);   // OT6 at @087
    CHECK(d[53 + 4] == 0x01 && d[53 + 5] == 0x01);   // OT3 (USB streaming) untouched
    CHECK(rewrite_config(d, n, RW_TERMINAL, &notes) == 0);   // idempotent
    memcpy(d, orig, n);
    int all = rewrite_config(d, n, RW_TERMINAL | RW_SINGLE_RATE | RW_ALT1_ONLY | RW_MPS48, &notes);
    for (int k = 0; k < notes.n; k++) printf("  %s\n", notes.text[k]);
    // terminals 2, alt 1's rate list 1 (alts 2/3 are hidden before their lists), alts 2/3 hidden 2,
    // 0x03 alt 1 packet size 1 (0x83 is already 192)
    CHECK(all == 6);
    CHECK(d[203 + 5] == 0xFF && d[249 + 5] == 0xFF && d[157 + 5] == 0x01);
    CHECK(d[187 + 4] == 192 && d[187 + 5] == 0);   // 0x03 alt 1: 384 -> 192
    CHECK(d[132 + 4] == 192);                      // 0x83 already 192
    CHECK(d[173 + 8] == 0x80 && d[173 + 11] == 0x80 && d[173 + 12] == 0xBB);   // alt 1 rates: 48000, 48000
    CHECK(d[0] == 9 && d[2] == 0x59 && d[3] == 0x01);   // wTotalLength kept
    CHECK(rewrite_config(d, 5, RW_TERMINAL, &notes) == 0);   // too short
    unsigned r;
    const char *bad;
    CHECK(rewrite_parse("terminal", &r, &bad) == 0 && r == RW_TERMINAL);
    CHECK(rewrite_parse(" none ", &r, &bad) == 0 && r == 0);
    CHECK(rewrite_parse("it_mic,single_rate", &r, &bad) == 0 && r == (RW_IT_MIC | RW_SINGLE_RATE));
    CHECK(rewrite_parse("terminal,bogus", &r, &bad) == -1 && !strcmp(bad, "bogus"));
    rewrite_names(RW_TERMINAL | RW_MPS48, out, sizeof(out));
    CHECK(!strcmp(out, "ot_headphones,it_mic,mps48"));
    config_defaults(&c);
    CHECK(c.rewrite == RW_TERMINAL);
    CHECK(config_parse(&c, "rewrite=none\n", warn) == 0 && c.rewrite == 0);
    CHECK(config_parse(&c, "rewrite=terminal,nope\n", warn) == 1 && c.rewrite == RW_TERMINAL);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
