#!/bin/bash
# tools/hevctrace/mkpatch.sh FFMPEG_TARBALL - remakes 0001-hevc-trace.patch:
# FFmpeg 5.1.10 with hevc_trace.c/.h and hevcdec/hevc_ctrls.h put into
# libavcodec and the two hooks in hevcdec.c.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
TAR=${1:?usage: tools/hevctrace/mkpatch.sh ffmpeg-5.1.10.tar.xz}
T=$(mktemp -d)
tar xf "$TAR" -C "$T"
cd "$T"/ffmpeg-5.1.10
git init -q && git add -A && git -c user.name=base -c user.email=base commit -qm base
cp "$HERE/hevc_trace.c" "$HERE/hevc_trace.h" "$TOP/hevcdec/hevc_ctrls.h" libavcodec/
python3 - <<'PY'
p='libavcodec/hevcdec.c'; s=open(p).read()
def rep(a,b):
    global s
    assert s.count(a)==1,a; s=s.replace(a,b)
rep('#include "hevcdec.h"\n','#include "hevcdec.h"\n#include "hevc_trace.h"\n')
rep('''        if (s->avctx->hwaccel) {
            ret = s->avctx->hwaccel->decode_slice(s->avctx, nal->raw_data, nal->raw_size);''','''        ff_hevc_trace_slice(s, nal->raw_data, nal->raw_size);
        if (s->avctx->hwaccel) {
            ret = s->avctx->hwaccel->decode_slice(s->avctx, nal->raw_data, nal->raw_size);''')
rep('''    s->ref = NULL;
    ret    = decode_nal_units(s, avpkt->data, avpkt->size);
    if (ret < 0)
        return ret;
''','''    s->ref = NULL;
    ret    = decode_nal_units(s, avpkt->data, avpkt->size);
    if (ret < 0)
        return ret;
    ff_hevc_trace_end(s);
''')
open(p,'w').write(s)
p='libavcodec/Makefile'; s=open(p).read()
a='OBJS-$(CONFIG_HEVC_DECODER)            += hevcdec.o'
assert a in s
s=s.replace(a,a+' hevc_trace.o',1)
open(p,'w').write(s)
PY
git add -A
git -c user.name="Andrew Youll" -c user.email=andrewyoull86@gmail.com commit -qm "avcodec/hevc: trace for hardware decoder tests (riscos-reelhwaccel's hevctrace)

With HEVC_TRACE set, the HEVC decoder writes each picture's V4L2 stateless
HEVC controls, its slices and Adler-32s of the decoded picture to that
file. Decode with one thread."
git format-patch -q -1 --stdout > "$HERE/0001-hevc-trace.patch"
rm -rf "$T"
echo "tools/hevctrace/0001-hevc-trace.patch remade"
