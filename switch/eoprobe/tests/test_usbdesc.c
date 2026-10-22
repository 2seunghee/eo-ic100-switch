// Host-side unit test for source/usbdesc.c against the EO-IC100 fixture.
//   make -C switch/eoprobe/tests        (needs a host C compiler, not devkitPro)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/usbdesc.h"

static int g_fail, g_pass;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) g_pass++;                                                    \
        else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

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

static void test_fixture(const uint8_t *buf, size_t len) {
    static ud_config c;
    CHECK(len == 305);
    CHECK(ud_parse_config(buf, len, &c));
    CHECK(c.warnings == 0);
    CHECK(c.total_length == 305 && c.num_interfaces == 5 && c.max_power == 50);
    CHECK(c.has_ac && c.ac_iface == 0 && c.bcd_adc == 0x0100);
    CHECK(c.collection_count == 2 && c.collection[0] == 1 && c.collection[1] == 2);
    CHECK(c.unit_count == 6);
    CHECK(c.alt_count == 8);

    const ud_unit *fu2 = ud_find_unit(&c, 2);
    CHECK(fu2 && fu2->subtype == UD_AC_FU && fu2->control_count == 3 && fu2->controls[0] == 0x3);
    const ud_unit *ot3 = ud_find_unit(&c, 3);
    CHECK(ot3 && ot3->subtype == UD_AC_OT && ot3->terminal_type == 0x0402 && ot3->sources[0] == 2);

    ud_stream p;
    CHECK(ud_find_stream(&c, true, 48000, 16, 2, &p));
    CHECK(p.alt && p.alt->number == 2 && p.alt->alt == 1);
    CHECK(p.ep && p.ep->address == 0x03 && ud_ep_packet(p.ep) == 384 && p.ep->length == 9);
    CHECK(ud_ep_sync(p.ep) == UD_SYNC_SYNC);
    CHECK(p.bytes_per_ms == 192);
    CHECK(p.has_rate_ctrl && p.needs_rate_set);
    CHECK(p.fu_id == 2 && (p.fu_controls & (UD_FU_MUTE | UD_FU_VOLUME)) == (UD_FU_MUTE | UD_FU_VOLUME));
    CHECK(p.terminal_id == 3 && p.terminal_type == 0x0402);
    CHECK(p.alt->rate_count == 3 && p.alt->rates[1] == 48000 && p.alt->rates[2] == 96000);

    ud_stream p24;
    CHECK(ud_find_stream(&c, true, 96000, 24, 2, &p24));
    CHECK(p24.alt && p24.alt->alt == 2 && ud_ep_packet(p24.ep) == 576);
    CHECK(!ud_find_stream(&c, true, 48000, 16, 1, &p24));

    ud_stream cap;
    CHECK(ud_find_stream(&c, false, 48000, 16, 2, &cap));
    CHECK(cap.alt && cap.alt->number == 1 && cap.ep->address == 0x83 && !cap.needs_rate_set);
    CHECK(cap.terminal_id == 4 && cap.fu_id == 5);

    int hid = 0;
    for (int i = 0; i < c.alt_count; i++)
        if (c.alts[i].has_hid) {
            hid++;
            CHECK(c.alts[i].ep_count == 1 && ud_ep_xfer(&c.alts[i].eps[0]) == 3);
        }
    CHECK(hid == 2);

    uint32_t f = ud_check(&c);
    uint32_t expect = UD_FLAG_CAPTURE_FIRST | UD_FLAG_OT_NOT_OUTPUT | UD_FLAG_MULTI_RATE |
                      UD_FLAG_SYNC_NOT_ADAPTIVE | UD_FLAG_SHARED_EP_NUMBER | UD_FLAG_STEREO_MIC |
                      UD_FLAG_EXTRA_INTERFACES;
    CHECK(f == expect);
    if (f != expect) printf("  flags 0x%x expected 0x%x\n", f, expect);
}

