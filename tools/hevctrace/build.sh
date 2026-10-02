#!/bin/bash
# tools/hevctrace/build.sh FFMPEG_TARBALL [OUTDIR] - a host (x86-64 Linux)
# ffmpeg with 0001-hevc-trace.patch: only what tracing HEVC needs (the HEVC
# decoder, MP4 and raw HEVC in, framecrc and null out). OUTDIR/ffmpeg.
# Rebuilt only when the patch changes.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
TAR=${1:?usage: tools/hevctrace/build.sh ffmpeg-5.1.10.tar.xz [OUTDIR]}
O=${2:-$TOP/out/hevctrace}
mkdir -p "$O"
O=$(cd "$O" && pwd)
STAMP=$(cat "$HERE/0001-hevc-trace.patch" "$0" | sha256sum | cut -c1-16)
if [ -x "$O/ffmpeg" ] && [ "$(cat "$O/stamp" 2>/dev/null)" = "$STAMP" ]; then echo "$O/ffmpeg (up to date)"; exit 0; fi
rm -rf "$O/src" && mkdir -p "$O/src"
tar xf "$TAR" -C "$O/src"
cd "$O/src/ffmpeg-5.1.10"
patch -p1 -s < "$HERE/0001-hevc-trace.patch"
./configure --disable-everything --disable-doc --disable-asm --disable-autodetect --disable-network \
  --enable-decoder=hevc --enable-parser=hevc --enable-demuxer=mov,hevc --enable-muxer=framecrc,null,rawvideo \
  --enable-encoder=rawvideo --enable-protocol=file,pipe --enable-filter=null,format,scale,copy --enable-swscale > "$O/configure.log" 2>&1
make -j"$(nproc)" ffmpeg > "$O/make.log" 2>&1
cp ffmpeg "$O/ffmpeg"
echo "$STAMP" > "$O/stamp"
echo "$O/ffmpeg"
