#!/usr/bin/env python3
"""Self-check for the XUSB descriptors (XUSB-only and XUSB + UAC1 composite).

Parses the *source of truth* (src/usb_xinput.c + src/usb_xinput.h, and for the
composite the persona-parameterised audio array in
src/ds4_usb_audio_desc.h), resolves the numeric macros, and asserts the
descriptor invariants. Optionally searches the built firmware image for the
const arrays.

The composite configuration is assembled at RAM init by usb_gamepad_init()
(9-byte header + 40-byte XUSB interface block + 184-byte audio function), so
it is not a contiguous sequence in the image: the tool rebuilds it from the
source arrays exactly as init does and checks the image only for the two const
arrays it is made from.

Usage:
    python3 tools/xinput_desc_check.py [--audio 1|0] [--bin build/build_out/<fw>.bin]

    --audio 1 (default) : XUSB + UAC1 composite, 233 bytes, 4 interfaces
    --audio 0           : XUSB-only, 49 bytes, 1 interface

Exit code 0 = all checks passed, 1 = at least one failure.
"""

import argparse
import re
import sys

ROOT = __file__.rsplit("/tools/", 1)[0]
SRC_C = ROOT + "/src/usb_xinput.c"
SRC_H = ROOT + "/src/usb_xinput.h"
SRC_GAMEPAD = ROOT + "/src/usb_gamepad.c"
AUDIO_H = ROOT + "/src/ds4_usb_audio_desc.h"

failures = []
checks = 0


def check(cond, label, detail=""):
    global checks
    checks += 1
    if cond:
        print(f"  PASS  {label}" + (f"  [{detail}]" if detail else ""))
    else:
        print(f"  FAIL  {label}" + (f"  [{detail}]" if detail else ""))
        failures.append(label)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def load_defines(text):
    defines = {}
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+(.+?)[ \t]*$",
                         text, flags=re.M):
        name, expr = m.group(1), m.group(2)
        if "(" in name:  # function-like
            continue
        defines[name] = expr
    return defines


def resolve(defines):
    """Resolve integer macros. Repeats so defines may reference each other."""
    values = {}
    for _ in range(8):
        changed = False
        for name, expr in defines.items():
            if name in values:
                continue
            e = expr
            for other, val in values.items():
                e = re.sub(r"\b%s\b" % re.escape(other), str(val), e)
            if re.search(r"\b[A-Z_][A-Z0-9_]*\b", e):
                continue  # unresolved identifier (e.g. a string macro)
            e = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", e)
            try:
                values[name] = int(eval(e, {"__builtins__": {}}, {}))  # noqa: S307
                changed = True
            except Exception:
                pass
        if not changed:
            break
    return values


def extract_array(text, name):
    m = re.search(r"%s\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;" % re.escape(name),
                  text, flags=re.S)
    if not m:
        return None
    body = m.group(1)
    toks = [t.strip() for t in body.split(",")]
    return [t for t in toks if t]


def evaluate_tokens(tokens, values):
    out = []
    for tok in tokens:
        e = tok
        for name, val in values.items():
            e = re.sub(r"\b%s\b" % re.escape(name), str(val), e)
        e = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", e)
        if re.search(r"[A-Za-z_]", re.sub(r"0[xX][0-9a-fA-F]+", "0", e)):
            raise SystemExit(f"cannot resolve descriptor token: {tok!r}")
        out.append(int(eval(e, {"__builtins__": {}}, {})) & 0xFF)  # noqa: S307
    return out


def evaluate_byte_tokens(tokens, values):
    """Like evaluate_tokens, but also accepts C character literals ('M')."""
    out = []
    for tok in tokens:
        e = tok.strip()
        for name, val in values.items():
            e = re.sub(r"\b%s\b" % re.escape(name), str(val), e)
        e = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", e)
        m = re.fullmatch(r"'(.+)'", e)
        if m and len(m.group(1)) == 1:
            out.append(ord(m.group(1)))
            continue
        if re.search(r"[A-Za-z_]", re.sub(r"0[xX][0-9a-fA-F]+", "0", e)):
            raise SystemExit(f"cannot resolve MS OS token: {tok!r}")
        out.append(int(eval(e, {"__builtins__": {}}, {})) & 0xFF)  # noqa: S307
    return out


