#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CFG_MAX_PROGRAMS 8
#define CFG_MAX_SERVICES 2

// sdmc:/switch/usbhs-logmitm/config.ini (any section). Unknown keys are reported, never fatal.
typedef struct {
    bool     enabled;            // enabled=1: install the mitm at start; 0: do nothing (the process idles)
    bool     all;                // programs=all: every client (except this process)
    bool     homebrew;           // programs=...,homebrew: processes with Atmosphère's hbl override flag
    int      n_programs;
    uint64_t programs[CFG_MAX_PROGRAMS];   // programs=audio,hid,homebrew,0100000000000014,...
    int      n_services;
    char     services[CFG_MAX_SERVICES][9];   // services=usb:hs[,usb:hs:a]
    bool     read_ctrl_data;     // read the data stage of control transfers from the client's memory
    bool     sync;               // fsync each traced line before the request goes on (survives a reset)
    int      hot_detail;         // detailed lines per session for hot commands (isoc, frame number)
    int      buf_dump;           // bytes of each IPC buffer dumped as hex
    int      log_keep;           // log files kept
    unsigned rewrite;            // RW_* rules applied to the EO-IC100's config descriptor (rewrite.h)
    bool     remote;             // read the earphone's remote buttons (hides them from hid; needs programs=...,hid)
    bool     deny_mic;           // boot in speaker mode (spk.c); eofm switches the mode at run time
} lm_config;

void config_defaults(lm_config *c);
// Parses text into c (defaults first). Returns the number of problems; each one is passed to warn().
int config_parse(lm_config *c, const char *text, void (*warn)(const char *msg));
bool config_wants(const lm_config *c, uint64_t program, uint64_t flags);
