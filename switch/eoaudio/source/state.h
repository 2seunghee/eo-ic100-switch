// What eoaudio remembers between runs: sdmc:/switch/eoaudio/state.ini (written by eoaudio, safe to
// delete). No libnx dependency: unit-tested on the host (tests/test_policy.c).
//   volume_db                  last earphone volume (remote or console volume)
//   hos                        firmware the verdicts below were measured on (other firmware: forgotten)
//   verdict.<target>.<method>  does silencing <target> with <method> keep the recording? kept|silenced
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { VERDICT_UNKNOWN = 0, VERDICT_KEPT, VERDICT_SILENCED } eo_verdict;

// Targets use the libnx AudioTarget values (1 speaker, 2 headphone jack, 3 TV, 4 USB, 5 Bluetooth),
// methods the speaker.h values (1 SetTargetMute, 2 SetTargetVolume(min), 3 SetSystemOutputMasterVolume(0)).
#define ST_TARGETS 6
#define ST_METHODS 4

typedef struct {
    bool       have_volume;
    float      volume_db;
    char       hos[16];
    eo_verdict verdict[ST_TARGETS][ST_METHODS];
} eo_state;

void        st_init(eo_state *s);
void        st_parse(eo_state *s, const char *text, size_t len);   // unknown lines are ignored
int         st_format(const eo_state *s, char *out, size_t size);
void        st_forget_verdicts(eo_state *s);
const char *st_verdict_name(eo_verdict v);
const char *st_target_key(int target);   // "speaker", "tv", ... (NULL if out of range)
const char *st_method_key(int method);   // "mute", "volume0" (NULL if out of range)
