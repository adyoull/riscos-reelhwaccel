#!/bin/bash
# vcdec/build.sh OUTDIR - libvcdec.a for RISC OS (GCCSDK GCC 10 at CROSS,
# hard float as riscos-ffmpeg), with vcdec.h beside it.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
O=${1:?usage: vcdec/build.sh OUTDIR}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
mkdir -p "$O"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -Wextra -c -o "$O/vcdec.o" "$HERE/vcdec.c"
${CROSS}gcc -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -c -o "$O/vcdec_copy.o" "$HERE/vcdec_copy.S"
rm -f "$O/libvcdec.a"
${CROSS}ar rcs "$O/libvcdec.a" "$O/vcdec.o" "$O/vcdec_copy.o"
cp "$HERE/vcdec.h" "$O/"
