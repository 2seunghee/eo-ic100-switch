#!/usr/bin/env python3
"""USB / UAC1 descriptor analyzer for the "EO-IC100 on Nintendo Switch" project.

Input: a raw configuration descriptor, either binary (.bin, as written by
pc/dump_usb_descriptors.ps1 or the Switch probe) or hex text (.hex/.txt, '#' comments).

Subcommands:
  show   <cfg>            lsusb -v style tree with UAC1 class-specific descriptors decoded
  check  <cfg>            Switch-compatibility flags (hypotheses to test, not facts)
  diff   <cfgA> <cfgB>    side-by-side profile, e.g. a working earphone vs. the EO-IC100
  json   <cfg>            machine-readable model
  usbhs  <file>           decode raw UsbHsInterface arrays saved by the Switch probe
  hid    <file>           decode + validate a HID report descriptor (*_hid_if<N>.bin)
With no subcommand, `show` + `check` are run on the given file.

No third-party dependencies (Python 3.8+).
"""
import argparse
import json
import struct
import sys
from dataclasses import asdict, dataclass, field
from typing import List, Optional

DT_DEVICE, DT_CONFIG, DT_STRING, DT_INTERFACE, DT_ENDPOINT = 1, 2, 3, 4, 5
DT_IAD, DT_HID, DT_CS_INTERFACE, DT_CS_ENDPOINT = 0x0B, 0x21, 0x24, 0x25

AC_HEADER, AC_IT, AC_OT, AC_MIXER, AC_SELECTOR, AC_FU, AC_PROC, AC_EXT = range(1, 9)
AS_GENERAL, AS_FORMAT_TYPE = 1, 2
EP_GENERAL = 1

TT_USB_STREAMING = 0x0101

TERMINAL_TYPES = {
    0x0100: "USB Undefined", 0x0101: "USB Streaming", 0x01FF: "USB Vendor Specific",
    0x0200: "Input Undefined", 0x0201: "Microphone", 0x0202: "Desktop Microphone",
    0x0203: "Personal Microphone", 0x0204: "Omni-directional Microphone",
    0x0205: "Microphone Array", 0x0206: "Processing Microphone Array",
    0x0300: "Output Undefined", 0x0301: "Speaker", 0x0302: "Headphones",
    0x0303: "Head Mounted Display Audio", 0x0304: "Desktop Speaker", 0x0305: "Room Speaker",
    0x0306: "Communication Speaker", 0x0307: "Low Frequency Effects Speaker",
    0x0400: "Bi-directional Undefined", 0x0401: "Handset", 0x0402: "Headset",
    0x0403: "Speakerphone (no echo reduction)", 0x0404: "Echo-suppressing Speakerphone",
    0x0405: "Echo-canceling Speakerphone",
    0x0500: "Telephony Undefined", 0x0501: "Phone Line", 0x0502: "Telephone", 0x0503: "Down Line Phone",
    0x0600: "External Undefined", 0x0601: "Analog Connector", 0x0602: "Digital Audio Interface",
    0x0603: "Line Connector", 0x0605: "SPDIF Interface",
}
FU_CONTROL_NAMES = ["Mute", "Volume", "Bass", "Mid", "Treble", "Graphic EQ", "AGC",
                    "Delay", "Bass Boost", "Loudness"]
XFER_NAMES = ["Control", "Isochronous", "Bulk", "Interrupt"]
SYNC_NAMES = ["None", "Asynchronous", "Adaptive", "Synchronous"]
USAGE_NAMES = ["Data", "Feedback", "Implicit feedback", "Reserved"]
UNIT_NAMES = {AC_HEADER: "HEADER", AC_IT: "INPUT_TERMINAL", AC_OT: "OUTPUT_TERMINAL",
              AC_MIXER: "MIXER_UNIT", AC_SELECTOR: "SELECTOR_UNIT", AC_FU: "FEATURE_UNIT",
              AC_PROC: "PROCESSING_UNIT", AC_EXT: "EXTENSION_UNIT"}
