#include "config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char cfg_default_text[] =
    "# eoaudio 설정 (Samsung EO-IC100 USB-C 이어폰으로 스위치 소리 듣기)\n"
    "# 대부분의 값은 고치면 2초 안에 바로 적용됩니다 (Ultrahand 패키지로도 바꿀 수 있음).\n"
    "# mode, target_ms, rec_*, pids, iso_*, start_delay_s, log_keep는 재부팅해야 적용됩니다.\n"
    "# 문법: 키=값 / # 뒤는 주석 / 이 파일을 지우면 다음 시작 때 기본값으로 다시 만들어집니다.\n"
    "[eoaudio]\n"
    "config_version=3\n"
    "\n"
    "# 0이면 바로 꺼진다 (이어폰 재생을 멈추고 스피커를 되돌림). 1이면 다시 켜진다\n"
    "enabled=1\n"
    "\n"
    "# 동작 모드\n"
    "#   stream  : 기본. 스위치 소리(최종 믹스)를 모두 이어폰으로 보낸다\n"
    "#   capture : 진단용. 이어폰으로 소리를 내지 않고, 녹음되는지만 로그로 남긴다\n"
    "#   tone    : 진단용. 녹음 없이 1 kHz 시험음만 이어폰으로 보낸다 (전송 경로 확인)\n"
    "#   off     : 아무것도 하지 않는다 (끄기)\n"
    "mode=stream\n"
    "\n"
    "# 부팅 후 시작까지 기다리는 시간(초)\n"
    "start_delay_s=5\n"
    "\n"
    "# ---- 로그: sdmc:/switch/eoaudio/logs/eoaudio_YYYYMMDD_HHMMSS.txt (실행마다 새 파일)\n"
    "# info: 사건(연결, 버튼, 볼륨, 스피커, 상태 변화) + health_s초마다 상태 한 줄\n"
    "# debug: 위에 더해 1초마다 녹음/전송 상세, 리모컨 원본 리포트\n"
    "log_level=info\n"
    "health_s=10\n"
    "# 남겨 둘 로그 파일 수 (오래된 eoaudio_*.txt부터 지운다, 0 = 지우지 않음)\n"
    "log_keep=30\n"
    "\n"
    "# ---- 이어폰으로 재생하는 동안 콘솔 스피커(또는 TV) 끄기\n"
    "#   auto    : 마스터 볼륨 0 -> 음소거(SetTargetMute) -> 볼륨 0 순서로 써 보고, 녹음까지 조용해지는 방법은 되돌린다.\n"
    "#             둘 다 녹음을 죽이면 스피커는 그대로 둔다. 결과는 state.ini에 기억한다\n"
    "#   master0 / mute / volume0 : 그 방법만 (녹음이 죽으면 되돌린다)\n"
    "#   off     : 스피커를 건드리지 않는다\n"
    "# 3.5 mm 이어폰, 블루투스, 닌텐도가 인식한 다른 USB 헤드셋이 켜지면 eoaudio는 물러난다\n"
    "speaker_mute=auto\n"
    "\n"
    "# ---- 이어폰 볼륨 (장치 볼륨 FU5, 이 이어폰의 범위는 -60~0 dB)\n"
    "# 처음 시작 볼륨. 리모컨으로 바꾼 값은 state.ini에 저장되어 다음에도 쓰인다\n"
    "volume_db=-20\n"
    "volume_step_db=3\n"
    "volume_min_db=-42\n"
    "volume_max_db=0\n"
    "# 리모컨으로 볼륨을 바꾸면 화면 왼쪽에 이어폰 볼륨 막대를 1.5초 보여 준다 (0 = 끔)\n"
    "volume_osd=1\n"
    "# 1이면 이어폰 볼륨을 콘솔 볼륨에 묶는다 (스피커를 음소거로 끈 동안만). 0이면 따로 움직인다\n"
    "volume_follow=0\n"
    "# 리모컨 가운데 버튼\n"
    "#   media : media_apps의 앱이 실행 중일 때만 media_button을 누른다 (재생/정지). 다른 앱에서는 아무것도 안 함\n"
    "#   mute  : 이어폰 소리 끄기/켜기\n"
    "#   none  : 아무것도 안 함\n"
    "play_button=media\n"
    "# 미디어 앱의 프로그램 ID (16자리, 쉼표로 구분). 로그의 'app: application ... running' 줄에서 확인\n"
    "# 기본값은 YouTube(01003A400C3DA000)\n"
    "media_apps=01003A400C3DA000\n"
    "# 누를 버튼: a | b | x | y | l | r | zl | zr | plus | minus\n"
    "media_button=a\n"
    "\n"
    "# 지터 버퍼 목표(ms). 소리가 끊기면 늘리고, 지연을 줄이려면 줄인다\n"
    "target_ms=20\n"
    "\n"
    "# ---- 녹음 (audrec FinalOutputRecorder)\n"
    "# 녹음 버퍼 1개의 길이(프레임, 48 kHz에서 240 = 5 ms)와 녹음기에 넣어 둘 버퍼 수\n"
    "rec_frames=240\n"
    "rec_buffers=8\n"
    "\n"
    "# ---- 진단 (capture / tone 모드)\n"
    "# capture: 게임 소리가 10초 이어지면 부팅당 한 번 스피커를 3가지 방법으로 4초씩 끄고 녹음이 유지되는지 기록\n"
    "mute_test=1\n"
    "# capture: 이어폰을 확보해 연결 타이밍과 리모컨 버튼을 기록 (소리는 안 냄)\n"
    "acquire=1\n"
    "tone_hz=1000\n"
    "tone_dbfs=-30\n"
    "\n"
    "# ---- 장치\n"
    "# 받아들일 Samsung(04e8) 제품 ID. 다른 장치는 절대 건드리지 않는다\n"
    "pids=a054,a059,a05b\n"
    "# 꽂은 뒤 재열거가 잦아들 때까지 기다리는 시간(ms)\n"
    "settle_ms=200\n"
    "# 재생 인터페이스를 시스템이 먼저 잡았을 때 소프트웨어 재연결(ResetDevice) 시도 횟수\n"
    "resets=2\n"
    "# isoc 전송: 배치당 urb(1 ms) 수, 동시에 보내 둘 배치 수(곱은 16 이하), 시작 프레임 여유\n"
    "iso_urbs=4\n"
    "iso_batches=4\n"
    "iso_lead=16\n";

