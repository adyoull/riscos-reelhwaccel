#!/bin/bash
# tests/host/ffmpeg/hevc_run.sh OUTDIR - FFmpeg 5.1.10 with ffmpeg/0001 (h264_vchiq) and
# 0002-avcodec-hevc_hwdec.patch, built for arm-linux (only what the tests need, no asm) and
# linked with hevcdec and the fake HEVC block (tests/host/fake_hevc.c, given its pictures by
# hevc_fake_env.c); then, under qemu, the ffmpeg command with -c:v hevc_hwdec on the clips
# tests/host/hevc/run.sh made (OUTDIR/hevc: each MP4, its trace and FFmpeg's decode):
#   every picture exactly FFmpeg's, pipelined (phase 2 slow) and not, frames cacheable and not;
#   with -ss, the same frames as the hevc decoder's; a raw .hevc stream (no extradata);
#   10-bit clips (YUV420P10 out, or with -output_8bit 1 YUV420P, each sample's top 8 bits), 4K;
#   4:2:2, 12-bit and a machine without the block refused (ENOSYS: "Not for the HEVC block",
#   "can't be used"); the fake left clean every time.
# Needs FFMPEG_TARBALL and the clips; skipped (and said so) without them. FFmpeg is rebuilt only
# when a patch changes.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
mkdir -p "${1:?usage: tests/host/ffmpeg/hevc_run.sh OUTDIR}"
C=$(cd "$1" && pwd)/hevc
O=$(cd "$1" && pwd)/ffmpeg-hevc
TAR=${FFMPEG_TARBALL:-}
P1=$TOP/ffmpeg/0001-avcodec-h264_vchiq.patch
P2=$TOP/ffmpeg/0002-avcodec-hevc_hwdec.patch
if [ -z "$TAR" ] || [ ! -f "$TAR" ] || [ ! -f "$C/small.trace" ]; then
  echo "hevc_hwdec tests: skipped (set FFMPEG_TARBALL; needs tests/host/hevc's clips)"
  exit 0
fi
mkdir -p "$O/lib"
bad=0
# the host libhevcdec (hevcdec, rpivid_h265.c), and beside it the fake block and its pictures
# from the environment, linked into every program as objects
CC="arm-linux-gnueabihf-gcc -O1 -marm -mfpu=neon -mno-unaligned-access -I$TOP/hevcdec -I$TOP/hevcdec/shim -I$TOP/tests/host -Wall -Wno-unused-function"
$CC -c "$TOP/hevcdec/hevcdec.c" -o "$O/lib/hevcdec.o" && $CC -c "$TOP/hevcdec/hevcdec_conv.c" -o "$O/lib/hevcdec_conv.o" &&
  $CC -Wno-pointer-sign -Wno-sign-compare -Wno-unused-parameter -Wno-unused-variable -c "$TOP/hevcdec/rpivid_h265.c" \
    -o "$O/lib/rpivid_h265.o" && $CC -c "$TOP/tests/host/fake_hevc.c" -o "$O/fake_hevc.o" &&
  $CC -c "$HERE/hevc_fake_env.c" -o "$O/hevc_fake_env.o" || exit 1
