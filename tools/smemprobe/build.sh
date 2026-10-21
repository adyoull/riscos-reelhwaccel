#!/bin/bash
# tools/smemprobe/build.sh [VERSION] - builds SMEMProbe-VERSION.zip in dist/:
# smemprobe for RISC OS (linked with libvcdec), two H.264 clips made here
# with the host's ffmpeg and x264 (Annex B, an access unit delimiter before
# each picture, no B pictures: 640x360 Main, 30 pictures; 1920x1080 High,
# 120 pictures) with FFmpeg's own checksums (framecrc) and block-mean
# signatures, the Obey files and the ReadMe.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
. "$TOP/common/toolchain.sh"
V=${1:-0.1}
FFMPEG=${FFMPEG:-ffmpeg}
TMP=$(mktemp -d)
mkdir -p "$TMP/SMEMProbe/clips"
CROSS=$CROSS "$TOP/vcdec/build.sh" "$TMP/lib"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -Wextra -static \
  -I"$TMP/lib" -I"$TOP/vcdec" -o "$TMP/smemprobe.elf" "$HERE/smemprobe.c" -L"$TMP/lib" -lvcdec
"$ELF2AIF" -e "$TMP/smemprobe.elf" "$TMP/SMEMProbe/smemprobe,ff8" >/dev/null
[ -z "${KEEP_ELF:-}" ] || { mkdir -p "$KEEP_ELF"; cp "$TMP/smemprobe.elf" "$KEEP_ELF/"; }   # for ci/build.sh
C="$TMP/SMEMProbe/clips"
$FFMPEG -v error -y -f lavfi -i testsrc2=size=640x360:rate=25 -frames:v 30 -c:v libx264 -profile:v main -bf 0 -g 15 \
  -x264-params aud=1 -pix_fmt yuv420p -f h264 "$C/small.h264"
$FFMPEG -v error -y -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 120 -c:v libx264 -profile:v high \
  -level 4.1 -crf 26 -bf 0 -g 50 -x264-params aud=1 -pix_fmt yuv420p -f h264 "$C/hd.h264"
for c in small hd; do      # (clips/NAME/...: no dots in RISC OS names)
  mkdir "$C/$c"
  mv "$C/$c.h264" "$C/$c/h264"
  $FFMPEG -v error -y -i "$C/$c/h264" -f framecrc -pix_fmt yuv420p "$C/$c/crc"
  dims=$(sed -n 's/^#dimensions 0: //p' "$C/$c/crc")
  $FFMPEG -v error -i "$C/$c/h264" -f rawvideo -pix_fmt yuv420p - | python3 "$TOP/tools/mmaldecode/mksig.py" ${dims%x*} ${dims#*x} > "$C/$c/sig"
done
cp "$HERE"/app/* "$TMP/SMEMProbe/"
mkdir -p "$TOP/dist"
rm -f "$TOP/dist/SMEMProbe-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/SMEMProbe-$V.zip" SMEMProbe )
rm -rf "$TMP"
echo "dist/SMEMProbe-$V.zip"
