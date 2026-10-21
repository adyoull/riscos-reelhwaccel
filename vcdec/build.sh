#!/bin/bash
# vcdec/build.sh OUTDIR - libvcdec.a for RISC OS (GCCSDK GCC 10 at CROSS,
# hard float as riscos-ffmpeg), with vcdec.h and vcenc.h beside it.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
. "$TOP/common/toolchain.sh"
O=${1:?usage: vcdec/build.sh OUTDIR}
mkdir -p "$O"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -Wextra -c -o "$O/vcdec.o" "$HERE/vcdec.c"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -Wextra -c -o "$O/vcenc.o" "$HERE/vcenc.c"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -Wextra -c -o "$O/vcmmal.o" "$HERE/vcmmal.c"
${CROSS}gcc -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -c -o "$O/vcdec_copy.o" "$HERE/vcdec_copy.S"
rm -f "$O/libvcdec.a"
${CROSS}ar rcs "$O/libvcdec.a" "$O/vcdec.o" "$O/vcenc.o" "$O/vcmmal.o" "$O/vcdec_copy.o"
cp "$HERE/vcdec.h" "$HERE/vcenc.h" "$O/"
