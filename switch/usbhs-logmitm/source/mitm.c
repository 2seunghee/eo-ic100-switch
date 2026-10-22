#include "mitm.h"

#include "common.h"
#include "decode.h"
#include "log.h"
#include "rewrite.h"

// One thread serves everything with svcReplyAndReceive: the mitm ports (new clients), the query
// sessions (sm asks whether to intercept a process) and the intercepted sessions. A request is
// forwarded by sending the received message as is to the real session (as libstratosphere's
// ServerSession::ForwardRequest does) and the real reply is sent back unchanged. The only rewrite is
// that a returned IClientIfSession/IClientEpSession handle is swapped for one of ours, so that its
// requests come here too. The sm protocol details below were learned the hard way by eoaudio's
// audout mitm (v0.4.x, see switch/eoaudio/README.md).

#define MAX_SERVICES CFG_MAX_SERVICES
#define MAX_SESSIONS 48            // + ports + queries <= 64 handles for svcReplyAndReceive
#define MAX_OBJECTS  16            // per session: 1 without a domain
#define PTRBUF_SIZE  0x1000
#define CTRL_DUMP    512           // bytes of a control transfer's data stage that are logged
#define HOT_SUMMARY_MS 5000

#define SM_INSTALL_MITM      65000
#define SM_UNINSTALL_MITM    65001
#define SM_ACKNOWLEDGE_MITM  65003
#define SM_CLEAR_FUTURE_MITM 65007
#define QUERY_SHOULD_MITM    65000
#define MITM_PID_TAG         0xFFFE000000000000ULL   // forwarded PID: the client's, tagged for the kernel

enum { CTL_TO_DOMAIN = 0, CTL_CLONE = 2, CTL_QUERY_PTRBUF = 3, CTL_CLONE_EX = 4 };
enum { IF_SET_INTERFACE = 1, IF_CTRL_XFER = 5, IF_CTRL_REPORT = 7, IF_OPEN_EP = 9 };
enum { ROOT_ACQUIRE = 7 };

typedef struct {
    u64 process_id, program_id, keys_held, flags;
} mitm_info;

// One CMIF object: the session itself, or one object of a domain.
typedef struct {
    u32  id;                 // domain object id (0 without a domain)
    u8   kind;               // OBJ_ROOT / OBJ_IF / OBJ_EP
    u8   ifnum, alt, ep;     // IF: bInterfaceNumber / alt setting; EP: endpoint address
    s32  if_id;              // usb:hs interface ID
    u16  vid, pid;           // IF/EP: the device, from the interface lists usb:hs returned
    bool ctrl_pending;       // IN control transfer submitted, its data comes with GetCtrlXferReport
    u8   setup[8];
    u64  ctrl_buf;
    u32  hot_detail;         // hot requests logged in detail so far
    u32  hot_n[10], hot_fail;   // since the last summary
    u64  hot_since;
} objst;

typedef struct {
    Handle srv, fwd;
    bool   domain;
    int    svc;
    int    serial;
    u64    program, pid;
    objst  o[MAX_OBJECTS];
    int    no;
} msess;

#define MAX_KNOWN 32

static struct {
    const lm_config *cfg;
    struct {
        s32 id;
        u16 vid, pid;
    } known[MAX_KNOWN];      // interface ID -> device, from Query*Interfaces answers
    int     nknown;
    u32     rewrites;
    TipcService sm;
    bool    sm_ok;
    int     nsvc;
    char    name[MAX_SERVICES][9];
    Handle  port[MAX_SERVICES], query[MAX_SERVICES];
    bool    installed[MAX_SERVICES];
    Thread  thread;
    msess   s[MAX_SESSIONS];
    int     n;
    int     serial;
    u8      ptrbuf[PTRBUF_SIZE] __attribute__((aligned(16)));
    u8      data[CTRL_DUMP];
    u64     self;
    // counters for mitm_status (written by the mitm thread only)
    u32     queries, intercepted, clients, requests, failures, dbg_fail;
    Result  last_dbg_rc;
} M;

// ------------------------------------------------------------------ logging

static void trace_sync(void) {
    if (M.cfg->sync) log_sync();
}

static const char *program_name(u64 program) {
    static char bufs[4][20];
    static int k;
    switch (program) {
        case 0x0100000000000012ULL: return "bsdsockets";
        case PROGRAM_HID:           return "hid";
        case PROGRAM_AUDIO:         return "audio";
    }
    char *b = bufs[k++ & 3];
    snprintf(b, sizeof(bufs[0]), "%016llx", (unsigned long long)program);
    return b;
}

static const char *obj_label(const msess *s, const objst *o) {
    static char bufs[4][64];
    static int k;
    char *b = bufs[k++ & 3];
    int n = snprintf(b, sizeof(bufs[0]), "S%d %s ", s->serial, program_name(s->program));
    if (s->domain) n += snprintf(b + n, sizeof(bufs[0]) - (size_t)n, "obj%u ", o->id);
    switch (o->kind) {
        case OBJ_ROOT: snprintf(b + n, sizeof(bufs[0]) - (size_t)n, "%s", M.name[s->svc]); break;
        case OBJ_IF:   snprintf(b + n, sizeof(bufs[0]) - (size_t)n, "IF[id %d if %u]", o->if_id, o->ifnum); break;
        case OBJ_EP:   snprintf(b + n, sizeof(bufs[0]) - (size_t)n, "EP[0x%02x]", o->ep); break;
        default:       snprintf(b + n, sizeof(bufs[0]) - (size_t)n, "?");
    }
    return b;
}

static void hex_line(char *out, size_t size, const u8 *p, size_t len, size_t max) {
    int n = 0;
    out[0] = 0;
    for (size_t i = 0; i < len && i < max && (size_t)n + 4 < size; i++)
        n += snprintf(out + n, size - (size_t)n, i % 4 == 3 ? "%02x " : "%02x", p[i]);
    if (len > max && (size_t)n + 4 < size) snprintf(out + n, size - (size_t)n, "...");
}

static void log_ep_list(const char *indent, const struct usb_endpoint_descriptor *eps, const char *what) {
    char line[160];
    int n = 0;
    line[0] = 0;
    for (int i = 0; i < 15; i++) {
        const struct usb_endpoint_descriptor *e = &eps[i];
        if (!e->bLength) continue;
        n += snprintf(line + n, sizeof(line) - (size_t)n, " 0x%02x(attr 0x%02x mps %u int %u)", e->bEndpointAddress,
                      e->bmAttributes, e->wMaxPacketSize, e->bInterval);
        if ((size_t)n >= sizeof(line) - 40) break;
    }
    if (n) log_printf("%s%s eps:%s", indent, what, line);
}