rm -f "$O/lib/libhevcdec.a"
arm-linux-gnueabihf-ar rcs "$O/lib/libhevcdec.a" "$O/lib/"*.o
cp "$TOP/hevcdec/hwhevcdec.h" "$TOP/hevcdec/hevc_ctrls.h" "$O/lib/"
# FFmpeg, patched (again only if a patch has changed)
stamp=$(cat "$P1" "$P2" | sha256sum | cut -c1-16)
if [ "$(cat "$O/stamp" 2>/dev/null)" != "$stamp" ]; then
  rm -rf "$O/src" "$O/build"
  mkdir -p "$O/src" "$O/build"
  tar xf "$TAR" -C "$O/src"
  ( cd "$O/src/ffmpeg-5.1.10" && patch -s -p1 < "$P1" && patch -s -p1 < "$P2" ) || { echo "FAIL: the patches don't apply"; exit 1; }
  ( cd "$O/build" && "$O/src/ffmpeg-5.1.10/configure" --enable-cross-compile --arch=arm --target-os=linux \
      --cross-prefix=arm-linux-gnueabihf- --disable-asm --disable-doc --disable-everything --enable-gpl --enable-libhevcdec \
      --enable-decoder=hevc,hevc_hwdec --enable-hwaccel=hevc_hwdec --enable-demuxer=mov,hevc --enable-parser=hevc \
      --enable-protocol=file,pipe --enable-muxer=framecrc,rawvideo,null --enable-encoder=rawvideo,wrapped_avframe \
      --enable-filter=null,format,scale,buffer,buffersink --disable-ffplay --disable-ffprobe \
      --extra-cflags="-mno-unaligned-access -mfpu=neon -I$O/lib" --extra-ldflags="-L$O/lib -no-pie" \
      --extra-libs="$O/hevc_fake_env.o $O/fake_hevc.o" > configure.log 2>&1 ) ||
    { echo "FAIL: FFmpeg's configure (see $O/build/configure.log)"; exit 1; }
  grep -q "CONFIG_HEVC_HWDEC_DECODER 1" "$O/build/config_components.h" &&
    grep -q "CONFIG_HEVC_HWDEC_HWACCEL 1" "$O/build/config_components.h" ||
    { echo "FAIL: hevc_hwdec not enabled by configure"; exit 1; }
  echo "$stamp" > "$O/stamp"
fi
# the patch's hevc_hwdec.c must be ffmpeg/hevc_hwdec.c (else: ffmpeg/mkpatch-hevc.sh)
cmp -s "$O/src/ffmpeg-5.1.10/libavcodec/hevc_hwdec.c" "$TOP/ffmpeg/hevc_hwdec.c" ||
  { echo "FAIL: ffmpeg/hevc_hwdec.c differs from the patch's (run ffmpeg/mkpatch-hevc.sh)"; bad=1; }
( cd "$O/build" && rm -f ffmpeg ffmpeg_g && make -j"$(nproc)" ffmpeg > make.log 2>&1 ) ||
  { echo "FAIL: building FFmpeg (see $O/build/make.log)"; exit 1; }
FF=$O/build/ffmpeg
Q="$TOP/tests/qemu/aligntrap.sh"
echo "== hevc_hwdec in FFmpeg 5.1.10 (the ffmpeg command on the fake HEVC block: every picture, pipelined"
echo "   and not, cached and not, -ss, a raw stream, cropped; 10-bit, 8-bit from 10-bit, 4K; 4:2:2, 12-bit and"
echo "   no block refused)"

clean() {   # the fake's report in file $1: no complaints, nothing wrong, nothing left
  grep -q "fake_hevc: 0 complaints, .* 0 references wrong, 0 pictures unknown, 0 stale factors, 0 evictions, 0 buffers not freed" "$1"
}