// The user's own unit (04e8:a05b): terminal IDs swapped vs a054, 48/96 kHz only, extra 32-bit alt3.
static void test_a05b(const uint8_t *buf, size_t len) {
    static ud_config c;
    CHECK(len == 345);
    CHECK(ud_parse_config(buf, len, &c));
    CHECK(c.warnings == 0 && c.total_length == 345 && c.num_interfaces == 5);
    CHECK(c.unit_count == 6 && c.alt_count == 9);

    ud_stream p;
    CHECK(ud_find_stream(&c, true, 48000, 16, 2, &p));
    CHECK(p.alt && p.alt->number == 2 && p.alt->alt == 1 && p.alt->rate_count == 2);
    CHECK(p.ep && p.ep->address == 0x03 && ud_ep_packet(p.ep) == 384);
    CHECK(p.bytes_per_ms == 192 && p.needs_rate_set);
    CHECK(p.fu_id == 5 && p.terminal_id == 6 && p.terminal_type == 0x0402);

    ud_stream p32;
    CHECK(ud_find_stream(&c, true, 48000, 32, 2, &p32));
    CHECK(p32.alt && p32.alt->alt == 3 && ud_ep_packet(p32.ep) == 768);
    CHECK(!ud_find_stream(&c, true, 44100, 16, 2, &p32));

    ud_stream cap;
    CHECK(ud_find_stream(&c, false, 48000, 16, 2, &cap));
    CHECK(cap.alt && cap.alt->number == 1 && cap.fu_id == 2 && cap.terminal_id == 1 && cap.terminal_type == 0x0402);

    for (int i = 0; i < c.alt_count; i++)
        if (c.alts[i].has_hid) CHECK(c.alts[i].eps[0].interval == 4);

    uint32_t expect = UD_FLAG_CAPTURE_FIRST | UD_FLAG_OT_NOT_OUTPUT | UD_FLAG_MULTI_RATE |
                      UD_FLAG_SYNC_NOT_ADAPTIVE | UD_FLAG_SHARED_EP_NUMBER | UD_FLAG_STEREO_MIC |
                      UD_FLAG_EXTRA_INTERFACES;
    CHECK(ud_check(&c) == expect);
}

static void test_variants(const uint8_t *orig, size_t len) {
    static uint8_t buf[512];
    static ud_config c;

    // The Headset (02 04) output terminal (id3 on a054, id6 on a05b) -> Headphones (02 03)
    memcpy(buf, orig, len);
    uint8_t *hit = NULL;
    for (size_t i = 0; i + 6 <= len; i++)
        if (buf[i] == 0x09 && buf[i + 1] == 0x24 && buf[i + 2] == 0x03 && buf[i + 4] == 0x02 && buf[i + 5] == 0x04)
            hit = buf + i;
    CHECK(hit != NULL);
    if (hit) hit[5] = 0x03;
    CHECK(ud_parse_config(buf, len, &c));
    CHECK(!(ud_check(&c) & UD_FLAG_OT_NOT_OUTPUT));

    // Playback endpoints synchronous (0x0d) -> adaptive (0x09)
    memcpy(buf, orig, len);
    for (size_t i = 0; i + 4 <= len; i++)
        if (buf[i] == 0x09 && buf[i + 1] == 0x05 && buf[i + 2] == 0x03 && buf[i + 3] == 0x0d) buf[i + 3] = 0x09;
    CHECK(ud_parse_config(buf, len, &c));
    CHECK(!(ud_check(&c) & UD_FLAG_SYNC_NOT_ADAPTIVE));

    // Truncation and garbage must not crash.
    CHECK(ud_parse_config(orig, 100, &c) && c.warnings > 0 && c.has_ac);
    CHECK(!ud_parse_config(orig, 8, &c));
    memcpy(buf, orig, len);
    buf[40] = 0;  // bLength 0 in the middle
    CHECK(ud_parse_config(buf, len, &c) && c.warnings > 0);
    for (int seed = 1; seed <= 2000; seed++) {
        memcpy(buf, orig, len);
        unsigned x = (unsigned)seed * 2654435761u;
        for (int k = 0; k < 6; k++) { x = x * 1103515245u + 12345u; buf[9 + (x >> 8) % (len - 9)] = (uint8_t)(x >> 16); }
        ud_parse_config(buf, len, &c);
        ud_stream s;
        ud_find_stream(&c, true, 48000, 16, 2, &s);
        ud_check(&c);
    }
    g_pass++;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "../../../fixtures";
    static uint8_t buf[4096];
    char path[512];

    snprintf(path, sizeof(path), "%s/eo-ic100_04e8_a054_config.hex", dir);
    size_t len = load_hex(path, buf, sizeof(buf));
    test_fixture(buf, len);
    test_variants(buf, len);

    snprintf(path, sizeof(path), "%s/eo-ic100_04e8_a05b_config.hex", dir);
    len = load_hex(path, buf, sizeof(buf));
    test_a05b(buf, len);
    test_variants(buf, len);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