void cfg_defaults(eo_config *c) {
    memset(c, 0, sizeof(*c));
    c->config_version = 0;
    c->mode = MODE_STREAM;
    c->enabled = true;
    c->start_delay_s = 5;
    c->log_level = LOGLVL_INFO;
    c->health_s = 10;
    c->log_keep = 30;
    c->speaker_mute = SPK_AUTO;
    c->volume_db = -20.0f;
    c->volume_step_db = 3.0f;
    c->volume_min_db = -42.0f;
    c->volume_max_db = 0.0f;
    c->volume_follow = false;
    c->volume_osd = true;
    c->play_button = PLAY_MEDIA;
    c->media_button = BTN_A;
    c->media_apps[0] = 0x01003A400C3DA000ULL;
    c->media_app_count = 1;
    c->target_ms = 20;
    c->rec_frames = 240;
    c->rec_buffers = 8;
    c->mute_test = true;
    c->acquire = true;
    c->tone_hz = 1000.0f;
    c->tone_dbfs = -30.0f;
    c->pids[0] = 0xA054;
    c->pids[1] = 0xA059;
    c->pids[2] = 0xA05B;
    c->pid_count = 3;
    c->settle_ms = 200;
    c->resets = 2;
    c->iso_urbs = 4;
    c->iso_batches = 4;
    c->iso_lead = 16;
}

typedef enum { K_INT, K_FLOAT, K_BOOL, K_ENUM, K_PIDS, K_APPS } key_kind;

typedef struct {
    const char *name;
    int         value;
} enum_item;

static const enum_item k_modes[] = {
    {"off", MODE_OFF}, {"capture", MODE_CAPTURE}, {"tone", MODE_TONE}, {"stream", MODE_STREAM}, {NULL, 0}};
static const enum_item k_speaker[] = {
    {"off", SPK_OFF}, {"auto", SPK_AUTO}, {"mute", SPK_MUTE}, {"volume0", SPK_VOLUME0}, {"master0", SPK_MASTER0}, {NULL, 0}};