for clip in small:352x288 slices:416x240 nowpp:416x240 odd:426x240 small10:352x288 slices10:416x240 odd10:426x240 \
            uhd:3840x2160 uhd10:3840x2160; do
  n=${clip%%:*} sz=${clip#*:}
  pf=yuv420p; case $n in *10) pf=yuv420p10le ;; esac
  [ -f "$C/$n.trace" ] || { echo "FAIL: no clip $n"; bad=1; continue; }
  export HEVC_FAKE_TRACE=$C/$n.trace HEVC_FAKE_YUV=$C/$n.yuv HEVC_FAKE_SIZE=$sz
  optss=("" "-pipelined 0" "-cached_frames 0")
  case $n in uhd*) optss=("") ;; esac      # (4K: 12-17 MB frames under qemu; once)
  for opts in "${optss[@]}"; do
    tag="$n${opts:+ $opts}"
    HEVC_FAKE_P2=4 "$Q" "$FF" -nostdin -loglevel error -c:v hevc_hwdec $opts -i "$C/$n.mp4" -f rawvideo -pix_fmt $pf \
      -y "$O/$n.yuv" 2> "$O/$n.err" || { echo "FAIL: ffmpeg -c:v hevc_hwdec $tag:"; cat "$O/$n.err"; bad=1; }
    cmp -s "$O/$n.yuv" "$C/$n.yuv" || { echo "FAIL: ffmpeg -c:v hevc_hwdec $tag: not FFmpeg's pictures"; bad=1; }
    clean "$O/$n.err" || { echo "FAIL: the fake after ffmpeg -c:v hevc_hwdec $tag:"; cat "$O/$n.err"; bad=1; }
    case "$opts" in
      "-pipelined 0") grep -q " 0 overlaps" "$O/$n.err" || { echo "FAIL: $tag: the phases overlapped"; bad=1; } ;;
      "") grep -q " [1-9][0-9]* overlaps" "$O/$n.err" || { echo "FAIL: $tag: the phases never overlapped"; bad=1; } ;;
    esac
  done
  case $n in uhd*) continue ;; esac
  # -ss: the same frames as the hevc decoder from there
  for dec in hevc hevc_hwdec; do
    "$Q" "$FF" -nostdin -loglevel error -ss 0.2 -c:v $dec -i "$C/$n.mp4" -f framecrc - > "$O/$n.$dec.crc" 2> "$O/$n.$dec.err" ||
      { echo "FAIL: ffmpeg -ss 0.2 -c:v $dec $n"; cat "$O/$n.$dec.err"; bad=1; }
  done
  a=$(awk -F', ' '!/^#/{print $6}' "$O/$n.hevc.crc" | tr '\n' ' ')
  b=$(awk -F', ' '!/^#/{print $6}' "$O/$n.hevc_hwdec.crc" | tr '\n' ' ')
  [ -n "$a" ] && [ "$a" = "$b" ] || { echo "FAIL: -ss 0.2 $n: hevc $a, hevc_hwdec $b"; bad=1; }
  clean "$O/$n.hevc_hwdec.err" || { echo "FAIL: the fake after -ss $n:"; cat "$O/$n.hevc_hwdec.err"; bad=1; }
done

# a raw stream (no extradata: hevcdec opened at the first picture)
export HEVC_FAKE_TRACE=$C/small.trace HEVC_FAKE_YUV=$C/small.yuv HEVC_FAKE_SIZE=352x288
ffmpeg -v error -y -i "$C/small.mp4" -c copy -bsf:v hevc_mp4toannexb -f hevc "$O/small.hevc" || bad=1
"$Q" "$FF" -nostdin -loglevel error -c:v hevc_hwdec -i "$O/small.hevc" -f rawvideo -pix_fmt yuv420p -y "$O/raw.yuv" \
  2> "$O/raw.err" || { echo "FAIL: ffmpeg -c:v hevc_hwdec on a raw stream:"; cat "$O/raw.err"; bad=1; }
cmp -s "$O/raw.yuv" "$C/small.yuv" || { echo "FAIL: a raw stream: not FFmpeg's pictures"; bad=1; }
clean "$O/raw.err" || { echo "FAIL: the fake after a raw stream:"; cat "$O/raw.err"; bad=1; }

# cropped on the left and at the top (the SPS's conformance window, set by hevc_metadata: the
# slices are small's, so the fake gives small's whole pictures; the output is exactly their window)
ffmpeg -v error -y -i "$C/small.mp4" -c copy -bsf:v hevc_metadata=crop_left=16:crop_top=8:crop_right=32:crop_bottom=4 \
  "$O/crop.mp4" && ffmpeg -v error -y -flags unaligned -i "$O/crop.mp4" -f rawvideo -pix_fmt yuv420p "$O/crop.ref.yuv" ||
  bad=1   # (unaligned: the exact window; otherwise FFmpeg keeps a left crop that would misalign the planes)