static void log_info(const char *indent, const UsbHsInterfaceInfo *inf) {
    const struct usb_interface_descriptor *d = &inf->interface_desc;
    log_printf("%sid %d dev2 %u unk 0x%x: if %u alt %u eps %u class %02x/%02x/%02x", indent, inf->ID, inf->deviceID_2,
               inf->unk_x8, d->bInterfaceNumber, d->bAlternateSetting, d->bNumEndpoints, d->bInterfaceClass,
               d->bInterfaceSubClass, d->bInterfaceProtocol);
    log_ep_list(indent, inf->input_endpoint_descs, "  in ");
    log_ep_list(indent, inf->output_endpoint_descs, "  out");
}

static void log_interface(const char *indent, const UsbHsInterface *it) {
    log_info(indent, &it->inf);
    struct usb_device_descriptor dd;
    memcpy(&dd, &it->device_desc, sizeof(dd));
    log_printf("%s  dev %04x:%04x bcd %04x bus %u dev %u cfg total %u ifs %u, ts %llu, path %.64s", indent, dd.idVendor,
               dd.idProduct, dd.bcdDevice, it->busID, it->deviceID, it->config_desc.wTotalLength,
               it->config_desc.bNumInterfaces, (unsigned long long)it->timestamp, it->pathstr);
}

static void remember_device(const UsbHsInterface *it) {
    struct usb_device_descriptor dd;
    memcpy(&dd, &it->device_desc, sizeof(dd));
    s32 id = it->inf.ID;
    int k = 0;
    while (k < M.nknown && M.known[k].id != id) k++;
    if (k == M.nknown) {
        if (M.nknown == MAX_KNOWN) k = (int)(id & 7) + MAX_KNOWN - 8;   // full: reuse one of the last slots
        else M.nknown++;
    }
    M.known[k].id = id;
    M.known[k].vid = dd.idVendor;
    M.known[k].pid = dd.idProduct;
}

static void lookup_device(s32 id, u16 *vid, u16 *pid) {
    for (int k = 0; k < M.nknown; k++)
        if (M.known[k].id == id) {
            *vid = M.known[k].vid;
            *pid = M.known[k].pid;
            return;
        }
}

// Decodes an output buffer by its size: interface lists, interface info, transfer reports.
static void log_out_buffer(const void *p, size_t size, s32 count_hint) {
    if (!p || !size) return;
    if (size % sizeof(UsbHsInterface) == 0) {
        size_t n = size / sizeof(UsbHsInterface);
        if (count_hint >= 0 && (size_t)count_hint < n) n = (size_t)count_hint;
        for (size_t i = 0; i < n; i++) {
            const UsbHsInterface *it = (const UsbHsInterface *)p + i;
            if (!it->inf.ID && !it->device_desc.idVendor) continue;
            log_interface("     ", it);
            remember_device(it);
        }
        return;
    }
    if (size == sizeof(UsbHsInterfaceInfo)) {
        log_info("     ", p);
        return;
    }
    if (size % sizeof(UsbHsXferReport) == 0 && size <= 64 * sizeof(UsbHsXferReport)) {
        size_t n = size / sizeof(UsbHsXferReport);
        if (count_hint >= 0 && (size_t)count_hint < n) n = (size_t)count_hint;
        for (size_t i = 0; i < n && i < 8; i++) {
            const UsbHsXferReport *r = (const UsbHsXferReport *)p + i;
            log_printf("     report xfer %u res %s %u/%u bytes id 0x%llx", r->xferId, rc_str(r->res), r->transferredSize,
                       r->requestedSize, (unsigned long long)r->id);
        }
        return;
    }
    size_t max = (size_t)M.cfg->buf_dump;
    if (max) log_hex("     ", p, size < max ? size : max);
}

// ------------------------------------------------------------------ sm (Atmosphère extensions, tipc)

// Like libstratosphere: the mitm talks to sm over a session of its own (connected to "sm:" and
// registered), not over libnx's session that the rest of the process uses.
static Result sm_open(void) {
    Handle h;
    Result rc = svcConnectToNamedPort(&h, "sm:");
    if (R_FAILED(rc)) return rc;
    tipcCreate(&M.sm, h);
    rc = tipcDispatch(&M.sm, 0, .in_send_pid = true);   // RegisterClient
    if (R_FAILED(rc)) tipcClose(&M.sm);
    return rc;
}

static Result sm_install(SmServiceName name, Handle *port, Handle *query) {
    Handle h[2] = {INVALID_HANDLE, INVALID_HANDLE};
    Result rc = tipcDispatchIn(&M.sm, SM_INSTALL_MITM, name,
                               .out_handle_attrs = {SfOutHandleAttr_HipcMove, SfOutHandleAttr_HipcMove}, .out_handles = h);
    if (R_SUCCEEDED(rc)) {
        *port = h[0];
        *query = h[1];
    }
    return rc;
}

// InstallMitm leaves a "future mitm" declaration in sm, and sm defers every GetServiceHandle for the
// service until it is cleared (eoaudio v0.4.0-0.4.2 hung the console without this). Also clears a
// declaration from boot2's mitm.lst when the install failed, so that clients do not wait forever.
static Result sm_clear_future(SmServiceName name) { return tipcDispatchIn(&M.sm, SM_CLEAR_FUTURE_MITM, name); }

static Result sm_uninstall(SmServiceName name) { return tipcDispatchIn(&M.sm, SM_UNINSTALL_MITM, name); }

static Result sm_acknowledge(SmServiceName name, mitm_info *info, Handle *fwd) {
    return tipcDispatchInOut(&M.sm, SM_ACKNOWLEDGE_MITM, name, *info, .out_handle_attrs = {SfOutHandleAttr_HipcMove},
                             .out_handles = fwd);
}

// ------------------------------------------------------------------ message helpers

static void *tls(void) { return armGetTls(); }

static void prepare_receive(void) {
    HipcRequest r = hipcMakeRequestInline(tls(), .num_recv_statics = HIPC_AUTO_RECV_STATIC);
    r.recv_list[0] = hipcMakeRecvStatic(M.ptrbuf, sizeof(M.ptrbuf));
}

