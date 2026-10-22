// Portable USB / UAC1 configuration-descriptor parser.
// Copied unchanged from switch/eoprobe (unit-tested there in tests/test_usbdesc.c, and here against
// the user's a05b dump in tests/test_config.c). Mirrors the logic of pc/uacdesc.py.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UD_MAX_ALTS  24
#define UD_MAX_EPS   4
#define UD_MAX_UNITS 24
#define UD_MAX_RATES 8
#define UD_MAX_SRC   4
#define UD_MAX_CTRLS 9   // FU: master + 8 channels
#define UD_MAX_COLL  8

enum {
    UD_DT_DEVICE       = 0x01,
    UD_DT_CONFIG       = 0x02,
    UD_DT_STRING       = 0x03,
    UD_DT_INTERFACE    = 0x04,
    UD_DT_ENDPOINT     = 0x05,
    UD_DT_HID          = 0x21,
    UD_DT_HID_REPORT   = 0x22,
    UD_DT_CS_INTERFACE = 0x24,
    UD_DT_CS_ENDPOINT  = 0x25,
};

enum { UD_AC_HEADER = 1, UD_AC_IT, UD_AC_OT, UD_AC_MIXER, UD_AC_SELECTOR, UD_AC_FU, UD_AC_PROC, UD_AC_EXT };
enum { UD_AS_GENERAL = 1, UD_AS_FORMAT_TYPE = 2 };
enum { UD_SYNC_NONE = 0, UD_SYNC_ASYNC = 1, UD_SYNC_ADAPTIVE = 2, UD_SYNC_SYNC = 3 };

#define UD_TT_USB_STREAMING 0x0101
#define UD_FU_MUTE   (1u << 0)
#define UD_FU_VOLUME (1u << 1)

typedef struct {
    uint8_t  address, attributes, interval, length;
    uint16_t max_packet;
    uint8_t  refresh, synch_address;   // 9-byte audio endpoint descriptors only
    bool     has_cs;                   // class-specific EP_GENERAL present
    uint8_t  cs_attributes;            // bit0: sampling frequency control
} ud_endpoint;

typedef struct {
    uint8_t     number, alt, num_eps, cls, subcls, proto, iface_str;
    uint8_t     ep_count;
    ud_endpoint eps[UD_MAX_EPS];
    bool        has_as_general;
    uint8_t     terminal_link, delay;
    uint16_t    format_tag;
    bool        has_format;
    uint8_t     format_type, channels, subframe, bits;
    uint8_t     rate_count;            // 0 with has_format: continuous rate_min..rate_max
    uint32_t    rates[UD_MAX_RATES];
    uint32_t    rate_min, rate_max;
    bool        has_hid;
    uint16_t    hid_report_len;
} ud_alt;

typedef struct {
    uint8_t  subtype, id;
    uint16_t terminal_type;
    uint8_t  nr_channels;
    uint16_t channel_config;
    uint8_t  string_index;
    uint8_t  source_count;
    uint8_t  sources[UD_MAX_SRC];
    uint8_t  control_size, control_count;
    uint32_t controls[UD_MAX_CTRLS];   // FU: [0] = master
} ud_unit;

typedef struct {
    uint16_t total_length;
    uint8_t  num_interfaces, config_value, attributes, max_power;  // max_power in 2 mA units
    bool     has_ac;
    uint8_t  ac_iface;
    uint16_t bcd_adc;
    uint8_t  collection_count;
    uint8_t  collection[UD_MAX_COLL];
    uint8_t  unit_count;
    ud_unit  units[UD_MAX_UNITS];
    uint8_t  alt_count;
    ud_alt   alts[UD_MAX_ALTS];
    int      warnings;
    char     first_warning[96];
} ud_config;

static inline bool     ud_ep_is_in(const ud_endpoint *e)  { return (e->address & 0x80) != 0; }
static inline uint8_t  ud_ep_xfer(const ud_endpoint *e)   { return e->attributes & 3; }
static inline uint8_t  ud_ep_sync(const ud_endpoint *e)   { return (e->attributes >> 2) & 3; }
static inline uint8_t  ud_ep_usage(const ud_endpoint *e)  { return (e->attributes >> 4) & 3; }
static inline uint16_t ud_ep_packet(const ud_endpoint *e) { return e->max_packet & 0x7FF; }
static inline bool     ud_ep_has_rate_ctrl(const ud_endpoint *e) { return e->has_cs && (e->cs_attributes & 1); }

// Parses a configuration descriptor. Returns false if buf is not one; malformed or truncated
// input is parsed as far as possible and counted in out->warnings.
bool ud_parse_config(const uint8_t *buf, size_t len, ud_config *out);

const ud_unit     *ud_find_unit(const ud_config *c, uint8_t id);
const ud_endpoint *ud_iso_data_ep(const ud_alt *a);   // isochronous data endpoint, or NULL
bool ud_alt_supports(const ud_alt *a, uint32_t rate, uint8_t bits, uint8_t channels);

typedef struct {
    const ud_alt      *alt;
    const ud_endpoint *ep;
    uint8_t  fu_id;             // feature unit on the path, 0 if none
    uint32_t fu_controls;       // its master controls (UD_FU_*)
    uint8_t  terminal_id;       // playback: physical output terminal; capture: physical input terminal
    uint16_t terminal_type;
    uint16_t bytes_per_ms;      // at the requested rate (integer rates only)
    bool     has_rate_ctrl;     // endpoint accepts SET_CUR SAMPLING_FREQ
    bool     needs_rate_set;    // ...and the alt offers more than one rate
} ud_stream;

// First AS alt setting in the given direction that supports PCM rate/bits/channels.
bool ud_find_stream(const ud_config *c, bool playback, uint32_t rate, uint8_t bits, uint8_t channels,
                    ud_stream *out);

// Flags for the Switch-compatibility hypotheses (see docs/PLAN.md, H1..H7).
enum {
    UD_FLAG_NO_AUDIO          = 1u << 0,
    UD_FLAG_UAC2              = 1u << 1,
    UD_FLAG_NO_PLAYBACK       = 1u << 2,
    UD_FLAG_NO_48K16          = 1u << 3,
    UD_FLAG_CAPTURE_FIRST     = 1u << 4,   // H1
    UD_FLAG_OT_NOT_OUTPUT     = 1u << 5,   // H2
    UD_FLAG_MULTI_RATE        = 1u << 6,   // H3
    UD_FLAG_SYNC_NOT_ADAPTIVE = 1u << 7,   // H4
    UD_FLAG_SHARED_EP_NUMBER  = 1u << 8,   // H5
    UD_FLAG_STEREO_MIC        = 1u << 9,   // H6
    UD_FLAG_EXTRA_INTERFACES  = 1u << 10,  // H7
    UD_FLAG_COUNT             = 11,
};

uint32_t    ud_check(const ud_config *c);
const char *ud_flag_name(uint32_t flag);     // e.g. "H1 CAPTURE_FIRST"
const char *ud_terminal_name(uint16_t type);
const char *ud_sync_name(uint8_t sync);