"$Q" "$FF" -nostdin -loglevel error -c:v hevc_hwdec -i "$O/crop.mp4" -f rawvideo -pix_fmt yuv420p -y "$O/crop.yuv" \
  2> "$O/crop.err" || { echo "FAIL: ffmpeg -c:v hevc_hwdec, cropped:"; cat "$O/crop.err"; bad=1; }
[ -s "$O/crop.ref.yuv" ] && cmp -s "$O/crop.yuv" "$O/crop.ref.yuv" || { echo "FAIL: cropped: not FFmpeg's pictures"; bad=1; }
clean "$O/crop.err" || { echo "FAIL: the fake after the cropped clip:"; cat "$O/crop.err"; bad=1; }

# 10-bit, cropped (on the left by 16: mid-word, as above, but three samples a word)
export HEVC_FAKE_TRACE=$C/small10.trace HEVC_FAKE_YUV=$C/small10.yuv HEVC_FAKE_SIZE=352x288
ffmpeg -v error -y -i "$C/small10.mp4" -c copy -bsf:v hevc_metadata=crop_left=16:crop_top=8:crop_right=32:crop_bottom=4 \
  "$O/crop10.mp4" && ffmpeg -v error -y -flags unaligned -i "$O/crop10.mp4" -f rawvideo -pix_fmt yuv420p10le \
  "$O/crop10.ref.yuv" || bad=1
"$Q" "$FF" -nostdin -loglevel error -c:v hevc_hwdec -i "$O/crop10.mp4" -f rawvideo -pix_fmt yuv420p10le -y "$O/crop10.yuv" \
  2> "$O/crop10.err" || { echo "FAIL: ffmpeg -c:v hevc_hwdec, 10-bit cropped:"; cat "$O/crop10.err"; bad=1; }
[ -s "$O/crop10.ref.yuv" ] && cmp -s "$O/crop10.yuv" "$O/crop10.ref.yuv" ||
  { echo "FAIL: 10-bit cropped: not FFmpeg's pictures"; bad=1; }
clean "$O/crop10.err" || { echo "FAIL: the fake after the 10-bit cropped clip:"; cat "$O/crop10.err"; bad=1; }

# 10-bit given out as 8-bit (-output_8bit 1): YUV420P, each sample FFmpeg's 10-bit one's top 8 bits
"$Q" "$FF" -nostdin -loglevel error -c:v hevc_hwdec -output_8bit 1 -i "$C/small10.mp4" -f rawvideo -y "$O/small10.8.yuv" \
  2> "$O/small10.8.err" || { echo "FAIL: ffmpeg -c:v hevc_hwdec -output_8bit 1:"; cat "$O/small10.8.err"; bad=1; }
python3 -c "
import sys, array
a = array.array('H'); a.frombytes(open(sys.argv[1], 'rb').read())
sys.exit(0 if bytes(v >> 2 for v in a) == open(sys.argv[2], 'rb').read() else 1)" "$C/small10.yuv" "$O/small10.8.yuv" ||
  { echo "FAIL: -output_8bit 1: not the top 8 bits of FFmpeg's 10-bit pictures"; bad=1; }
clean "$O/small10.8.err" || { echo "FAIL: the fake after -output_8bit 1:"; cat "$O/small10.8.err"; bad=1; }

# drop_before: pictures with an earlier pts decoded by the block (all 20 phase 2s) but not converted
# or given out; the rest exactly the hevc decoder's last pictures; pipelined and not
export HEVC_FAKE_TRACE=$C/small.trace HEVC_FAKE_YUV=$C/small.yuv HEVC_FAKE_SIZE=352x288
N=$(ffprobe -v error -select_streams v -show_entries frame=pts -of csv=p=0 "$C/small.mp4" | sort -n | sed -n 8p)
K=$(ffprobe -v error -select_streams v -show_entries frame=pts -of csv=p=0 "$C/small.mp4" | awk -v n="$N" '$1 >= n' | wc -l)
"$Q" "$FF" -nostdin -loglevel error -c:v hevc -i "$C/small.mp4" -vsync passthrough -f framecrc - 2>/dev/null |
  awk -F', ' '!/^#/{print $6}' | tail -n "$K" > "$O/drop.want"
