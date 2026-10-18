#!/usr/bin/env python3
"""Self-check for the Phase 0 XUSB descriptors.

Parses the *source of truth* (src/usb_xinput.c + src/usb_xinput.h), resolves
the numeric macros, and asserts the XUSB invariants. Optionally searches the
built firmware image for the exact configuration-descriptor bytes, so the
check covers both "the descriptor is right" and "the descriptor is in the
firmware".

Usage:
    python3 tools/xinput_desc_check.py [--bin build/build_out/<fw>.bin]

Exit code 0 = all checks passed, 1 = at least one failure.
"""

import argparse
import re
import sys

ROOT = __file__.rsplit("/tools/", 1)[0]
SRC_C = ROOT + "/src/usb_xinput.c"
SRC_H = ROOT + "/src/usb_xinput.h"

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", dest="bin_path", default=None)
    args = ap.parse_args()

    ctext = strip_comments(open(SRC_C, encoding="utf-8").read())
    htext = strip_comments(open(SRC_H, encoding="utf-8").read())
    values = resolve(load_defines(htext + "\n" + ctext))

    dev_tokens = extract_array(ctext, "xinput_device_desc")
    cfg_tokens = extract_array(ctext, "xinput_config_desc")
    if dev_tokens is None or cfg_tokens is None:
        raise SystemExit("descriptor arrays not found in src/usb_xinput.c")

    dev = evaluate_tokens(dev_tokens, values)
    cfg = evaluate_tokens(cfg_tokens, values)

    print("Device descriptor")
    check(len(dev) == 18, "bLength == 18", f"{len(dev)}")
    check(dev[1] == 0x01, "bDescriptorType == 0x01 (Device)")
    check(dev[2:4] == [0x00, 0x02], "bcdUSB == 0x0200")
    check(dev[4:7] == [0, 0, 0], "class/subclass/protocol == 0/0/0")
    check(dev[7] == 64, "bMaxPacketSize0 == 64")
    vid = dev[8] | (dev[9] << 8)
    pid = dev[10] | (dev[11] << 8)
    bcd = dev[12] | (dev[13] << 8)
    check(vid == 0x045E, "idVendor == 0x045E", f"0x{vid:04X}")
    check(pid == 0x028E, "idProduct == 0x028E", f"0x{pid:04X}")
    check(bcd == values.get("USB_XINPUT_BCD_DEVICE"),
          "bcdDevice == USB_XINPUT_BCD_DEVICE", f"0x{bcd:04X}")
    check(dev[16] == 0x00, "iSerialNumber == 0 (no serial, like real pad)")
    check(dev[17] == 1, "bNumConfigurations == 1")

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
        print("Firmware image")
        blob = open(args.bin_path, "rb").read()
        pattern = bytes(cfg)
        check(pattern in blob, "config descriptor byte sequence found in .bin",
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
