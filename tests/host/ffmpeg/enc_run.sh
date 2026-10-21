#!/bin/bash
# tests/host/ffmpeg/enc_run.sh OUTDIR - FFmpeg 5.1.10 with ffmpeg/0001 and
# 0003-avcodec-h264_encvchiq.patch, built for arm-linux (only what the tests need, no
# asm) and linked with vcdec and vcenc (VCDEC_HOST) and the fake VCHIQ module and MMAL encoder
# (fake_vcenc_lib.c); then under qemu:
#   vchiqenc_test  the h264_encvchiq encoder through libavcodec (see vchiqenc_test.c)
#   ffmpeg itself  raw YUV to -c:v h264_encvchiq -profile:v main, to a raw .h264 stream (the SPS and PPS in front)
#                  and to MP4 (global headers: an avcC), the fake left clean.
# Needs FFMPEG_TARBALL as run.sh; FFmpeg is rebuilt only when a patch changes.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
mkdir -p "${1:?usage: tests/host/ffmpeg/enc_run.sh OUTDIR}"
O=$(cd "$1" && pwd)/ffmpeg-enc
TAR=${FFMPEG_TARBALL:-}
P1=$TOP/ffmpeg/0001-avcodec-h264_vchiq.patch
P3=$TOP/ffmpeg/0003-avcodec-h264_encvchiq.patch
if [ -z "$TAR" ] || [ ! -f "$TAR" ]; then
  echo "ffmpeg encoder tests: skipped (set FFMPEG_TARBALL to ffmpeg-5.1.10.tar.xz)"
  exit 0
fi
[ "$(sha256sum < "$TAR" | cut -c1-16)" = 208592f16306932f ] || { echo "FAIL: $TAR isn't FFmpeg 5.1.10's tarball"; exit 1; }
mkdir -p "$O/lib"
bad=0
# the host libvcdec: vcdec, vcenc, the copy routines and the fake encoder, all in one
CC="arm-linux-gnueabihf-gcc -O1 -marm -mno-unaligned-access -DVCDEC_HOST -I$TOP/tests/host/fake -I$TOP/vcdec -Wall -Wno-unused-function"
$CC -c "$TOP/vcdec/vcdec.c" -o "$O/lib/vcdec.o" && $CC -c "$TOP/vcdec/vcenc.c" -o "$O/lib/vcenc.o" &&
  $CC -c "$TOP/vcdec/vcmmal.c" -o "$O/lib/vcmmal.o" &&
  $CC -c "$TOP/vcdec/vcdec_copy.S" -o "$O/lib/vcdec_copy.o" &&
  $CC -c "$TOP/tests/host/fake_vcenc_lib.c" -o "$O/lib/fake_vcenc_lib.o" || exit 1
rm -f "$O/lib/libvcdec.a"
arm-linux-gnueabihf-ar rcs "$O/lib/libvcdec.a" "$O/lib/"*.o
cp "$TOP/vcdec/vcdec.h" "$TOP/vcdec/vcenc.h" "$TOP/tests/host/fake_vcenc.h" "$O/lib/"
stamp=$(cat "$P1" "$P3" | sha256sum | cut -c1-16)
if [ "$(cat "$O/stamp" 2>/dev/null)" != "$stamp" ]; then
  rm -rf "$O/src" "$O/build"
  mkdir -p "$O/src" "$O/build"
  tar xf "$TAR" -C "$O/src"
  ( cd "$O/src/ffmpeg-5.1.10" && patch -s -p1 < "$P1" && patch -s -p1 < "$P3" ) || { echo "FAIL: the patches don't apply"; exit 1; }
  ( cd "$O/build" && "$O/src/ffmpeg-5.1.10/configure" --enable-cross-compile --arch=arm --target-os=linux \
      --cross-prefix=arm-linux-gnueabihf- --disable-asm --disable-doc --disable-everything --enable-gpl --enable-vchiq \
      --enable-encoder=h264_encvchiq --enable-decoder=rawvideo --enable-demuxer=rawvideo --enable-muxer=h264,mp4 \
      --enable-protocol=file,pipe --enable-filter=null,format,scale,buffer,buffersink --disable-ffplay --disable-ffprobe \
      --extra-cflags="-mno-unaligned-access -I$O/lib" --extra-ldflags="-L$O/lib -no-pie" > configure.log 2>&1 ) ||
    { echo "FAIL: FFmpeg's configure (see $O/build/configure.log)"; exit 1; }
  grep -q "CONFIG_H264_ENCVCHIQ_ENCODER 1" "$O/build/config_components.h" || { echo "FAIL: the h264_encvchiq encoder not enabled by configure"; exit 1; }
  echo "$stamp" > "$O/stamp"
