#pragma once
#include <switch.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EOFM_VERSION "0.1.6"
#define APP_DIR      "sdmc:/switch/eofm"
#define TARGET_VID   0x04E8   // Samsung; EO-IC100 PIDs: a054, a059, a05b

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

// libnx console colours
#define C_RED    "\x1b[31;1m"
#define C_GREEN  "\x1b[32;1m"
#define C_YELLOW "\x1b[33;1m"
#define C_CYAN   "\x1b[36;1m"
#define C_DIM    "\x1b[37m"
#define C_RESET  "\x1b[0m"

static inline u64 now_ms(void) { return armTicksToNs(armGetSystemTick()) / 1000000; }