static const enum_item k_play[] = {{"none", PLAY_NONE}, {"mute", PLAY_MUTE}, {"media", PLAY_MEDIA}, {NULL, 0}};
static const enum_item k_buttons[] = {{"a", BTN_A},   {"b", BTN_B},   {"x", BTN_X},       {"y", BTN_Y},
                                      {"l", BTN_L},   {"r", BTN_R},   {"zl", BTN_ZL},     {"zr", BTN_ZR},
                                      {"plus", BTN_PLUS}, {"minus", BTN_MINUS}, {NULL, 0}};
static const enum_item k_level[] = {{"info", LOGLVL_INFO}, {"debug", LOGLVL_DEBUG}, {NULL, 0}};

typedef struct {
    const char      *name;
    key_kind         kind;
    size_t           off;
    double           lo, hi;
    const enum_item *items;   // K_ENUM
} key_def;

#define F(field) offsetof(eo_config, field)
static const key_def k_keys[] = {
    {"config_version", K_INT,   F(config_version), 0, 1000, NULL},
    {"mode",           K_ENUM,  F(mode),           0, 0, k_modes},
    {"enabled",        K_BOOL,  F(enabled),        0, 0, NULL},
    {"start_delay_s",  K_INT,   F(start_delay_s),  0, 120, NULL},
    {"log_level",      K_ENUM,  F(log_level),      0, 0, k_level},
    {"health_s",       K_INT,   F(health_s),       1, 600, NULL},
    {"log_keep",       K_INT,   F(log_keep),       0, 1000, NULL},
    {"speaker_mute",   K_ENUM,  F(speaker_mute),   0, 0, k_speaker},
    {"volume_db",      K_FLOAT, F(volume_db),      -100, 0, NULL},
    {"volume_step_db", K_FLOAT, F(volume_step_db), 0.5, 12, NULL},
    {"volume_min_db",  K_FLOAT, F(volume_min_db),  -100, 0, NULL},
    {"volume_max_db",  K_FLOAT, F(volume_max_db),  -100, 0, NULL},
    {"volume_follow",  K_BOOL,  F(volume_follow),  0, 0, NULL},
    {"play_button",    K_ENUM,  F(play_button),    0, 0, k_play},
    {"media_button",   K_ENUM,  F(media_button),   0, 0, k_buttons},
    {"media_apps",     K_APPS,  F(media_apps),     0, 0, NULL},
    {"volume_osd",     K_BOOL,  F(volume_osd),     0, 0, NULL},
    {"target_ms",      K_INT,   F(target_ms),      4, 150, NULL},
    {"rec_frames",     K_INT,   F(rec_frames),     48, 4800, NULL},
    {"rec_buffers",    K_INT,   F(rec_buffers),    2, 32, NULL},
    {"mute_test",      K_BOOL,  F(mute_test),      0, 0, NULL},
    {"acquire",        K_BOOL,  F(acquire),        0, 0, NULL},
    {"tone_hz",        K_FLOAT, F(tone_hz),        20, 20000, NULL},
    {"tone_dbfs",      K_FLOAT, F(tone_dbfs),      -60, -6, NULL},
    {"pids",           K_PIDS,  F(pids),           0, 0, NULL},
    {"settle_ms",      K_INT,   F(settle_ms),      0, 10000, NULL},
    {"resets",         K_INT,   F(resets),         0, 5, NULL},
    {"iso_urbs",       K_INT,   F(iso_urbs),       1, 16, NULL},
    {"iso_batches",    K_INT,   F(iso_batches),    2, 16, NULL},
    {"iso_lead",       K_INT,   F(iso_lead),       4, 256, NULL},
};
#undef F

static bool word_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

static void report(cfg_warn_fn warn, void *ctx, int line, int *problems, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

static void report(cfg_warn_fn warn, void *ctx, int line, int *problems, const char *fmt, ...) {
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    (*problems)++;
    if (warn) warn(ctx, line, msg);
}

static bool parse_bool(const char *v, bool *out) {
    static const char *yes[] = {"1", "true", "on", "yes"}, *no[] = {"0", "false", "off", "no"};
    for (int i = 0; i < 4; i++) {
        if (word_eq(v, yes[i])) { *out = true; return true; }
        if (word_eq(v, no[i])) { *out = false; return true; }
    }
    return false;
}

static bool parse_number(const char *v, double *out) {
    char *end;
    double d = strtod(v, &end);
    if (end == v || *end) return false;
    *out = d;
    return true;
}

static bool parse_pids(const char *v, eo_config *c) {
    uint16_t pids[CFG_MAX_PIDS];
    int n = 0;
    const char *p = v;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *end;
        unsigned long x = strtoul(p, &end, 16);
        if (end == p || x == 0 || x > 0xFFFF || n >= CFG_MAX_PIDS) return false;
        pids[n++] = (uint16_t)x;
        p = end;
        if (*p && *p != ',' && *p != ' ' && *p != '\t') return false;
    }
    if (n == 0) return false;
    memcpy(c->pids, pids, sizeof(uint16_t) * (size_t)n);
    c->pid_count = n;
    return true;
}

