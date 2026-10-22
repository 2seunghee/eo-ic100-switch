import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(HERE))

import uacdesc  # noqa: E402

FIX = os.path.join(ROOT, "fixtures")
A054_CFG = os.path.join(FIX, "eo-ic100_04e8_a054_config.hex")   # public lsusb dump, rebuilt
A054_DEV = os.path.join(FIX, "eo-ic100_04e8_a054_device.hex")
A05B_CFG = os.path.join(FIX, "eo-ic100_04e8_a05b_config.hex")   # the user's unit, raw Windows dump
A05B_DEV = os.path.join(FIX, "eo-ic100_04e8_a05b_device.hex")

# Flags both firmware variants raise; the hypotheses don't depend on the variant.
COMMON_CODES = ("UAC1", "PLAYBACK_48K", "MULTI_RATE", "OT_NOT_OUTPUT_TYPE", "CAPTURE_FIRST", "SYNC_SYNCHRONOUS",
                "SHARED_EP_NUMBER", "STEREO_MIC", "EXTRA_INTERFACES", "MAXPACKET_OVERSIZE")


def patch(data, needle, replacement):
    i = data.find(needle)
    assert i >= 0 and data.find(needle, i + 1) < 0, "needle must occur exactly once"
    return data[:i] + replacement + data[i + len(replacement):]


class A054Tests(unittest.TestCase):
    def setUp(self):
        self.raw = uacdesc.load_bytes(A054_CFG)
        self.cfg = uacdesc.parse_config(self.raw)

    def test_sizes_match_lsusb(self):
        self.assertEqual(len(self.raw), 305)
        self.assertEqual(self.cfg.total_length, 305)
        self.assertEqual(self.cfg.num_interfaces, 5)
        self.assertEqual(self.cfg.max_power_ma, 100)
        self.assertEqual(self.cfg.ac_total_length, 78)
        self.assertEqual(self.cfg.warnings, [])

    def test_audio_control(self):
        self.assertEqual(self.cfg.ac_iface, 0)
        self.assertEqual(self.cfg.bcd_adc, 0x0100)
        self.assertEqual(self.cfg.collection, [1, 2])
        self.assertEqual([(u.subtype, u.id) for u in self.cfg.units],
                         [(2, 1), (6, 2), (3, 3), (2, 4), (6, 5), (3, 6)])
        fu2 = self.cfg.unit(2)
        self.assertEqual(fu2.controls, [0x0003, 0, 0])
        self.assertEqual(fu2.string_index, 6)

    def test_alt_settings(self):
        self.assertEqual([(a.number, a.alt) for a in self.cfg.alts],
                         [(0, 0), (1, 0), (1, 1), (2, 0), (2, 1), (2, 2), (3, 0), (4, 0)])

    def test_playback_choice(self):
        a, ep = uacdesc.find_playback_choice(self.cfg)
        self.assertEqual((a.number, a.alt), (2, 1))
        self.assertEqual(ep.address, 0x03)
        self.assertEqual(ep.packet_bytes, 384)
        self.assertEqual(ep.sync_type, 3)
        self.assertTrue(ep.has_rate_control)
        self.assertEqual(a.rates(), [44100, 48000, 96000])
        self.assertEqual(ep.length, 9)

    def test_capture(self):
        cap = self.cfg.streaming(playback=False)
        self.assertEqual(len(cap), 1)
        a, ep = cap[0]
        self.assertEqual((a.number, a.alt, ep.address, ep.packet_bytes), (1, 1, 0x83, 192))
        self.assertEqual(a.rates(), [48000])

    def test_paths(self):
        paths = uacdesc.playback_path(self.cfg, 1)
        self.assertEqual(len(paths), 1)
        self.assertEqual(paths[0]["ot"].id, 3)
        self.assertEqual(paths[0]["ot"].terminal_type, 0x0402)
        self.assertEqual([f.id for f in paths[0]["fus"]], [2])
        cp = uacdesc.capture_path(self.cfg, 6)
        self.assertEqual(cp["it"].id, 4)
        self.assertEqual([f.id for f in cp["fus"]], [5])

    def test_hid(self):
        hid = [a for a in self.cfg.alts if a.cls == 3]
        self.assertEqual([(a.number, a.hid["report_length"], a.endpoints[0].address) for a in hid],
                         [(3, 77, 0x84), (4, 20, 0x85)])

    def test_check_codes(self):
        res = uacdesc.check(self.cfg)
        codes = {c for _, c, _ in res}
        for expected in COMMON_CODES:
            self.assertIn(expected, codes)
        self.assertFalse(any(level == "RISK" for level, _, _ in res))

    def test_headphones_variant(self):
        # OT id3: 09 24 03 03 [02 04] 00 02 00 -> terminal type 0x0302 Headphones
        raw = patch(self.raw, bytes.fromhex("0924030302040002"), bytes.fromhex("0924030302030002"))
        cfg = uacdesc.parse_config(raw)
        codes = {c for _, c, _ in uacdesc.check(cfg)}
        self.assertIn("OT_TYPE", codes)
        self.assertNotIn("OT_NOT_OUTPUT_TYPE", codes)
        pa, pb = uacdesc.profile(self.cfg), uacdesc.profile(cfg)
        diff = [k for k in set(pa) | set(pb) if pa.get(k) != pb.get(k)]
        self.assertEqual(diff, ["play output terminal"])

    def test_adaptive_variant(self):
        # Playback EPs: bmAttributes 0x0d (sync) -> 0x09 (adaptive)
        raw = self.raw.replace(bytes.fromhex("0905030d"), bytes.fromhex("09050309"))
        codes = {c for _, c, _ in uacdesc.check(uacdesc.parse_config(raw))}
        self.assertIn("SYNC_ADAPTIVE", codes)
        self.assertNotIn("SYNC_SYNCHRONOUS", codes)

    def test_truncated_input(self):
        cfg = uacdesc.parse_config(self.raw[:100])
        self.assertTrue(cfg.warnings)
        self.assertEqual(cfg.ac_iface, 0)

    def test_not_a_config(self):
        with self.assertRaises(ValueError):
            uacdesc.parse_config(bytes.fromhex("12 01 00 02"))


