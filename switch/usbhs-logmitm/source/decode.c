#include "decode.h"

#include <stdio.h>

const char *usbhs_cmd_name(int kind, uint32_t cmd) {
    static const char *root[] = {"BindClientProcess",           "QueryAllInterfaces",
                                 "QueryAvailableInterfaces",    "QueryAcquiredInterfaces",
                                 "CreateInterfaceAvailableEvent", "DestroyInterfaceAvailableEvent",
                                 "GetInterfaceStateChangeEvent", "AcquireUsbIf"};
    static const char *ifs[] = {"GetStateChangeEvent", "SetInterface",     "GetInterface",
                                "GetAlternateInterface", "GetCurrentFrame", "CtrlXferAsync",
                                "GetCtrlXferCompletionEvent", "GetCtrlXferReport", "ResetDevice",
                                "OpenUsbEp"};
    static const char *ep[] = {"ReOpen",         "Close",           "GetCompletionEvent",
                               "PopulateRing",   "PostBufferAsync", "GetXferReport",
                               "BatchBufferAsync", "CreateSmmuSpace", "ShareReportRing"};
    switch (kind) {
        case OBJ_ROOT: return cmd < sizeof(root) / sizeof(root[0]) ? root[cmd] : NULL;
        case OBJ_IF:   return cmd < sizeof(ifs) / sizeof(ifs[0]) ? ifs[cmd] : NULL;
        case OBJ_EP:   return cmd < sizeof(ep) / sizeof(ep[0]) ? ep[cmd] : NULL;
    }
    return NULL;
}

const char *cmif_control_name(uint32_t cmd) {
    switch (cmd) {
        case 0: return "ConvertCurrentObjectToDomain";
        case 1: return "CopyFromCurrentDomain";
        case 2: return "CloneCurrentObject";
        case 3: return "QueryPointerBufferSize";
        case 4: return "CloneCurrentObjectEx";
    }
    return NULL;
}

int usbhs_cmd_is_hot(int kind, uint32_t cmd) {
    if (kind == OBJ_IF) return cmd == 4;                           // GetCurrentFrame
    if (kind == OBJ_EP) return cmd == 3 || cmd == 4 || cmd == 5 || cmd == 6;   // ring, post, report, batch
    return 0;
}

static const char *std_req(uint8_t r) {
    static const char *n[] = {"GET_STATUS", "CLEAR_FEATURE", "req2",          "SET_FEATURE",
                              "req4",       "SET_ADDRESS",   "GET_DESCRIPTOR", "SET_DESCRIPTOR",
                              "GET_CONFIGURATION", "SET_CONFIGURATION", "GET_INTERFACE", "SET_INTERFACE",
                              "SYNCH_FRAME"};
    return r < sizeof(n) / sizeof(n[0]) ? n[r] : NULL;
}

static const char *desc_type(uint8_t t) {
    switch (t) {
        case 1: return "DEVICE";
        case 2: return "CONFIG";
        case 3: return "STRING";
        case 4: return "INTERFACE";
        case 5: return "ENDPOINT";
        case 6: return "DEVICE_QUALIFIER";
        case 0x0B: return "IAD";
        case 0x0F: return "BOS";
        case 0x21: return "HID";
        case 0x22: return "HID_REPORT";
        case 0x24: return "CS_INTERFACE";
        case 0x25: return "CS_ENDPOINT";
    }
    return NULL;
}

static const char *uac_req(uint8_t r) {
    switch (r) {
        case 0x01: return "SET_CUR";
        case 0x02: return "SET_MIN";
        case 0x03: return "SET_MAX";
        case 0x04: return "SET_RES";
        case 0x05: return "SET_MEM";
        case 0x81: return "GET_CUR";
        case 0x82: return "GET_MIN";
        case 0x83: return "GET_MAX";
        case 0x84: return "GET_RES";
        case 0x85: return "GET_MEM";
        case 0xFF: return "GET_STAT";
    }
    return NULL;
}

// UAC1 feature unit control selectors (wValue high byte, entity != 0 on an interface recipient).
static const char *uac_fu_sel(uint8_t s) {
    static const char *n[] = {NULL,        "MUTE",     "VOLUME",      "BASS",          "MID",
                              "TREBLE",    "GRAPHIC_EQ", "AUTO_GAIN", "DELAY",         "BASS_BOOST",
                              "LOUDNESS"};
    return s < sizeof(n) / sizeof(n[0]) ? n[s] : NULL;
}

