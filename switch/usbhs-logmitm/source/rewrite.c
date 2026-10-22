#include "rewrite.h"

#include <stdio.h>
#include <string.h>

static const struct {
    const char *name;
    unsigned bits;
} RULES[] = {
    {"terminal", RW_TERMINAL},       {"ot_headphones", RW_OT_HEADPHONES}, {"it_mic", RW_IT_MIC},
    {"single_rate", RW_SINGLE_RATE}, {"alt1_only", RW_ALT1_ONLY},         {"mps48", RW_MPS48},
};

int rewrite_parse(const char *list, unsigned *rules, const char **bad) {
    static char word[32];
    unsigned r = 0;
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        size_t len = strcspn(p, ", ");
        if (!len) break;
        if (len >= sizeof(word)) len = sizeof(word) - 1;
        memcpy(word, p, len);
        word[len] = 0;
        p += strcspn(p, ", ");
        if (!strcmp(word, "none")) continue;
        unsigned bits = 0;
        for (size_t i = 0; i < sizeof(RULES) / sizeof(RULES[0]); i++)
            if (!strcmp(word, RULES[i].name)) bits = RULES[i].bits;
        if (!bits) {
            if (bad) *bad = word;
            return -1;
        }
        r |= bits;
    }
    *rules = r;
    return 0;
}

void rewrite_names(unsigned rules, char *out, size_t size) {
    static const struct {
        const char *name;
        unsigned bit;
    } one[] = {{"ot_headphones", RW_OT_HEADPHONES}, {"it_mic", RW_IT_MIC}, {"single_rate", RW_SINGLE_RATE},
               {"alt1_only", RW_ALT1_ONLY},         {"mps48", RW_MPS48}};
    int n = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof(one) / sizeof(one[0]); i++)
        if (rules & one[i].bit && (size_t)n < size)
            n += snprintf(out + n, size - (size_t)n, "%s%s", n ? "," : "", one[i].name);
    if (!n) snprintf(out, size, "none");
}

static void note(rw_notes *notes, size_t at, const char *fmt, unsigned a, unsigned b, unsigned c) {
    if (!notes || notes->n >= RW_MAX_NOTES) return;
    int k = snprintf(notes->text[notes->n], sizeof(notes->text[0]), "@%03u ", (unsigned)at);
    snprintf(notes->text[notes->n] + k, sizeof(notes->text[0]) - (size_t)k, fmt, a, b, c);
    notes->n++;
}

static unsigned rd16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static void wr16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

int rewrite_config(uint8_t *d, size_t n, unsigned rules, rw_notes *notes) {
    if (notes) notes->n = 0;
    if (n < 9 || d[1] != 2) return 0;   // not a configuration descriptor
    int changed = 0;
    int cls = -1, sub = -1, ifnum = -1, alt = -1;
    unsigned ch = 0, subframe = 0;      // of the current AS alt setting (from its FORMAT_TYPE)
    for (size_t i = 0; i + 2 <= n && d[i] >= 2 && i + d[i] <= n; i += d[i]) {
        uint8_t *p = d + i;
        uint8_t len = p[0], type = p[1];
        if (type == 4 && len >= 9) {   // INTERFACE
            ifnum = p[2], alt = p[3], cls = p[5], sub = p[6];
            ch = subframe = 0;
            if (rules & RW_ALT1_ONLY && cls == 1 && sub == 2 && alt >= 2) {
                p[5] = 0xFF;
                note(notes, i, "if %u alt %u: class 01/02 -> ff (hidden)", (unsigned)ifnum, (unsigned)alt, 0);
                changed++;
                cls = 0xFF;
            }
            continue;
        }
        if (type == 0x24 && cls == 1 && sub == 1 && len >= 6) {   // AudioControl class-specific
            unsigned subtype = p[2], id = p[3], tt = rd16(p + 4);
            if (subtype == 3 && rules & RW_OT_HEADPHONES && (tt >> 8) == 0x04) {
                wr16(p + 4, 0x0302);
                note(notes, i, "output terminal %u: type 0x%04x -> 0x0302 (Headphones)", id, tt, 0);
                changed++;
            } else if (subtype == 2 && rules & RW_IT_MIC && (tt >> 8) == 0x04) {
                wr16(p + 4, 0x0201);
                note(notes, i, "input terminal %u: type 0x%04x -> 0x0201 (Microphone)", id, tt, 0);
                changed++;
            }
            continue;
        }
        if (type == 0x24 && cls == 1 && sub == 2 && len >= 8 && p[2] == 2 && p[3] == 1) {   // FORMAT_TYPE I
            ch = p[4], subframe = p[5];
            unsigned nf = p[7];
            if (rules & RW_SINGLE_RATE && nf >= 2 && len >= 8 + 3 * nf) {
                int has48 = 0;
                for (unsigned k = 0; k < nf; k++) {
                    const uint8_t *f = p + 8 + 3 * k;
                    has48 |= (f[0] | f[1] << 8 | f[2] << 16) == 48000;
                }
                if (has48) {
                    for (unsigned k = 0; k < nf; k++) {
                        uint8_t *f = p + 8 + 3 * k;
                        f[0] = 0x80, f[1] = 0xBB, f[2] = 0x00;
                    }
                    note(notes, i, "if %u alt %u: %u rates -> 48000 in every slot", (unsigned)ifnum, (unsigned)alt, nf);
                    changed++;
                }
            }
            continue;
        }
        if (type == 5 && len >= 7 && cls == 1 && sub == 2 && (p[3] & 3) == 1 && rules & RW_MPS48 && ch && subframe) {
            unsigned want = ch * subframe * 48, mps = rd16(p + 4);
            if (mps != want) {
                wr16(p + 4, want);
                note(notes, i, "endpoint 0x%02x: wMaxPacketSize %u -> %u", p[2], mps, want);
                changed++;
            }
        }
    }
    return changed;
}
