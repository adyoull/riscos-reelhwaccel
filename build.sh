#!/bin/bash
# build.sh - builds every Pi test zip and the devkit into dist/.
#
# The toolchain: riscos-crossdev's ($RISCOS_TOOLCHAIN, as in its container
# image, which GitHub builds in), or a GCCSDK environment (common/toolchain.sh).
# Some test zips carry clips made here, and are skipped (and said so)
# without what makes them:
#   MMALDecode, VCDecTest, SMEMProbe  the host's ffmpeg with libx264 (H.264 clips)
#   HEVCTest               FFMPEG_TARBALL (ffmpeg-5.1.10.tar.xz, for its
#                          traces) and the host's ffmpeg with libx265
# The versions are the current ones; set them to build others, e.g.
#   MMALDECODE=0.13 ./build.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$HERE
. "$TOP/common/toolchain.sh"
FFMPEG=${FFMPEG:-ffmpeg}
has_encoder() { "$FFMPEG" -hide_banner -encoders 2>/dev/null | grep -q " $1 "; }
"$HERE/tools/vchiqprobe/build.sh" "${VCHIQPROBE:-0.1}"
"$HERE/tools/mmalprobe/build.sh" "${MMALPROBE:-0.1}"
if has_encoder libx264; then
  "$HERE/tools/mmaldecode/build.sh" "${MMALDECODE:-0.17}"
  "$HERE/tools/vcdectest/build.sh" "${VCDECTEST:-0.3.7}"
else
  echo "MMALDecode, VCDecTest: skipped (their clips need $FFMPEG with libx264)"
fi
"$HERE/tools/vcenctest/build.sh" "${VCENCTEST:-0.1.3}"
if has_encoder libx264; then "$HERE/tools/smemprobe/build.sh" "${SMEMPROBE:-0.1.1}"
else echo "SMEMProbe: skipped (its clips need $FFMPEG with libx264)"; fi
"$HERE/tools/hevcprobe/build.sh" "${HEVCPROBE:-0.1}"
"$HERE/hevchw/build.sh" "${HEVCHW:-0.1}"
if [ -n "${FFMPEG_TARBALL:-}" ] && has_encoder libx265; then "$HERE/tools/hevctest/build.sh" "${HEVCTEST:-0.1.14}"
else echo "HEVCTest: skipped (its traces need FFMPEG_TARBALL and $FFMPEG with libx265)"; fi
"$HERE/devkit/build.sh" "${DEVKIT:-0.2.18}"
