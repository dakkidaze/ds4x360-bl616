import re

text = open('/mnt/c/Users/dakki/AppData/Local/Temp/opencode/usbetl/tl2_full.txt',
            encoding='utf-8', errors='replace').read()

m = re.search(r'ConfigurationDescriptorLength[^0-9]*(\d+)', text)
print('len:', m.group(1) if m else '?')
b = [int(x) for x in re.findall(r'ConfigurationDescriptor[^0-9]*?>(\d+)<', text)]
print('bytes:', len(b))
print(' '.join('%02X' % x for x in b))