static u8 *cmif_raw(const HipcParsedRequest *p) { return (u8 *)ALIGN_UP((uintptr_t)p->data.data_words, 16); }

static Result reply(Handle session) {
    s32 idx;
    Result rc = svcReplyAndReceive(&idx, NULL, 0, session, 0);
    return rc == KERNELRESULT(TimedOut) ? 0 : rc;
}

static Result reply_data(Handle session, Result result, const void *out, u32 size) {
    HipcRequest r = hipcMakeRequestInline(tls(), .type = CmifCommandType_Invalid,
                                          .num_data_words = (16 + sizeof(CmifOutHeader) + size + 3) / 4);
    CmifOutHeader *h = (CmifOutHeader *)ALIGN_UP((uintptr_t)r.data_words, 16);
    *h = (CmifOutHeader){CMIF_OUT_HEADER_MAGIC, 0, result, 0};
    if (size) memcpy(h + 1, out, size);
    return reply(session);
}

static msess *add_session(Handle srv, Handle fwd, int svc, u64 program, u64 pid, const objst *obj) {
    if (M.n >= MAX_SESSIONS) {
        log_printf("mitm: too many sessions, refusing one from %s", program_name(program));
        svcCloseHandle(srv);
        svcCloseHandle(fwd);
        return NULL;
    }
    msess *s = &M.s[M.n++];
    *s = (msess){.srv = srv, .fwd = fwd, .svc = svc, .serial = ++M.serial, .program = program, .pid = pid, .no = 1};
    s->o[0] = *obj;
    s->o[0].id = 0;
    s->o[0].hot_detail = 0;
    s->o[0].hot_since = now_ms();
    return s;
}

static void close_session(int i, const char *why) {
    msess *s = &M.s[i];
    log_printf("%s: session closed (%s), %d left", obj_label(s, &s->o[0]), why, M.n - 1);
    svcCloseHandle(s->srv);
    if (s->fwd != INVALID_HANDLE) svcCloseHandle(s->fwd);
    M.s[i] = M.s[--M.n];
}

// Sends the received message (in TLS) to the real session; the real reply is left in TLS. The client's
// PID goes along tagged, and its receive list is pointed at our buffer (the reply's statics land there
// and the kernel copies them on to the client when we reply). Copy handles the client sent were
// copied into our table: closed after forwarding.
static Result forward(msess *s) {
    void *base = tls();
    HipcParsedRequest p = hipcParseRequest(base);
    if (p.meta.send_pid) {
        u8 *pidp = (u8 *)base + sizeof(HipcHeader) + sizeof(HipcSpecialHeader);
        u64 pid;
        memcpy(&pid, pidp, sizeof(pid));
        pid = MITM_PID_TAG | (pid & 0x0000FFFFFFFFFFFFULL);
        memcpy(pidp, &pid, sizeof(pid));
    }
    if (p.meta.num_recv_statics) {
        HipcHeader *h = (HipcHeader *)base;
        h->recv_static_mode = 2;
        p.data.recv_list[0] = hipcMakeRecvStatic(M.ptrbuf, sizeof(M.ptrbuf));
    }
    Handle copies[8];
    u32 nc = p.meta.num_copy_handles < 8 ? p.meta.num_copy_handles : 8;
    for (u32 i = 0; i < nc; i++) copies[i] = p.data.copy_handles[i];
    Result rc = svcSendSyncRequest(s->fwd);
    for (u32 i = 0; i < nc; i++) svcCloseHandle(copies[i]);
    return rc;
}

static Result reply_forwarded(Handle session) {
    HipcResponse r = hipcParseResponse(tls());
    Handle copies[8];
    u32 nc = r.num_copy_handles < 8 ? r.num_copy_handles : 8;
    for (u32 i = 0; i < nc; i++) copies[i] = r.copy_handles[i];
    Result rc = reply(session);
    for (u32 i = 0; i < nc; i++) svcCloseHandle(copies[i]);
    return rc;
}

// The forwarded reply carries a session (move handle 0): keep the real one, give the client ours.
static bool swap_returned_session(Handle *real_out, Handle *our_server) {
    HipcResponse r = hipcParseResponse(tls());
    if (r.num_move_handles < 1) return false;
    Handle srv, cli;
    if (R_FAILED(svcCreateSession(&srv, &cli, false, 0))) return false;
    *real_out = r.move_handles[0];
    *our_server = srv;
    r.move_handles[0] = cli;   // moved to the client by the reply
    return true;
}

// ------------------------------------------------------------------ client memory

// The data stage of a control transfer is in the client's own memory (the usb sysmodule reaches it
// through the process handle from BindClientProcess). svcMapProcessMemory refuses heap memory
// (eoaudio v0.4.7: 0xD401), so it is read as a debugger would: attach, read, detach. The client is
// paused only for the read; it is blocked in its request to us anyway.
static Result read_client(u64 pid, u64 addr, void *dst, size_t size) {
    Handle d;
    Result rc = svcDebugActiveProcess(&d, pid);
    if (R_FAILED(rc)) return rc;
    rc = svcReadDebugProcessMemory(dst, d, addr, size);
    svcCloseHandle(d);
    return rc;
}

static void dump_ctrl_data(msess *s, const objst *o, u64 addr, size_t len, const char *what) {
    if (!M.cfg->read_ctrl_data || !len || !addr) return;
    size_t n = len < sizeof(M.data) ? len : sizeof(M.data);
    Result rc = read_client(s->pid, addr, M.data, n);
    if (R_FAILED(rc)) {
        M.dbg_fail++;
        M.last_dbg_rc = rc;
        log_printf("     %s data: cannot read the client's memory at 0x%llx (%s)", what, (unsigned long long)addr,
                   rc_str(rc));
        return;
    }
    log_printf("     %s data (%u bytes%s):", what, (unsigned)len, n < len ? ", first ones" : "");
    log_hex("       ", M.data, n);
    // Walk descriptor lists (GET_DESCRIPTOR answers): one decoded line per descriptor.
    if (o->setup[0] == 0x80 && o->setup[1] == 6) {
        for (size_t i = 0; i + 2 <= n && M.data[i] >= 2; i += M.data[i]) {
            char line[160];
            size_t dl = M.data[i] <= n - i ? M.data[i] : n - i;
            usb_desc_str(line, sizeof(line), M.data + i, dl);
            log_printf("       @%03u %s", (unsigned)i, line);
        }
    }
}

