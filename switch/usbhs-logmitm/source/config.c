#include "config.h"

#include "rewrite.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGRAM_HID   0x0100000000000013ULL
#define PROGRAM_AUDIO 0x0100000000000014ULL

void config_defaults(lm_config *c) {
    memset(c, 0, sizeof(*c));
    c->enabled = true;
    c->homebrew = true;
    c->n_programs = 1;
    c->programs[0] = PROGRAM_AUDIO;
    c->n_services = 1;
    strcpy(c->services[0], "usb:hs");
    c->read_ctrl_data = true;
    c->sync = true;
    c->hot_detail = 40;
    c->buf_dump = 64;
    c->log_keep = 20;
    c->rewrite = RW_TERMINAL;
    c->remote = false;   // experimental: hid must be intercepted (programs=...,hid)
    c->deny_mic = false;   // experimental: radio to the speaker (eofm), with rewrite=none
}

static void trim(char **s, char **e) {
    while (*s < *e && isspace((unsigned char)**s)) (*s)++;
    while (*e > *s && isspace((unsigned char)(*e)[-1])) (*e)--;
}

static bool parse_bool(const char *v, bool *out) {
    if (!strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "yes")) *out = true;
    else if (!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "no")) *out = false;
    else return false;
    return true;
}

static bool parse_int(const char *v, int lo, int hi, int *out) {
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end || n < lo || n > hi) return false;
    *out = (int)n;
    return true;
}

static int report(void (*warn)(const char *), int line, const char *what, const char *v) {
    char msg[160];
    snprintf(msg, sizeof(msg), "config.ini line %d: %s \"%.60s\" (ignored)", line, what, v);
    if (warn) warn(msg);
    return 1;
}

static int parse_programs(lm_config *c, char *v, void (*warn)(const char *), int line) {
    int problems = 0;
    c->all = c->homebrew = false;
    c->n_programs = 0;
    for (char *tok = strtok(v, ", "); tok; tok = strtok(NULL, ", ")) {
        uint64_t id = 0;
        if (!strcmp(tok, "all")) c->all = true;
        else if (!strcmp(tok, "homebrew")) c->homebrew = true;
        else if (!strcmp(tok, "audio")) id = PROGRAM_AUDIO;
        else if (!strcmp(tok, "hid")) id = PROGRAM_HID;
        else {
            char *end;
            id = strtoull(tok, &end, 16);
            if (*end || strlen(tok) != 16 || !id) problems += report(warn, line, "not a program", tok), id = 0;
        }
        if (id && c->n_programs < CFG_MAX_PROGRAMS) c->programs[c->n_programs++] = id;
    }
    return problems;
}

static int parse_services(lm_config *c, char *v, void (*warn)(const char *), int line) {
    int problems = 0;
    c->n_services = 0;
    for (char *tok = strtok(v, ", "); tok; tok = strtok(NULL, ", ")) {
        if (strcmp(tok, "usb:hs") && strcmp(tok, "usb:hs:a")) {
            problems += report(warn, line, "not usb:hs or usb:hs:a", tok);
            continue;
        }
        bool dup = false;
        for (int i = 0; i < c->n_services; i++) dup |= !strcmp(c->services[i], tok);
        if (!dup && c->n_services < CFG_MAX_SERVICES) strcpy(c->services[c->n_services++], tok);
    }
    if (!c->n_services) {   // never nothing: usb:hs is what this tool is for
        strcpy(c->services[0], "usb:hs");
        c->n_services = 1;
    }
    return problems;
}

int config_parse(lm_config *c, const char *text, void (*warn)(const char *)) {
    config_defaults(c);
    int problems = 0, line_no = 0;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[256];
        line_no++;
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = nl ? nl + 1 : p + strlen(p);

        char *s = line, *e = line + strlen(line);
        char *hash = strpbrk(s, "#;");
        if (hash) e = hash;
        trim(&s, &e);
        if (s == e || *s == '[') continue;
        *e = 0;
        char *eq = strchr(s, '=');
        if (!eq) {
            problems += report(warn, line_no, "no '=' in", s);
            continue;
        }
        char *ks = s, *ke = eq, *vs = eq + 1, *ve = e;
        trim(&ks, &ke);
        trim(&vs, &ve);
        *ke = 0;
        *ve = 0;
        for (char *q = vs; *q; q++) *q = (char)tolower((unsigned char)*q);
        int n;
        if (!strcmp(ks, "enabled")) {
            if (!parse_bool(vs, &c->enabled)) problems += report(warn, line_no, "enabled: not 0/1", vs);
        } else if (!strcmp(ks, "programs")) {
            problems += parse_programs(c, vs, warn, line_no);
        } else if (!strcmp(ks, "services")) {
            problems += parse_services(c, vs, warn, line_no);
        } else if (!strcmp(ks, "read_ctrl_data")) {
            if (!parse_bool(vs, &c->read_ctrl_data)) problems += report(warn, line_no, "read_ctrl_data: not 0/1", vs);
        } else if (!strcmp(ks, "sync")) {
            if (!parse_bool(vs, &c->sync)) problems += report(warn, line_no, "sync: not 0/1", vs);
        } else if (!strcmp(ks, "hot_detail")) {
            if (parse_int(vs, 0, 100000, &n)) c->hot_detail = n;
            else problems += report(warn, line_no, "hot_detail: not 0..100000", vs);
        } else if (!strcmp(ks, "buf_dump")) {
            if (parse_int(vs, 0, 4096, &n)) c->buf_dump = n;
            else problems += report(warn, line_no, "buf_dump: not 0..4096", vs);
        } else if (!strcmp(ks, "log_keep")) {
            if (parse_int(vs, 1, 200, &n)) c->log_keep = n;
            else problems += report(warn, line_no, "log_keep: not 1..200", vs);
        } else if (!strcmp(ks, "remote")) {
            if (!parse_bool(vs, &c->remote)) problems += report(warn, line_no, "remote: not 0/1", vs);
        } else if (!strcmp(ks, "deny_mic")) {
            if (!parse_bool(vs, &c->deny_mic)) problems += report(warn, line_no, "deny_mic: not 0/1", vs);
        } else if (!strcmp(ks, "rewrite")) {
            const char *bad = NULL;
            unsigned r;
            if (rewrite_parse(vs, &r, &bad) == 0) c->rewrite = r;
            else problems += report(warn, line_no, "rewrite: unknown rule", bad ? bad : vs);
        } else {
            problems += report(warn, line_no, "unknown key", ks);
        }
    }
    return problems;
}

bool config_wants(const lm_config *c, uint64_t program, uint64_t flags) {
    if (c->all) return true;
    if (c->homebrew && (flags & 1)) return true;   // Atmosphère override flag bit 0: hbl (homebrew)
    for (int i = 0; i < c->n_programs; i++)
        if (c->programs[i] == program) return true;
    return false;
}