CLASS_NAMES = {0: "Per-interface", 1: "Audio", 2: "CDC", 3: "HID", 8: "Mass Storage",
               9: "Hub", 0x0E: "Video", 0xFE: "Application", 0xFF: "Vendor"}


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def u24(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def terminal_name(t):
    return TERMINAL_TYPES.get(t, "unknown")


# --------------------------------------------------------------------------- model

@dataclass
class Endpoint:
    address: int
    attributes: int
    max_packet: int
    interval: int
    length: int
    refresh: Optional[int] = None
    synch_address: Optional[int] = None
    cs: Optional[dict] = None  # class-specific EP_GENERAL

    @property
    def number(self):
        return self.address & 0x0F

    @property
    def is_in(self):
        return bool(self.address & 0x80)

    @property
    def xfer_type(self):
        return self.attributes & 3

    @property
    def sync_type(self):
        return (self.attributes >> 2) & 3

    @property
    def usage_type(self):
        return (self.attributes >> 4) & 3

    @property
    def packet_bytes(self):
        return self.max_packet & 0x7FF

    @property
    def has_rate_control(self):
        return bool(self.cs and self.cs["attributes"] & 0x01)

    def label(self):
        return "EP 0x%02x %s" % (self.address, "IN" if self.is_in else "OUT")


@dataclass
class AltSetting:
    number: int
    alt: int
    num_eps: int
    cls: int
    subcls: int
    proto: int
    iface_str: int
    endpoints: List[Endpoint] = field(default_factory=list)
    as_general: Optional[dict] = None
    fmt: Optional[dict] = None
    hid: Optional[dict] = None
    other: List[str] = field(default_factory=list)

    @property
    def is_audio_streaming(self):
        return self.cls == 1 and self.subcls == 2

    def iso_data_ep(self):
        for ep in self.endpoints:
            if ep.xfer_type == 1 and ep.usage_type == 0:
                return ep
        return None

    def rates(self):
        if not self.fmt:
            return []
        return self.fmt.get("rates") or []

    def supports(self, rate, bits, channels):
        f = self.fmt
        if not f or f.get("format_type") != 1 or (self.as_general or {}).get("format_tag") != 1:
            return False
        if f["bits"] != bits or f["channels"] != channels:
            return False
        if f.get("rate_range"):
            lo, hi = f["rate_range"]
            return lo <= rate <= hi
        return rate in f["rates"]


@dataclass
class Unit:
    subtype: int
    id: int
    terminal_type: Optional[int] = None
    assoc_terminal: Optional[int] = None
    sources: List[int] = field(default_factory=list)
    nr_channels: Optional[int] = None
    channel_config: Optional[int] = None
    control_size: Optional[int] = None
    controls: List[int] = field(default_factory=list)  # FU: [master, ch1, ch2, ...]
    string_index: Optional[int] = None


@dataclass
class Config:
    total_length: int
    num_interfaces: int
    value: int
    config_str: int
    attributes: int
    max_power_ma: int
    raw_len: int = 0
    ac_iface: Optional[int] = None
    bcd_adc: Optional[int] = None
    ac_total_length: Optional[int] = None
    collection: List[int] = field(default_factory=list)
    units: List[Unit] = field(default_factory=list)
    alts: List[AltSetting] = field(default_factory=list)
    iads: List[dict] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)

    def unit(self, uid):
        for u in self.units:
            if u.id == uid:
                return u
        return None

    def interface_numbers(self):
        seen = []
        for a in self.alts:
            if a.number not in seen:
                seen.append(a.number)
        return seen

    def streaming(self, playback):
        """(alt, endpoint) pairs of AS alt settings with an isochronous data EP in the given direction."""
        out = []
        for a in self.alts:
            if not a.is_audio_streaming:
                continue
            ep = a.iso_data_ep()
            if ep is not None and ep.is_in != playback:
                out.append((a, ep))
        return out


# --------------------------------------------------------------------------- parsing

def parse_hex_text(text):
    out = bytearray()
    for line in text.splitlines():
        line = line.split("#", 1)[0]
        for tok in line.replace(",", " ").split():
            if tok.lower().startswith("0x"):
                tok = tok[2:]
            if len(tok) % 2:
                raise ValueError("odd-length hex token %r" % tok)
            out += bytes.fromhex(tok)
    return bytes(out)


def load_bytes(path, force_hex=False):
    with open(path, "rb") as f:
        data = f.read()
    if force_hex or path.lower().endswith((".hex", ".txt")):
        return parse_hex_text(data.decode("utf-8", "replace"))
    return data


def _parse_ac(cfg, alt, d):
    st = d[2]
    if st == AC_HEADER and len(d) >= 8:
        cfg.ac_iface = alt.number
        cfg.bcd_adc = u16(d, 3)
        if cfg.bcd_adc >= 0x0200:
            cfg.warnings.append("AudioControl header bcdADC=%x.%02x (UAC2+): unit layouts not decoded"
                                % (cfg.bcd_adc >> 8, cfg.bcd_adc & 0xFF))
            return
        cfg.ac_total_length = u16(d, 5)
        n = d[7]
        cfg.collection = list(d[8:8 + n])
        return
    if cfg.bcd_adc is not None and cfg.bcd_adc >= 0x0200:
        alt.other.append("CS_INTERFACE subtype %d (UAC2, not decoded): %s" % (st, d.hex(" ")))
        return
    if len(d) < 4:
        alt.other.append("short AC descriptor: " + d.hex(" "))
        return
    u = Unit(subtype=st, id=d[3])
    if st == AC_IT and len(d) >= 12:
        u.terminal_type, u.assoc_terminal = u16(d, 4), d[6]
        u.nr_channels, u.channel_config, u.string_index = d[7], u16(d, 8), d[11]
    elif st == AC_OT and len(d) >= 9:
        u.terminal_type, u.assoc_terminal, u.sources, u.string_index = u16(d, 4), d[6], [d[7]], d[8]
    elif st in (AC_MIXER, AC_SELECTOR) and len(d) >= 5:
        p = d[4]
        u.sources = list(d[5:5 + p])
    elif st == AC_FU and len(d) >= 7:
        n = d[5]
        u.sources, u.control_size = [d[4]], n
        if n:
            body = d[6:len(d) - 1]
            for i in range(0, len(body) - n + 1, n):
                u.controls.append(int.from_bytes(body[i:i + n], "little"))
        u.string_index = d[-1]
    elif st in (AC_PROC, AC_EXT) and len(d) >= 7:
        p = d[6]
        u.sources = list(d[7:7 + p])
    cfg.units.append(u)


def _parse_as(alt, d):
    st = d[2]
    if st == AS_GENERAL and len(d) >= 7:
        alt.as_general = {"terminal_link": d[3], "delay": d[4], "format_tag": u16(d, 5)}
    elif st == AS_FORMAT_TYPE and len(d) >= 8:
        ft = d[3]
        if ft in (1, 3):
            f = {"format_type": ft, "channels": d[4], "subframe": d[5], "bits": d[6], "rates": []}
            n = d[7]
            if n == 0 and len(d) >= 14:
                f["rate_range"] = (u24(d, 8), u24(d, 11))
            else:
                f["rates"] = [u24(d, 8 + 3 * i) for i in range(n) if 8 + 3 * i + 3 <= len(d)]
            alt.fmt = f
        else:
            alt.fmt = {"format_type": ft, "raw": d.hex(" ")}
    else:
        alt.other.append("AS CS_INTERFACE subtype %d: %s" % (st, d.hex(" ")))


