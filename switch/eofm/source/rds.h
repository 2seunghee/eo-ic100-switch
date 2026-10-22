#pragma once
#include <stdbool.h>
#include <stdint.h>

// RDS decoding of the groups the EO-IC100 reports (QUERY type 2: four 16-bit blocks A-D).
// Only what a radio screen needs: PI, programme service name (group 0) and radiotext (group 2).
// Pure code: also built into the host tests.
typedef struct {
    uint16_t pi;
    char     ps[9];          // 8 characters, '\0'-terminated; unknown segments stay ' '
    char     rt[65];         // 64 characters
    uint8_t  ps_seen;        // bit per received 2-character segment
    uint8_t  rt_ab;          // radiotext A/B flag of the current text
    bool     rt_any;
    uint32_t groups;         // groups decoded
} rds_state;

void rds_reset(rds_state *r);
// Blocks in the order A, B, C, D. Returns true if something shown on screen changed.
bool rds_group(rds_state *r, const uint16_t blk[4]);
// The 8 RDS bytes of a QUERY reply (res[3..10]) -> blocks (each block little-endian on the wire).
void rds_blocks_from_query(const uint8_t raw[8], uint16_t blk[4]);
