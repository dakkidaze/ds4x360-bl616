import os, termios, fcntl, struct, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
out = sys.argv[2] if len(sys.argv) > 2 else "/home/dakki/ds45/ds4dongle-bl618/fw_log.txt"
dur = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[4] = attrs[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, attrs)
bits = struct.pack("i", 0x002 | 0x004)  # TIOCM_DTR | TIOCM_RTS
try:
    fcntl.ioctl(fd, 0x5416, bits)        # TIOCMBIS
except Exception as e:
    sys.stderr.write("ioctl failed: %s\n" % e)

f = open(out, "wb")
deadline = time.time() + dur
while time.time() < deadline:
    try:
        d = os.read(fd, 4096)
        if d:
            f.write(d)
            f.flush()
    except BlockingIOError:
        pass
    time.sleep(0.03)
f.close()
os.close(fd)
sys.stderr.write("done\n")