def parse_config(data):
    if len(data) < 9 or data[1] != DT_CONFIG:
        raise ValueError("not a configuration descriptor (need >= 9 bytes starting with 09 02)")
    total = u16(data, 2)
    cfg = Config(total, data[4], data[5], data[6], data[7], data[8] * 2, raw_len=len(data))
    if total != len(data):
        cfg.warnings.append("wTotalLength=%d but %d bytes supplied" % (total, len(data)))
    end = min(total, len(data))
    pos = data[0]
    cur_alt, cur_ep = None, None
    while pos + 2 <= end:
        blen, btype = data[pos], data[pos + 1]
        if blen < 2:
            cfg.warnings.append("descriptor with bLength=%d at offset %d; stopping" % (blen, pos))
            break
        if pos + blen > end:
            cfg.warnings.append("descriptor at offset %d overruns the buffer; stopping" % pos)
            break
        d = data[pos:pos + blen]
        if btype == DT_INTERFACE and blen >= 9:
            cur_alt = AltSetting(d[2], d[3], d[4], d[5], d[6], d[7], d[8])
            cfg.alts.append(cur_alt)
            cur_ep = None
        elif btype == DT_ENDPOINT and blen >= 7:
            cur_ep = Endpoint(d[2], d[3], u16(d, 4), d[6], blen,
                              d[7] if blen >= 9 else None, d[8] if blen >= 9 else None)
            if cur_alt is None:
                cfg.warnings.append("endpoint before any interface at offset %d" % pos)
            else:
                cur_alt.endpoints.append(cur_ep)
        elif btype == DT_CS_INTERFACE and cur_alt is not None and cur_alt.cls == 1 and cur_alt.subcls in (1, 2):
            if cur_alt.subcls == 1:
                _parse_ac(cfg, cur_alt, d)
            else:
                _parse_as(cur_alt, d)
        elif btype == DT_CS_ENDPOINT and cur_ep is not None and blen >= 7 and d[2] == EP_GENERAL:
            cur_ep.cs = {"attributes": d[3], "lock_delay_units": d[4], "lock_delay": u16(d, 5)}
        elif btype == DT_HID and cur_alt is not None and blen >= 9:
            cur_alt.hid = {"bcd_hid": u16(d, 2), "country": d[4], "num_desc": d[5],
                           "report_type": d[6], "report_length": u16(d, 7)}
        elif btype == DT_IAD and blen >= 8:
            cfg.iads.append({"first": d[2], "count": d[3], "cls": d[4], "subcls": d[5], "proto": d[6]})
        else:
            target = cur_alt.other if cur_alt is not None else cfg.warnings
            target.append("descriptor type 0x%02x len %d: %s" % (btype, blen, d.hex(" ")))
        pos += blen
    return cfg


def parse_device(data):
    if len(data) < 18 or data[1] != DT_DEVICE:
        raise ValueError("not a device descriptor")
    return {"bcdUSB": u16(data, 2), "class": data[4], "subclass": data[5], "protocol": data[6],
            "max_packet0": data[7], "vid": u16(data, 8), "pid": u16(data, 10), "bcdDevice": u16(data, 12),
            "iManufacturer": data[14], "iProduct": data[15], "iSerial": data[16], "num_configs": data[17]}


# --------------------------------------------------------------------------- analysis helpers

def walk_to_source(cfg, start_unit, target_id, limit=12):
    """Follow first-source links back from start_unit; return the chain if target_id is reached."""
    chain, node = [], start_unit
    for _ in range(limit):
        if node is None:
            return None
        chain.append(node)
        if node.id == target_id:
            return chain
        if not node.sources:
            return None
        node = cfg.unit(node.sources[0])
    return None


def playback_path(cfg, link_id):
    """Physical output terminal(s) and feature units fed by the USB-streaming input terminal link_id."""
    paths = []
    for u in cfg.units:
        if u.subtype == AC_OT and u.terminal_type != TT_USB_STREAMING:
            chain = walk_to_source(cfg, u, link_id)
            if chain:
                paths.append({"ot": u, "fus": [n for n in chain if n.subtype == AC_FU]})
    return paths


def capture_path(cfg, link_id):
    """Physical input terminal and feature units feeding the USB-streaming output terminal link_id."""
    ot = cfg.unit(link_id)
    if ot is None or ot.subtype != AC_OT:
        return None
    chain, node = [], ot
    for _ in range(12):
        chain.append(node)
        if node.subtype == AC_IT:
            return {"it": node, "fus": [n for n in chain if n.subtype == AC_FU]}
        if not node.sources:
            return None
        node = cfg.unit(node.sources[0])
        if node is None:
            return None
    return None


def bytes_per_ms(rate, channels, subframe):
    return rate * channels * subframe // 1000


def find_playback_choice(cfg, rate=48000, bits=16, channels=2):
    for a, ep in cfg.streaming(playback=True):
        if a.supports(rate, bits, channels):
            return a, ep
    return None