class A05bTests(unittest.TestCase):
    """The user's own unit: different firmware, terminal IDs swapped relative to a054."""

    def setUp(self):
        self.raw = uacdesc.load_bytes(A05B_CFG)
        self.cfg = uacdesc.parse_config(self.raw)

    def test_sizes(self):
        self.assertEqual(len(self.raw), 345)
        self.assertEqual((self.cfg.total_length, self.cfg.num_interfaces, self.cfg.max_power_ma), (345, 5, 100))
        self.assertEqual(self.cfg.ac_total_length, 78)
        self.assertEqual(self.cfg.warnings, [])

    def test_topology(self):
        units = {u.id: u for u in self.cfg.units}
        self.assertEqual((units[1].subtype, units[1].terminal_type, units[1].assoc_terminal), (2, 0x0402, 6))
        self.assertEqual((units[3].subtype, units[3].terminal_type), (3, 0x0101))
        self.assertEqual((units[4].subtype, units[4].terminal_type), (2, 0x0101))
        self.assertEqual((units[6].subtype, units[6].terminal_type, units[6].sources), (3, 0x0402, [5]))
        self.assertEqual(units[5].controls[0], 0x0003)

    def test_alt_settings(self):
        self.assertEqual([(a.number, a.alt) for a in self.cfg.alts],
                         [(0, 0), (1, 0), (1, 1), (2, 0), (2, 1), (2, 2), (2, 3), (3, 0), (4, 0)])

    def test_playback(self):
        a, ep = uacdesc.find_playback_choice(self.cfg)
        self.assertEqual((a.number, a.alt, ep.address, ep.packet_bytes), (2, 1, 0x03, 384))
        self.assertEqual(a.rates(), [48000, 96000])
        self.assertTrue(ep.has_rate_control)
        alt3 = [x for x, _ in self.cfg.streaming(True) if x.supports(48000, 32, 2)]
        self.assertEqual([(x.alt, x.fmt["subframe"]) for x in alt3], [(3, 4)])

    def test_paths(self):
        paths = uacdesc.playback_path(self.cfg, 4)
        self.assertEqual([(p["ot"].id, p["ot"].terminal_type, [f.id for f in p["fus"]]) for p in paths],
                         [(6, 0x0402, [5])])
        cp = uacdesc.capture_path(self.cfg, 3)
        self.assertEqual((cp["it"].id, [f.id for f in cp["fus"]]), (1, [2]))

    def test_hid(self):
        hid = [(a.number, a.hid["report_length"], a.endpoints[0].address, a.endpoints[0].interval)
               for a in self.cfg.alts if a.cls == 3]
        self.assertEqual(hid, [(3, 47, 0x84, 4), (4, 20, 0x85, 4)])

    def test_check_codes_match_a054(self):
        res = uacdesc.check(self.cfg)
        codes = {c for _, c, _ in res}
        for expected in COMMON_CODES:
            self.assertIn(expected, codes)
        self.assertFalse(any(level == "RISK" for level, _, _ in res))

    def test_diff_against_a054(self):
        pa = uacdesc.profile(uacdesc.parse_config(uacdesc.load_bytes(A054_CFG)))
        pb = uacdesc.profile(self.cfg)
        diff = sorted(k for k in set(pa) | set(pb) if pa.get(k) != pb.get(k))
        self.assertEqual(diff, ["cap path", "hid IF3", "hid IF4", "play path", "play[0] format", "play[1] format",
                                "play[2] endpoint", "play[2] format"])
        self.assertEqual(pb["play path"], "IT4 > FU5 > OT6")
        self.assertEqual(pa["play path"], "IT1 > FU2 > OT3")
        self.assertEqual(pb["cap path"], "IT1 > FU2 > OT3")


