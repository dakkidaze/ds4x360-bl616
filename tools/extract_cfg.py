import csv

with open('/mnt/c/Users/dakki/AppData/Local/Temp/opencode/usbetl/dump2.csv') as f:
    rows = list(csv.reader(f))

found = 0
for r in rows:
    if len(r) > 10 and 'USBHUB3' in r[0] and r[8] == '43':
        vals = [int(c) for c in r if c.strip().isdigit()]
        if len(vals) < 220:
            continue
        cfg = vals
        print('total ints:', len(cfg))
        print(' '.join('%02X' % b for b in cfg))
        found += 1
        break
if not found:
    print('id=43 row not found')