static bool is_eo_ic100(u16 vid, u16 pid) { return vid == 0x04E8 && (pid == 0xA054 || pid == 0xA059 || pid == 0xA05B); }

// Approach C: the audio sysmodule's GET_DESCRIPTOR(CONFIG) answer is rewritten in its own buffer
// before it gets the transfer report, i.e. before it can parse it. Only rules that keep every length
// (rewrite.c). The write goes through the same debug handle as the reads; the kernel writes through
// its own mapping of the page, so the client sees the new bytes on its next access.
// The rules for this plug-in: config.ini's rewrite=, unless an app asked for others in
// OVERRIDE_PATH (eofm's speaker output: the microphone only, so that the console does not take the
// earphone as its output and plays on its own speaker). Read at each descriptor read, which happens
// only while the earphone enumerates.
#define OVERRIDE_PATH "sdmc:/switch/usbhs-logmitm/rewrite_override.txt"

// The override file's first line, "" without one.
static void override_text(char *text, size_t size) {
    text[0] = 0;
    FILE *f = fopen(OVERRIDE_PATH, "rb");
    if (!f) return;
    size_t n = fread(text, 1, size - 1, f);
    fclose(f);
    text[n] = 0;
    for (char *c = text; *c; c++)
        if (*c == '\r' || *c == '\n') *c = 0;
}

// "hide": the audio sysmodule does not see the EO-IC100 at all (eofm's speaker output takes its
// microphone itself, and the console plays on its speaker as no USB output exists).
// Switched off (0.3.1): with the earphone left out of its list, the audio sysmodule acquired the
// emptied slots (interface ID 0, eight times) and then crashed on a null pointer (0.3.0 on the
// console, fatal in 0100000000000014 and then in qlaunch). It does not trust the count.
static bool audio_hidden(void) { return false; }

static unsigned current_rules(bool *overridden) {
    *overridden = false;
    char text[64];
    override_text(text, sizeof(text));
    if (!text[0]) return M.cfg->rewrite;
    if (!strcmp(text, "hide")) {
        *overridden = true;
        return 0;
    }
    unsigned rules;
    const char *bad = NULL;
    if (rewrite_parse(text, &rules, &bad) != 0) {
        log_printf("     rewrite: %s has an unknown rule \"%s\", using config.ini", OVERRIDE_PATH, bad ? bad : "?");
        return M.cfg->rewrite;
    }
    *overridden = true;
    return rules;
}

static void rewrite_ctrl_data(msess *s, objst *o, size_t len) {
    if (s->program != PROGRAM_AUDIO || !is_eo_ic100(o->vid, o->pid)) return;
    if (o->setup[0] != 0x80 || o->setup[1] != 6 || o->setup[3] != 2 || len < 9 || !o->ctrl_buf) return;
    bool overridden;
    unsigned rules = current_rules(&overridden);
    if (!rules) {
        if (overridden) log_printf("     rewrite: none (asked for by %s)", OVERRIDE_PATH);
        return;
    }
    static u8 buf[CTRL_DUMP], check[CTRL_DUMP];
    size_t n = len < sizeof(buf) ? len : sizeof(buf);
    Handle d;
    Result rc = svcDebugActiveProcess(&d, s->pid);
    if (R_FAILED(rc)) {
        log_printf("     rewrite: cannot attach to audio (%s), descriptor left as is", rc_str(rc));
        return;
    }
    rc = svcReadDebugProcessMemory(buf, d, o->ctrl_buf, n);
    rw_notes notes;
    int changed = R_SUCCEEDED(rc) ? rewrite_config(buf, n, rules, &notes) : 0;
    Result wr = 0, rr = 0;
    bool same = false;
    if (changed) {
        wr = svcWriteDebugProcessMemory(d, buf, o->ctrl_buf, n);
        if (R_SUCCEEDED(wr)) rr = svcReadDebugProcessMemory(check, d, o->ctrl_buf, n);
        same = R_SUCCEEDED(wr) && R_SUCCEEDED(rr) && !memcmp(buf, check, n);
    }
    svcCloseHandle(d);
    if (R_FAILED(rc)) {
        log_printf("     rewrite: read failed (%s), descriptor left as is", rc_str(rc));
        return;
    }
    char names[100];
    rewrite_names(rules, names, sizeof(names));
    if (overridden) strcat(names, ", from the override file");
    if (!changed) {
        log_printf("     rewrite (%s): nothing to change in these %u bytes", names, (unsigned)n);
        return;
    }
    for (int k = 0; k < notes.n; k++) log_printf("     rewrite: %s", notes.text[k]);
    if (same) M.rewrites++;
    log_printf("     rewrite (%s): %d field(s) written to audio's buffer: %s", names, changed,
               R_FAILED(wr) ? rc_str(wr) : same ? "OK, read back the same" : "written, but the read-back differs");
}

// Interfaces an intercepted client must not see: the remote's HID interfaces for hid (this
// process reads the buttons itself, remote.c), and the whole earphone for audio while eofm's speaker
// output wants it.
static bool hidden_from(const msess *s, const UsbHsInterface *it) {
    struct usb_device_descriptor dd;
    memcpy(&dd, &it->device_desc, sizeof(dd));
    if (!is_eo_ic100(dd.idVendor, dd.idProduct)) return false;
    if (s->program == PROGRAM_HID) return M.cfg->remote && it->inf.interface_desc.bInterfaceClass == 3;
    if (s->program == PROGRAM_AUDIO) return audio_hidden();
    return false;
}

// Removes hidden interfaces from a Query*Interfaces answer: the list in the client's buffer and
// the count in the reply.
static void hide_interfaces(msess *s, objst *o, void *list, size_t size, u8 *count_out) {
    if (!list || size < sizeof(UsbHsInterface) || !count_out) return;
    s32 n;
    memcpy(&n, count_out, 4);
    size_t max = size / sizeof(UsbHsInterface);
    if (n < 0 || (size_t)n > max) return;
    UsbHsInterface *v = list;
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (hidden_from(s, &v[i])) continue;
        if (kept != i) memmove(&v[kept], &v[i], sizeof(UsbHsInterface));
        kept++;
    }
    if (kept == n) return;
    memset(&v[kept], 0, (size_t)(n - kept) * sizeof(UsbHsInterface));
    s32 k = kept;
    memcpy(count_out, &k, 4);
    log_printf("     hidden from %s: %d EO-IC100 interface(s), %d left in the list", program_name(s->program), n - kept, kept);
}

