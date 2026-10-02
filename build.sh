#!/bin/bash
# build.sh - builds every Pi test zip into dist/ (needs GCCSDK GCC 10 at
# CROSS, tools/elf2aif built - make -C tools/elf2aif GCCSDK_SRC=... - and,
# for MMALDecode's clips, the host's ffmpeg with libx264).
# The versions are the current ones; set them to build others, e.g.
#   MMALDECODE=0.13 ./build.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/tools/vchiqprobe/build.sh" "${VCHIQPROBE:-0.1}"
"$HERE/tools/mmalprobe/build.sh" "${MMALPROBE:-0.1}"
"$HERE/tools/mmaldecode/build.sh" "${MMALDECODE:-0.17}"
"$HERE/tools/vcdectest/build.sh" "${VCDECTEST:-0.3.1}"
"$HERE/tools/hevcprobe/build.sh" "${HEVCPROBE:-0.1}"
"$HERE/hevchw/build.sh" "${HEVCHW:-0.1}"
if [ -n "${FFMPEG_TARBALL:-}" ]; then "$HERE/tools/hevctest/build.sh" "${HEVCTEST:-0.1.4}"
else echo "HEVCTest: skipped (set FFMPEG_TARBALL for its traces)"; fi
"$HERE/devkit/build.sh" "${DEVKIT:-0.2.1}"
