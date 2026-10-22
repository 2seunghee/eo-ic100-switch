#include "usbdesc.h"

#include <stdio.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd24(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }

static void warn(ud_config *c, const char *msg, size_t off) {
    if (c->warnings++ == 0)
        snprintf(c->first_warning, sizeof(c->first_warning), "%s (offset %u)", msg, (unsigned)off);
}

static void parse_ac(ud_config *c, const ud_alt *alt, const uint8_t *d, uint8_t len, size_t off) {
    uint8_t subtype = d[2];
    if (subtype == UD_AC_HEADER) {
        if (len < 8) { warn(c, "short AC header", off); return; }
        c->has_ac = true;
        c->ac_iface = alt->number;
        c->bcd_adc = rd16(d + 3);
        if (c->bcd_adc >= 0x0200) return;  // UAC2 layouts are not decoded
        for (uint8_t i = 0; i < d[7] && i < UD_MAX_COLL && 8 + i < len; i++)
            c->collection[c->collection_count++] = d[8 + i];
        return;
    }
    if (c->bcd_adc >= 0x0200 || len < 4) return;
    if (c->unit_count >= UD_MAX_UNITS) { warn(c, "too many AC units", off); return; }

    ud_unit *u = &c->units[c->unit_count];
    memset(u, 0, sizeof(*u));
    u->subtype = subtype;
    u->id = d[3];
    switch (subtype) {
    case UD_AC_IT:
        if (len < 12) return;
        u->terminal_type = rd16(d + 4);
        u->nr_channels = d[7];
        u->channel_config = rd16(d + 8);
        u->string_index = d[11];
        break;
    case UD_AC_OT:
        if (len < 9) return;
        u->terminal_type = rd16(d + 4);
        u->sources[u->source_count++] = d[7];
        u->string_index = d[8];
        break;
    case UD_AC_MIXER:
    case UD_AC_SELECTOR:
        if (len < 5) return;
        for (uint8_t i = 0; i < d[4] && i < UD_MAX_SRC && 5 + i < len; i++)
            u->sources[u->source_count++] = d[5 + i];
        break;
    case UD_AC_FU: {
        if (len < 7) return;
        uint8_t n = d[5];
        u->sources[u->source_count++] = d[4];
        u->control_size = n;
        if (n >= 1 && n <= 4) {
            for (size_t p = 6; p + n <= (size_t)len - 1 && u->control_count < UD_MAX_CTRLS; p += n) {
                uint32_t v = 0;
                for (uint8_t k = 0; k < n; k++) v |= (uint32_t)d[p + k] << (8 * k);
                u->controls[u->control_count++] = v;
            }
        }
        u->string_index = d[len - 1];
        break;
    }
    case UD_AC_PROC:
    case UD_AC_EXT:
        if (len < 7) return;
        for (uint8_t i = 0; i < d[6] && i < UD_MAX_SRC && 7 + i < len; i++)
            u->sources[u->source_count++] = d[7 + i];
        break;
    default:
        return;
    }
    c->unit_count++;
}

static void parse_as(ud_alt *a, const uint8_t *d, uint8_t len) {
    if (d[2] == UD_AS_GENERAL && len >= 7) {
        a->has_as_general = true;
        a->terminal_link = d[3];
        a->delay = d[4];
        a->format_tag = rd16(d + 5);
    } else if (d[2] == UD_AS_FORMAT_TYPE && len >= 8) {
        a->has_format = true;
        a->format_type = d[3];
        if (d[3] != 1 && d[3] != 3) return;
        a->channels = d[4];
        a->subframe = d[5];
        a->bits = d[6];
        if (d[7] == 0) {
            if (len >= 14) { a->rate_min = rd24(d + 8); a->rate_max = rd24(d + 11); }
        } else {
            for (uint8_t i = 0; i < d[7] && i < UD_MAX_RATES && 8 + 3 * i + 3 <= len; i++)
                a->rates[a->rate_count++] = rd24(d + 8 + 3 * i);
        }
    }
}

