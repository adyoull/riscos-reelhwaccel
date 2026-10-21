#!/bin/bash
# ci/build.sh - the build GitHub runs (.github/workflows/), in
# riscos-crossdev's container image: ./build.sh, then riscos-crossdev's
# checks on every program it linked and on the libraries:
#   check-all.sh     static linking, stack probes, no VFPv4-only
#                    instructions in the programs or libvcdec/libhevcdec
#                    (hevcdec_conv.c is built for NEON but must stay VFPv3
#                    otherwise), UnixLib consistency
#   check-unaligned  casts that could make unaligned loads, in our sources.
#                    ci/unaligned-safe lists the ones checked by hand: MMAL
#                    messages are uint32_t arrays (LDRD needs only word
#                    alignment), vb2_buffer is the first member of
#                    vb2_v4l2_buffer, and picture rows from the VideoCore
#                    start on word boundaries.
# Without the image (RCD_DIR unset), it builds and says it hasn't checked.
# The host tests (tests/host/run.sh) need FFMPEG_TARBALL, libx264 and
# libx265, which the image doesn't have, so they're run before a release.
set -e
TOP=$(cd "$(dirname "$0")/.." && pwd)
cd "$TOP"
rm -rf out/elf out/lib
KEEP_ELF=$TOP/out/elf ./build.sh
if [ -z "${RCD_DIR:-}" ] || [ ! -x "$RCD_DIR/checks/check-all.sh" ]; then
  echo "ci/build.sh: built; riscos-crossdev's checks not run (set RCD_DIR)"
  exit 0
fi
vcdec/build.sh out/lib/vcdec >/dev/null
hevcdec/build.sh out/lib/hevcdec >/dev/null
rc=0
for e in out/elf/*.elf; do
  echo "== $(basename "$e")"
  "$RCD_DIR/checks/check-all.sh" "$e" out/lib/vcdec/libvcdec.a out/lib/hevcdec/libhevcdec.a || rc=1
done
echo "== unaligned accesses"
"$RCD_DIR/checks/check-unaligned.sh" -s ci/unaligned-safe -Ivcdec -Ihevcdec -Ihevcdec/shim \
  vcdec/vcdec.c vcdec/vcenc.c vcdec/vcmmal.c hevcdec/hevcdec.c hevcdec/hevcdec_hw.c hevcdec/hevcdec_conv.c \
  hevcdec/rpivid_h265.c tools/hevcprobe/hevcprobe.c tools/hevctest/hevctest.c \
  tools/mmaldecode/mmaldecode.c tools/mmalprobe/mmalprobe.c tools/vcdectest/vcdectest.c \
  tools/vcenctest/vcenctest.c tools/vchiqprobe/vchiqprobe.c tools/smemprobe/smemprobe.c || rc=1
[ $rc = 0 ] && echo "ci/build.sh: built, all checks passed" || echo "ci/build.sh: CHECKS FAILED"
exit $rc