def check(cfg):
    """Return a list of (level, code, message). Levels: RISK > WARN > INFO > OK."""
    out = []

    def add(level, code, msg):
        out.append((level, code, msg))

    if cfg.ac_iface is None:
        add("RISK", "NO_AUDIO_CONTROL", "no AudioControl interface found: not a USB Audio Class device")
        return out
    if cfg.bcd_adc is not None and cfg.bcd_adc >= 0x0200:
        add("RISK", "UAC2", "bcdADC %x.%02x: UAC2. The Switch is reported to support UAC1 only."
            % (cfg.bcd_adc >> 8, cfg.bcd_adc & 0xFF))
    else:
        add("OK", "UAC1", "bcdADC 1.00 (UAC1)")

    play = cfg.streaming(playback=True)
    cap = cfg.streaming(playback=False)
    if not play:
        add("RISK", "NO_PLAYBACK", "no AudioStreaming alt setting with an isochronous OUT endpoint")
        return out

    choice = find_playback_choice(cfg)
    if choice is None:
        add("RISK", "NO_48K_16_2CH", "no playback alt setting offers PCM 48000 Hz / 16-bit / 2ch")
    else:
        a, ep = choice
        need = bytes_per_ms(48000, 2, a.fmt["subframe"])
        add("OK", "PLAYBACK_48K", "playback IF%d alt%d %s: PCM 48000 Hz / 16-bit / 2ch, %d bytes per 1 ms frame "
            "(wMaxPacketSize %d)" % (a.number, a.alt, ep.label(), need, ep.packet_bytes))
        if len(a.rates()) > 1 or a.fmt.get("rate_range"):
            if ep.has_rate_control:
                add("WARN", "MULTI_RATE", "H3: playback alt%d offers %s Hz; the host must send SET_CUR "
                    "SAMPLING_FREQ (48000) to %s, or the device may run at another rate"
                    % (a.alt, "/".join(str(r) for r in a.rates()) or "a range", ep.label()))
            else:
                add("WARN", "MULTI_RATE_NO_CTRL", "playback alt offers several rates but the endpoint has no "
                    "sampling-frequency control")
        if ep.packet_bytes > need + 2 * a.fmt["subframe"] * 2:
            add("INFO", "MAXPACKET_OVERSIZE", "wMaxPacketSize %d is sized for the highest rate; at 48 kHz the "
                "host must send %d-byte packets (a driver that sizes packets from wMaxPacketSize would play "
                "at the wrong speed)" % (ep.packet_bytes, need))
        sync = ep.sync_type
        if sync == 1:
            fb = [e for e in a.endpoints if e.usage_type == 1]
            if fb:
                add("WARN", "SYNC_ASYNC", "H4: playback EP is asynchronous with feedback %s; the host must "
                    "implement feedback" % fb[0].label())
            else:
                add("RISK", "SYNC_ASYNC_NO_FEEDBACK", "playback EP is asynchronous without a feedback endpoint")
        elif sync == 3:
            add("INFO", "SYNC_SYNCHRONOUS", "H4: playback EP sync type is Synchronous (cheap dongles usually "
                "report Adaptive). Unknown whether the Switch driver accepts it.")
        elif sync == 2:
            add("OK", "SYNC_ADAPTIVE", "playback EP sync type is Adaptive")
        else:
            add("INFO", "SYNC_NONE", "playback EP sync type is None")

        link = (a.as_general or {}).get("terminal_link")
        paths = playback_path(cfg, link) if link is not None else []
        if not paths:
            add("WARN", "NO_OUTPUT_PATH", "could not trace the playback input terminal (id %s) to a physical "
                "output terminal" % link)
        for p in paths:
            t = p["ot"].terminal_type
            if (t >> 8) != 0x03:
                add("WARN", "OT_NOT_OUTPUT_TYPE", "H2: playback ends at output terminal id%d of type 0x%04x (%s), "
                    "which is not an Output Terminal Type (0x03xx such as 0x0301 Speaker / 0x0302 Headphones)"
                    % (p["ot"].id, t, terminal_name(t)))
            else:
                add("OK", "OT_TYPE", "playback output terminal type 0x%04x (%s)" % (t, terminal_name(t)))
            for fu in p["fus"]:
                names = [FU_CONTROL_NAMES[i] for i in range(len(FU_CONTROL_NAMES))
                         if fu.controls and fu.controls[0] & (1 << i)]
                add("INFO", "PLAYBACK_FU", "playback feature unit id%d master controls: %s"
                    % (fu.id, ", ".join(names) or "none"))

    # Interface order hypothesis (H1).
    as_numbers = []
    for a in cfg.alts:
        if a.is_audio_streaming and a.number not in as_numbers:
            as_numbers.append(a.number)
    direction = {}
    for a, ep in play:
        direction.setdefault(a.number, "playback")
    for a, ep in cap:
        direction.setdefault(a.number, "capture")
    order = ["IF%d=%s" % (n, direction.get(n, "?")) for n in as_numbers]
    if as_numbers and direction.get(as_numbers[0]) == "capture":
        add("WARN", "CAPTURE_FIRST", "H1: the first AudioStreaming interface is capture (%s). A driver that "
            "assumes the first AS interface is playback would fail." % ", ".join(order))
    elif order:
        add("OK", "PLAYBACK_FIRST", "AudioStreaming order: " + ", ".join(order))
    if cfg.collection and as_numbers and cfg.collection != sorted(cfg.collection):
        add("INFO", "COLLECTION_ORDER", "AC header baInterfaceNr is not sorted: %s" % cfg.collection)

    # Endpoint numbers shared between directions (H5).
    nums = {}
    for a in cfg.alts:
        for ep in a.endpoints:
            nums.setdefault(ep.number, set()).add("IN" if ep.is_in else "OUT")
    shared = sorted(n for n, dirs in nums.items() if len(dirs) == 2)
    if shared:
        add("INFO", "SHARED_EP_NUMBER", "H5: endpoint number(s) %s are used for both IN and OUT "
            "(legal, but a driver keyed on the endpoint number alone would collide)"
            % ", ".join(str(n) for n in shared))

    # Capture channels (H6).
    for a, ep in cap:
        if a.fmt and a.fmt.get("channels", 1) > 1:
            add("INFO", "STEREO_MIC", "H6: capture IF%d alt%d has %d channels (headset mics are usually mono)"
                % (a.number, a.alt, a.fmt["channels"]))
            break

    # Other interfaces (H7).
    extra = sorted({a.number for a in cfg.alts if a.cls != 1})
    if extra:
        kinds = sorted({CLASS_NAMES.get(a.cls, "0x%02x" % a.cls) for a in cfg.alts if a.cls != 1})
        add("INFO", "EXTRA_INTERFACES", "H7: non-audio interface(s) %s (%s). The Switch hid sysmodule watches "
            "every HID-class interface." % (", ".join("IF%d" % n for n in extra), ", ".join(kinds)))
    if cfg.max_power_ma > 500:
        add("WARN", "HIGH_POWER", "bMaxPower %d mA exceeds 500 mA" % cfg.max_power_ma)
    for w in cfg.warnings:
        add("WARN", "PARSE", w)
    return out


