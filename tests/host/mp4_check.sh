#!/bin/bash
# tests/host/mp4_check.sh TESTBIN OUTDIR - mmaldecode's MP4 input against
# FFmpeg: real MP4s made here with x264 go through mmaldecode -x (the
# Annex B stream exactly as it would be sent to the VideoCore, and each
# sample's times); FFmpeg must decode that stream to the same pictures as
# the MP4, and the pts must be FFmpeg's, and the keyframes its.
# Needs the host's ffmpeg with libx264; skipped (and said so) without.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
BIN=$1
T=$2/mp4
mkdir -p "$T"
if ! ffmpeg -nostdin -v error -f lavfi -i testsrc2=size=64x64 -frames:v 1 -c:v libx264 -f null - 2>/dev/null; then
  echo "mp4_check: skipped (no ffmpeg with libx264)"
  exit 0
fi
bad=0
check() {        # name, then ffmpeg's encoding options
  local n=$1; shift
  ffmpeg -nostdin -v error -y -f lavfi -i testsrc2=size=${SIZE}:rate=25 -frames:v ${FRAMES} -c:v libx264 "$@" \
    -pix_fmt yuv420p "$T/$n.mp4" || { echo "FAIL: $n: ffmpeg"; bad=1; return; }
  "$TOP/tests/qemu/aligntrap.sh" "$BIN" -x "$T/$n.annexb" "$T/$n.mp4" > "$T/$n.log" || { echo "FAIL: $n: -x"; cat "$T/$n.log"; bad=1; return; }
  ffmpeg -nostdin -v error -y -i "$T/$n.mp4" -f framecrc -pix_fmt yuv420p "$T/$n.crc"
  # the pictures: the same checksums, in the same order
  ffmpeg -nostdin -v error -i "$T/$n.annexb" -f framecrc -pix_fmt yuv420p - | awk -F', ' '!/^#/{print $6}' > "$T/$n.got"
  awk -F', ' '!/^#/{print $6}' "$T/$n.crc" > "$T/$n.want"
  cmp -s "$T/$n.got" "$T/$n.want" || { echo "FAIL: $n: the Annex B stream decodes differently"; bad=1; }
  # the times: each sample's pts, sorted, are the pictures' pts (in microseconds)
  tb=$(sed -n 's/^#tb 0: //p' "$T/$n.crc")
  awk '{print $1}' "$T/$n.annexb.pts" | sort -n > "$T/$n.ptsgot"
  awk -F', ' -v tb="$tb" 'BEGIN{split(tb,a,"/")} !/^#/{printf "%d\n", $3*1000000*a[1]/a[2]}' "$T/$n.crc" > "$T/$n.ptswant"
  cmp -s "$T/$n.ptsgot" "$T/$n.ptswant" || { echo "FAIL: $n: pts differ from FFmpeg's"; bad=1; }
  # the keyframes: the samples FFmpeg calls key, in decode order
  ffprobe -v error -show_entries packet=flags -of csv=p=0 "$T/$n.mp4" | awk '{print substr($1,1,1)=="K"}' > "$T/$n.keywant"
  awk '{print $3}' "$T/$n.annexb.pts" > "$T/$n.keygot"
  cmp -s "$T/$n.keygot" "$T/$n.keywant" || { echo "FAIL: $n: keyframes differ from FFmpeg's"; bad=1; }
  echo "  $n: $(sed 's/,.*//' "$T/$n.log"), $(wc -l < "$T/$n.want") pictures, $(grep -c ' 1$' "$T/$n.annexb.pts") keyframes"
}
echo "== mp4_check (real MP4s through -x: the Annex B stream decodes as the MP4, pts and keyframes as FFmpeg's)"
SIZE=640x360 FRAMES=60 check main-b2 -profile:v main -bf 2 -g 20
SIZE=426x240 FRAMES=30 check baseline -profile:v baseline -g 15
SIZE=320x240 FRAMES=50 check high-pyramid -profile:v high -bf 3 -b_strategy 2 -x264-params b-pyramid=normal:keyint=24
SIZE=320x240 FRAMES=40 check faststart -profile:v high -bf 2 -g 10 -movflags +faststart
SIZE=320x240 FRAMES=40 check inband-ps -profile:v main -bf 2 -g 10 -x264-params repeat-headers=1
[ $bad = 0 ] && echo "mp4_check: all passed" || echo "mp4_check: FAILED"
exit $bad