// ------------------------------------------------------------------ request handling

typedef struct {
    const void *addr;
    size_t size;
} bufref;

typedef struct {
    bufref a[4], b[4], x[4];
    u32 na, nb, nx, nc;
} reqbufs;

static void save_bufs(const HipcParsedRequest *p, reqbufs *rb) {
    memset(rb, 0, sizeof(*rb));
    for (u32 i = 0; i < p->meta.num_send_buffers && rb->na < 4; i++, rb->na++)
        rb->a[i] = (bufref){hipcGetBufferAddress(&p->data.send_buffers[i]), hipcGetBufferSize(&p->data.send_buffers[i])};
    for (u32 i = 0; i < p->meta.num_recv_buffers && rb->nb < 4; i++, rb->nb++)
        rb->b[i] = (bufref){hipcGetBufferAddress(&p->data.recv_buffers[i]), hipcGetBufferSize(&p->data.recv_buffers[i])};
    for (u32 i = 0; i < p->meta.num_send_statics && rb->nx < 4; i++, rb->nx++)
        rb->x[i] = (bufref){hipcGetStaticAddress(&p->data.send_statics[i]), hipcGetStaticSize(&p->data.send_statics[i])};
    rb->nc = p->meta.num_recv_statics;
}

static objst *find_obj(msess *s, u32 id) {
    for (int i = 0; i < s->no; i++)
        if (s->o[i].id == id) return &s->o[i];
    return NULL;
}

static objst *add_obj(msess *s, u32 id, const objst *proto) {
    if (s->no >= MAX_OBJECTS) return NULL;
    objst *o = &s->o[s->no++];
    *o = *proto;
    o->id = id;
    o->hot_detail = 0;
    o->hot_since = now_ms();
    return o;
}

static void hot_summary(msess *s, objst *o, bool force) {
    u64 t = now_ms();
    if (!force && t - o->hot_since < HOT_SUMMARY_MS) return;
    char line[200];
    int n = 0;
    u32 total = 0;
    for (u32 c = 0; c < 10; c++) {
        if (!o->hot_n[c]) continue;
        const char *nm = usbhs_cmd_name(o->kind, c);
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %u x %s", o->hot_n[c], nm ? nm : "?");
        total += o->hot_n[c];
        o->hot_n[c] = 0;
    }
    if (total)
        log_printf("%s: %s in %llu ms (%u failed)", obj_label(s, o), line + 1, (unsigned long long)(t - o->hot_since),
                   o->hot_fail);
    o->hot_fail = 0;
    o->hot_since = t;
}

// Input summary for the commands that matter; raw hex for everything else.
static void log_request(msess *s, objst *o, u32 cmd, const u8 *in, size_t in_len, const reqbufs *rb, const char *name,
                        bool detail) {
    char line[256];
    const char *label = obj_label(s, o);
    if (o->kind == OBJ_IF && cmd == IF_CTRL_XFER && in_len >= 16) {
        char setup[160];
        u64 buf;
        memcpy(&buf, in + 8, 8);
        usb_setup_str(setup, sizeof(setup), in);
        log_printf("%s #%u CtrlXferAsync: %s  [%02x %02x %02x%02x %02x%02x %02x%02x] buf 0x%llx", label, cmd, setup, in[0],
                   in[1], in[3], in[2], in[5], in[4], in[7], in[6], (unsigned long long)buf);
        return;
    }
    if (!detail) return;
    if (o->kind == OBJ_EP && cmd == 6 && in_len >= 32) {   // BatchBufferAsync
        u32 urbs, unk1, unk2;
        u64 buf, id;
        memcpy(&urbs, in, 4);
        memcpy(&unk1, in + 4, 4);
        memcpy(&unk2, in + 8, 4);
        memcpy(&buf, in + 16, 8);
        memcpy(&id, in + 24, 8);
        int n = snprintf(line, sizeof(line), "%s #6 BatchBufferAsync: %u urbs unk1 %u unk2 %u (frame) buf 0x%llx id 0x%llx sizes",
                         label, urbs, unk1, unk2, (unsigned long long)buf, (unsigned long long)id);
        // The urb sizes: an X static from audio (v0.2.0 log), an A buffer from libnx.
        const bufref *sz = rb->nx && rb->x[0].addr ? &rb->x[0] : rb->na && rb->a[0].addr ? &rb->a[0] : NULL;
        for (size_t i = 0; sz && i < sz->size / 4 && i < 8 && n < (int)sizeof(line) - 12; i++) {
            u32 v;
            memcpy(&v, (const u8 *)sz->addr + 4 * i, 4);
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %u", v);
        }
        log_printf("%s", line);
        return;
    }
    char hex[120];
    hex_line(hex, sizeof(hex), in, in_len, 40);
    int n = snprintf(line, sizeof(line), "%s #%u %s: in [%s]", label, cmd, name ? name : "?", hex);
    for (u32 i = 0; i < rb->na; i++) n += snprintf(line + n, sizeof(line) - (size_t)n, " A+0x%zx", rb->a[i].size);
    for (u32 i = 0; i < rb->nx; i++) n += snprintf(line + n, sizeof(line) - (size_t)n, " X+0x%zx", rb->x[i].size);
    for (u32 i = 0; i < rb->nb; i++) n += snprintf(line + n, sizeof(line) - (size_t)n, " B+0x%zx", rb->b[i].size);
    if (rb->nc) snprintf(line + n, sizeof(line) - (size_t)n, " C");
    log_printf("%s", line);
    // Small input buffers (interface filters, urb sizes): their bytes.
    for (u32 i = 0; i < rb->na; i++)
        if (rb->a[i].addr && rb->a[i].size && rb->a[i].size <= 256) log_hex("     A: ", rb->a[i].addr, rb->a[i].size);
    for (u32 i = 0; i < rb->nx; i++)
        if (rb->x[i].addr && rb->x[i].size && rb->x[i].size <= 256) log_hex("     X: ", rb->x[i].addr, rb->x[i].size);
}