def profile(cfg):
    """Flat key -> value summary used by `diff`."""
    p = {"interfaces": str(cfg.num_interfaces),
         "bcdADC": ("%x.%02x" % (cfg.bcd_adc >> 8, cfg.bcd_adc & 0xFF)) if cfg.bcd_adc is not None else "-",
         "AC collection": str(cfg.collection), "MaxPower": "%d mA" % cfg.max_power_ma}
    as_numbers = []
    for a in cfg.alts:
        if a.is_audio_streaming and a.number not in as_numbers:
            as_numbers.append(a.number)
    play_ifs = {a.number for a, _ in cfg.streaming(True)}
    p["AS order"] = ", ".join("%s" % ("play" if n in play_ifs else "cap") for n in as_numbers)
    for name, is_play in (("play", True), ("cap", False)):
        for idx, (a, ep) in enumerate(cfg.streaming(is_play)):
            f = a.fmt or {}
            rates = "/".join(str(r) for r in f.get("rates", [])) or str(f.get("rate_range", "-"))
            p["%s[%d] format" % (name, idx)] = "%sch %sbit %s" % (f.get("channels"), f.get("bits"), rates)
            p["%s[%d] endpoint" % (name, idx)] = "0x%02x %s maxpkt %d interval %d rate-ctrl %s" % (
                ep.address, SYNC_NAMES[ep.sync_type], ep.packet_bytes, ep.interval,
                "yes" if ep.has_rate_control else "no")
    choice = find_playback_choice(cfg)
    if choice:
        link = (choice[0].as_general or {}).get("terminal_link")
        paths = playback_path(cfg, link) if link is not None else []
        p["play output terminal"] = ", ".join("0x%04x %s" % (x["ot"].terminal_type, terminal_name(x["ot"].terminal_type))
                                              for x in paths) or "-"
        p["play path"] = ", ".join(" > ".join(["IT%d" % link] + ["FU%d" % f.id for f in reversed(x["fus"])] +
                                              ["OT%d" % x["ot"].id]) for x in paths) or "-"
    cap = cfg.streaming(False)
    if cap:
        link = (cap[0][0].as_general or {}).get("terminal_link")
        cp = capture_path(cfg, link) if link is not None else None
        if cp:
            p["cap input terminal"] = "0x%04x %s" % (cp["it"].terminal_type, terminal_name(cp["it"].terminal_type))
            p["cap path"] = " > ".join(["IT%d" % cp["it"].id] + ["FU%d" % f.id for f in reversed(cp["fus"])] +
                                       ["OT%d" % link])
    p["non-audio interfaces"] = ", ".join("IF%d %s" % (a.number, CLASS_NAMES.get(a.cls, hex(a.cls)))
                                          for a in cfg.alts if a.cls != 1 and a.alt == 0) or "-"
    for a in cfg.alts:
        if a.hid and a.alt == 0:
            ep = a.endpoints[0] if a.endpoints else None
            p["hid IF%d" % a.number] = "report %d B" % a.hid["report_length"] + (
                ", EP 0x%02x %d B every %d ms" % (ep.address, ep.packet_bytes, ep.interval) if ep else "")
    return p


# --------------------------------------------------------------------------- printing