void usb_setup_str(char *out, size_t size, const uint8_t s[8]) {
    uint8_t rt = s[0], req = s[1];
    unsigned wValue = s[2] | (unsigned)s[3] << 8, wIndex = s[4] | (unsigned)s[5] << 8,
             wLength = s[6] | (unsigned)s[7] << 8;
    static const char *types[] = {"std", "class", "vendor", "rsvd"};
    static const char *recips[] = {"dev", "if", "ep", "other"};
    const char *dir = rt & 0x80 ? "IN " : "OUT";
    const char *type = types[(rt >> 5) & 3];
    const char *recip = (rt & 0x1F) < 4 ? recips[rt & 0x1F] : "?";
    int n = snprintf(out, size, "%s %-6s %-3s ", dir, type, recip);
    if (n < 0 || (size_t)n >= size) return;
    char *o = out + n;
    size_t left = size - (size_t)n;

    if (((rt >> 5) & 3) == 0) {
        const char *name = std_req(req);
        if (req == 6 || req == 7) {
            const char *dt = desc_type((uint8_t)(wValue >> 8));
            if (dt) snprintf(o, left, "%s %s idx %u lang/if 0x%x len %u", name, dt, wValue & 0xFF, wIndex, wLength);
            else snprintf(o, left, "%s type 0x%02x idx %u lang/if 0x%x len %u", name, wValue >> 8, wValue & 0xFF, wIndex, wLength);
        } else if (req == 11) {
            snprintf(o, left, "SET_INTERFACE if %u alt %u", wIndex, wValue);
        } else if (name) {
            snprintf(o, left, "%s wValue 0x%04x wIndex 0x%04x len %u", name, wValue, wIndex, wLength);
        } else {
            snprintf(o, left, "req 0x%02x wValue 0x%04x wIndex 0x%04x len %u", req, wValue, wIndex, wLength);
        }
        return;
    }
    if (((rt >> 5) & 3) == 1 && uac_req(req)) {   // audio class (other classes share the codes rarely)
        uint8_t sel = (uint8_t)(wValue >> 8), cn = (uint8_t)wValue;
        if ((rt & 0x1F) == 2) {   // endpoint: sampling frequency control lives here in UAC1
            snprintf(o, left, "UAC %s ep 0x%02x sel 0x%02x%s len %u", uac_req(req), wIndex & 0xFF, sel,
                     sel == 1 ? "(SAMPLING_FREQ)" : sel == 2 ? "(PITCH)" : "", wLength);
        } else {
            uint8_t entity = (uint8_t)(wIndex >> 8), iface = (uint8_t)wIndex;
            const char *fs = uac_fu_sel(sel);
            snprintf(o, left, "UAC %s entity %u if %u sel 0x%02x%s%s%s ch %u len %u", uac_req(req), entity, iface, sel,
                     fs ? "(" : "", fs ? fs : "", fs ? ")" : "", cn, wLength);
        }
        return;
    }
    if (((rt >> 5) & 3) == 1 && (rt & 0x1F) == 1 && req >= 0x01 && req <= 0x0B) {   // HID class
        static const char *hid[] = {NULL, "GET_REPORT", "GET_IDLE", "GET_PROTOCOL", NULL, NULL, NULL, NULL,
                                    NULL, "SET_REPORT", "SET_IDLE", "SET_PROTOCOL"};
        if (hid[req]) {
            snprintf(o, left, "HID %s wValue 0x%04x if %u len %u", hid[req], wValue, wIndex, wLength);
            return;
        }
    }
    snprintf(o, left, "req 0x%02x wValue 0x%04x wIndex 0x%04x len %u", req, wValue, wIndex, wLength);
}

void usb_desc_str(char *out, size_t size, const uint8_t *p, size_t len) {
    if (len < 2) {
        snprintf(out, size, "(short)");
        return;
    }
    uint8_t bl = p[0], t = p[1];
    if (t == 1 && len >= 18)
        snprintf(out, size, "DEVICE usb %x.%02x class %02x/%02x/%02x mps0 %u vid:pid %04x:%04x bcd %04x cfgs %u", p[3], p[2],
                 p[4], p[5], p[6], p[7], p[8] | p[9] << 8, p[10] | p[11] << 8, p[12] | p[13] << 8, p[17]);
    else if (t == 2 && len >= 9)
        snprintf(out, size, "CONFIG total %u ifs %u value %u attr 0x%02x power %u mA", p[2] | p[3] << 8, p[4], p[5], p[7],
                 p[8] * 2);
    else if (t == 4 && len >= 9)
        snprintf(out, size, "INTERFACE if %u alt %u eps %u class %02x/%02x/%02x", p[2], p[3], p[4], p[5], p[6], p[7]);
    else if (t == 5 && len >= 7)
        snprintf(out, size, "ENDPOINT 0x%02x attr 0x%02x mps %u interval %u", p[2], p[3], p[4] | p[5] << 8, p[6]);
    else {
        const char *dt = desc_type(t);
        if (dt) snprintf(out, size, "%s len %u", dt, bl);
        else snprintf(out, size, "type 0x%02x len %u", t, bl);
    }
}
