#!/bin/bash
# tools/mmaldecode/build.sh [VERSION] - builds MMALDecode-VERSION.zip in dist/:
# mmaldecode for RISC OS, three H.264 test clips made here with the host's
# ffmpeg and x264 (640x360 Main with B frames, 426x240 Baseline, 1920x1080
# High), each as MP4 and as raw H.264, with FFmpeg's own per-picture
# checksums (-f framecrc) and block-mean signatures, the Obey files and
# the ReadMe.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
V=${1:-0.1}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
ELF2AIF=${ELF2AIF:-$TOP/tools/elf2aif/elf2aif}
FFMPEG=${FFMPEG:-ffmpeg}
TMP=$(mktemp -d)
mkdir -p "$TMP/MMALDecode/clips"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -static \
  -o "$TMP/mmaldecode.elf" "$HERE/mmaldecode.c"
"$ELF2AIF" -e "$TMP/mmaldecode.elf" "$TMP/MMALDecode/mmaldecode,ff8" >/dev/null
C="$TMP/MMALDecode/clips"
# Each clip is encoded straight to MP4 (so it has real pts: B frames in
# display order), then the same bitstream as raw H.264 (Annex B, FFmpeg's
# h264_mp4toannexb); FFmpeg's checksums are made from the MP4.
$FFMPEG -v error -y -f lavfi -i testsrc2=size=640x360:rate=25 -frames:v 60 -c:v libx264 -profile:v main -bf 2 -g 20 \
  -pix_fmt yuv420p "$C/small.mp4"
$FFMPEG -v error -y -f lavfi -i testsrc2=size=426x240:rate=25 -frames:v 30 -c:v libx264 -profile:v baseline -g 15 \
  -pix_fmt yuv420p "$C/odd.mp4"
$FFMPEG -v error -y -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 150 -c:v libx264 -profile:v high \
  -level 4.1 -crf 26 -bf 3 -g 50 -pix_fmt yuv420p "$C/hd.mp4"
for c in small odd hd; do      # (clips/NAME/...: no dots in RISC OS names)
  mkdir "$C/$c"
  mv "$C/$c.mp4" "$C/$c/mp4"
  $FFMPEG -v error -y -i "$C/$c/mp4" -c copy -bsf:v h264_mp4toannexb -f h264 "$C/$c/stream"
  $FFMPEG -v error -y -i "$C/$c/mp4" -f framecrc -pix_fmt yuv420p "$C/$c/crc"
  # the raw stream must decode to the same pictures
  cmp -s <($FFMPEG -v error -i "$C/$c/stream" -f framecrc -pix_fmt yuv420p - | awk -F', ' '!/^#/{print $6}') \
         <(awk -F', ' '!/^#/{print $6}' "$C/$c/crc") || { echo "mmaldecode: $c's raw stream decodes differently"; exit 1; }
  dims=$(sed -n 's/^#dimensions 0: //p' "$C/$c/crc")
  $FFMPEG -v error -i "$C/$c/mp4" -f rawvideo -pix_fmt yuv420p - | python3 "$HERE/mksig.py" ${dims%x*} ${dims#*x} > "$C/$c/sig"
done
cp "$HERE"/app/* "$TMP/MMALDecode/"
mkdir -p "$TOP/dist"
rm -f "$TOP/dist/MMALDecode-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/MMALDecode-$V.zip" MMALDecode )
rm -rf "$TMP"
echo "dist/MMALDecode-$V.zip"