static bool parse_apps(const char *v, eo_config *c) {
    uint64_t apps[CFG_MAX_APPS];
    int n = 0;
    const char *p = v;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *end;
        unsigned long long x = strtoull(p, &end, 16);
        if (end == p || x == 0 || n >= CFG_MAX_APPS) return false;
        apps[n++] = (uint64_t)x;
        p = end;
        if (*p && *p != ',' && *p != ' ' && *p != '\t') return false;
    }
    memcpy(c->media_apps, apps, sizeof(uint64_t) * (size_t)n);   // empty = no media apps
    c->media_app_count = n;
    return true;
}

static int apply(eo_config *c, const key_def *k, const char *v, int line, cfg_warn_fn warn, void *ctx) {
    int problems = 0;
    uint8_t *base = (uint8_t *)c;
    double d;
    bool b;
    switch (k->kind) {
    case K_ENUM: {
        char choices[64] = "";
        size_t n = 0;
        for (const enum_item *it = k->items; it->name; it++) {
            if (word_eq(v, it->name)) {
                *(int *)(base + k->off) = it->value;
                return 0;
            }
            n += (size_t)snprintf(choices + n, sizeof(choices) - n, "%s%s", n ? "|" : "", it->name);
        }
        report(warn, ctx, line, &problems, "%s: unknown value \"%s\" (%s)", k->name, v, choices);
        return problems;
    }
    case K_PIDS:
        if (!parse_pids(v, c)) report(warn, ctx, line, &problems, "pids: expected hex IDs like a054,a05b, got \"%s\"", v);
        return problems;
    case K_APPS:
        if (!parse_apps(v, c))
            report(warn, ctx, line, &problems, "media_apps: expected 16-digit hex program IDs, got \"%s\"", v);
        return problems;
    case K_BOOL:
        if (parse_bool(v, &b)) *(bool *)(base + k->off) = b;
        else report(warn, ctx, line, &problems, "%s: expected 0 or 1", k->name);
        return problems;
    case K_INT:
    case K_FLOAT:
        if (!parse_number(v, &d)) {
            report(warn, ctx, line, &problems, "%s: not a number", k->name);
            return problems;
        }
        if (d < k->lo || d > k->hi) {
            report(warn, ctx, line, &problems, "%s: out of range %g..%g, clamped", k->name, k->lo, k->hi);
            d = d < k->lo ? k->lo : k->hi;
        }
        if (k->kind == K_INT) *(int *)(base + k->off) = (int)(d < 0 ? d - 0.5 : d + 0.5);
        else *(float *)(base + k->off) = (float)d;
        return problems;
    }
    return problems;
}