// Returns false if the session is gone.
static bool handle_request(int i) {
    msess *s = &M.s[i];
    void *base = tls();
    HipcParsedRequest p = hipcParseRequest(base);
    u32 type = p.meta.type;
    if (type == CmifCommandType_Close) {
        close_session(i, "client closed it");
        return false;
    }
    bool control = type == CmifCommandType_Control || type == CmifCommandType_ControlWithContext;
    bool request = type == CmifCommandType_Request || type == CmifCommandType_RequestWithContext;
    u8 *raw = cmif_raw(&p);
    u8 *end = (u8 *)p.data.data_words + p.meta.num_data_words * 4;
    objst *o = &s->o[0];
    u32 cmd = 0xFFFFFFFF;
    const u8 *in = NULL;
    size_t in_len = 0;
    bool domain_close = false;
    u32 domain_obj = 0;
    if (request && s->domain) {
        CmifDomainInHeader dh;
        memcpy(&dh, raw, sizeof(dh));
        domain_obj = dh.object_id;
        o = find_obj(s, dh.object_id);
        if (!o) o = &s->o[0];   // unknown object: labelled as the first one, still forwarded
        if (dh.type == CmifDomainRequestType_Close) domain_close = true;
        else if (dh.data_size >= sizeof(CmifInHeader)) {
            memcpy(&cmd, raw + 16 + 8, 4);
            in = raw + 32;
            in_len = dh.data_size - sizeof(CmifInHeader);
        }
    } else if (request || control) {
        memcpy(&cmd, raw + 8, 4);
        in = raw + 16;
        in_len = end > in ? (size_t)(end - in) : 0;
    }
    // The input lives in TLS, which the forwarded reply overwrites: keep a copy.
    u8 inb[64];
    if (in_len > sizeof(inb)) in_len = sizeof(inb);
    if (in) memcpy(inb, in, in_len);
    in = inb;
    reqbufs rb;
    save_bufs(&p, &rb);
    M.requests++;

    const char *name = control ? cmif_control_name(cmd) : usbhs_cmd_name(o->kind, cmd);
    bool hot = request && !domain_close && usbhs_cmd_is_hot(o->kind, cmd);
    bool detail = !hot || o->hot_detail < (u32)M.cfg->hot_detail;
    if (hot) {
        if (cmd < 10) o->hot_n[cmd]++;
        if (detail) o->hot_detail++;
        if (o->hot_detail == (u32)M.cfg->hot_detail && detail)
            log_printf("%s: further %s-type requests are only counted (every %d s)", obj_label(s, o), name ? name : "?",
                       HOT_SUMMARY_MS / 1000);
    }
    if (domain_close) {
        log_printf("%s: domain object %u closed", obj_label(s, o), domain_obj);
    } else if (control) {
        log_printf("%s: control #%u %s", obj_label(s, o), cmd, name ? name : "?");
    } else if (request) {
        log_request(s, o, cmd, in, in_len, &rb, name, detail);
        if (o->kind == OBJ_IF && cmd == IF_CTRL_XFER && in_len >= 16) {
            memcpy(o->setup, in, 8);
            memcpy(&o->ctrl_buf, in + 8, 8);
            u16 wlen = (u16)(in[6] | in[7] << 8);
            o->ctrl_pending = (in[0] & 0x80) && wlen;
            if (!(in[0] & 0x80) && wlen) dump_ctrl_data(s, o, o->ctrl_buf, wlen, "OUT");
        }
    } else {
        log_printf("%s: HIPC type %u", obj_label(s, o), type);
    }
    // On the SD card before the request goes on, except for streaming requests: those carry an
    // isochronous start frame ~13 ms ahead, and v0.2.0 made them miss it by waiting for the SD card.
    if (!hot) trace_sync();

    Result rc = forward(s);
    if (R_FAILED(rc)) {
        M.failures++;
        log_printf("%s: forwarding failed: %s", obj_label(s, o), rc_str(rc));
        close_session(i, "the real session failed");
        return false;
    }

    // ---- the real reply (in TLS)
    HipcParsedRequest r = hipcParseRequest(base);
    u8 *rraw = cmif_raw(&r);
    u8 *rend = (u8 *)r.data.data_words + r.meta.num_data_words * 4;
    CmifOutHeader oh = {0};
    const u8 *out = NULL;
    size_t out_len = 0;
    u32 n_objs = 0;
    const u8 *objs = NULL;   // out object IDs (u32 each, not necessarily aligned)
    if (request && s->domain && !domain_close) {
        CmifDomainOutHeader dh;
        memcpy(&dh, rraw, sizeof(dh));
        memcpy(&oh, rraw + 16, sizeof(oh));
        out = rraw + 32;
        n_objs = dh.num_out_objects;
        // The IDs follow the raw output at its exact size, unaligned: OpenUsbEp returns a 7-byte
        // endpoint descriptor and then the ID (v0.2.0 log: "0905030d 8001010b" = desc + 0x0b).
        // For other commands: where the 16-byte alignment pad ends.
        int known = (o->kind == OBJ_ROOT && cmd == ROOT_ACQUIRE) ? 0 : (o->kind == OBJ_IF && cmd == IF_OPEN_EP) ? 7 : -1;
        if (n_objs && n_objs <= 8) {
            if (known >= 0) objs = out + known;
            else if (rraw + r.meta.num_data_words * 4 - 16 - 4 * n_objs >= out)
                objs = rraw + r.meta.num_data_words * 4 - 16 - 4 * n_objs;
        }
        out_len = objs ? (size_t)(objs - out) : (rend > out ? (size_t)(rend - out) : 0);
    } else if (!domain_close && rend >= rraw + 16) {
        memcpy(&oh, rraw, sizeof(oh));
        out = rraw + 16;
        out_len = rend > out ? (size_t)(rend - out) : 0;
    }
    bool ok = oh.magic == CMIF_OUT_HEADER_MAGIC && R_SUCCEEDED(oh.result);
    if (hot && !ok) o->hot_fail++;

    if (!hot || detail || !ok) {
        char hex[120];
        hex_line(hex, sizeof(hex), out, out_len, 32);
        if (domain_close)
            ;
        else if (oh.magic != CMIF_OUT_HEADER_MAGIC)
            log_printf("  -> (no CMIF reply header, %u data words)", r.meta.num_data_words);
        else
            log_printf("  -> %s%s%s%s%s", rc_str(oh.result), out_len ? " out [" : "", out_len ? hex : "", out_len ? "]" : "",
                       r.meta.num_move_handles ? " +session" : r.meta.num_copy_handles ? " +handle" : "");
        if (ok && request) {
            s32 count = -1;
            if (out_len >= 4) memcpy(&count, out, 4);
            for (u32 k = 0; k < rb.nb; k++) log_out_buffer(rb.b[k].addr, rb.b[k].size, count);
            for (u32 k = 0; k < r.meta.num_send_statics; k++)   // C buffers: the reply's statics, in our ptrbuf
                log_out_buffer(hipcGetStaticAddress(&r.data.send_statics[k]), hipcGetStaticSize(&r.data.send_statics[k]),
                               count);
        }
    }

    // ---- bookkeeping on the reply
    if (ok && request && o->kind == OBJ_ROOT && (cmd == 1 || cmd == 2) && rb.nb && out_len >= 4)
        hide_interfaces(s, o, (void *)rb.b[0].addr, rb.b[0].size, (u8 *)out);
    if (ok && request && o->kind == OBJ_IF && cmd == IF_CTRL_REPORT && o->ctrl_pending) {
        o->ctrl_pending = false;
        const UsbHsXferReport *rep = NULL;
        if (rb.nb && rb.b[0].size >= sizeof(UsbHsXferReport)) rep = rb.b[0].addr;
        else if (r.meta.num_send_statics && hipcGetStaticSize(&r.data.send_statics[0]) >= sizeof(UsbHsXferReport))
            rep = hipcGetStaticAddress(&r.data.send_statics[0]);
        if (rep && R_SUCCEEDED(rep->res)) {
            dump_ctrl_data(s, o, o->ctrl_buf, rep->transferredSize, "IN");
            rewrite_ctrl_data(s, o, rep->transferredSize);
        }
    }
    if (ok && request && o->kind == OBJ_IF && cmd == IF_SET_INTERFACE && rb.nb && rb.b[0].addr &&
        rb.b[0].size >= sizeof(UsbHsInterfaceInfo)) {
        const UsbHsInterfaceInfo *inf = rb.b[0].addr;
        o->alt = inf->interface_desc.bAlternateSetting;
    }

    objst child = {0};
    bool new_obj = false;
    if (ok && request && o->kind == OBJ_ROOT && cmd == ROOT_ACQUIRE) {
        child.kind = OBJ_IF;
        if (in_len >= 4) memcpy(&child.if_id, in, 4);
        lookup_device(child.if_id, &child.vid, &child.pid);
        for (u32 k = 0; k < rb.nb; k++)
            if (rb.b[k].size >= sizeof(UsbHsInterfaceInfo) && rb.b[k].addr) {
                child.ifnum = ((const UsbHsInterfaceInfo *)rb.b[k].addr)->interface_desc.bInterfaceNumber;
                break;
            }
        new_obj = true;
    } else if (ok && request && o->kind == OBJ_IF && cmd == IF_OPEN_EP) {
        child.kind = OBJ_EP;
        child.if_id = o->if_id;
        child.vid = o->vid;
        child.pid = o->pid;
        child.ifnum = o->ifnum;
        // Output: the endpoint descriptor (bLength, bDescriptorType, bEndpointAddress, ...).
        if (out_len >= 3 && out[1] == USB_DT_ENDPOINT) child.ep = out[2];
        else if (in_len >= 16) {   // in: maxUrbCount, epType, epNumber, epDirection
            u32 num, dir;
            memcpy(&num, in + 8, 4);
            memcpy(&dir, in + 12, 4);
            child.ep = (u8)((num & 0xF) | (dir == 1 ? 0x80 : 0));
        }
        new_obj = true;
    }

    if (new_obj && s->domain) {
        if (objs && n_objs) {
            u32 id;
            memcpy(&id, objs, 4);
            objst *c = add_obj(s, id, &child);
            if (c) log_printf("   new %s", obj_label(s, c));
        }
    } else if (new_obj) {
        Handle real, srv;
        if (swap_returned_session(&real, &srv)) {
            msess *c = add_session(srv, real, s->svc, s->program, s->pid, &child);
            s = &M.s[i];
            if (c) log_printf("   new session %s", obj_label(c, &c->o[0]));
        }
    } else if (control && (cmd == CTL_CLONE || cmd == CTL_CLONE_EX) && ok) {
        Handle real, srv;
        if (swap_returned_session(&real, &srv)) {
            msess *c = add_session(srv, real, s->svc, s->program, s->pid, &s->o[0]);
            s = &M.s[i];
            if (c) log_printf("   clone: new session S%d", c->serial);
        }
    } else if (control && cmd == CTL_TO_DOMAIN && ok && out_len >= 4) {
        s->domain = true;
        memcpy(&s->o[0].id, out, 4);
        log_printf("   %s is a domain now (object %u)", obj_label(s, &s->o[0]), s->o[0].id);
    } else if (control && cmd == CTL_QUERY_PTRBUF && ok && out_len >= 2) {
        // The client may send statics up to this size: no more than both we and the real service take.
        u16 size;
        memcpy(&size, out, 2);
        if (size > PTRBUF_SIZE) {
            size = PTRBUF_SIZE;
            memcpy((u8 *)out, &size, 2);
        }
    }
    if (domain_close && s->domain) {
        for (int k = 0; k < s->no; k++)
            if (s->o[k].id == domain_obj && k) {
                s->o[k] = s->o[--s->no];
                break;
            }
    }
    o = NULL;   // the tables may have moved

    if (hot && !detail) hot_summary(s, find_obj(s, domain_obj) ? find_obj(s, domain_obj) : &s->o[0], false);

    if (R_FAILED(reply_forwarded(s->srv))) {
        close_session(i, "reply failed");
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ sm's side

static void handle_query(int k) {
    HipcParsedRequest p = hipcParseRequest(tls());
    if (p.meta.type == CmifCommandType_Close) {
        svcCloseHandle(M.query[k]);
        M.query[k] = INVALID_HANDLE;
        log_printf("mitm: sm closed the query session of %s", M.name[k]);
        return;
    }
    u32 *raw = (u32 *)cmif_raw(&p);
    u32 cmd = raw[2];   // read now: reply_data() below overwrites TLS (v0.1.0 logged "cmd 0")
    mitm_info info = {0};
    memcpy(&info, raw + 4, sizeof(info));
    bool want = cmd == QUERY_SHOULD_MITM && info.process_id != M.self &&
                config_wants(M.cfg, info.program_id, info.flags);
    u8 yes = want;
    Result rc = reply_data(M.query[k], 0, &yes, sizeof(yes));
    M.queries++;
    log_printf("mitm: sm asks (cmd %u) about %s %s (pid %llu, flags 0x%llx): %s, reply %s", cmd, M.name[k],
               program_name(info.program_id), (unsigned long long)info.process_id, (unsigned long long)info.flags,
               yes ? "INTERCEPT" : "leave", rc_str(rc));
    trace_sync();
}

static void accept_client(int k) {
    Handle srv;
    Result rc = svcAcceptSession(&srv, M.port[k]);
    if (R_FAILED(rc)) {
        log_printf("mitm: AcceptSession(%s): %s", M.name[k], rc_str(rc));
        return;
    }
    mitm_info info = {0};
    Handle fwd = INVALID_HANDLE;
    rc = sm_acknowledge(smEncodeName(M.name[k]), &info, &fwd);
    if (R_FAILED(rc)) {
        log_printf("mitm: AcknowledgeMitmSession(%s): %s", M.name[k], rc_str(rc));
        svcCloseHandle(srv);
        return;
    }
    M.clients++;
    if (info.program_id == PROGRAM_AUDIO) M.intercepted++;
    objst root = {.kind = OBJ_ROOT};
    msess *s = add_session(srv, fwd, k, info.program_id, info.process_id, &root);
    if (s)
        log_printf("mitm: %s (pid %llu) connected to %s: session S%d", program_name(info.program_id),
                   (unsigned long long)info.process_id, M.name[k], s->serial);
    trace_sync();
}

static void mitm_thread(void *arg) {
    Handle h[MAX_SESSIONS + 2 * MAX_SERVICES];
    int map[2 * MAX_SERVICES];   // wait-list index -> service*2 + (0 port | 1 query)
    for (;;) {
        int n = 0;
        for (int k = 0; k < M.nsvc; k++) {
            if (M.port[k] != INVALID_HANDLE) map[n] = k * 2, h[n++] = M.port[k];
            if (M.query[k] != INVALID_HANDLE) map[n] = k * 2 + 1, h[n++] = M.query[k];
        }
        int base = n;
        for (int i = 0; i < M.n; i++) h[n++] = M.s[i].srv;
        if (!n) {
            svcSleepThread(1000000000ULL);
            continue;
        }
        prepare_receive();
        s32 idx = -1;
        Result rc = svcReplyAndReceive(&idx, h, n, INVALID_HANDLE, UINT64_MAX);
        if (idx < 0 || idx >= n) {
            if (R_FAILED(rc)) {
                log_printf("mitm: ReplyAndReceive: %s", rc_str(rc));
                svcSleepThread(1000000ULL);
            }
            continue;
        }
        if (idx < base) {
            int k = map[idx] / 2;
            bool is_query = map[idx] & 1;
            if (R_FAILED(rc)) {
                log_printf("mitm: %s %s: %s (dropped)", M.name[k], is_query ? "query session" : "port", rc_str(rc));
                if (is_query) svcCloseHandle(M.query[k]), M.query[k] = INVALID_HANDLE;
                else svcCloseHandle(M.port[k]), M.port[k] = INVALID_HANDLE;
            } else if (is_query) {
                handle_query(k);
            } else {
                accept_client(k);
            }
        } else if (R_FAILED(rc)) {
            close_session(idx - base, rc == KERNELRESULT(ConnectionClosed) ? "client gone" : rc_str(rc));
        } else {
            handle_request(idx - base);
        }
    }
}

// ------------------------------------------------------------------ public

int mitm_start(const lm_config *cfg) {
    M.cfg = cfg;
    svcGetProcessId(&M.self, CUR_PROCESS_HANDLE);
    M.nsvc = cfg->n_services;
    for (int k = 0; k < M.nsvc; k++) {
        strcpy(M.name[k], cfg->services[k]);
        M.port[k] = M.query[k] = INVALID_HANDLE;
    }
    Result rc = sm_open();
    if (R_FAILED(rc)) {
        log_printf("mitm: own sm session: %s (nothing installed)", rc_str(rc));
        return 0;
    }
    M.sm_ok = true;
    int ok = 0;
    for (int k = 0; k < M.nsvc; k++) {
        rc = sm_install(smEncodeName(M.name[k]), &M.port[k], &M.query[k]);
        M.installed[k] = R_SUCCEEDED(rc);
        ok += M.installed[k];
        log_printf("mitm: InstallMitm(%s): %s", M.name[k], rc_str(rc));
    }
    if (ok) {
        rc = thread_spawn(&M.thread, mitm_thread, NULL, 0x4000, PRIO_MITM);
        if (R_FAILED(rc)) {   // nobody would answer sm: take everything down again
            log_printf("mitm: thread: %s, uninstalling", rc_str(rc));
            mitm_uninstall_all();
            ok = 0;
        }
    }
    // Only now may clients connect (the thread answers sm); without this they would wait forever.
    // Also for a failed install: a mitm.lst declaration must not leave the clients waiting.
    for (int k = 0; k < M.nsvc; k++) {
        Result rf = sm_clear_future(smEncodeName(M.name[k]));
        log_printf("mitm: ClearFutureMitm(%s): %s", M.name[k], rc_str(rf));
    }
    return ok;
}

void mitm_uninstall_all(void) {
    if (!M.sm_ok) return;
    for (int k = 0; k < M.nsvc; k++) {
        if (!M.installed[k]) continue;
        M.installed[k] = false;
        Result rc = sm_uninstall(smEncodeName(M.name[k]));
        log_printf("mitm: UninstallMitm(%s): %s", M.name[k], rc_str(rc));
    }
}

void mitm_status(char *buf, size_t size) {
    snprintf(buf, size,
             "status: %u sm queries, %u client(s) intercepted (%u from audio), %d session(s) open, %u requests, %u forward "
             "failures, %u descriptor rewrite(s), %u memory reads failed%s%s",
             M.queries, M.clients, M.intercepted, M.n, M.requests, M.failures, M.rewrites, M.dbg_fail, M.dbg_fail ? " last " : "",
             M.dbg_fail ? rc_str(M.last_dbg_rc) : "");
}