bool ud_parse_config(const uint8_t *buf, size_t len, ud_config *c) {
    memset(c, 0, sizeof(*c));
    if (!buf || len < 9 || buf[1] != UD_DT_CONFIG) return false;
    c->total_length = rd16(buf + 2);
    c->num_interfaces = buf[4];
    c->config_value = buf[5];
    c->attributes = buf[7];
    c->max_power = buf[8];
    if (c->total_length != len) warn(c, "wTotalLength differs from buffer size", 0);

    size_t end = c->total_length < len ? c->total_length : len;
    size_t pos = buf[0] >= 9 ? buf[0] : 9;
    ud_alt *alt = NULL;
    ud_endpoint *ep = NULL;
    while (pos + 2 <= end) {
        uint8_t blen = buf[pos], type = buf[pos + 1];
        if (blen < 2) { warn(c, "bLength < 2", pos); break; }
        if (pos + blen > end) { warn(c, "descriptor overruns buffer", pos); break; }
        const uint8_t *d = buf + pos;

        if (type == UD_DT_INTERFACE && blen >= 9) {
            ep = NULL;
            if (c->alt_count < UD_MAX_ALTS) {
                alt = &c->alts[c->alt_count++];
                alt->number = d[2];
                alt->alt = d[3];
                alt->num_eps = d[4];
                alt->cls = d[5];
                alt->subcls = d[6];
                alt->proto = d[7];
                alt->iface_str = d[8];
            } else {
                warn(c, "too many interface descriptors", pos);
                alt = NULL;
            }
        } else if (type == UD_DT_ENDPOINT && blen >= 7) {
            ep = NULL;
            if (alt && alt->ep_count < UD_MAX_EPS) {
                ep = &alt->eps[alt->ep_count++];
                ep->address = d[2];
                ep->attributes = d[3];
                ep->max_packet = rd16(d + 4);
                ep->interval = d[6];
                ep->length = blen;
                if (blen >= 9) { ep->refresh = d[7]; ep->synch_address = d[8]; }
            } else {
                warn(c, "endpoint outside an interface (or too many)", pos);
            }
        } else if (type == UD_DT_CS_INTERFACE && alt && alt->cls == 1 && blen >= 3) {
            if (alt->subcls == 1) parse_ac(c, alt, d, blen, pos);
            else if (alt->subcls == 2) parse_as(alt, d, blen);
        } else if (type == UD_DT_CS_ENDPOINT && ep && blen >= 7 && d[2] == 1) {
            ep->has_cs = true;
            ep->cs_attributes = d[3];
        } else if (type == UD_DT_HID && alt && blen >= 9) {
            alt->has_hid = true;
            alt->hid_report_len = rd16(d + 7);
        }
        pos += blen;
    }
    return true;
}

const ud_unit *ud_find_unit(const ud_config *c, uint8_t id) {
    for (uint8_t i = 0; i < c->unit_count; i++)
        if (c->units[i].id == id) return &c->units[i];
    return NULL;
}

const ud_endpoint *ud_iso_data_ep(const ud_alt *a) {
    for (uint8_t i = 0; i < a->ep_count; i++)
        if (ud_ep_xfer(&a->eps[i]) == 1 && ud_ep_usage(&a->eps[i]) == 0) return &a->eps[i];
    return NULL;
}

bool ud_alt_supports(const ud_alt *a, uint32_t rate, uint8_t bits, uint8_t channels) {
    if (!a->has_format || a->format_type != 1 || !a->has_as_general || a->format_tag != 1) return false;
    if (a->bits != bits || a->channels != channels) return false;
    if (a->rate_count == 0) return a->rate_min <= rate && rate <= a->rate_max;
    for (uint8_t i = 0; i < a->rate_count; i++)
        if (a->rates[i] == rate) return true;
    return false;
}

static bool is_streaming(const ud_alt *a) { return a->cls == 1 && a->subcls == 2; }

// Walks first-source links from `from` towards `target_id`; reports the first feature unit seen.
static bool walk_back(const ud_config *c, const ud_unit *from, uint8_t target_id, const ud_unit **fu) {
    const ud_unit *node = from;
    *fu = NULL;
    for (int step = 0; node && step < 12; step++) {
        if (node->subtype == UD_AC_FU && !*fu) *fu = node;
        if (node->id == target_id) return true;
        if (node->source_count == 0) return false;
        node = ud_find_unit(c, node->sources[0]);
    }
    return false;
}

static void trace_playback(const ud_config *c, ud_stream *s) {
    for (uint8_t i = 0; i < c->unit_count; i++) {
        const ud_unit *ot = &c->units[i];
        const ud_unit *fu;
        if (ot->subtype != UD_AC_OT || ot->terminal_type == UD_TT_USB_STREAMING) continue;
        if (walk_back(c, ot, s->alt->terminal_link, &fu)) {
            s->terminal_id = ot->id;
            s->terminal_type = ot->terminal_type;
            if (fu) { s->fu_id = fu->id; s->fu_controls = fu->control_count ? fu->controls[0] : 0; }
            return;
        }
    }
}

static void trace_capture(const ud_config *c, ud_stream *s) {
    const ud_unit *node = ud_find_unit(c, s->alt->terminal_link);
    for (int step = 0; node && step < 12; step++) {
        if (node->subtype == UD_AC_FU && !s->fu_id) {
            s->fu_id = node->id;
            s->fu_controls = node->control_count ? node->controls[0] : 0;
        }
        if (node->subtype == UD_AC_IT) {
            s->terminal_id = node->id;
            s->terminal_type = node->terminal_type;
            return;
        }
        if (node->source_count == 0) return;
        node = ud_find_unit(c, node->sources[0]);
    }
}

