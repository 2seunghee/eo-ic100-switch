#include "rds.h"

#include <string.h>

void rds_reset(rds_state *r) {
    memset(r, 0, sizeof(*r));
    memset(r->ps, ' ', 8);
    memset(r->rt, ' ', 64);
}

// RDS uses its own character table; ASCII covers what stations here send. Anything else -> '?'.
static char rds_char(uint8_t c) {
    if (c == 0x0D || c == 0) return ' ';
    return c >= 0x20 && c < 0x7F ? (char)c : '?';
}

void rds_blocks_from_query(const uint8_t raw[8], uint16_t blk[4]) {
    for (int i = 0; i < 4; i++) blk[i] = (uint16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
}

bool rds_group(rds_state *r, const uint16_t blk[4]) {
    uint16_t a = blk[0], b = blk[1], c = blk[2], d = blk[3];
    unsigned type = b >> 12, version_b = (b >> 11) & 1;
    bool changed = false;
    r->groups++;
    if (a && a != r->pi) {   // another station: forget the old texts
        uint16_t pi = a;
        uint32_t groups = r->groups;
        rds_reset(r);
        r->pi = pi;
        r->groups = groups;
        changed = true;
    }
    if (type == 0) {   // 0A/0B: basic tuning, 2 PS characters per group
        unsigned seg = b & 3;
        char c0 = rds_char((uint8_t)(d >> 8)), c1 = rds_char((uint8_t)d);
        if (r->ps[2 * seg] != c0 || r->ps[2 * seg + 1] != c1) changed = true;
        r->ps[2 * seg] = c0;
        r->ps[2 * seg + 1] = c1;
        r->ps_seen |= (uint8_t)(1u << seg);
    } else if (type == 2) {   // 2A: 4 radiotext characters (blocks C, D); 2B: 2 (block D)
        unsigned seg = b & 0xF, ab = (b >> 4) & 1;
        if (r->rt_any && ab != r->rt_ab) {   // the station started a new text
            memset(r->rt, ' ', 64);
            changed = true;
        }
        r->rt_ab = (uint8_t)ab;
        r->rt_any = true;
        char t[4];
        int n;
        if (!version_b) {
            t[0] = rds_char((uint8_t)(c >> 8)), t[1] = rds_char((uint8_t)c);
            t[2] = rds_char((uint8_t)(d >> 8)), t[3] = rds_char((uint8_t)d);
            n = 4;
        } else {
            t[0] = rds_char((uint8_t)(d >> 8)), t[1] = rds_char((uint8_t)d);
            n = 2;
        }
        unsigned at = seg * (unsigned)n;
        for (int k = 0; k < n && at + (unsigned)k < 64; k++) {
            if (r->rt[at + k] != t[k]) changed = true;
            r->rt[at + k] = t[k];
        }
    }
    r->ps[8] = 0;
    r->rt[64] = 0;
    return changed;
}