def show(cfg, w=print):
    w("Configuration: wTotalLength %d (%d bytes read), %d interfaces, bConfigurationValue %d, "
      "bmAttributes 0x%02x, MaxPower %d mA" % (cfg.total_length, cfg.raw_len, cfg.num_interfaces, cfg.value,
                                             cfg.attributes, cfg.max_power_ma))
    for iad in cfg.iads:
        w("  Interface Association: first IF%d count %d class %d/%d/%d"
          % (iad["first"], iad["count"], iad["cls"], iad["subcls"], iad["proto"]))
    for a in cfg.alts:
        kind = CLASS_NAMES.get(a.cls, "0x%02x" % a.cls)
        if a.cls == 1:
            kind += {1: " Control", 2: " Streaming", 3: " MIDI Streaming"}.get(a.subcls, "")
        w("  IF%d alt%d  class %d/%d/%d %-18s %d endpoint(s)%s"
          % (a.number, a.alt, a.cls, a.subcls, a.proto, kind, a.num_eps,
             "  iInterface %d" % a.iface_str if a.iface_str else ""))
        if a.cls == 1 and a.subcls == 1 and a.number == cfg.ac_iface:
            if cfg.bcd_adc is not None:
                w("      AC HEADER bcdADC %x.%02x wTotalLength %s baInterfaceNr %s"
                  % (cfg.bcd_adc >> 8, cfg.bcd_adc & 0xFF, cfg.ac_total_length, cfg.collection))
            for u in cfg.units:
                name = UNIT_NAMES.get(u.subtype, "subtype %d" % u.subtype)
                if u.subtype in (AC_IT, AC_OT):
                    extra = " type 0x%04x (%s)" % (u.terminal_type, terminal_name(u.terminal_type))
                    if u.subtype == AC_IT:
                        extra += " %dch config 0x%04x" % (u.nr_channels, u.channel_config)
                    else:
                        extra += " source %d" % u.sources[0]
                elif u.subtype == AC_FU:
                    parts = []
                    for i, c in enumerate(u.controls):
                        names = [FU_CONTROL_NAMES[b] for b in range(len(FU_CONTROL_NAMES)) if c & (1 << b)]
                        parts.append("%s:%s" % ("master" if i == 0 else "ch%d" % i, "+".join(names) or "-"))
                    extra = " source %d controls %s" % (u.sources[0], " ".join(parts))
                else:
                    extra = " sources %s" % u.sources
                if u.string_index:
                    extra += " iString %d" % u.string_index
                w("      %-15s id%-2d%s" % (name, u.id, extra))
        if a.as_general:
            g = a.as_general
            w("      AS_GENERAL bTerminalLink %d bDelay %d wFormatTag 0x%04x%s"
              % (g["terminal_link"], g["delay"], g["format_tag"], " (PCM)" if g["format_tag"] == 1 else ""))
        if a.fmt:
            f = a.fmt
            if "raw" in f:
                w("      FORMAT_TYPE %d (not decoded) %s" % (f["format_type"], f["raw"]))
            else:
                rates = ("continuous %d..%d Hz" % f["rate_range"]) if f.get("rate_range") else \
                    ", ".join("%d" % r for r in f["rates"]) + " Hz"
                w("      FORMAT_TYPE_%s %dch subframe %dB %d-bit, %s"
                  % ("I" if f["format_type"] == 1 else "III", f["channels"], f["subframe"], f["bits"], rates))
        if a.hid:
            h = a.hid
            w("      HID bcdHID %x.%02x report descriptor %d bytes"
              % (h["bcd_hid"] >> 8, h["bcd_hid"] & 0xFF, h["report_length"]))
        for ep in a.endpoints:
            line = "      %s %s" % (ep.label(), XFER_NAMES[ep.xfer_type])
            if ep.xfer_type == 1:
                line += " %s/%s" % (SYNC_NAMES[ep.sync_type], USAGE_NAMES[ep.usage_type])
            line += " wMaxPacketSize %d bInterval %d" % (ep.packet_bytes, ep.interval)
            if ep.synch_address:
                line += " bSynchAddress 0x%02x" % ep.synch_address
            if ep.cs:
                line += " [EP_GENERAL%s]" % (" sampling-freq-ctrl" if ep.cs["attributes"] & 1 else "")
            w(line)
        for o in a.other:
            w("      (other) " + o)


def print_check(cfg, w=print):
    order = {"RISK": 0, "WARN": 1, "INFO": 2, "OK": 3}
    results = check(cfg)
    w("Switch compatibility check (H1..H7 are hypotheses to test, not confirmed causes):")
    for level, code, msg in sorted(results, key=lambda r: order[r[0]]):
        w("  [%-4s] %-22s %s" % (level, code, msg))
    return results


def print_diff(pa, pb, name_a, name_b, w=print):
    keys = list(pa.keys()) + [k for k in pb.keys() if k not in pa]
    wa = max([len(name_a)] + [len(str(pa.get(k, "-"))) for k in keys])
    w("%-22s | %-*s | %s" % ("property", wa, name_a, name_b))
    w("-" * (25 + wa + 3 + max(len(name_b), 20)))
    ndiff = 0
    for k in keys:
        va, vb = pa.get(k, "-"), pb.get(k, "-")
        mark = "  " if va == vb else "<>"
        ndiff += va != vb
        w("%-22s | %-*s | %s %s" % (k, wa, va, vb, mark))
    w("%d differing propert%s" % (ndiff, "y" if ndiff == 1 else "ies"))
    return ndiff


# --------------------------------------------------------------------------- HID report descriptors

HID_PAGES = {0x01: "Generic Desktop", 0x07: "Keyboard", 0x08: "LED", 0x09: "Button", 0x0B: "Telephony",
             0x0C: "Consumer"}
HID_USAGES = {
    (0x0C, 0x01): "Consumer Control", (0x0C, 0xB5): "Scan Next Track", (0x0C, 0xB6): "Scan Previous Track",
    (0x0C, 0xB7): "Stop", (0x0C, 0xCD): "Play/Pause", (0x0C, 0xCF): "Voice Command", (0x0C, 0xE2): "Mute",
    (0x0C, 0xE9): "Volume Increment", (0x0C, 0xEA): "Volume Decrement",
    (0x01, 0x01): "Pointer", (0x01, 0x02): "Mouse", (0x01, 0x05): "Game Pad", (0x01, 0x06): "Keyboard",
    (0x01, 0x80): "System Control",
}
HID_MAIN = {0x8: "Input", 0x9: "Output", 0xB: "Feature", 0xA: "Collection", 0xC: "End Collection"}
HID_GLOBAL = {0x0: "Usage Page", 0x1: "Logical Minimum", 0x2: "Logical Maximum", 0x3: "Physical Minimum",
              0x4: "Physical Maximum", 0x5: "Unit Exponent", 0x6: "Unit", 0x7: "Report Size", 0x8: "Report ID",
              0x9: "Report Count", 0xA: "Push", 0xB: "Pop"}