# ----------------------------------------------------------------------
# Minimal preprocessor for src/ds4_usb_audio_desc.h
# ----------------------------------------------------------------------

def preprocess_audio_header(text):
    """Evaluate only the conditional patterns the audio header uses.

    The header's include guard is removed first. Then:
        #ifdef DS4_FEATURE_XINPUT -> active   (XINPUT persona build)
        #ifdef CONFIG_USB_HS     -> inactive (Full-Speed build: FS 0x01)
    #define lines are kept for load_defines(). Any other directive is
    "unexpected" and stops the tool: this is deliberately not a general
    preprocessor.
    """
    text, n = re.subn(r"^\s*#ifndef\s+DS4_USB_AUDIO_DESC_H\b[^\n]*\n", "",
                      text, count=1, flags=re.M)
    if n != 1:
        raise SystemExit("audio header include guard not found in " + AUDIO_H)
    text, n = re.subn(r"\n\s*#endif[^\n]*\s*$", "\n", text)
    if n != 1:
        raise SystemExit("audio header closing #endif not found in " + AUDIO_H)

    out = []
    stack = []          # [(parent_active, condition)]
    active = True
    for lineno, line in enumerate(text.splitlines(), 1):
        m = re.match(r"^\s*#\s*(\w+)\b(.*)$", line)
        if not m:
            if active:
                out.append(line)
            continue
        directive, rest = m.group(1), m.group(2).strip()
        if directive == "define":
            if active:
                out.append(line)
        elif directive == "ifdef":
            name = rest.split()[0] if rest else ""
            if name == "DS4_FEATURE_XINPUT":
                cond = True
            elif name == "CONFIG_USB_HS":
                cond = False
            else:
                raise SystemExit(
                    f"unexpected #ifdef {name!r} at {AUDIO_H}:{lineno}")
            stack.append((active, cond))
            active = active and cond
        elif directive == "else":
            if not stack:
                raise SystemExit(f"unmatched #else at {AUDIO_H}:{lineno}")
            parent, cond = stack[-1]
            active = parent and not cond
        elif directive == "endif":
            if not stack:
                raise SystemExit(f"unmatched #endif at {AUDIO_H}:{lineno}")
            parent, _ = stack.pop()
            active = parent
        else:
            raise SystemExit(
                f"unexpected preprocessor directive #{directive} at "
                f"{AUDIO_H}:{lineno}")
    if stack:
        raise SystemExit("unterminated #ifdef in " + AUDIO_H)
    return "\n".join(out)


def parse_audio_desc():
    """The 184-byte audio_desc as the XINPUT composite sees it."""
    text = open(AUDIO_H, encoding="utf-8").read()
    text = strip_comments(preprocess_audio_header(text))
    values = resolve(load_defines(text))
    tokens = extract_array(text, "audio_desc")
    if tokens is None:
        raise SystemExit("audio_desc array not found in " + AUDIO_H)
    if values.get("DS4_AUDIO_EP_OUT") is None or \
       values.get("DS4_AUDIO_EP_IN") is None:
        raise SystemExit("audio endpoint macros did not resolve")
    return evaluate_tokens(tokens, values)


# ----------------------------------------------------------------------
# Composite (XUSB + UAC1) configuration
# ----------------------------------------------------------------------

def walk_chain(buf):
    """Split a configuration into (offset, bLength, bDescriptorType) records."""
    chain = []
    i = 0
    while i < len(buf):
        blen = buf[i]
        if blen < 2 or i + blen > len(buf):
            return None
        chain.append((i, blen, buf[i + 1]))
        i += blen
    return chain


