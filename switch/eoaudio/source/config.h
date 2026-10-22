// eoaudio settings: sdmc:/switch/eoaudio/config.ini ("key=value", '#' or ';' starts a comment).
// No libnx dependency: unit-tested on the host (tests/test_config.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_VERSION 3   // older config.ini files are kept as config.ini.v<N>.bak and replaced

typedef enum {
    MODE_OFF = 0,
    MODE_CAPTURE,   // diagnostics: record the final mix and log levels/timing; no sound on the earphone
    MODE_TONE,      // diagnostics: test tone on the earphone (transport check without the recorder)
    MODE_STREAM,    // default: the final mix plays on the earphone
} eo_mode;

typedef enum {
    SPK_OFF = 0,    // leave the console speaker alone
    SPK_AUTO,       // try SetTargetMute, then SetTargetVolume(min); keep the first that keeps the recording
    SPK_MUTE,       // only SetTargetMute(active target)
    SPK_VOLUME0,    // only SetTargetVolume(active target, min)
    SPK_MASTER0,    // only SetSystemOutputMasterVolume(0)
} eo_speaker_mode;

typedef enum {
    PLAY_NONE = 0,  // middle button does nothing
    PLAY_MUTE,      // middle button stops/resumes the sound on the earphone
    PLAY_MEDIA,     // middle button presses media_button, but only while an app from media_apps runs
} eo_play_action;

// Buttons the middle button can press in media apps (index into cfg_button_names / HidNpadButton bit).
typedef enum { BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_L = 6, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS } eo_button;

#define CFG_MAX_APPS 8

typedef enum { LOGLVL_INFO = 0, LOGLVL_DEBUG = 1 } eo_log_level;

#define CFG_MAX_PIDS 8

typedef struct {
    int     config_version;
    eo_mode mode;
    bool    enabled;        // live on/off (config.ini is re-read while running)
    int     start_delay_s;
    // logging
    eo_log_level log_level;
    int     health_s, log_keep;
    // output
    eo_speaker_mode speaker_mute;
    float   volume_db, volume_step_db, volume_min_db, volume_max_db;
    bool    volume_follow, volume_osd;
    eo_play_action play_button;
    eo_button media_button;
    uint64_t media_apps[CFG_MAX_APPS];
    int     media_app_count;
    int     target_ms;
    // recorder
    int     rec_frames, rec_buffers;
    // diagnostics
    bool    mute_test, acquire;
    float   tone_hz, tone_dbfs;
    // device
    uint16_t pids[CFG_MAX_PIDS];
    int     pid_count;
    int     settle_ms, resets;
    int     iso_urbs, iso_batches, iso_lead;
} eo_config;

typedef void (*cfg_warn_fn)(void *ctx, int line, const char *msg);

void cfg_defaults(eo_config *c);   // config_version stays 0 until a file sets it
// Applies the settings found in `text` on top of *c. Unknown keys and bad values are skipped and
// reported through `warn` (may be NULL). Returns the number of problems.
int  cfg_parse(eo_config *c, const char *text, size_t len, cfg_warn_fn warn, void *ctx);
// One-line summary for the log.
int  cfg_describe(const eo_config *c, char *out, size_t size);
const char *cfg_mode_name(eo_mode m);
const char *cfg_speaker_name(eo_speaker_mode m);
const char *cfg_play_name(eo_play_action a);
bool cfg_pid_ok(const eo_config *c, uint16_t pid);
bool cfg_media_app(const eo_config *c, uint64_t program_id);
const char *cfg_button_name(eo_button b);

// The config.ini written on first start (documented defaults, Korean comments, UTF-8).
extern const char cfg_default_text[];