class HidReportTests(unittest.TestCase):
    """HID report descriptors read from the user's unit on the Switch."""

    def test_if3_remote_buttons(self):
        lines, reports, issues = uacdesc.parse_hid_report(uacdesc.load_bytes(os.path.join(FIX, "eo-ic100_04e8_a05b_hid_if3.hex")))
        self.assertEqual(issues, [])
        self.assertEqual(list(reports), [1])
        self.assertEqual(reports[1]["input_bits"], 16)   # + report ID = 3 bytes = wMaxPacketSize
        named = [(off, names) for off, size, count, names, flags in reports[1]["input"] if names]
        self.assertEqual(named, [(0, ["Volume Increment"]), (1, ["Volume Decrement"]), (2, ["Play/Pause"])])

    def test_if4_invalid_logical_max(self):
        lines, reports, issues = uacdesc.parse_hid_report(uacdesc.load_bytes(os.path.join(FIX, "eo-ic100_04e8_a05b_hid_if4.hex")))
        self.assertEqual(reports[2]["input_bits"], 24)
        self.assertEqual(len(issues), 1)
        self.assertTrue(issues[0].startswith("LOGICAL_MAX_LT_MIN"))
        self.assertIn("Reserved 0xfeff", "\n".join(lines))

    def test_fixed_if4_is_clean(self):
        data = uacdesc.load_bytes(os.path.join(FIX, "eo-ic100_04e8_a05b_hid_if4.hex"))
        fixed = data.replace(bytes.fromhex("25ff"), bytes.fromhex("26ff00"))   # Logical Maximum 255, 2-byte form
        self.assertEqual(uacdesc.parse_hid_report(fixed)[2], [])

    def test_unbalanced_and_truncated(self):
        self.assertTrue(any(i.startswith("UNBALANCED") for i in uacdesc.parse_hid_report(bytes.fromhex("a101"))[2]))
        self.assertTrue(any(i.startswith("TRUNCATED") for i in uacdesc.parse_hid_report(bytes.fromhex("26ff"))[2]))


class MiscTests(unittest.TestCase):
    def test_hex_text(self):
        self.assertEqual(uacdesc.parse_hex_text("# c\n0x09 02, 31 # tail\n0100"), bytes([9, 2, 0x31, 1, 0]))

    def test_device_descriptors(self):
        d = uacdesc.parse_device(uacdesc.load_bytes(A054_DEV))
        self.assertEqual((d["vid"], d["pid"], d["bcdUSB"], d["max_packet0"]), (0x04E8, 0xA054, 0x0200, 64))
        d = uacdesc.parse_device(uacdesc.load_bytes(A05B_DEV))
        self.assertEqual((d["vid"], d["pid"], d["iSerial"]), (0x04E8, 0xA05B, 3))

    def test_usbhs_decode(self):
        rec = bytearray(uacdesc.USBHS_IFACE_SIZE)
        struct.pack_into("<i", rec, 0, 7)
        rec[0xC:0xC + 9] = bytes.fromhex("090402010101020000")
        rec[0x8C:0x8C + 7] = bytes.fromhex("0705030d800101")
        rec[0x1B8:0x1B8 + 5] = b"HsDev"
        struct.pack_into("<II", rec, 0x1F8, 1, 2)
        rec[0x200:0x212] = uacdesc.load_bytes(A054_DEV)
        lines = []
        uacdesc.decode_usbhs(bytes(rec), w=lines.append)
        text = "\n".join(lines)
        self.assertIn("ID 7 bus 1 dev 2 path HsDev", text)
        self.assertIn("04e8:a054", text)
        self.assertIn("OUT slot  0: addr 0x03", text)

    def test_cli_default_command(self):
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = uacdesc.main([A05B_CFG])
        self.assertEqual(rc, 0)
        self.assertIn("CAPTURE_FIRST", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
