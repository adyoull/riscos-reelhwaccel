# ReelHWAccel - hardware video decoding for RISC OS on the Raspberry Pi

ReelHWAccel decodes video with the Raspberry Pi's own hardware instead of
the ARM, for Reel and for FFmpeg on RISC OS (github.com/adyoull/riscos-ffmpeg,
where it started). It has three parts:

| Part | What it drives | Codecs | Boards |
|---|---|---|---|
| `vcdec` | the VideoCore's decoder, through RISC OS's VCHIQ module and the firmware's MMAL service | H.264, Motion JPEG (and MPEG-2 / VC-1 with the licences on a Pi 1-3) | Pi 2, 3, 4 (and 1/Zero, though Reel and FFmpeg need ARMv7) |
| `hevchw` | the Pi 4's HEVC block ("rpivid" on Linux) directly, with the `HEVCHW` module for its registers and interrupt | HEVC (H.265), 8 and 10-bit | Pi 4, 400, CM4 |
| `hwdec` | one small interface over both: can this codec and size be decoded in hardware, and do it | - | - |

Reel and FFmpeg will only use `hwdec` (`libhwdec`, `hwdec.h`), from this
project's devkit; in FFmpeg the decoders will appear as `h264_vchiq` and
`hevc_hwdec` (not `h264_mmal`, which is FFmpeg's existing decoder for
Linux's MMAL library).

## Where it's got to

Nothing here is for everyday use yet: these are the test tools that
found out how to drive the hardware from RISC OS, each with what it
showed on a Raspberry Pi 4.

- **`tools/vchiqprobe`**: the VCHIQ module's SWIs (VCHIQ 0.14, in the
  ROM) and how BCMSound and BCMVideo call them.
- **`tools/mmalprobe`**: the firmware's `ril.video_decode` answers through
  VCHIQ and MMAL (H.264, MVC and MJPEG on a Pi 4).
- **`tools/mmaldecode`**: whole H.264 decodes on the VideoCore, every
  picture checked against FFmpeg's (within rounding: the VideoCore's
  chroma can be 1 lower). On a Pi 4: 640x360 at 193 pictures a second,
  426x240 at 333, and 1080p High (with `gpu_mem=128`; at 64 MB the decoder
  stalls without a word). What it took: physically contiguous buffers,
  buffers numbered from 1, output buffers only after the decoder's format
  change, every receive queued as its message arrives, and the 20 input
  buffers the decoder recommends.
- **`tools/hevcprobe`** and **`hevchw/`** (the HEVCHW module): the Pi 4's
  HEVC block answers, and its registers (30-bit, addresses in 64-byte
  units), contiguous memory and interrupt (GIC 130, device 34) all work
  from RISC OS.

Next: the `vcdec` library and Reel's use of it; then the HEVC decoder.

## Building

    ./build.sh                 # every Pi test zip, into dist/
    tests/host/run.sh          # the host tests

`build.sh` needs GCCSDK GCC 10 (`CROSS=.../arm-riscos-gnueabihf-`),
`tools/elf2aif` built (`make -C tools/elf2aif GCCSDK_SRC=<gccsdk>`), and
the host's ffmpeg with libx264 for MMALDecode's clips. The HEVCHW module
is built with `arm-linux-gnueabihf-gcc` (position independent, no GOT).
The host tests run each tool as an arm-linux program under a QEMU that
traps unaligned accesses as RISC OS does (`tests/qemu/build-qemu.sh`),
against fakes of the VCHIQ module, the MMAL firmware, the firmware
mailbox and the HEVC block; the fakes behave as the Pi did in each of
the runs above.

## Licence

GPL version 2 or later (see `COPYING`). Reel and FFmpeg (riscos-ffmpeg),
which will use it, are GPL already (FFmpeg's build includes x264).

The build and test helpers come from riscos-ffmpeg: `tools/elf2aif` is
GCCSDK's elf2aif (GPL v2 or later); `tools/mkrozip.py`,
`tests/host/fake/kernel.h` and `tests/qemu` are riscos-ffmpeg's.

What it's built from, and how:

| Source | Licence | Used for |
|---|---|---|
| Raspberry Pi userland, MMAL client (`interface/mmal/vc/mmal_vc_msgs.h`, `mmal_vc_client.c`) | BSD-3-Clause (Broadcom) | MMAL message layouts and how the client handles them: facts only, no code copied |
| Linux `vchiq-mmal` (`mmal-msg*.h`, `mmal-vchiq.c`) | GPL-2.0 only | the same layouts, checked against these: facts only, no code copied |
| Linux `rpivid` (`rpivid_hw.h`) and device tree (`bcm2711*.dtsi`) | GPL-2.0 | the HEVC block's register offsets, addresses and interrupt: facts only |
| RISC OS's VCHIQ, BCMSound and BCMVideo modules (in the ROM) | - | the VCHIQ SWI conventions, learnt by running and disassembling them |

The HEVC decoder itself (still to come) may follow rpivid's two-phase
pipeline (`rpivid_h265.c`, GPL-2.0-or-later); any part translated from
it stays GPL-2.0-or-later. Code from `mmal-vchiq.c` (GPL-2.0 only) is
not to be copied.
