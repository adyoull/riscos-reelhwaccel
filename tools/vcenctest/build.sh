#!/bin/bash
# tools/vcenctest/build.sh [VERSION] - builds VCEncTest-VERSION.zip in dist/:
# vcenctest for RISC OS (linked with libvcdec, which holds vcenc), the Obey
# files and the ReadMe. The test pictures are made by the program itself.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
V=${1:-0.1}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
ELF2AIF=${ELF2AIF:-$TOP/tools/elf2aif/elf2aif}
TMP=$(mktemp -d)
mkdir -p "$TMP/VCEncTest"
CROSS=$CROSS "$TOP/vcdec/build.sh" "$TMP/lib"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -Wextra -static -I"$TMP/lib" \
  -o "$TMP/vcenctest.elf" "$HERE/vcenctest.c" -L"$TMP/lib" -lvcdec -lm
"$ELF2AIF" -e "$TMP/vcenctest.elf" "$TMP/VCEncTest/vcenctest,ff8" >/dev/null
cp "$HERE"/app/* "$TMP/VCEncTest/"
mkdir -p "$TOP/dist"
rm -f "$TOP/dist/VCEncTest-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/VCEncTest-$V.zip" VCEncTest )
rm -rf "$TMP"
echo "dist/VCEncTest-$V.zip"
