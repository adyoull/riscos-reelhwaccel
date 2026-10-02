#!/bin/bash
# devkit/build.sh [VERSION] - dist/riscos-reelhwaccel-devkit-VERSION.tgz: libvcdec.a and
# vcdec.h (GCCSDK GCC 10 at CROSS), the FFmpeg patch, README and COPYING.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
V=${1:-0.2}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
TMP=$(mktemp -d)
D=$TMP/riscos-reelhwaccel-devkit-$V
mkdir -p "$D/include" "$D/lib" "$D/ffmpeg"
CROSS=$CROSS "$TOP/vcdec/build.sh" "$TMP/build"
cp "$TMP/build/libvcdec.a" "$D/lib/"
cp "$TOP/vcdec/vcdec.h" "$D/include/"
cp "$TOP/ffmpeg/0001-avcodec-h264_vchiq.patch" "$D/ffmpeg/"
cp "$HERE/README.md" "$TOP/COPYING" "$D/"
# the packaged library must link on its own: both objects in it, a program built with GCCSDK
[ "$(${CROSS}ar t "$D/lib/libvcdec.a" | tr '\n' ' ')" = "vcdec.o vcdec_copy.o " ] ||
  { echo "devkit: libvcdec.a doesn't hold vcdec.o and vcdec_copy.o"; exit 1; }
printf '#include "vcdec.h"\nint main(void)\n{\n    vcdec *d;\n    vcdec_config c;\n    vcdec_config_init(&c);\n    return vcdec_open(&d, &c) == VCDEC_OK && vcdec_gpu_mem() > 0;\n}\n' > "$TMP/t.c"
${CROSS}gcc -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -I"$D/include" "$TMP/t.c" -L"$D/lib" -lvcdec -o "$TMP/t" ||
  { echo "devkit: a program doesn't link with the packaged libvcdec.a"; exit 1; }
mkdir -p "$TOP/dist"
( cd "$TMP" && tar --owner=0 --group=0 --sort=name --mtime=@0 -czf "$TOP/dist/riscos-reelhwaccel-devkit-$V.tgz" \
    "riscos-reelhwaccel-devkit-$V" )
rm -rf "$TMP"
echo "dist/riscos-reelhwaccel-devkit-$V.tgz"