bool ud_find_stream(const ud_config *c, bool playback, uint32_t rate, uint8_t bits, uint8_t channels,
                    ud_stream *out) {
    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0; i < c->alt_count; i++) {
        const ud_alt *a = &c->alts[i];
        if (!is_streaming(a)) continue;
        const ud_endpoint *ep = ud_iso_data_ep(a);
        if (!ep || ud_ep_is_in(ep) == playback) continue;
        if (!ud_alt_supports(a, rate, bits, channels)) continue;
        out->alt = a;
        out->ep = ep;
        out->bytes_per_ms = (uint16_t)(rate / 1000 * channels * a->subframe);
        out->has_rate_ctrl = ud_ep_has_rate_ctrl(ep);
        out->needs_rate_set = out->has_rate_ctrl && a->rate_count != 1;
        if (playback) trace_playback(c, out);
        else trace_capture(c, out);
        return true;
    }
    return false;
}

uint32_t ud_check(const ud_config *c) {
    uint32_t f = 0;
    if (!c->has_ac) return UD_FLAG_NO_AUDIO;
    if (c->bcd_adc >= 0x0200) f |= UD_FLAG_UAC2;

    // Direction of each AS interface, in order of first appearance.
    int8_t dir[16];     // -1 unknown, 0 playback, 1 capture
    uint8_t order[16], n_order = 0;
    memset(dir, -1, sizeof(dir));
    bool any_playback = false;
    for (uint8_t i = 0; i < c->alt_count; i++) {
        const ud_alt *a = &c->alts[i];
        if (!is_streaming(a) || a->number >= 16) continue;
        bool seen = false;
        for (uint8_t k = 0; k < n_order; k++) seen |= order[k] == a->number;
        if (!seen) order[n_order++] = a->number;
        const ud_endpoint *ep = ud_iso_data_ep(a);
        if (!ep) continue;
        if (dir[a->number] < 0) dir[a->number] = ud_ep_is_in(ep) ? 1 : 0;
        if (!ud_ep_is_in(ep)) any_playback = true;
        if (ud_ep_is_in(ep) && a->has_format && a->channels > 1) f |= UD_FLAG_STEREO_MIC;
    }
    if (!any_playback) f |= UD_FLAG_NO_PLAYBACK;
    if (n_order && dir[order[0]] == 1 && any_playback) f |= UD_FLAG_CAPTURE_FIRST;

    ud_stream s;
    if (!ud_find_stream(c, true, 48000, 16, 2, &s)) {
        f |= UD_FLAG_NO_48K16;
    } else {
        if (s.terminal_type && (s.terminal_type >> 8) != 0x03) f |= UD_FLAG_OT_NOT_OUTPUT;
        if (s.alt->rate_count != 1) f |= UD_FLAG_MULTI_RATE;
        if (ud_ep_sync(s.ep) != UD_SYNC_ADAPTIVE) f |= UD_FLAG_SYNC_NOT_ADAPTIVE;
    }

    uint8_t in_mask[16] = {0}, out_mask[16] = {0};
    for (uint8_t i = 0; i < c->alt_count; i++) {
        const ud_alt *a = &c->alts[i];
        if (a->cls != 1) f |= UD_FLAG_EXTRA_INTERFACES;
        for (uint8_t e = 0; e < a->ep_count; e++) {
            uint8_t num = a->eps[e].address & 0x0F;
            if (ud_ep_is_in(&a->eps[e])) in_mask[num] = 1; else out_mask[num] = 1;
        }
    }
    for (int n = 1; n < 16; n++)
        if (in_mask[n] && out_mask[n]) f |= UD_FLAG_SHARED_EP_NUMBER;
    return f;
}

const char *ud_flag_name(uint32_t flag) {
    switch (flag) {
    case UD_FLAG_NO_AUDIO:          return "NO_AUDIO_CONTROL";
    case UD_FLAG_UAC2:              return "UAC2";
    case UD_FLAG_NO_PLAYBACK:       return "NO_PLAYBACK";
    case UD_FLAG_NO_48K16:          return "NO_48K_16BIT_STEREO";
    case UD_FLAG_CAPTURE_FIRST:     return "H1 CAPTURE_FIRST";
    case UD_FLAG_OT_NOT_OUTPUT:     return "H2 OT_NOT_OUTPUT_TYPE";
    case UD_FLAG_MULTI_RATE:        return "H3 MULTI_RATE";
    case UD_FLAG_SYNC_NOT_ADAPTIVE: return "H4 SYNC_NOT_ADAPTIVE";
    case UD_FLAG_SHARED_EP_NUMBER:  return "H5 SHARED_EP_NUMBER";
    case UD_FLAG_STEREO_MIC:        return "H6 STEREO_MIC";
    case UD_FLAG_EXTRA_INTERFACES:  return "H7 EXTRA_INTERFACES";
    default:                        return "?";
    }
}

const char *ud_terminal_name(uint16_t type) {
    switch (type) {
    case 0x0101: return "USB Streaming";
    case 0x0201: return "Microphone";
    case 0x0301: return "Speaker";
    case 0x0302: return "Headphones";
    case 0x0304: return "Desktop Speaker";
    case 0x0401: return "Handset";
    case 0x0402: return "Headset";
    case 0x0603: return "Line Connector";
    default:     return "other";
    }
}

const char *ud_sync_name(uint8_t sync) {
    static const char *names[] = {"None", "Async", "Adaptive", "Sync"};
    return names[sync & 3];
}