for opts in "" "-pipelined 0"; do
  "$Q" "$FF" -nostdin -loglevel verbose -c:v hevc_hwdec $opts -drop_before "$N" -i "$C/small.mp4" -vsync passthrough \
    -f framecrc - 2> "$O/drop.err" | awk -F', ' '!/^#/{print $6}' > "$O/drop.got"
  [ -n "$N" ] && [ "$K" -gt 0 ] && [ "$K" -lt 20 ] && cmp -s "$O/drop.got" "$O/drop.want" &&
    grep -q "$((20 - K)) late pictures decoded but not converted (drop_before)" "$O/drop.err" &&
    grep -q "fake_hevc: 0 complaints, 20 phase 1s, 20 phase 2s, .* 0 buffers not freed" "$O/drop.err" ||
    { echo "FAIL: -drop_before $N $opts (keep $K): got $(wc -l < "$O/drop.got") pictures"; grep "late\|fake_hevc" "$O/drop.err"; bad=1; }
done

# output_hw: frames handed out unconverted, kept in a queue of 12 (as Reel), converted when shown straight into
# the caller's planes, 1:1 (FFmpeg's pictures exactly) and halved (2x2 means), the last ones after the decoder
# is closed; then nothing left (hevcdec closed with the last frame)
B=$O/build
arm-linux-gnueabihf-gcc -O1 -marm -mno-unaligned-access -I"$B" -I"$O/src/ffmpeg-5.1.10" -I"$O/lib" -no-pie \
  -o "$O/hevc_hw_test" "$HERE/hevc_hw_test.c" "$B/libavformat/libavformat.a" "$B/libavcodec/libavcodec.a" \
  "$B/libswresample/libswresample.a" "$B/libavutil/libavutil.a" -L"$O/lib" -lhevcdec "$O/hevc_fake_env.o" "$O/fake_hevc.o" \
  -lm -lpthread -latomic || { echo "FAIL: building hevc_hw_test"; bad=1; }
for t in small:small.mp4:352x288:8:small.yuv small10:small10.mp4:352x288:10:small10.yuv crop10:crop10.mp4:304x276:10:crop10.ref.yuv; do
  IFS=: read -r n mp sz bits ref <<< "$t"
  case $n in crop10) mpp=$O/$mp refp=$O/$ref ;; *) mpp=$C/$mp refp=$C/$ref ;; esac
  export HEVC_FAKE_TRACE=$C/${n/crop10/small10}.trace HEVC_FAKE_YUV=$C/${n/crop10/small10}.yuv HEVC_FAKE_SIZE=352x288
  "$Q" "$O/hevc_hw_test" "$mpp" "$O/hw.$n.yuv" "$O/hw.$n.half.yuv" 12 > "$O/hw.$n.out" 2> "$O/hw.$n.err" &&
    python3 "$HERE/halve.py" "$refp" "$sz" "$bits" "$O/hw.$n.want.half.yuv" &&
    cmp -s "$O/hw.$n.yuv" "$refp" && cmp -s "$O/hw.$n.half.yuv" "$O/hw.$n.want.half.yuv" && clean "$O/hw.$n.err" ||
    { echo "FAIL: output_hw, $n: $(cat "$O/hw.$n.out")"; cmp "$O/hw.$n.yuv" "$refp"; cmp "$O/hw.$n.half.yuv" "$O/hw.$n.want.half.yuv";
      cat "$O/hw.$n.err"; bad=1; }
done

