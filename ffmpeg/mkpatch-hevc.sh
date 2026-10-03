#!/bin/bash
# ffmpeg/mkpatch-hevc.sh FFMPEG_TARBALL - remakes 0002-avcodec-hevc_hwdec.patch
# after hevc_hwdec.c has changed: FFmpeg 5.1.10 with 0001 (h264_vchiq) and
# 0002 applied, the new hevc_hwdec.c put in, 0002's commit amended (its
# message and author kept).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TAR=${1:?usage: ffmpeg/mkpatch-hevc.sh ffmpeg-5.1.10.tar.xz}
T=$(mktemp -d)
tar xf "$TAR" -C "$T"
cd "$T"/ffmpeg-5.1.10
git init -q && git add -A && git -c user.name=base -c user.email=base commit -qm base
git -c user.name="Andrew Youll" -c user.email=andrewyoull86@gmail.com am -q "$HERE/0001-avcodec-h264_vchiq.patch"
git -c user.name="Andrew Youll" -c user.email=andrewyoull86@gmail.com am -q "$HERE/0002-avcodec-hevc_hwdec.patch"
cp "$HERE/hevc_hwdec.c" libavcodec/hevc_hwdec.c
git -c user.name="Andrew Youll" -c user.email=andrewyoull86@gmail.com commit -q -a --amend --no-edit
git format-patch -q -1 --stdout > "$HERE/0002-avcodec-hevc_hwdec.patch"
rm -rf "$T"
echo "ffmpeg/0002-avcodec-hevc_hwdec.patch remade"
