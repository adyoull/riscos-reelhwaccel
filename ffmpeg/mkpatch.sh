#!/bin/bash
# ffmpeg/mkpatch.sh FFMPEG_TARBALL - remakes 0001-avcodec-h264_vchiq.patch
# after vchiqdec.c has changed: the patch applied to FFmpeg 5.1.10, the new
# vchiqdec.c put in, the commit amended (its message and author kept).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TAR=${1:?usage: ffmpeg/mkpatch.sh ffmpeg-5.1.10.tar.xz}
T=$(mktemp -d)
tar xf "$TAR" -C "$T"
cd "$T"/ffmpeg-5.1.10
git init -q && git add -A && git -c user.name=base -c user.email=base commit -qm base
git -c user.name="Andrew Youll" -c user.email=andrewyoull86@gmail.com am -q "$HERE/0001-avcodec-h264_vchiq.patch"
cp "$HERE/vchiqdec.c" libavcodec/vchiqdec.c
git -c user.name="Andrew Youll" -c user.email=andrewyoull86@gmail.com commit -q -a --amend --no-edit
git format-patch -q -1 --stdout > "$HERE/0001-avcodec-h264_vchiq.patch"
rm -rf "$T"
echo "ffmpeg/0001-avcodec-h264_vchiq.patch remade"
