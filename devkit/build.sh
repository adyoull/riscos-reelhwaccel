#!/bin/bash
# devkit/build.sh [VERSION] - dist/riscos-reelhwaccel-devkit-VERSION.tgz: libvcdec.a and
# vcdec.h, libhevcdec.a with hwhevcdec.h and hevc_ctrls.h (GCCSDK GCC 10 at CROSS), the FFmpeg
# patches (0001 h264_vchiq, 0002 hevc_hwdec), README and COPYING.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
V=${1:-0.2.8}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
TMP=$(mktemp -d)
D=$TMP/riscos-reelhwaccel-devkit-$V
mkdir -p "$D/include" "$D/lib" "$D/ffmpeg"
CROSS=$CROSS "$TOP/vcdec/build.sh" "$TMP/build"
cp "$TMP/build/libvcdec.a" "$D/lib/"
cp "$TOP/vcdec/vcdec.h" "$D/include/"
cp "$TOP/ffmpeg/0001-avcodec-h264_vchiq.patch" "$TOP/ffmpeg/0002-avcodec-hevc_hwdec.patch" "$D/ffmpeg/"
CROSS=$CROSS "$TOP/hevcdec/build.sh" "$TMP/hbuild"
cp "$TMP/hbuild/libhevcdec.a" "$D/lib/"
cp "$TOP/hevcdec/hwhevcdec.h" "$TOP/hevcdec/hevc_ctrls.h" "$D/include/"
cp "$HERE/README.md" "$TOP/COPYING" "$D/"
# the packaged library must link on its own: both objects in it, a program built with GCCSDK
[ "$(${CROSS}ar t "$D/lib/libvcdec.a" | tr '\n' ' ')" = "vcdec.o vcdec_copy.o " ] ||
  { echo "devkit: libvcdec.a doesn't hold vcdec.o and vcdec_copy.o"; exit 1; }
printf '#include "vcdec.h"\nint main(void)\n{\n    vcdec *d;\n    vcdec_config c;\n    vcdec_config_init(&c);\n    return vcdec_open(&d, &c) == VCDEC_OK && vcdec_gpu_mem() > 0;\n}\n' > "$TMP/t.c"
${CROSS}gcc -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -I"$D/include" "$TMP/t.c" -L"$D/lib" -lvcdec -o "$TMP/t" ||
  { echo "devkit: a program doesn't link with the packaged libvcdec.a"; exit 1; }
[ "$(${CROSS}ar t "$D/lib/libhevcdec.a" | sort | tr '\n' ' ')" = "hevcdec.o hevcdec_conv.o hevcdec_hw.o hevcdec_svc.o rpivid_h265.o " ] ||
  { echo "devkit: libhevcdec.a doesn't hold its five objects"; exit 1; }
printf '#include "hwhevcdec.h"\nint main(void)\n{\n    hevcdec *d;\n    hevcdec_config c;\n    hevcdec_config_init(&c);\n    c.width = 1920; c.height = 1080;\n    if (hevcdec_open(&d, &c) != HEVCDEC_OK) return 1;\n    hevcdec_frame_wait(d, hevcdec_frame_new(d));\n    hevcdec_close(d);\n    return 0;\n}\n' > "$TMP/h.c"
${CROSS}gcc -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -I"$D/include" "$TMP/h.c" -L"$D/lib" -lhevcdec -o "$TMP/h" ||
  { echo "devkit: a program doesn't link with the packaged libhevcdec.a"; exit 1; }
mkdir -p "$TOP/dist"
( cd "$TMP" && tar --owner=0 --group=0 --sort=name --mtime=@0 -czf "$TOP/dist/riscos-reelhwaccel-devkit-$V.tgz" \
    "riscos-reelhwaccel-devkit-$V" )
rm -rf "$TMP"
echo "dist/riscos-reelhwaccel-devkit-$V.tgz"
