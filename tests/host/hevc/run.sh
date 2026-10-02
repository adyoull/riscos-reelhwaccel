#!/bin/bash
# tests/host/hevc/run.sh OUTDIR - hevcdec and tools/hevctest against the
# fake HEVC block (tests/host/fake_hevc.c), on traces of x265 clips made by
# tools/hevctrace (FFmpeg 5.1.10's HEVC decoder with the trace hooks).
# Needs FFMPEG_TARBALL and the host's ffmpeg with libx265; the refusal
# tests alone run without them.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
mkdir -p "${1:?usage: tests/host/hevc/run.sh OUTDIR}"
O=$(cd "$1" && pwd)/hevc
mkdir -p "$O"
bad=0
H=$TOP/tests/host
arm-linux-gnueabihf-gcc -O1 -marm -mno-unaligned-access -DPROBE_TEST -Wall -Wno-unused-function \
  -I"$TOP/hevcdec" -I"$TOP/hevcdec/shim" -I"$H" -no-pie -o "$O/hevcdec_test" "$H/hevcdec_test.c" "$H/fake_hevc.c" \
  "$TOP/hevcdec/hevcdec.c" "$TOP/tools/hevctest/hevctest.c" \
  -Wno-pointer-sign -Wno-sign-compare -Wno-unused-parameter -Wno-unused-variable "$TOP/hevcdec/rpivid_h265.c" || exit 1
args=()
if [ -n "${FFMPEG_TARBALL:-}" ] && ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx265; then
  T=$("$TOP/tools/hevctrace/build.sh" "$FFMPEG_TARBALL" "$TOP/out/hevctrace" | tail -1 | sed 's/ (up to date)//')
  clip() {   # name size frames x265-params
    [ -f "$O/$1.trace" ] && [ -f "$O/$1.yuv" ] && [ "$O/$1.trace" -nt "$T" ] && { args+=("$O/$1.trace" "$O/$1.yuv" "$2"); return; }
    ffmpeg -v error -y -f lavfi -i testsrc2=size=$2:rate=25 -frames:v $3 -c:v libx265 \
      -x265-params "log-level=error:$4" -pix_fmt yuv420p "$O/$1.mp4" &&
    HEVC_TRACE="$O/$1.trace" "$T" -v error -threads 1 -i "$O/$1.mp4" -f framecrc - > /dev/null &&
    ffmpeg -v error -y -i "$O/$1.mp4" -f rawvideo -pix_fmt yuv420p "$O/$1.yuv" &&
    python3 "$TOP/tools/hevctrace/hvtdump.py" --check "$O/$1.trace" "$O/$1.yuv" "$2" > /dev/null &&
    args+=("$O/$1.trace" "$O/$1.yuv" "$2") || { echo "FAIL: trace of $1"; bad=1; }
  }
  clip small 352x288 20 "keyint=10:bframes=2"
  clip slices 416x240 12 "keyint=6:bframes=2:slices=4"
  clip nowpp 416x240 12 "keyint=12:bframes=3:wpp=0:scaling-list=default"
  clip odd 426x240 8 "keyint=4:bframes=0:wpp=0"
else
  echo "hevc tests: traces skipped (set FFMPEG_TARBALL; needs the host's ffmpeg with libx265)"
fi
echo "== hevcdec_test (hevcdec and hevctest on traces, the fake HEVC block writing FFmpeg's pictures: bitstreams,"
echo "   references, frames, 128-byte columns; PU buffer grown; phases that never finish; refusals)"
"$TOP/tests/qemu/aligntrap.sh" "$O/hevcdec_test" "${args[@]}" > "$O/hevcdec_test.out" 2>&1 || bad=1
grep "^FAIL\|hevcdec_test:\|matched" "$O/hevcdec_test.out"
grep -q "hevcdec_test: all passed" "$O/hevcdec_test.out" || bad=1
exit $bad
