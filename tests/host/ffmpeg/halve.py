#!/usr/bin/env python3
# halve.py IN.yuv WxH BITS OUT.yuv - each picture of IN (yuv420p, or
# yuv420p10le for BITS 10) halved as hevcdec_frame_to_i420_half does it:
# each output sample the rounded mean of a 2x2 block, 8-bit (10-bit:
# (sum + 8) >> 4); w/2 x h/2, U and V ((w/2)+1)/2 x ((h/2)+1)/2 (only
# whole blocks: the sizes used here have no part blocks).
import sys
import numpy as np
src, size, bits, dst = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
w, h = map(int, size.split('x'))
cw, ch = (w + 1) // 2, (h + 1) // 2
dt = np.uint16 if bits > 8 else np.uint8
raw = np.fromfile(src, dtype=dt)
fs = w * h + 2 * cw * ch
out = []
def halve(p, pw, ph, ow, oh):
    p = p[:2 * oh, :2 * ow].astype(np.uint32)
    s = p[0::2, 0::2] + p[0::2, 1::2] + p[1::2, 0::2] + p[1::2, 1::2]
    return ((s + 8) >> 4 if bits > 8 else (s + 2) >> 2).astype(np.uint8)
hw, hh = w // 2, h // 2
for i in range(len(raw) // fs):
    f = raw[i * fs:(i + 1) * fs]
    y = f[:w * h].reshape(h, w)
    u = f[w * h:w * h + cw * ch].reshape(ch, cw)
    v = f[w * h + cw * ch:].reshape(ch, cw)
    out.append(halve(y, w, h, hw, hh).tobytes())
    for c in (u, v):
        out.append(halve(c, cw, ch, (hw + 1) // 2, (hh + 1) // 2).tobytes())
open(dst, 'wb').write(b''.join(out))
