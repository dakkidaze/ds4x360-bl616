import re, subprocess

# Expected config bytes via the host-side assembly test
# Rebuild it to print bytes: the descriptor walk in tools/desc_test_cdc.c.
expected = None
# Instead, fetch the current config from the device and also print it fully.
out = subprocess.check_output(['/home/dakki/ds45/ds4dongle-bl618/tools/getdesc',
                               '/dev/bus/usb/001/020', '0x02', '0', '284'])
text = out.decode()
body = text.split('\n', 1)[1]
fw = [int(m, 16) for m in re.findall(r'\b([0-9a-f]{2})\b', body)]
print("fw config bytes:", len(fw))

# Walk descriptors and print
off = 0
while off < len(fw):
    ln = fw[off]
    if ln == 0:
        print("!! bLength=0 at", off)
        break
    t = fw[off+1] if off+1 < len(fw) else -1
    name = {0x02:'CFG',0x04:'INTF',0x05:'EP',0x21:'HID',0x24:'CS-IF',0x25:'CS-EP',0x0b:'IAD'}.get(t, '?')
    extra = ""
    if t == 0x04:
        extra = "intf=%d alt=%d nep=%d cls=%02x sub=%02x" % (fw[off+2], fw[off+3], fw[off+4], fw[off+5], fw[off+6])
    elif t == 0x05:
        extra = "ep=0x%02x attr=0x%02x mps=%d ivl=%d" % (fw[off+2], fw[off+3], fw[off+4]|(fw[off+5]<<8), fw[off+6])
    elif t == 0x21:
        extra = "bcd=%02x%02x country=%d ndesc=%d type=%02x wdlen=%d" % (fw[off+3],fw[off+2],fw[off+4],fw[off+5],fw[off+6],fw[off+7]|(fw[off+8]<<8))
    print("@%3d len=%2d %-5s %s" % (off, ln, name, extra))
    off += ln
print("end", off)
