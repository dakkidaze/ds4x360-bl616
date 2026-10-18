import re, sys
src = open('/home/dakki/ds45/ds4dongle-bl618/src/ds4_usb_audio_desc.h').read()
body = src.split('audio_desc[DS4_AUDIO_DESC_SIZE] = {',1)[1].split('};',1)[0]
toks = re.findall(r'0x[0-9A-Fa-f]{1,2}|DS4_AUDIO_EP_OUT', body)
vals = []
for t in toks:
    if t == 'DS4_AUDIO_EP_OUT':
        vals.append(0x01)
    else:
        vals.append(int(t, 16))
print('total bytes:', len(vals))
i = 0
while i < len(vals):
    ln = vals[i]
    typ = vals[i+1] if i+1 < len(vals) else 0
    print(f'[{i:3d}] len={ln:2d} type=0x{typ:02x}  ' + ' '.join(f'{b:02x}' for b in vals[i:i+ln]))
    if ln == 0:
        break
    i += ln
