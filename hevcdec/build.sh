#!/bin/bash
# hevcdec/build.sh OUTDIR - libhevcdec.a for RISC OS (GCCSDK GCC 10 at CROSS,
# hard float), with hevcdec.h and hevc_ctrls.h beside it. rpivid_h265.c is
# Raspberry Pi's file as it is, so it's built as the kernel builds it
# (no warnings about signedness or unused parameters).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
O=${1:?usage: hevcdec/build.sh OUTDIR}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
CF="-O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall"
mkdir -p "$O"
${CROSS}gcc $CF -Wextra -I"$HERE" -c -o "$O/hevcdec.o" "$HERE/hevcdec.c"
${CROSS}gcc $CF -Wextra -I"$HERE" -c -o "$O/hevcdec_hw.o" "$HERE/hevcdec_hw.c"
${CROSS}gcc $CF -Wno-pointer-sign -Wno-sign-compare -Wno-unused-parameter -Wno-unused-variable -I"$HERE" -I"$HERE/shim" \
  -c -o "$O/rpivid_h265.o" "$HERE/rpivid_h265.c"
${CROSS}gcc -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -c -o "$O/hevcdec_svc.o" "$HERE/hevcdec_svc.S"
rm -f "$O/libhevcdec.a"
${CROSS}ar rcs "$O/libhevcdec.a" "$O/hevcdec.o" "$O/hevcdec_hw.o" "$O/rpivid_h265.o" "$O/hevcdec_svc.o"
cp "$HERE/hevcdec.h" "$HERE/hevc_ctrls.h" "$O/"
