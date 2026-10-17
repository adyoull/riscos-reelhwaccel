#!/bin/bash
# tools/mmalprobe/build.sh [VERSION] - builds MMALProbe-VERSION.zip in dist/
# (mmalprobe for RISC OS, the Probe Obey file and the ReadMe).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
. "$TOP/common/toolchain.sh"
V=${1:-0.1}
TMP=$(mktemp -d)
mkdir "$TMP/MMALProbe"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -static \
  -o "$TMP/mmalprobe.elf" "$HERE/mmalprobe.c"
"$ELF2AIF" -e "$TMP/mmalprobe.elf" "$TMP/MMALProbe/mmalprobe,ff8" >/dev/null
[ -z "${KEEP_ELF:-}" ] || { mkdir -p "$KEEP_ELF"; cp "$TMP/mmalprobe.elf" "$KEEP_ELF/"; }   # for ci/checks.sh
cp "$HERE"/app/* "$TMP/MMALProbe/"
mkdir -p "$TOP/dist"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/MMALProbe-$V.zip" MMALProbe )
rm -rf "$TMP"
echo "dist/MMALProbe-$V.zip"
