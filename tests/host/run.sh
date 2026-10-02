#!/bin/bash
# tests/host/run.sh - every host test: each test tool and the HEVCHW module
# run as arm-linux programs under qemu, with RISC OS's alignment rules
# (tests/qemu/aligntrap.sh), against fakes of the VCHIQ module, the MMAL
# firmware, the firmware mailbox and the HEVC block.
#
# Needs: arm-linux-gnueabihf-gcc and libc6-dev-armhf-cross, and qemu-arm
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
echo "== mmaldecode_test (stream in by bulk, pictures back by bulk, format changes, buffer numbering, checksums, clean-up)"
"$TOP/tests/qemu/aligntrap.sh" "$O/mmaldecode_test" | grep "FAIL\|mmaldecode_test:" || bad=1

# hevchw/module: the HEVCHW module (its C, and header.s's veneers and IRQ handler) on a fake RISC OS
arm-linux-gnueabihf-gcc -c -o "$O/hevchw_header.o" "$TOP/hevchw/module/header.s" &&
  arm-linux-gnueabihf-objcopy --weaken-symbol=hw_swi "$O/hevchw_header.o" &&
  arm-linux-gnueabihf-gcc -O1 -marm -DHW_TEST -Wall -no-pie -o "$O/hevchw_test" "$TOP/hevchw/module/hevchw.c" \
    "$HERE/hevchw_test.c" "$O/hevchw_header.o" || bad=1
echo "== hevchw_test (the HEVCHW module: maps, register test, the interrupt found then claimed, memory)"
"$TOP/tests/qemu/aligntrap.sh" "$O/hevchw_test" | grep -v "^  &\|^$" || bad=1
"$TOP/hevchw/module/build.sh" "$O/hevchw" | tail -1 || bad=1

[ $bad = 0 ] && echo "all host tests passed" || echo "SOME HOST TESTS FAILED"
exit $bad