int cfg_parse(eo_config *c, const char *text, size_t len, cfg_warn_fn warn, void *ctx) {
    int problems = 0, line_no = 0;
    size_t pos = 0;
    if (len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) pos = 3;
    while (pos < len) {
        char line[256];
        size_t n = 0;
        line_no++;
        while (pos < len && text[pos] != '\n') {
            if (n + 1 < sizeof(line)) line[n++] = text[pos];
            pos++;
        }
        pos++;   // '\n'
        line[n] = 0;
        {   // [section] lines (Ultrahand's set-ini-val writes them) are accepted and ignored
            const char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            if (*t == '[') continue;
        }
        char *hash = strpbrk(line, "#;");
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        char *k = line;
        while (*k == ' ' || *k == '\t') k++;
        size_t klen = strlen(k);
        while (klen && isspace((unsigned char)k[klen - 1])) k[--klen] = 0;
        if (!*k) continue;
        if (!eq) {
            report(warn, ctx, line_no, &problems, "ignored line without '=': \"%s\"", k);
            continue;
        }
        *eq = 0;
        char *v = eq + 1;
        klen = strlen(k);
        while (klen && isspace((unsigned char)k[klen - 1])) k[--klen] = 0;
        while (*v == ' ' || *v == '\t') v++;
        size_t vlen = strlen(v);
        while (vlen && isspace((unsigned char)v[vlen - 1])) v[--vlen] = 0;

        const key_def *def = NULL;
        for (size_t i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]) && !def; i++)
            if (word_eq(k, k_keys[i].name)) def = &k_keys[i];
        if (!def) {
            // Retired keys stay in old files without a warning: audout_mitm (v0.4.0-0.4.7, removed in
            // v0.5.0: the kernel does not let another process read an audout client's buffers).
            if (!word_eq(k, "audout_mitm")) report(warn, ctx, line_no, &problems, "unknown key \"%s\"", k);
            continue;
        }
        problems += apply(c, def, v, line_no, warn, ctx);
    }
    // Every urb in flight counts against OpenUsbEp's maxUrbCount (16).
    if (c->iso_urbs * c->iso_batches > 16) {
        c->iso_batches = 16 / c->iso_urbs < 2 ? 2 : 16 / c->iso_urbs;
        if (c->iso_urbs * c->iso_batches > 16) c->iso_urbs = 8;
        report(warn, ctx, 0, &problems, "iso_urbs x iso_batches must be <= 16, reduced");
    }
    if (c->volume_min_db > c->volume_max_db) {
        c->volume_min_db = c->volume_max_db;
        report(warn, ctx, 0, &problems, "volume_min_db above volume_max_db, set equal");
    }
    if (c->volume_db > c->volume_max_db) c->volume_db = c->volume_max_db;
    if (c->volume_db < c->volume_min_db) c->volume_db = c->volume_min_db;
    return problems;
}

static const char *enum_name(const enum_item *items, int v) {
    for (const enum_item *it = items; it->name; it++)
        if (it->value == v) return it->name;
    return "?";
}

const char *cfg_mode_name(eo_mode m) { return enum_name(k_modes, m); }
const char *cfg_speaker_name(eo_speaker_mode m) { return enum_name(k_speaker, m); }
const char *cfg_play_name(eo_play_action a) { return enum_name(k_play, a); }

const char *cfg_button_name(eo_button b) { return enum_name(k_buttons, (int)b); }

bool cfg_media_app(const eo_config *c, uint64_t program_id) {
    for (int i = 0; i < c->media_app_count; i++)
        if (c->media_apps[i] == program_id) return true;
    return false;
}

bool cfg_pid_ok(const eo_config *c, uint16_t pid) {
    for (int i = 0; i < c->pid_count; i++)
        if (c->pids[i] == pid) return true;
    return false;
}

int cfg_describe(const eo_config *c, char *out, size_t size) {
    char pids[CFG_MAX_PIDS * 5 + 1] = "";
    size_t n = 0;
    for (int i = 0; i < c->pid_count && n + 6 <= sizeof(pids); i++)
        n += (size_t)snprintf(pids + n, sizeof(pids) - n, "%s%04x", i ? "," : "", c->pids[i]);
    return snprintf(out, size,
                    "v%d mode=%s start_delay=%ds log_level=%s health=%ds log_keep=%d speaker_mute=%s volume=%.1fdB "
                    "(step %.1f, %.1f..%.1f) follow=%d play_button=%s target=%dms rec=%dx%d frames mute_test=%d "
                    "acquire=%d tone=%.0fHz/%.0fdBFS pids=%s settle=%dms resets=%d iso=%dx%d lead %d",
                    c->config_version, cfg_mode_name(c->mode), c->start_delay_s, enum_name(k_level, c->log_level),
                    c->health_s, c->log_keep, cfg_speaker_name(c->speaker_mute), (double)c->volume_db,
                    (double)c->volume_step_db, (double)c->volume_min_db, (double)c->volume_max_db, c->volume_follow,
                    cfg_play_name(c->play_button), c->target_ms, c->rec_frames, c->rec_buffers, c->mute_test,
                    c->acquire, (double)c->tone_hz, (double)c->tone_dbfs, pids, c->settle_ms, c->resets, c->iso_urbs,
                    c->iso_batches, c->iso_lead);
}