fi
cmp -s "$O/src/ffmpeg-5.1.10/libavcodec/vchiqenc.c" "$TOP/ffmpeg/vchiqenc.c" ||
  { echo "FAIL: ffmpeg/vchiqenc.c differs from the patch's (run ffmpeg/mkpatch-enc.sh)"; bad=1; }
( cd "$O/build" && rm -f ffmpeg ffmpeg_g && make -j"$(nproc)" ffmpeg > make.log 2>&1 ) ||
  { echo "FAIL: building FFmpeg (see $O/build/make.log)"; exit 1; }
B=$O/build
arm-linux-gnueabihf-gcc -O1 -marm -mno-unaligned-access -Wall -I"$B" -I"$O/src/ffmpeg-5.1.10" -I"$O/lib" \
  -o "$O/vchiqenc_test" "$HERE/vchiqenc_test.c" "$B/libavcodec/libavcodec.a" "$B/libavutil/libavutil.a" \
  -L"$O/lib" -lvcdec -lm -lpthread -no-pie || exit 1
echo "== the h264_encvchiq encoder in FFmpeg 5.1.10 (libavcodec's API; the ffmpeg command to .h264 and MP4)"
"$TOP/tests/qemu/aligntrap.sh" "$O/vchiqenc_test" > "$O/vchiqenc_test.out" 2>&1 || bad=1
grep "FAIL\|vchiqenc_test:" "$O/vchiqenc_test.out"
# ffmpeg itself: 30 frames of raw 64x48 YUV
python3 -c "
import sys
f = open('$O/in.yuv', 'wb')
for k in range(30):
    f.write(bytes((x * 3 + y * 5 + k * 9) & 255 for y in range(48) for x in range(64)))
    f.write(bytes((x + k * 4 + 60) & 255 for y in range(24) for x in range(32)) * 2)
"
for fmt in h264 mp4; do
  FAKE_VCENC_REPORT=1 "$TOP/tests/qemu/aligntrap.sh" "$B/ffmpeg" -nostdin -loglevel error -f rawvideo -pix_fmt yuv420p \
    -s 64x48 -r 25 -i "$O/in.yuv" -c:v h264_encvchiq -g 10 -b:v 1M -profile:v main -y "$O/out.$fmt" 2> "$O/ffmpeg-$fmt.err" ||
    { echo "FAIL: ffmpeg to $fmt:"; cat "$O/ffmpeg-$fmt.err"; bad=1; }
  grep -q "fake_vcenc: 0 failures, 30 pictures, padding wrong 0" "$O/ffmpeg-$fmt.err" && ! grep -q FAIL "$O/ffmpeg-$fmt.err" ||
    { echo "FAIL: the fake after ffmpeg to $fmt:"; cat "$O/ffmpeg-$fmt.err"; bad=1; }
done
python3 - "$O/out.h264" "$O/out.mp4" <<'PY' || bad=1
import sys
h = open(sys.argv[1], 'rb').read()
units = h.split(b'\x00\x00\x00\x01')[1:]
types = [u[0] & 31 for u in units]
ok = types[:2] == [7, 8] and units[0][1] == 26 and types[2:].count(5) == 3 and types[2:].count(1) == 27 and len(types) == 32
m = open(sys.argv[2], 'rb').read()
ok2 = b'avcC' in m and b'mdat' in m and b'\x00\x00\x00\x01\x67' not in m[m.index(b'mdat'):]
print('ffmpeg: .h264 %s (%d units), MP4 %s' % ('right' if ok else 'WRONG', len(types), 'right (avcC, no start codes)' if ok2 else 'WRONG'))
sys.exit(0 if ok and ok2 else 1)
PY
[ $bad = 0 ] && echo "ffmpeg encoder tests: all passed" || echo "ffmpeg encoder tests: FAILED"
exit $bad
