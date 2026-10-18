import re, subprocess, sys

# Extract Pico DS4Dongle report descriptor bytes (comments stripped)
pico_lines = []
with open('/mnt/c/Users/dakki/ds45/DS4Dongle/src/usb_descriptors.cpp') as f:
    lines = f.readlines()
# array starts at line 532 (1-indexed) "uint8_t const desc_hid_report_ds4[] = {"
start = None
for i, ln in enumerate(lines):
    if 'desc_hid_report_ds4[] = {' in ln:
        start = i
        break
assert start is not None
pico = []
for ln in lines[start+1:]:
    ln = ln.split('//')[0]
    if '};' in ln:
        break
    for m in re.findall(r'0x([0-9A-Fa-f]{2})', ln):
        pico.append(int(m, 16))

# Fetch firmware-provided report descriptor via getdesc
out = subprocess.check_output(['/home/dakki/ds45/ds4dongle-bl618/tools/getdesc',
                               '/dev/bus/usb/001/018', '0x22', '0', '507'])
text = out.decode()
fw = [int(m, 16) for m in re.findall(r'\b([0-9a-f]{2})\b', text.split('\n', 1)[1])]

print("pico bytes:", len(pico))
print("fw bytes:", len(fw))
if len(pico) != len(fw):
    print("LENGTH MISMATCH")
n = min(len(pico), len(fw))
diffs = [(i, pico[i], fw[i]) for i in range(n) if pico[i] != fw[i]]
print("diff count:", len(diffs))
for d in diffs[:40]:
    print("  offset %d: pico=%02X fw=%02X" % d)
