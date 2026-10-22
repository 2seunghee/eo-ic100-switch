#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_targets[ST_TARGETS] = {NULL, "speaker", "headphone", "tv", "usb", "bluetooth"};
static const char *const k_methods[ST_METHODS] = {NULL, "mute", "volume0", "master0"};

const char *st_target_key(int target) { return target > 0 && target < ST_TARGETS ? k_targets[target] : NULL; }
const char *st_method_key(int method) { return method > 0 && method < ST_METHODS ? k_methods[method] : NULL; }

const char *st_verdict_name(eo_verdict v) {
    switch (v) {
    case VERDICT_KEPT:     return "kept";
    case VERDICT_SILENCED: return "silenced";
    default:               return "unknown";
    }
}

void st_init(eo_state *s) { memset(s, 0, sizeof(*s)); }

void st_forget_verdicts(eo_state *s) { memset(s->verdict, 0, sizeof(s->verdict)); }

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    size_t k = 0;
    while (s[k] == ' ' || s[k] == '\t') k++;
    if (k) memmove(s, s + k, n - k + 1);
}

void st_parse(eo_state *s, const char *text, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        char line[128];
        size_t n = 0;
        while (pos < len && text[pos] != '\n') {
            if (n + 1 < sizeof(line)) line[n++] = text[pos];
            pos++;
        }
        pos++;
        line[n] = 0;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key);
        trim(val);
        if (strcmp(key, "volume_db") == 0) {
            char *end;
            double d = strtod(val, &end);
            if (end != val && !*end && d >= -127.0 && d <= 0.0) {
                s->volume_db = (float)d;
                s->have_volume = true;
            }
        } else if (strcmp(key, "hos") == 0) {
            snprintf(s->hos, sizeof(s->hos), "%s", val);
        } else if (strncmp(key, "verdict.", 8) == 0) {
            char *dot = strchr(key + 8, '.');
            if (!dot) continue;
            *dot = 0;
            int t = 0, m = 0;
            for (int i = 1; i < ST_TARGETS; i++)
                if (strcmp(key + 8, k_targets[i]) == 0) t = i;
            for (int i = 1; i < ST_METHODS; i++)
                if (strcmp(dot + 1, k_methods[i]) == 0) m = i;
            if (!t || !m) continue;
            if (strcmp(val, "kept") == 0) s->verdict[t][m] = VERDICT_KEPT;
            else if (strcmp(val, "silenced") == 0) s->verdict[t][m] = VERDICT_SILENCED;
        }
    }
}

int st_format(const eo_state *s, char *out, size_t size) {
    size_t n = 0;
    int k = snprintf(out, size, "# eoaudio state: written by eoaudio, delete this file to forget it\n");
    if (k < 0 || (size_t)k >= size) return k;
    n = (size_t)k;
    if (s->have_volume) {
        k = snprintf(out + n, size - n, "volume_db=%.1f\n", (double)s->volume_db);
        if (k < 0 || (size_t)k >= size - n) return -1;
        n += (size_t)k;
    }
    k = snprintf(out + n, size - n, "hos=%s\n", s->hos);
    if (k < 0 || (size_t)k >= size - n) return -1;
    n += (size_t)k;
    for (int t = 1; t < ST_TARGETS; t++)
        for (int m = 1; m < ST_METHODS; m++) {
            if (s->verdict[t][m] == VERDICT_UNKNOWN) continue;
            k = snprintf(out + n, size - n, "verdict.%s.%s=%s\n", k_targets[t], k_methods[m],
                         st_verdict_name(s->verdict[t][m]));
            if (k < 0 || (size_t)k >= size - n) return -1;
            n += (size_t)k;
        }
    return (int)n;
}
