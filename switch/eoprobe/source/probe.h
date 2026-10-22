#pragma once
#include "common.h"

#define PR_MAX_DEVS  6
#define PR_MAX_IFS   12
#define PR_MAX_SESS  16
#define PR_QUERY_MAX 32

typedef enum {
    IF_HELD_OTHER,   // listed by QueryAllInterfaces but not available: another process holds it
    IF_AVAILABLE,    // listed by QueryAvailableInterfaces
    IF_OURS,         // we hold a session on it
} pr_if_state;

typedef struct {
    UsbHsInterface info;    // QueryAll entry (ID = -1) or QueryAvailable entry
    UsbHsInterface avail;   // QueryAvailable entry, valid when state == IF_AVAILABLE
    pr_if_state    state;
} pr_iface;

typedef struct {
    u32  bus_id, device_id;
    char path[0x40];
    struct usb_device_descriptor dev;
    struct usb_config_descriptor cfg_hdr;
    int      if_count;
    pr_iface ifs[PR_MAX_IFS];
    // Filled by pr_read_descriptors():
    bool      have_cfg;
    u16       raw_cfg_len;
    u8        raw_cfg[1024];
    ud_config cfg;
    char      manufacturer[64], product[64];
} pr_device;

typedef struct {
    int       dev_count;
    pr_device devs[PR_MAX_DEVS];
    s32       n_all, n_avail;
    Result    rc_all, rc_avail;
} pr_state;

// Queries usb:hs and groups interfaces by device. Open sessions are kept.
Result pr_scan(pr_state *st);
void   pr_print(const pr_state *st, int target);
int    pr_pick_target(const pr_state *st, u16 vid);
const pr_iface *pr_find_iface(const pr_device *d, u8 ifnum);
u16    pr_vid(const pr_device *d);
u16    pr_pid(const pr_device *d);

// Interface session pool; survives rescans.
UsbHsClientIfSession *pr_session_get(const pr_device *d, u8 ifnum);
UsbHsClientIfSession *pr_session_any(const pr_device *d);
Result pr_session_acquire(UsbHsInterface *avail_if);
int    pr_acquire_all(pr_device *d);   // every available interface of d; returns the number acquired
void   pr_release_device(const pr_device *d);
void   pr_release_all(void);
void   pr_prune_sessions(void);        // drop sessions usb:hs no longer lists (device unplugged/reset)
int    pr_session_count(void);

// Reads device/config/string/HID-report descriptors through any session we hold and saves them
// under APP_DIR as <vid>_<pid>_*.bin.
Result pr_read_descriptors(pr_device *d);
void   pr_print_analysis(const pr_device *d);
