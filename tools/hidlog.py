#!/usr/bin/env python3
"""Read the firmware debug log over HID feature report 0xB4.

Works on Windows and Linux (uses hidapi, same dependency as config_tool.py).

Usage:
    python hidlog.py [seconds] [outfile]

The DEBUG_CONSOLE build keeps log text in an internal ring and hands it out
through feature report 0xB4; this polls it and appends everything received
to the screen and (optionally) to a file. Release builds leave 0xB4 alone
(unchanged fall-through behaviour), so this only works on DEBUG_CONSOLE
builds.

Why 0xB4: the firmware reports a 507-byte HID report descriptor that is
byte-identical to a real DS4 v2 in every build. 0xB4 is one of the reports
that stock descriptor already declares — usage page 0xFF80, usage 0x55,
63 Feature bytes (504 bits) — so the debug reader needs no descriptor item
and the descriptor never grows past 507 bytes. It is safe to repurpose
because this firmware does not use 0xB4 for anything else and Linux
hid-sony does not use it in normal operation. (An earlier revision declared
its own vendor debug report, growing the descriptor to 525 bytes; that crossed
the 512-byte boundary and is believed to have broken HID enumeration on
Windows. It is gone.)

The report is fixed-size: 63 Feature bytes, so every GET_REPORT returns
exactly 64 bytes (report ID + 63 data bytes). Only the bytes actually popped
from the ring are meaningful; the rest is zero padding added by the firmware
so the HID stack accepts the response. This script strips that trailing
padding.

Collection selection: 0xB4 is declared *inside* the descriptor's existing
top-level application collection (Usage Page 0x01 Generic Desktop, Usage 0x05
Gamepad) — not in a second, separate vendor collection. The dongle therefore
exposes exactly one top-level collection on its HID interface, and the handle
for that collection is the one that accepts GET_FEATURE(0xB4). This script
still matches the collection by usage page/usage explicitly instead of taking
"the first matching interface", so it can never open a different collection
that would reject the request.

Equivalent on Windows without Python (hidapitester, one 64-byte read per
invocation):

    hidapitester --vidpid 0x054C/0x09CC --open --length 64 \
                 --read-feature 0xB4 --close

hidapitester opens by VID/PID; because the dongle exposes a single HID
collection, this cannot select a different collection. If a future build ever
exposed more than one top-level collection, select the gamepad one explicitly
with hidapitester's usage page/usage options (0x01 / 0x05) instead of relying
on VID/PID alone.

(`--read-feature` is the current flag; some other HID tools call it
`--get-feature`. If your hidapitester build rejects the hex ID, use the
decimal form `--read-feature 180`.)

Notes:
- UART is the primary log path; feature 0xB4 is the optional secondary
  reader (DEBUG_CONSOLE=1 builds only). No COM port is created by the
  firmware's USB function.
- Keep the polling interval modest (2 ms): each GET_REPORT is an EP0 transfer
  and the dongle also runs audio.
"""
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("Missing dependency. Install with:  pip install hidapi")

VID = 0x054C
PIDS = {0x09CC}
REPORT_ID = 0xB4
REPORT_LEN = 64        # declared report: ID + 63 Feature bytes, always fixed
INTERFACE = 3          # HID gamepad interface in the audio build (0 without)
USAGE_PAGE = 0x01      # Generic Desktop
USAGE = 0x05           # Gamepad


def find_device():
    candidates = [d for d in hid.enumerate(VID) if d["product_id"] in PIDS]
    if not candidates:
        return None

    # 0xB4 lives in the gamepad application collection. Match it explicitly;
    # never fall back to "the first matching interface", which could be a
    # different collection whose handle rejects GET_FEATURE(0xB4).
    matches = [d for d in candidates
               if d.get("usage_page") == USAGE_PAGE and d.get("usage") == USAGE]
    if not matches and len(candidates) == 1:
        # Some hidapi builds do not report usage/usage_page. Accept the only
        # candidate, but only because it is unambiguous.
        matches = candidates

    on_interface = [d for d in matches
                    if d.get("interface_number", -1) == INTERFACE]
    if on_interface:
        matches = on_interface

    return matches[0] if matches else None


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    outfile = sys.argv[2] if len(sys.argv) > 2 else None

    devinfo = find_device()
    if not devinfo:
        sys.exit("DS4 dongle not found (VID 054C / PID 09CC, "
                 "gamepad collection Usage Page 0x01 / Usage 0x05 on "
                 "interface %d)." % INTERFACE)
    print("using: %s (usage_page=0x%02X usage=0x%02X interface=%s)" % (
        devinfo.get("path"),
        devinfo.get("usage_page", 0),
        devinfo.get("usage", 0),
        devinfo.get("interface_number", "?")), file=sys.stderr)

    dev = hid.device()
    dev.open_path(devinfo["path"])
    dev.set_nonblocking(0)

    fh = open(outfile, "ab") if outfile else None
    deadline = time.time() + seconds
    total = 0
    try:
        while time.time() < deadline:
            try:
                data = dev.get_feature_report(REPORT_ID, REPORT_LEN)
            except Exception as exc:          # device gone / reset
                print("\n[hidlog] read failed: %s" % exc, file=sys.stderr)
                break
            if data and len(data) > 1:
                # Fixed 64-byte report: strip the firmware's NUL padding.
                chunk = bytes(data[1:]).rstrip(b"\x00")
                if chunk:
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()
                    if fh:
                        fh.write(chunk)
                        fh.flush()
                    total += len(chunk)
            time.sleep(0.002)
    finally:
        if fh:
            fh.close()
        dev.close()

    print("\n[hidlog] %d bytes in %.0fs" % (total, seconds), file=sys.stderr)


if __name__ == "__main__":
    main()