# a picture the block fails (its phase 1 doesn't finish properly): given out, flagged corrupt
HEVC_FAKE_FAIL=4 HEVC_FAKE_QUIET=1 "$Q" "$FF" -nostdin -loglevel warning -c:v hevc_hwdec -i "$C/small.mp4" -f null - \
  2> "$O/fail.err" || { echo "FAIL: ffmpeg with a picture failed stopped:"; cat "$O/fail.err"; bad=1; }
[ "$(grep -c "corrupt decoded frame" "$O/fail.err")" = 1 ] && grep -q "fake_hevc: .* 0 buffers not freed" "$O/fail.err" ||
  { echo "FAIL: a picture failed: not flagged once, or the fake left unclean:"; cat "$O/fail.err"; bad=1; }

# a stream whose depth changes part way (8-bit small, then 10-bit small10, same size): refused at the
# change with ENOSYS, said once (the frames of the 8-bit part still in use)
ffmpeg -v error -y -i "$C/small10.mp4" -c copy -bsf:v hevc_mp4toannexb -f hevc "$O/small10.hevc" &&
  cat "$O/small.hevc" "$O/small10.hevc" > "$O/mixed.hevc" || bad=1
export HEVC_FAKE_TRACE=$C/small.trace HEVC_FAKE_YUV=$C/small.yuv HEVC_FAKE_SIZE=352x288
"$Q" "$FF" -nostdin -loglevel error -c:v hevc_hwdec -i "$O/mixed.hevc" -f null - 2> "$O/mixed.err"
grep -q "the depth changed (352x288 8-bit to 352x288 10-bit) part way" "$O/mixed.err" &&
  [ "$(grep -c "Not for the HEVC block\|part way" "$O/mixed.err")" = 1 ] && grep -q "Function not implemented" "$O/mixed.err" ||
  { echo "FAIL: a depth change part way: not refused once with ENOSYS:"; cat "$O/mixed.err"; bad=1; }

# refused: 4:2:2 and 12-bit (from the MP4's extradata, and from a raw stream's first picture); no block
for k in p422:yuv422p10le p12:yuv420p12le; do
  t=${k%%:*} pf=${k#*:}
  [ -f "$O/$t.mp4" ] || ffmpeg -v error -y -f lavfi -i testsrc2=size=352x288:rate=25 -frames:v 3 -c:v libx265 \
    -x265-params log-level=error -pix_fmt $pf "$O/$t.mp4" || bad=1
  ffmpeg -v error -y -i "$O/$t.mp4" -c copy -bsf:v hevc_mp4toannexb -f hevc "$O/$t.hevc" || bad=1
  for f in $t.mp4 $t.hevc; do
    if "$Q" "$FF" -nostdin -loglevel error -c:v hevc_hwdec -i "$O/$f" -f null - 2> "$O/$f.err"; then
      echo "FAIL: $pf ($f) not refused"; bad=1
    fi
    grep -q "Not for the HEVC block" "$O/$f.err" || { echo "FAIL: $pf ($f) refused without saying why:"; cat "$O/$f.err"; bad=1; }
  done
  grep -q "Error while opening decoder" "$O/$t.mp4.err" ||   # (an MP4's from its extradata: at open)
    { echo "FAIL: $pf MP4 not refused at open:"; cat "$O/$t.mp4.err"; bad=1; }
done
if HEVC_FAKE_NOBLOCK=1 "$Q" "$FF" -nostdin -loglevel error -c:v hevc_hwdec -i "$C/small.mp4" -f null - 2> "$O/noblock.err"; then
  echo "FAIL: no HEVC block, not refused"; bad=1
fi
grep -q "The HEVC block can't be used: The HEVC block can't be mapped" "$O/noblock.err" ||
  { echo "FAIL: no HEVC block, refused without saying why:"; cat "$O/noblock.err"; bad=1; }

[ $bad = 0 ] && echo "hevc_hwdec tests: all passed" || echo "hevc_hwdec tests: FAILED"
exit $bad
