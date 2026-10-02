#!/bin/bash
# tools/vcdectest/build.sh [VERSION] - builds VCDecTest-VERSION.zip in dist/:
# vcdectest for RISC OS (linked with libvcdec), three H.264 MP4 test clips
# made here with the host's ffmpeg and x264 (the same as MMALDecode's:
# 640x360 Main with B frames, 426x240 Baseline, 1920x1080 High) with
# FFmpeg's own per-picture checksums (-f framecrc) and block-mean
# signatures, the Obey files and the ReadMe.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
V=${1:-0.1}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
ELF2AIF=${ELF2AIF:-$TOP/tools/elf2aif/elf2aif}
FFMPEG=${FFMPEG:-ffmpeg}
TMP=$(mktemp -d)
mkdir -p "$TMP/VCDecTest/clips"
CROSS=$CROSS "$TOP/vcdec/build.sh" "$TMP/lib"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -static -I"$TMP/lib" \
  -o "$TMP/vcdectest.elf" "$HERE/vcdectest.c" -L"$TMP/lib" -lvcdec
"$ELF2AIF" -e "$TMP/vcdectest.elf" "$TMP/VCDecTest/vcdectest,ff8" >/dev/null
C="$TMP/VCDecTest/clips"
$FFMPEG -v error -y -f lavfi -i testsrc2=size=640x360:rate=25 -frames:v 60 -c:v libx264 -profile:v main -bf 2 -g 20 \
  -pix_fmt yuv420p "$C/small.mp4"
$FFMPEG -v error -y -f lavfi -i testsrc2=size=426x240:rate=25 -frames:v 30 -c:v libx264 -profile:v baseline -g 15 \
  -pix_fmt yuv420p "$C/odd.mp4"
$FFMPEG -v error -y -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 150 -c:v libx264 -profile:v high \
  -level 4.1 -crf 26 -bf 3 -g 50 -pix_fmt yuv420p "$C/hd.mp4"
for c in small odd hd; do      # (clips/NAME/...: no dots in RISC OS names)
  mkdir "$C/$c"
  mv "$C/$c.mp4" "$C/$c/mp4"
  $FFMPEG -v error -y -i "$C/$c/mp4" -f framecrc -pix_fmt yuv420p "$C/$c/crc"
  dims=$(sed -n 's/^#dimensions 0: //p' "$C/$c/crc")
  $FFMPEG -v error -i "$C/$c/mp4" -f rawvideo -pix_fmt yuv420p - | python3 "$TOP/tools/mmaldecode/mksig.py" ${dims%x*} ${dims#*x} > "$C/$c/sig"
done
cp "$HERE"/app/* "$TMP/VCDecTest/"
mkdir -p "$TOP/dist"
rm -f "$TOP/dist/VCDecTest-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/VCDecTest-$V.zip" VCDecTest )
rm -rf "$TMP"
echo "dist/VCDecTest-$V.zip"
