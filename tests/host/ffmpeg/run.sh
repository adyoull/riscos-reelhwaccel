#!/bin/bash
# tests/host/ffmpeg/run.sh OUTDIR - FFmpeg 5.1.10 with ffmpeg/0001-avcodec-h264_vchiq.patch,
# built for arm-linux (only what the tests need, no asm) and linked with vcdec (VCDEC_HOST) and
# the fake VCHIQ module and MMAL decoder (fake_vc.c); then under qemu:
#   vchiq_test     h264_vchiq through libavformat/libavcodec (see vchiq_test.c)
#   ffmpeg itself  -c:v h264_vchiq on the fake MP4 to framecrc: every checksum as expected,
#                  whole and with -ss (a seek before any picture), the fake left clean.
# Needs FFmpeg's tarball: FFMPEG_TARBALL=.../ffmpeg-5.1.10.tar.xz (sha256 208592f1...);
# skipped (and said so) without it. FFmpeg is rebuilt only when the patch changes.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
mkdir -p "${1:?usage: tests/host/ffmpeg/run.sh OUTDIR}"
O=$(cd "$1" && pwd)/ffmpeg
TAR=${FFMPEG_TARBALL:-}
PATCH=$TOP/ffmpeg/0001-avcodec-h264_vchiq.patch
if [ -z "$TAR" ] || [ ! -f "$TAR" ]; then
  echo "ffmpeg tests: skipped (set FFMPEG_TARBALL to ffmpeg-5.1.10.tar.xz)"
  exit 0
fi
[ "$(sha256sum < "$TAR" | cut -c1-16)" = 208592f16306932f ] || { echo "FAIL: $TAR isn't FFmpeg 5.1.10's tarball"; exit 1; }
mkdir -p "$O/lib"
bad=0
# the host libvcdec: vcdec, its copy routines and the fake, all in one
CC="arm-linux-gnueabihf-gcc -O1 -marm -mno-unaligned-access -DVCDEC_HOST -I$TOP/tests/host/fake -I$TOP/vcdec -Wall -Wno-unused-function"
$CC -c "$TOP/vcdec/vcdec.c" -o "$O/lib/vcdec.o" && $CC -c "$TOP/vcdec/vcdec_copy.S" -o "$O/lib/vcdec_copy.o" &&
  $CC -c "$TOP/tests/host/fake_vc.c" -o "$O/lib/fake_vc.o" || exit 1
rm -f "$O/lib/libvcdec.a"
arm-linux-gnueabihf-ar rcs "$O/lib/libvcdec.a" "$O/lib/"*.o
cp "$TOP/vcdec/vcdec.h" "$TOP/tests/host/fake_vc.h" "$O/lib/"
# FFmpeg, patched (again only if the patch has changed)
stamp=$(sha256sum < "$PATCH" | cut -c1-16)
if [ "$(cat "$O/stamp" 2>/dev/null)" != "$stamp" ]; then
  rm -rf "$O/src" "$O/build"
  mkdir -p "$O/src" "$O/build"
  tar xf "$TAR" -C "$O/src"
  ( cd "$O/src/ffmpeg-5.1.10" && patch -s -p1 < "$PATCH" ) || { echo "FAIL: the patch doesn't apply"; exit 1; }
  ( cd "$O/build" && "$O/src/ffmpeg-5.1.10/configure" --enable-cross-compile --arch=arm --target-os=linux \
      --cross-prefix=arm-linux-gnueabihf- --disable-asm --disable-doc --disable-everything --enable-gpl --enable-vchiq \
      --enable-decoder=h264,h264_vchiq --enable-demuxer=mov,h264 --enable-parser=h264 --enable-bsf=h264_mp4toannexb \
      --enable-protocol=file,pipe --enable-muxer=framecrc --enable-encoder=rawvideo,wrapped_avframe \
      --enable-filter=null,format,scale,buffer,buffersink --disable-ffplay --disable-ffprobe \
      --extra-cflags="-mno-unaligned-access -I$O/lib" --extra-ldflags="-L$O/lib -no-pie" > configure.log 2>&1 ) ||
    { echo "FAIL: FFmpeg's configure (see $O/build/configure.log)"; exit 1; }
  grep -q "CONFIG_H264_VCHIQ_DECODER 1" "$O/build/config_components.h" || { echo "FAIL: h264_vchiq not enabled by configure"; exit 1; }
  echo "$stamp" > "$O/stamp"
fi
# the patch's vchiqdec.c must be ffmpeg/vchiqdec.c (else: ffmpeg/mkpatch.sh)
cmp -s "$O/src/ffmpeg-5.1.10/libavcodec/vchiqdec.c" "$TOP/ffmpeg/vchiqdec.c" ||
  { echo "FAIL: ffmpeg/vchiqdec.c differs from the patch's (run ffmpeg/mkpatch.sh)"; bad=1; }
( cd "$O/build" && rm -f ffmpeg ffmpeg_g && make -j"$(nproc)" ffmpeg > make.log 2>&1 ) ||
  { echo "FAIL: building FFmpeg (see $O/build/make.log)"; exit 1; }
B=$O/build
arm-linux-gnueabihf-gcc -O1 -marm -mno-unaligned-access -Wall -I"$B" -I"$O/src/ffmpeg-5.1.10" -I"$O/lib" \
  -o "$O/vchiq_test" "$HERE/vchiq_test.c" "$B/libavformat/libavformat.a" "$B/libavcodec/libavcodec.a" \
  "$B/libswresample/libswresample.a" "$B/libavutil/libavutil.a" -L"$O/lib" -lvcdec -lm -lpthread -no-pie || exit 1
echo "== h264_vchiq in FFmpeg 5.1.10 (libavcodec's API; the ffmpeg command, whole and with -ss)"
"$TOP/tests/qemu/aligntrap.sh" "$O/vchiq_test" > "$O/vchiq_test.out" 2>&1 || bad=1
grep "FAIL\|vchiq_test:" "$O/vchiq_test.out"
# ffmpeg itself (the fake MP4 was written by vchiq_test)
want=$(awk -F', ' '!/^#/{print $6}' /tmp/mmaldecode_test4.crc | tr '\n' ' ')
for ss in "" 0.24; do
  FAKE_VC_REPORT=1 "$TOP/tests/qemu/aligntrap.sh" "$B/ffmpeg" -nostdin -loglevel quiet ${ss:+-ss $ss} -c:v h264_vchiq \
    -i /tmp/mmaldecode_test.mp4 -f framecrc - > "$O/ffmpeg$ss.crc" 2> "$O/ffmpeg$ss.err" || { echo "FAIL: ffmpeg $ss"; bad=1; }
  got=$(awk -F', ' '!/^#/{print $6}' "$O/ffmpeg$ss.crc" | tr '\n' ' ')
  exp=$want
  [ -n "$ss" ] && exp=$(echo "$want" | cut -d' ' -f7-)
  [ "$got" = "$exp" ] || { echo "FAIL: ffmpeg ${ss:+-ss $ss }-c:v h264_vchiq: $got"; bad=1; }
  grep -q "fake_vc: 0 failures" "$O/ffmpeg$ss.err" && ! grep -q FAIL "$O/ffmpeg$ss.err" ||
    { echo "FAIL: the fake after ffmpeg $ss:"; cat "$O/ffmpeg$ss.err"; bad=1; }
done
[ $bad = 0 ] && echo "ffmpeg tests: all passed" || echo "ffmpeg tests: FAILED"
exit $bad