HID_LOCAL = {0x0: "Usage", 0x1: "Usage Minimum", 0x2: "Usage Maximum", 0x7: "String Index"}
HID_COLLECTIONS = {0: "Physical", 1: "Application", 2: "Logical", 3: "Report", 4: "Named Array",
                   5: "Usage Switch", 6: "Usage Modifier"}


def hid_page_name(page):
    if page >= 0xFF00:
        return "Vendor 0x%04x" % page
    if 0xF1D1 <= page < 0xFF00:
        return "Reserved 0x%04x (vendor pages start at 0xFF00)" % page
    return HID_PAGES.get(page, "page 0x%02x" % page)


def hid_usage_name(page, usage):
    return HID_USAGES.get((page, usage), "0x%02x" % usage)


def parse_hid_report(data):
    """Decode a HID report descriptor (short items, HID 1.11).

    Returns (lines, reports, issues): `reports` maps report ID -> {"input": [(bit offset, size, count,
    usage names, flags)], "input_bits": n}; `issues` lists spec violations such as Logical Maximum < Minimum.
    """
    lines, issues, reports = [], [], {}
    g = {"page": 0, "lmin": 0, "lmax": 0, "size": None, "count": None, "id": 0}
    stack, usages, depth, pos = [], [], 0, 0
    while pos < len(data):
        prefix = data[pos]
        if prefix == 0xFE:  # long item
            n = data[pos + 1] if pos + 1 < len(data) else 0
            lines.append("%s(long item, %d bytes)" % ("  " * depth, n))
            pos += 3 + n
            continue
        size = (0, 1, 2, 4)[prefix & 3]
        kind, tag = (prefix >> 2) & 3, prefix >> 4
        raw = data[pos + 1:pos + 1 + size]
        if len(raw) < size:
            issues.append("TRUNCATED: item at offset %d needs %d data bytes" % (pos, size))
            break
        uval = int.from_bytes(raw, "little") if size else 0
        sval = int.from_bytes(raw, "little", signed=True) if size else 0
        hexs = " ".join("%02x" % b for b in data[pos:pos + 1 + size])
        if kind == 0:  # main
            name = HID_MAIN.get(tag, "Main 0x%x" % tag)
            if tag == 0xA:
                desc = "%s (%s)" % (name, HID_COLLECTIONS.get(uval, "0x%02x" % uval))
                lines.append("%-14s %s%s" % (hexs, "  " * depth, desc))
                depth += 1
                usages = []
            elif tag == 0xC:
                depth -= 1
                if depth < 0:
                    issues.append("UNBALANCED_COLLECTION: End Collection without Collection at offset %d" % pos)
                    depth = 0
                lines.append("%-14s %s%s" % (hexs, "  " * depth, name))
            else:
                flags = ("Const" if uval & 1 else "Data", "Var" if uval & 2 else "Array", "Rel" if uval & 4 else "Abs")
                lines.append("%-14s %s%s (%s)" % (hexs, "  " * depth, name, ",".join(flags)))
                if g["size"] is None or g["count"] is None:
                    issues.append("MISSING_SIZE_OR_COUNT: %s at offset %d without Report Size/Count" % (name, pos))
                elif not uval & 1 and g["lmax"] < g["lmin"]:
                    issues.append("LOGICAL_MAX_LT_MIN: %s at offset %d uses Logical Minimum %d > Logical Maximum %d "
                                  "(HID parsers such as Windows hidparse reject this)" % (name, pos, g["lmin"], g["lmax"]))
                if tag == 0x8 and g["size"] is not None and g["count"] is not None:
                    rep = reports.setdefault(g["id"], {"input": [], "input_bits": 0})
                    rep["input"].append((rep["input_bits"], g["size"], g["count"],
                                         [hid_usage_name(g["page"], u) for u in usages] if not uval & 1 else [],
                                         ",".join(flags)))
                    rep["input_bits"] += g["size"] * g["count"]
                usages = []
        elif kind == 1:  # global
            name = HID_GLOBAL.get(tag, "Global 0x%x" % tag)
            if tag == 0x0:
                g["page"] = uval
                val = hid_page_name(uval)
            elif tag in (0x1, 0x2, 0x3, 0x4):
                val = str(sval)
                if tag == 0x1:
                    g["lmin"] = sval
                elif tag == 0x2:
                    g["lmax"] = sval
            elif tag == 0x7:
                g["size"] = uval
                val = str(uval)
            elif tag == 0x8:
                g["id"] = uval
                val = str(uval)
                if uval == 0:
                    issues.append("REPORT_ID_ZERO at offset %d" % pos)
            elif tag == 0x9:
                g["count"] = uval
                val = str(uval)
            elif tag == 0xA:
                stack.append(dict(g))
                val = ""
            elif tag == 0xB:
                if stack:
                    g = stack.pop()
                val = ""
            else:
                val = str(uval)
            lines.append("%-14s %s%s %s" % (hexs, "  " * depth, name, val))
        elif kind == 2:  # local
            name = HID_LOCAL.get(tag, "Local 0x%x" % tag)
            if tag == 0x0:
                usages.append(uval)
                val = hid_usage_name(g["page"], uval)
            else:
                val = "0x%x" % uval
            lines.append("%-14s %s%s (%s)" % (hexs, "  " * depth, name, val))
        else:
            lines.append("%-14s %s(reserved item)" % (hexs, "  " * depth))
        pos += 1 + size
    if depth != 0:
        issues.append("UNBALANCED_COLLECTION: %d collection(s) not closed" % depth)
    return lines, reports, issues


