#!/bin/bash
# devkit/build.sh [VERSION] - dist/riscos-reelhwaccel-devkit-VERSION.tgz: libvcdec.a and
# vcdec.h (GCCSDK GCC 10 at CROSS), the FFmpeg patch, README and COPYING.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
V=${1:-0.1}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
TMP=$(mktemp -d)
D=$TMP/riscos-reelhwaccel-devkit-$V
mkdir -p "$D/include" "$D/lib" "$D/ffmpeg"
CROSS=$CROSS "$TOP/vcdec/build.sh" "$TMP/build"
cp "$TMP/build/libvcdec.a" "$D/lib/"
cp "$TOP/vcdec/vcdec.h" "$D/include/"
cp "$TOP/ffmpeg/0001-avcodec-h264_vchiq.patch" "$D/ffmpeg/"
cp "$HERE/README.md" "$TOP/COPYING" "$D/"
mkdir -p "$TOP/dist"
( cd "$TMP" && tar --owner=0 --group=0 --sort=name --mtime=@0 -czf "$TOP/dist/riscos-reelhwaccel-devkit-$V.tgz" \
    "riscos-reelhwaccel-devkit-$V" )
rm -rf "$TMP"
echo "dist/riscos-reelhwaccel-devkit-$V.tgz"
