#pragma once
#include <switch.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbdesc.h"

#define EOPROBE_VERSION "0.2.2"
#define APP_DIR         "sdmc:/switch/eoprobe"
#define TARGET_VID      0x04E8   // Samsung; EO-IC100 PIDs seen so far: a054, a059, a05b

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

// libnx console colours
#define C_RED    "\x1b[31;1m"
#define C_GREEN  "\x1b[32;1m"
#define C_YELLOW "\x1b[33;1m"
#define C_CYAN   "\x1b[36;1m"
#define C_RESET  "\x1b[0m"

static inline u64 ticks_to_us(u64 t) { return armTicksToNs(t) / 1000; }
static inline u64 now_us(void) { return ticks_to_us(armGetSystemTick()); }
