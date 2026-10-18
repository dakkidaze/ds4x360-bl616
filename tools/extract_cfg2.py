import re

with open('/mnt/c/Users/dakki/AppData/Local/Temp/opencode/usbetl/tl2_full.txt', 'r', encoding='utf-8', errors='replace') as f:
    text = f.read()

m = re.search(r'fid_ConfigurationDescriptorLength=(\d+)', text)
print('cfg len field:', m.group(1) if m else 'NOT FOUND')

bytes_hex = []
for mm in re.finditer(r'fid_ConfigurationDescriptor=(\d+);?', text):
    bytes_hex.append(int(mm.group(1)))
print('collected bytes:', len(bytes_hex))
if bytes_hex:
    print(' '.join('%02X' % b for b in bytes_hex))
