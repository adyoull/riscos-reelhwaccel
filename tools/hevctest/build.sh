#!/bin/bash
# tools/hevctest/build.sh [VERSION] - dist/HEVCTest-VERSION.zip: hevctest for
# RISC OS (with libhevcdec), HEVC test clips made here with the host's
# ffmpeg and libx265, their traces from tools/hevctrace (FFmpeg 5.1.10's HEVC
# decoder with the trace hooks: FFMPEG_TARBALL=.../ffmpeg-5.1.10.tar.xz), the
# Obey files and the ReadMe.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
V=${1:-0.1}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
ELF2AIF=${ELF2AIF:-$TOP/tools/elf2aif/elf2aif}
FFMPEG=${FFMPEG:-ffmpeg}
: "${FFMPEG_TARBALL:?FFMPEG_TARBALL (ffmpeg-5.1.10.tar.xz) is needed for the traces}"
TRACER=$("$TOP/tools/hevctrace/build.sh" "$FFMPEG_TARBALL" "$TOP/out/hevctrace" | tail -1 | sed 's/ (up to date)//')
TMP=$(mktemp -d)
mkdir -p "$TMP/HEVCTest/clips"
CROSS=$CROSS "$TOP/hevcdec/build.sh" "$TMP/lib"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -static -I"$TMP/lib" \
  -o "$TMP/hevctest.elf" "$HERE/hevctest.c" -L"$TMP/lib" -lhevcdec
"$ELF2AIF" -e "$TMP/hevctest.elf" "$TMP/HEVCTest/hevctest,ff8" >/dev/null
C="$TMP/HEVCTest/clips"
clip() {   # name size frames x265-params
  $FFMPEG -v error -y -f lavfi -i testsrc2=size=$2:rate=25 -frames:v $3 -c:v libx265 \
    -x265-params "log-level=error:$4" -pix_fmt yuv420p "$C/$1.mp4"
  HEVC_TRACE="$C/$1.trace" "$TRACER" -v error -threads 1 -i "$C/$1.mp4" -f framecrc - > /dev/null
  $FFMPEG -v error -y -i "$C/$1.mp4" -f rawvideo -pix_fmt yuv420p "$C/$1.yuv"
  python3 "$TOP/tools/hevctrace/hvtdump.py" --check "$C/$1.trace" "$C/$1.yuv" "$2" > /dev/null
  rm "$C/$1.yuv"
  mkdir "$C/$1"
  mv "$C/$1.trace" "$C/$1/trace"
  mv "$C/$1.mp4" "$C/$1/mp4"
}
clip small 352x288 20 "keyint=10:bframes=2"                         # WPP (x265's default), B frames
clip slices 416x240 12 "keyint=6:bframes=2:slices=4"                # four slices a picture
clip nowpp 416x240 12 "keyint=12:bframes=3:wpp=0:scaling-list=default"   # no WPP; scaling lists
clip odd 426x240 8 "keyint=4:bframes=0:wpp=0"                       # cropped (coded 432 wide)
clip hd 1920x1080 60 "keyint=30:bframes=3:crf=26"                   # 1080p
cp "$HERE"/app/* "$TMP/HEVCTest/"
mkdir -p "$TOP/dist"
rm -f "$TOP/dist/HEVCTest-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/HEVCTest-$V.zip" HEVCTest )
rm -rf "$TMP"
echo "dist/HEVCTest-$V.zip"
