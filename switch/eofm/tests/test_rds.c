// Host tests for the RDS decoder (PI, PS name, radiotext) and the QUERY byte order.
#include <stdio.h>
#include <string.h>

#include "../source/rds.h"

static int checks, failures;
#define CHECK(c)                                                    \
    do {                                                            \
        checks++;                                                   \
        if (!(c)) {                                                 \
            failures++;                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
        }                                                           \
    } while (0)

static void g0(rds_state *r, uint16_t pi, unsigned seg, char a, char b) {
    uint16_t blk[4] = {pi, (uint16_t)(0x0000 | seg), 0, (uint16_t)((uint8_t)a << 8 | (uint8_t)b)};
    rds_group(r, blk);
}

int main(void) {
    rds_state r;
    rds_reset(&r);
    CHECK(!strcmp(r.ps, "        ") && r.pi == 0);

    // PS "KBS FM 1" in four group-0A segments, out of order.
    g0(&r, 0x1234, 2, 'F', 'M');
    g0(&r, 0x1234, 0, 'K', 'B');
    g0(&r, 0x1234, 3, ' ', '1');
    CHECK(r.ps_seen == 0xD);
    g0(&r, 0x1234, 1, 'S', ' ');
    CHECK(r.ps_seen == 0xF && !strcmp(r.ps, "KBS FM 1") && r.pi == 0x1234);

    // Radiotext 2A: 4 characters per segment; the A/B flag change clears the text.
    uint16_t rt0[4] = {0x1234, 0x2000 | 0, ('H' << 8) | 'e', ('l' << 8) | 'l'};
    uint16_t rt1[4] = {0x1234, 0x2000 | 1, ('o' << 8) | '!', 0x0D20};
    rds_group(&r, rt0);
    rds_group(&r, rt1);
    CHECK(!strncmp(r.rt, "Hello! ", 7) && r.rt_any);
    uint16_t rtb[4] = {0x1234, 0x2000 | 0x10 | 0, ('N' << 8) | 'e', ('w' << 8) | ' '};
    CHECK(rds_group(&r, rtb));
    CHECK(!strncmp(r.rt, "New     ", 8));

    // 2B: 2 characters (block D) per segment.
    uint16_t b2[4] = {0x1234, 0x2800 | 0x10 | 2, 0, ('x' << 8) | 'y'};
    rds_group(&r, b2);
    CHECK(r.rt[4] == 'x' && r.rt[5] == 'y');

    // Another station (PI) forgets the old texts.
    g0(&r, 0x9999, 0, 'M', 'B');
    CHECK(r.pi == 0x9999 && !strcmp(r.ps, "MB      ") && !r.rt_any);

    // Non-ASCII becomes '?'.
    g0(&r, 0x9999, 1, (char)0xC4, 'C');
    CHECK(r.ps[2] == '?' && r.ps[3] == 'C');

    // QUERY bytes: each block little-endian (as kjy00302's get_status swaps them).
    uint8_t raw[8] = {0x34, 0x12, 0x01, 0x20, 0x48, 0x65, 0x6C, 0x6C};
    uint16_t blk[4];
    rds_blocks_from_query(raw, blk);
    CHECK(blk[0] == 0x1234 && blk[1] == 0x2001 && blk[2] == 0x6548 && blk[3] == 0x6C6C);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