def print_hid(data, w=print):
    lines, reports, issues = parse_hid_report(data)
    w("HID report descriptor, %d bytes:" % len(data))
    for l in lines:
        w("  " + l)
    for rid in sorted(reports):
        r = reports[rid]
        nbytes = (r["input_bits"] + 7) // 8 + (1 if rid else 0)
        w("Input report ID %d: %d bits -> %d bytes on the wire%s" % (rid, r["input_bits"], nbytes,
                                                                   " (incl. report ID)" if rid else ""))
        for off, size, count, names, flags in r["input"]:
            if names:
                for i, n in enumerate(names):
                    w("  bit %-3d size %d  %s" % (off + i * size, size, n))
                if count > len(names):
                    w("  bit %-3d size %d  (x%d more, last usage repeats)" % (off + len(names) * size, size,
                                                                          count - len(names)))
            else:
                w("  bit %-3d size %d x%d  %s" % (off, size, count, "padding" if "Const" in flags else flags))
    w("issues: " + ("; ".join(issues) if issues else "none"))
    return issues


# --------------------------------------------------------------------------- Switch probe raw dumps

USBHS_IFACE_SIZE = 0x228


def decode_usbhs(data, w=print):
    """Decode an array of libnx UsbHsInterface (0x228 bytes each, libnx's 8.0.0+ layout)."""
    if len(data) % USBHS_IFACE_SIZE:
        w("warning: %d bytes is not a multiple of 0x%x" % (len(data), USBHS_IFACE_SIZE))
    for i in range(len(data) // USBHS_IFACE_SIZE):
        r = data[i * USBHS_IFACE_SIZE:(i + 1) * USBHS_IFACE_SIZE]
        iid = struct.unpack_from("<i", r, 0)[0]
        intf = r[0xC:0xC + 9]
        path = r[0x1B8:0x1F8].split(b"\0", 1)[0].decode("ascii", "replace")
        bus, dev = struct.unpack_from("<II", r, 0x1F8)
        dd = parse_device(r[0x200:0x212]) if r[0x201] == DT_DEVICE else None
        w("[%d] ID %d bus %d dev %d path %s" % (i, iid, bus, dev, path))
        if dd:
            w("     device %04x:%04x bcdUSB %x.%02x" % (dd["vid"], dd["pid"], dd["bcdUSB"] >> 8, dd["bcdUSB"] & 0xFF))
        w("     IF%d alt%d class %d/%d/%d, %d endpoint(s)" % (intf[2], intf[3], intf[5], intf[6], intf[7], intf[4]))
        for base, direction in ((0x1C, "IN"), (0x8C, "OUT")):
            for k in range(15):
                e = r[base + 7 * k:base + 7 * k + 7]
                if e[0]:
                    w("       %-3s slot %2d: addr 0x%02x attr 0x%02x maxpkt %d interval %d"
                      % (direction, k, e[2], e[3], u16(e, 4) & 0x7FF, e[6]))


# --------------------------------------------------------------------------- CLI

def _load_cfg(path, force_hex):
    return parse_config(load_bytes(path, force_hex))


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    commands = ("all", "show", "check", "json", "diff", "usbhs", "hid")
    positional = [a for a in argv if not a.startswith("-")]
    if positional and positional[0] not in commands:
        argv.insert(argv.index(positional[0]), "all")  # bare file name: show + check

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hex", action="store_true", help="treat input files as hex text regardless of extension")
    sub = ap.add_subparsers(dest="cmd")
    for name in ("all", "show", "check", "json"):
        sp = sub.add_parser(name)
        sp.add_argument("file")
    sp = sub.add_parser("diff")
    sp.add_argument("a")
    sp.add_argument("b")
    sp = sub.add_parser("usbhs")
    sp.add_argument("file")
    sp = sub.add_parser("hid")
    sp.add_argument("file")
    args = ap.parse_args(argv)

    if args.cmd is None:
        ap.print_help()
        return 2
    if args.cmd == "all":
        cfg = _load_cfg(args.file, args.hex)
        show(cfg)
        print()
        res = print_check(cfg)
        return 1 if any(r[0] == "RISK" for r in res) else 0
    if args.cmd == "show":
        show(_load_cfg(args.file, args.hex))
    elif args.cmd == "check":
        res = print_check(_load_cfg(args.file, args.hex))
        return 1 if any(r[0] == "RISK" for r in res) else 0
    elif args.cmd == "json":
        cfg = _load_cfg(args.file, args.hex)
        model = asdict(cfg)
        model["check"] = [{"level": l, "code": c, "message": m} for l, c, m in check(cfg)]
        print(json.dumps(model, indent=2))
    elif args.cmd == "diff":
        pa = profile(_load_cfg(args.a, args.hex))
        pb = profile(_load_cfg(args.b, args.hex))
        print_diff(pa, pb, args.a.replace("\\", "/").rsplit("/", 1)[-1], args.b.replace("\\", "/").rsplit("/", 1)[-1])
    elif args.cmd == "usbhs":
        decode_usbhs(load_bytes(args.file, args.hex))
    elif args.cmd == "hid":
        return 1 if print_hid(load_bytes(args.file, args.hex)) else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
