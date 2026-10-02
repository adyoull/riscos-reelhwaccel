#!/bin/bash
# tests/host/run.sh - every host test: each test tool and the HEVCHW module
# run as arm-linux programs under qemu, with RISC OS's alignment rules
# (tests/qemu/aligntrap.sh), against fakes of the VCHIQ module, the MMAL
# firmware, the firmware mailbox and the HEVC block.
#
# Needs: arm-linux-gnueabihf-gcc and libc6-dev-armhf-cross, ffmpeg with libx264
# (for mp4_check.sh; skipped without), FFMPEG_TARBALL=.../ffmpeg-5.1.10.tar.xz for
# the h264_vchiq tests (tests/host/ffmpeg; skipped without), and qemu-arm
# with tests/qemu/qemu-8.2.2-align-trap.patch (tests/qemu/build-qemu.sh;
# QEMU=path if it isn't the qemu-arm on PATH). The HEVCHW module's build
# check uses arm-linux-gnueabihf-gcc too (MODULE_CROSS=... to change it).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
O=${OUT:-$TOP/out/host}
mkdir -p "$O"
bad=0

# tools/hevcprobe: the Pi 4 HEVC block probe, against a fake firmware and fake registers
arm-linux-gnueabihf-gcc -O1 -DPROBE_TEST -I$HERE/fake -Wall -no-pie -o "$O/hevcprobe_test" "$TOP/tools/hevcprobe/hevcprobe.c" "$HERE/hevcprobe_test.c" || bad=1
echo "== hevcprobe_test (the Pi 4 HEVC block probe: the clock on only around reads, put back)"
"$TOP/tests/qemu/aligntrap.sh" "$O/hevcprobe_test" | grep -v "^  &\|^hevcprobe:\|^$" || bad=1

# tools/vchiqprobe: the VCHIQ module's interface and clients, against fake modules
arm-linux-gnueabihf-gcc -O1 -DPROBE_TEST -I$HERE/fake -Wall -no-pie -o "$O/vchiqprobe_test" "$TOP/tools/vchiqprobe/vchiqprobe.c" "$HERE/vchiqprobe_test.c" || bad=1
echo "== vchiqprobe_test (VCHIQ's SWIs and commands, the modules calling it, copies saved)"
"$TOP/tests/qemu/aligntrap.sh" "$O/vchiqprobe_test" | grep "FAIL\|vchiqprobe_test:" || bad=1

# tools/mmalprobe: MMAL over VCHIQ to the video decoder, against a fake VCHIQ and firmware
arm-linux-gnueabihf-gcc -O1 -DPROBE_TEST -I$HERE/fake -Wall -no-pie -o "$O/mmalprobe_test" "$TOP/tools/mmalprobe/mmalprobe.c" "$HERE/mmalprobe_test.c" || bad=1
echo "== mmalprobe_test (VCHIQ service open, ril.video_decode created, ports, encodings, all closed)"
"$TOP/tests/qemu/aligntrap.sh" "$O/mmalprobe_test" | grep "FAIL\|mmalprobe_test:" || bad=1

# tools/mmaldecode: H.264 decoded through a fake VCHIQ + MMAL decoder that behaves as the Pi's did,
# the real callback stub, checksums as framecrc
arm-linux-gnueabihf-gcc -O1 -marm -DPROBE_TEST -I$HERE/fake -Wall -no-pie -o "$O/mmaldecode_test" "$TOP/tools/mmaldecode/mmaldecode.c" "$HERE/mmaldecode_test.c" || bad=1
echo "== mmaldecode_test (stream in by bulk, pictures back by bulk, format changes, buffer numbering, checksums, clean-up;"
echo "   MP4 input: access units with pts, display order, flush and seek, -t)"
"$TOP/tests/qemu/aligntrap.sh" "$O/mmaldecode_test" > "$O/mmaldecode_test.out" || bad=1
grep "FAIL\|mmaldecode_test:" "$O/mmaldecode_test.out"
# ... and its MP4 input against real MP4s from x264, decoded by FFmpeg (skipped without them)
"$HERE/mp4_check.sh" "$O/mmaldecode_test" "$O" || bad=1

# vcdec: the library, its copy routines (VCDEC_HOST: OS_EnterOS opens the fake's PCI memory), and
# tools/vcdectest, against the same fake VCHIQ + MMAL decoder (tests/host/mmaldecode_test.c, included)
arm-linux-gnueabihf-gcc -O1 -marm -mno-unaligned-access -DVCDEC_HOST -DPROBE_TEST -I$HERE/fake -I$TOP/vcdec -Wall \
  -Wno-unused-function -no-pie -o "$O/vcdec_test" "$HERE/vcdec_test.c" "$TOP/vcdec/vcdec.c" "$TOP/vcdec/vcdec_copy.S" \
  "$TOP/tools/vcdectest/vcdectest.c" || bad=1
echo "== vcdec_test (the library: a whole decode, receives finishing late, unaligned planes, seeks before and after"
echo "   the end, input full, bad calls, unsupported streams, decoders that misbehave; then VCDecTest on the fake)"
"$TOP/tests/qemu/aligntrap.sh" "$O/vcdec_test" > "$O/vcdec_test.out" || bad=1
grep "FAIL\|vcdec_test:" "$O/vcdec_test.out"
# ... and VCDecTest's MP4 reader against real MP4s (as mmaldecode's)
"$HERE/mp4_check.sh" "$O/vcdec_test" "$O/vcdectest" || bad=1

# ffmpeg/: the h264_vchiq decoder built into FFmpeg 5.1.10 (FFMPEG_TARBALL; skipped without it)
"$HERE/ffmpeg/run.sh" "$O" || bad=1

# hevcdec/ and tools/hevctest: against a fake HEVC block, on traces from tools/hevctrace
"$HERE/hevc/run.sh" "$O" || bad=1

# hevchw/module: the HEVCHW module (its C, and header.s's veneers and IRQ handler) on a fake RISC OS
arm-linux-gnueabihf-gcc -c -o "$O/hevchw_header.o" "$TOP/hevchw/module/header.s" &&
  arm-linux-gnueabihf-objcopy --weaken-symbol=hw_swi "$O/hevchw_header.o" &&
  arm-linux-gnueabihf-gcc -O1 -marm -DHW_TEST -Wall -no-pie -o "$O/hevchw_test" "$TOP/hevchw/module/hevchw.c" \
    "$HERE/hevchw_test.c" "$O/hevchw_header.o" || bad=1
echo "== hevchw_test (the HEVCHW module: maps, register test, the interrupt found then claimed, memory)"
"$TOP/tests/qemu/aligntrap.sh" "$O/hevchw_test" | grep -v "^  &\|^$" || bad=1
"$TOP/hevchw/module/build.sh" "$O/hevchw" | tail -1 || bad=1

# UnixLib's sscanf doesn't fill a long long (%lld: only the low word; MMALDecode 0.13 on the Pi)
if grep -n 'scanf[^;]*%ll' "$TOP"/tools/*/*.c "$TOP"/hevchw/*/*.c "$TOP"/vcdec/*.c "$TOP"/hevcdec/*.c; then
  echo "FAIL: scanf with %ll (UnixLib fills only the low word)"; bad=1
fi
[ $bad = 0 ] && echo "all host tests passed" || echo "SOME HOST TESTS FAILED"
exit $bad
