#!/usr/bin/env python3
"""hvtdump.py - reads a trace from tools/hevctrace (hevc_trace.c).

  hvtdump.py file.trace                 a line per picture
  hvtdump.py -v file.trace              and each slice
  hvtdump.py --check file.trace raw.yuv WxH
        checks every picture's Adler-32s against the pictures in raw.yuv
        (8-bit 4:2:0 from FFmpeg, display order): each picture traced must
        be one of them, and all of them must be traced

Part of riscos-reelhwaccel. GPL version 2.
"""
import struct, sys, zlib

SLICE_TYPES = {0: 'B', 1: 'P', 2: 'I'}


def read_trace(path):
    d = open(path, 'rb').read()
    if d[:4] != b'HVTR':
        raise SystemExit('%s: not a trace' % path)
    ver, s_sps, s_pps, s_sp, s_dec, s_sm = struct.unpack_from('<6I', d, 4)
    if ver != 1:
        raise SystemExit('%s: version %d' % (path, ver))
    o = 28
    pics = []
    while o < len(d):
        if d[o:o + 4] != b'PIC1':
            raise SystemExit('%s: bad record at %d' % (path, o))
        num, poc, w, h, depth, nsl, has_sm, ol, ot, ow, oh = struct.unpack_from('<IiIIIIIIIII', d, o + 4)
        o += 48
        sps = d[o:o + s_sps]; o += s_sps
        pps = d[o:o + s_pps]; o += s_pps
        dec = d[o:o + s_dec]; o += s_dec
        if has_sm:
            o += s_sm
        slices = []
        for _ in range(nsl):
            sp = d[o:o + s_sp]; o += s_sp
            n, = struct.unpack_from('<I', d, o); o += 4
            data = d[o:o + n]; o += (n + 3) & ~3
            bit_size, byte_off, n_ep, nal_type, tid1, stype = struct.unpack_from('<IIIBBB', sp, 0)
            seg, = struct.unpack_from('<I', sp, 36)
            slices.append(dict(bits=bit_size, off=byte_off, entries=n_ep, nal=nal_type, type=stype, addr=seg,
                               bytes=n, data=data))
        crc = struct.unpack_from('<3I', d, o); o += 12
        dpb_n = dec[8]
        pics.append(dict(num=num, poc=poc, w=w, h=h, depth=depth, window=(ol, ot, ow, oh), slices=slices, crc=crc, dpb=dpb_n,
                         scaling=bool(has_sm), flags=struct.unpack_from('<Q', dec, len(dec) - 8)[0]))
    return pics


def main():
    a = sys.argv[1:]
    verbose = '-v' in a
    a = [x for x in a if x != '-v']
    if a and a[0] == '--check':
        path, raw, size = a[1], a[2], a[3]
        w, h = map(int, size.split('x'))
        pics = read_trace(path)
        bps = 2 if pics and pics[0]['depth'] > 8 else 1    # (10-bit: the raw file is yuv420p10le)
        yn, cn = w * h * bps, ((w + 1) // 2) * ((h + 1) // 2) * bps
        fs = yn + 2 * cn
        r = open(raw, 'rb').read()
        want = []
        for i in range(len(r) // fs):
            f = r[i * fs:(i + 1) * fs]
            want.append((zlib.adler32(f[:yn]), zlib.adler32(f[yn:yn + cn]), zlib.adler32(f[yn + cn:])))
        got = [p['crc'] for p in pics]
        bad = [p['num'] for p in pics if p['crc'] not in want]
        missing = [i for i, c in enumerate(want) if c not in got]
        print('%d pictures traced, %d decoded; %d not among them, %d not traced' % (len(pics), len(want), len(bad),
                                                                                   len(missing)))
        sys.exit(1 if bad or missing or not pics else 0)
    pics = read_trace(a[0])
    for p in pics:
        types = ''.join(SLICE_TYPES.get(s['type'], '?') for s in p['slices'])
        print('#%d poc %d %dx%d (shown %dx%d at %d,%d) %d-bit, %d slice(s) %s, %d in the DPB%s%s, crc %08x %08x %08x' % (
            p['num'], p['poc'], p['w'], p['h'], p['window'][2], p['window'][3], p['window'][0], p['window'][1], p['depth'], len(p['slices']), types, p['dpb'],
            ', IRAP' if p['flags'] & 1 else '', ', IDR' if p['flags'] & 2 else '', *p['crc']))
        if verbose:
            for s in p['slices']:
                print('    NAL %d, segment at CTB %d, %d bytes, data from byte %d, %d entry points' % (
                    s['nal'], s['addr'], s['bytes'], s['off'], s['entries']))
    print('%d pictures' % len(pics))


if __name__ == '__main__':
    main()
