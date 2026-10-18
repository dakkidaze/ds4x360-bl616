#!/usr/bin/env python3
"""Read the DS4 dongle log over a serial port.

Logs go to the console UART in every build (there is no USB CDC console any
more). Open the board's UART port — the Type-C bridge on LCTech / M0S Dock, or
a USB-TTL adapter on the Ai-M61-32S-Kit — at the console baud rate.

Usage:
    python ttylog.py COM14 180 phaseb.log
    python ttylog.py /dev/ttyACM0 180 phaseb.log    (WSL/Linux)
    BAUD=2000000 python ttylog.py /dev/ttyUSB0 180  (non-default baud)

Requires: pip install pyserial
"""
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("Missing dependency. Install with:  pip install pyserial")


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: ttylog.py <port> [seconds] [outfile]")

    port = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    outfile = sys.argv[3] if len(sys.argv) > 3 else None

    baud = int(os.environ.get("BAUD", "115200"))

    ser = serial.Serial(port, baud, timeout=0.05)
    # USB-UART bridges are happy with these asserted; harmless either way.
    ser.dtr = True
    ser.rts = True

    print("reading %s at %d for %.0fs" % (port, baud, seconds), file=sys.stderr)

    fh = open(outfile, "ab") if outfile else None
    deadline = time.time() + seconds
    total = 0
    try:
        while time.time() < deadline:
            data = ser.read(4096)
            if not data:
                continue
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            if fh:
                fh.write(data)
                fh.flush()
            total += len(data)
    finally:
        if fh:
            fh.close()
        ser.close()

    print("\n[ttylog] %d bytes in %.0fs" % (total, seconds), file=sys.stderr)


if __name__ == "__main__":
    main()