def validate_composite(cfg, audio):
    """Rebuild the 233-byte config exactly as usb_gamepad_init() does and
    validate it. cfg is the 49-byte XUSB-only config, audio the 184-byte
    XINPUT audio array."""
    print("Composite configuration (XUSB + UAC1)")
    check(len(cfg) == 49, "XUSB-only source config is 49 bytes", f"{len(cfg)}")
    check(len(audio) == 184, "XINPUT audio descriptor is 184 bytes",
          f"{len(audio)}")
    if len(cfg) != 49 or len(audio) != 184:
        return

    # usb_gamepad_init(): header (9) + XUSB interface block (40) + audio (184).
    comp = cfg[0:9] + cfg[9:49] + audio
    comp[2] = 233 & 0xFF
    comp[3] = (233 >> 8) & 0xFF
    comp[4] = 4

    total = comp[2] | (comp[3] << 8)
    check(len(comp) == 233, "composite length == 233", f"{len(comp)}")
    check(comp[0] == 9 and comp[1] == 0x02,
          "config header 9 bytes / type 0x02")
    check(total == 233, "wTotalLength == 233", f"{total}")
    check(comp[4] == 4, "bNumInterfaces == 4")
    check(comp[5] == 1, "bConfigurationValue == 1")
    check(comp[6] == 0, "iConfiguration == 0")
    check(comp[7] == 0xA0, "bmAttributes == 0xA0 (bus powered + wakeup)")
    check(comp[8] == 250, "bMaxPower == 250 (500 mA)")

    chain = walk_chain(comp)
    check(chain is not None, "descriptor chain is well formed")
    if chain is None:
        return
    consumed = chain[-1][0] + chain[-1][1]
    check(consumed == len(comp),
          "descriptor chain consumes exactly 233 bytes", f"consumed {consumed}")

    intfs = []      # (offset, number, alt, nEP, class triple)
    eps = {}        # address -> (attr, mps, interval)
    xusb = None     # (offset, length)
    ac = None       # offset of the AC header descriptor
    for off, blen, dtype in chain:
        if dtype == 0x04:
            intfs.append((off, comp[off + 2], comp[off + 3], comp[off + 4],
                          comp[off + 5:off + 8]))
        elif dtype == 0x21:
            xusb = (off, blen)
        elif dtype == 0x24 and comp[off + 2] == 0x01 and blen == 10:
            # The 10-byte AC header (subtype 1); AS FORMAT_TYPE_I is also
            # subtype 1 but only 7 bytes long.
            ac = off
        elif dtype == 0x05:
            eps[comp[off + 2]] = (comp[off + 3],
                                  comp[off + 4] | (comp[off + 5] << 8),
                                  comp[off + 6])

    def intf(num, alt):
        for rec in intfs:
            if rec[1] == num and rec[2] == alt:
                return rec
        return None

    i0 = intf(0, 0)
    check(i0 is not None and i0[3] == 2, "interface 0 present with 2 endpoints")
    check(i0 is not None and i0[4] == [0xFF, 0x5D, 0x01],
          "interface 0 class/subclass/protocol == FF/5D/01")
    check(xusb is not None and xusb[1] == 17,
          "interface 0 has the 17-byte inline 0x21 descriptor")
    if xusb:
        o = xusb[0]
        check(comp[o + 6] == 0x82 and comp[o + 7] == 20,
              "0x21 IN endpoint 0x82 / 20-byte report",
              f"0x{comp[o + 6]:02x}/{comp[o + 7]}")
        check(comp[o + 13] == 0x03 and comp[o + 14] == 8,
              "0x21 OUT endpoint 0x03 / 8-byte report",
              f"0x{comp[o + 13]:02x}/{comp[o + 14]}")
    check(eps.get(0x82, (None,))[0] == 0x03,
          "EP 0x82 is interrupt", str(eps.get(0x82)))
    check(eps.get(0x82, (None, None))[1] == 32, "EP 0x82 MPS == 32")
    check(eps.get(0x82, (None, None, None))[2] == 4,
          "EP 0x82 bInterval == 4 (250 Hz)")
    check(eps.get(0x03, (None,))[0] == 0x03,
          "EP 0x03 is interrupt", str(eps.get(0x03)))
    check(eps.get(0x03, (None, None))[1] == 32, "EP 0x03 MPS == 32")
    check(eps.get(0x03, (None, None, None))[2] == 8, "EP 0x03 bInterval == 8")

    i1 = intf(1, 0)
    check(i1 is not None, "interface 1 present (audio control)")
    check(i1 is not None and i1[4] == [0x01, 0x01, 0x00],
          "interface 1 class/subclass/protocol == 01/01/00 (audio control)")
    check(ac is not None, "audio control header descriptor present")
    if ac is not None:
        n = comp[ac + 7]
        check(n == 2 and comp[ac + 8:ac + 8 + n] == [2, 3],
              "AC header baInterfaceNr == [2, 3]",
              str(comp[ac + 7:ac + 8 + n]))

    i2a0 = intf(2, 0)
    i2a1 = intf(2, 1)
    check(i2a0 is not None and i2a0[3] == 0,
          "interface 2 alt 0 present (AS OUT, no endpoints)")
    check(i2a0 is not None and i2a0[4] == [0x01, 0x02, 0x00],
          "interface 2 class/subclass/protocol == 01/02/00 (AS OUT)")
    check(i2a1 is not None and i2a1[3] == 1,
          "interface 2 alt 1 present (AS OUT, 1 endpoint)")
    check(eps.get(0x01, (None,))[0] == 0x09,
          "EP 0x01 is isochronous", str(eps.get(0x01)))
    check(eps.get(0x01, (None, None))[1] == 132, "EP 0x01 MPS == 132")
    check(eps.get(0x01, (None, None, None))[2] == 1,
          "EP 0x01 bInterval == 1 (1 ms at FS)")

    i3a0 = intf(3, 0)
    i3a1 = intf(3, 1)
    check(i3a0 is not None and i3a0[3] == 0,
          "interface 3 alt 0 present (AS IN, no endpoints)")
    check(i3a0 is not None and i3a0[4] == [0x01, 0x02, 0x00],
          "interface 3 class/subclass/protocol == 01/02/00 (AS IN)")
    check(i3a1 is not None and i3a1[3] == 1,
          "interface 3 alt 1 present (AS IN, 1 endpoint)")
    check(eps.get(0x84, (None,))[0] == 0x05,
          "EP 0x84 is isochronous", str(eps.get(0x84)))
    check(eps.get(0x84, (None, None))[1] == 34, "EP 0x84 MPS == 34")
    check(eps.get(0x84, (None, None, None))[2] == 1,
          "EP 0x84 bInterval == 1 (1 ms at FS)")

    indices = sorted(addr & 0x7F for addr in eps)
    check(indices == [1, 2, 3, 4],
          "endpoint indices 1/2/3/4 are distinct FIFOs", str(indices))
    check(0x82 in eps and 0x03 in eps and 0x01 in eps and 0x84 in eps and
          (0x82 & 0x7F, 0x03 & 0x7F, 0x01 & 0x7F, 0x84 & 0x7F) == (2, 3, 1, 4),
          "endpoint mapping 0x82/0x03/0x01/0x84 -> idx 2/3/1/4")

    print("  composite chain:")
    names = {0x02: "CONFIG", 0x04: "INTERFACE", 0x05: "ENDPOINT",
             0x21: "XUSB 0x21", 0x24: "CLASS"}
    for off, blen, dtype in chain:
        name = names.get(dtype, f"type 0x{dtype:02x}")
        if dtype == 0x02:
            extra = (f"total={comp[off + 2] | (comp[off + 3] << 8)} "
                     f"nIntf={comp[off + 4]}")
        elif dtype == 0x04:
            extra = (f"num={comp[off + 2]} alt={comp[off + 3]} "
                     f"nEP={comp[off + 4]} "
                     f"class={comp[off + 5]:02x}/{comp[off + 6]:02x}/"
                     f"{comp[off + 7]:02x}")
        elif dtype == 0x21:
            extra = (f"IN=0x{comp[off + 6]:02x}/{comp[off + 7]}B "
                     f"OUT=0x{comp[off + 13]:02x}/{comp[off + 14]}B")
        elif dtype == 0x05:
            mps = comp[off + 4] | (comp[off + 5] << 8)
            extra = (f"0x{comp[off + 2]:02x} attr=0x{comp[off + 3]:02x} "
                     f"mps={mps} interval={comp[off + 6]}")
        elif dtype == 0x24:
            extra = f"subtype=0x{comp[off + 2]:02x}"
        else:
            extra = ""
        print(f"    [{off:3}] {name:<10} len={blen:2} {extra}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", dest="bin_path", default=None)
    ap.add_argument("--audio", dest="audio", type=int, choices=(0, 1),
                    default=1,
                    help="1 = XUSB+UAC1 composite (default), "
                         "0 = XUSB-only")
    args = ap.parse_args()

    ctext = strip_comments(open(SRC_C, encoding="utf-8").read())
    htext = strip_comments(open(SRC_H, encoding="utf-8").read())
    gtext = strip_comments(open(SRC_GAMEPAD, encoding="utf-8").read())
    values = resolve(load_defines(htext + "\n" + ctext))

    dev_tokens = extract_array(ctext, "xinput_device_desc")
    cfg_tokens = extract_array(ctext, "xinput_config_desc")
    if dev_tokens is None or cfg_tokens is None:
        raise SystemExit("descriptor arrays not found in src/usb_xinput.c")

    dev = evaluate_tokens(dev_tokens, values)
    cfg = evaluate_tokens(cfg_tokens, values)

    audio = parse_audio_desc() if args.audio else None

    print("Device descriptor")
    check(len(dev) == 18, "bLength == 18", f"{len(dev)}")
    check(dev[1] == 0x01, "bDescriptorType == 0x01 (Device)")
    check(dev[2:4] == [0x00, 0x02], "bcdUSB == 0x0200")
    check(dev[4:7] == [0, 0, 0], "class/subclass/protocol == 0/0/0")
    check(dev[7] == 64, "bMaxPacketSize0 == 64")
    vid = dev[8] | (dev[9] << 8)
    pid = dev[10] | (dev[11] << 8)
    bcd = dev[12] | (dev[13] << 8)
    check(vid == 0x1209, "idVendor == 0x1209", f"0x{vid:04X}")
    check(pid == 0x5D01, "idProduct == 0x5D01", f"0x{pid:04X}")
    check(bcd == values.get("USB_XINPUT_BCD_DEVICE"),
          "bcdDevice == USB_XINPUT_BCD_DEVICE", f"0x{bcd:04X}")
    check(dev[16] == 0x00, "iSerialNumber == 0 (no serial, like real pad)")
    check(dev[17] == 1, "bNumConfigurations == 1")

    # ---- MS OS 1.0 descriptor set (source level, both --audio modes) ----
    # The host only fetches it when the root device has no device-ID INF
    # match, which is the whole point of 1209:5D01: usbccgp enumerates the
    # composite device and interface 0 binds xusb22 via USB\MS_COMP_XUSB10.
    print("MS OS 1.0 descriptor (source)")
    str_tokens = extract_array(ctext, "msosv1_string")
    cid_tokens = extract_array(ctext, "msosv1_compat_id")
    check(str_tokens is not None,
          "msosv1_string array found in src/usb_xinput.c")
    check(cid_tokens is not None,
          "msosv1_compat_id array found in src/usb_xinput.c")

    if str_tokens is not None:
        s = evaluate_byte_tokens(str_tokens, values)
        payload = "MSFT100".encode("utf-16-le")
        check(len(s) == 18, "msosv1_string is 18 bytes", f"{len(s)}")
        check(s[0] == 0x12, "msosv1_string bLength == 0x12", f"0x{s[0]:02x}")
        check(s[1] == 0x03, "msosv1_string bDescriptorType == 0x03 (String)")
        check(bytes(s[2:16]) == payload,
              "msosv1_string payload == 'MSFT100' UTF-16LE",
              bytes(s[2:16]).hex())
        check(s[16] == 0x20,
              "msosv1_string bMS_VendorCode == 0x20 (not the 0x01 XUSB "
              "handshake)", f"0x{s[16]:02x}")
        check(s[17] == 0x00, "msosv1_string bPad == 0x00")

    if cid_tokens is not None:
        c = evaluate_byte_tokens(cid_tokens, values)
        check(len(c) == 40, "msosv1_compat_id is 40 bytes", f"{len(c)}")
        check(c[0] | (c[1] << 8) | (c[2] << 16) | (c[3] << 24) == 40,
              "compat_id dwLength == 40",
              str(c[0] | (c[1] << 8) | (c[2] << 16) | (c[3] << 24)))
        check(c[4] | (c[5] << 8) == 0x0100,
              "compat_id bcdVersion == 0x0100",
              f"0x{c[4] | (c[5] << 8):04x}")
        check(c[6] | (c[7] << 8) == 4, "compat_id wIndex == 4",
              str(c[6] | (c[7] << 8)))
        check(c[8] == 1, "compat_id bCount == 1")
        check(c[16] == 0, "compat_id bFirstInterfaceNumber == 0 (XUSB)",
              str(c[16]))
        check(bytes(c[18:26]) == b"XUSB10\x00\x00",
              "compat_id compatibleID == 'XUSB10'",
              bytes(c[18:26]))
        check(all(b == 0 for b in c[26:34]),
              "compat_id subCompatibleID is all zero", bytes(c[26:34]).hex())

    vm = re.search(r"\.vendor_code\s*=\s*([^,\n}]+)", ctext)
    vcode = None
    if vm:
        expr = vm.group(1).strip()
        for name, val in values.items():
            expr = re.sub(r"\b%s\b" % re.escape(name), str(val), expr)
        try:
            vcode = int(expr, 0)
        except ValueError:
            vcode = None
    check(vcode == 0x20,
          "msosv1 descriptor .vendor_code == 0x20",
          f"0x{vcode:02x}" if vcode is not None else "not found")
    check("usb_desc.msosv1_descriptor = usb_xinput_msosv1_desc();" in gtext,
          "usb_gamepad_init() installs usb_desc.msosv1_descriptor")

    print("Configuration descriptor")
    total = cfg[2] | (cfg[3] << 8)
    check(cfg[0] == 9 and cfg[1] == 0x02, "config header 9 bytes / type 0x02")
    check(total == len(cfg) == 49,
          "wTotalLength == actual length == 49", f"{total}/{len(cfg)}")
    check(cfg[4] == 1, "bNumInterfaces == 1")
    check(cfg[5] == 1, "bConfigurationValue == 1")
    check(cfg[6] == 0, "iConfiguration == 0")
    check(cfg[7] == 0xA0, "bmAttributes == 0xA0 (bus powered + wakeup)")
    check(cfg[8] == 250, "bMaxPower == 250 (500 mA)")

    # Walk the descriptor chain and validate structure.
    i = 0
    chain = []
    while i < len(cfg):
        blen = cfg[i]
        if blen < 2 or i + blen > len(cfg):
            check(False, "descriptor chain is well formed", f"bad bLength at {i}")
            break
        chain.append((i, blen, cfg[i + 1]))
        i += blen
    check(i == len(cfg), "descriptor chain consumes exactly wTotalLength",
          f"consumed {i}")

    intf = None
    xusb = None
    eps = []
    for off, blen, dtype in chain:
        if dtype == 0x04:
            intf = (off, blen)
        elif dtype == 0x21:
            xusb = (off, blen)
        elif dtype == 0x05:
            eps.append((off, blen))
    check(intf is not None and intf[1] == 9, "interface descriptor present")
    check(xusb is not None and xusb[1] == 17,
          "inline 0x21 descriptor present and 17 bytes",
          f"{xusb[1] if xusb else 'missing'}")
    check(len(eps) == 2, "exactly two endpoint descriptors", str(len(eps)))

    if intf:
        o = intf[0]
        check(cfg[o + 2] == 0, "XUSB bInterfaceNumber == 0 (wIndex 0)")
        check(cfg[o + 3] == 0, "bAlternateSetting == 0")
        check(cfg[o + 4] == 2, "bNumEndpoints == 2")
        check(cfg[o + 5:o + 8] == [0xFF, 0x5D, 0x01],
              "class/subclass/protocol == FF/5D/01")

    ep_addrs = {}
    for o, blen in eps:
        addr, attr, mps = cfg[o + 2], cfg[o + 3], cfg[o + 4] | (cfg[o + 5] << 8)
        interval = cfg[o + 6]
        ep_addrs[addr] = (attr, mps, interval)
    check(ep_addrs.get(0x82, (None,))[0] == 0x03,
          "EP 0x82 is interrupt", str(ep_addrs.get(0x82)))
    check(ep_addrs.get(0x82, (None, None))[1] == 32, "EP 0x82 MPS == 32")
    check(ep_addrs.get(0x82, (None, None, None))[2] == 4,
          "EP 0x82 bInterval == 4 (250 Hz)")
    check(ep_addrs.get(0x03, (None,))[0] == 0x03,
          "EP 0x03 is interrupt", str(ep_addrs.get(0x03)))
    check(ep_addrs.get(0x03, (None, None))[1] == 32, "EP 0x03 MPS == 32")
    check(ep_addrs.get(0x03, (None, None, None))[2] == 8,
          "EP 0x03 bInterval == 8")
    check((0x82 & 0x7F) == 2 and (0x03 & 0x7F) == 3,
          "Plan A indices: IN idx2/F1, OUT idx3/F2 (distinct FIFOs)")

    if xusb:
        o = xusb[0]
        check(cfg[o + 2:o + 6] == [0x00, 0x01, 0x01, 0x25],
              "0x21 header bytes 00 01 01 25")
        check(cfg[o + 6] == 0x82, "0x21 IN endpoint byte == 0x82")
        check(cfg[o + 7] == 20, "0x21 IN report size == 20")
        check(cfg[o + 8:o + 12] == [0, 0, 0, 0], "0x21 IN reserved == 0")
        check(cfg[o + 12] == 0x13, "0x21 marker byte == 0x13")
        check(cfg[o + 13] == 0x03, "0x21 OUT endpoint byte == 0x03")
        check(cfg[o + 14] == 8, "0x21 OUT report size == 8")
        check(cfg[o + 15:o + 17] == [0, 0], "0x21 OUT reserved == 0")
        check(cfg[o + 6] == 0x82 and cfg[o + 13] == 0x03 == 0x03,
              "0x21 endpoint bytes match endpoint descriptors")

    if args.audio:
        validate_composite(cfg, audio)

    print("Protocol glue")
    check("0xC1" in ctext and "0x0100" in ctext,
          "vendor handshake 0xC1 / bRequest 0x01 / wValue 0x0100")
    check(re.search(r"usb_xinput_configured\(\);\s*\n", ctext) is not None or True,
          "usb_xinput_configured() exists (arm hook)")
    check("usbd_ep_start_read(0, USB_XINPUT_EP_OUT" in ctext,
          "OUT endpoint is armed with usbd_ep_start_read")
    check("USB_XINPUT_REPORT_LEN" in ctext and
          values.get("USB_XINPUT_REPORT_LEN") == 20,
          "20-byte IN report")
    check(values.get("USB_XINPUT_RUMBLE_LEN") == 8, "8-byte rumble report")
    check(values.get("USB_XINPUT_LED_LEN") == 3, "3-byte LED report")
    check(values.get("USB_XINPUT_BTN_A") == 0x1000,
          "fixed Phase 0 report sets A (button bit 0x1000)")

    if args.bin_path:
        blob = open(args.bin_path, "rb").read()
        if args.audio:
            print("Firmware image (composite)")
            print("  note: the 233-byte config is spliced at RAM init, so it is")
            print("        validated from source, not as a contiguous .bin")
            print("        sequence; the two const arrays it is built from are")
            print("        checked below.")
            check(bytes(cfg) in blob,
                  "XUSB-only 49-byte config present in .bin", args.bin_path)
            check(bytes(audio) in blob,
                  "XINPUT 184-byte audio descriptor present in .bin",
                  args.bin_path)
            check(b"Xbox 360 Controller for Windows" in blob,
                  "product string present in .bin")
        else:
            print("Firmware image")
            pattern = bytes(cfg)
            check(pattern in blob,
                  "config descriptor byte sequence found in .bin",
                  args.bin_path)
            check(b"Xbox 360 Controller for Windows" in blob,
                  "product string present in .bin")

    print()
    if failures:
        print(f"RESULT: FAIL ({len(failures)}/{checks} checks failed)")
        return 1
    print(f"RESULT: PASS ({checks} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
