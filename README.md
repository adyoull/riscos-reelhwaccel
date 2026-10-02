# ReelHWAccel - hardware video decoding for RISC OS on the Raspberry Pi

ReelHWAccel decodes video with the Raspberry Pi's own hardware instead of
the ARM, for Reel and for FFmpeg on RISC OS (github.com/adyoull/riscos-ffmpeg,
where it started). It has three parts:

| Part | What it drives | Codecs | Boards |
|---|---|---|---|
| `vcdec` | the VideoCore's decoder, through RISC OS's VCHIQ module and the firmware's MMAL service | H.264, Motion JPEG (and MPEG-2 / VC-1 with the licences on a Pi 1-3) | Pi 2, 3, 4 (and 1/Zero, though Reel and FFmpeg need ARMv7) |
| `hevcdec` | the Pi 4's HEVC block ("rpivid" on Linux) directly: its registers, clock and memory from RISC OS, its commands built by Raspberry Pi's `rpivid_h265.c` | HEVC (H.265), 8-bit so far (10-bit to come) | Pi 4, 400, CM4 |
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
  0.13 to 0.17 added MP4 input (one access unit a buffer, with its pts),
  display order by pts, flush and seek, and timing: on a Pi 4 every
  picture comes back with its own pts in display order, 1080p decodes
  and copies out at 58-59 pictures a second, and a flush is clean as
  long as no end-of-stream reached the decoder before it.
- **`vcdec/`** (0.4.1): the library itself, from all of that. Open, send
  an access unit with its pts, take a picture (copied into the caller's
  planes), flush for a seek (after the end of the stream, the decoder is
  created again), close; nothing waits for the decoder but open, flush
  and close, so a player can drive it from Wimp null events. It checks
  the profile, the size and `gpu_mem` (1080p needs 128 MB), and says
  when the stream isn't for the VideoCore so the caller can decode in
  software. Pictures arrive in a cacheable Physical Memory Pool (user
  readable; PCI memory if there's none) and are copied out by LDM 8:
  timed on the Pi with VCDecTest 0.2 and confirmed with 0.3 (every
  picture right), that took 1080p from 64 to 82 pictures a second.
  0.4.1 adds zero-copy: the caller can take a picture in vcdec's own
  buffer and give it back when done. **`tools/vcdectest`** drives it on
  the Pi as a player would: on a Pi 4 every picture right, seeks before
  and after the end of the stream losing nothing, open, flush and close
  in 0-3 cs.
- **`tools/hevcprobe`** and **`hevchw/`** (the HEVCHW module): the Pi 4's
  HEVC block answers, and its registers (30-bit, addresses in 64-byte
  units), contiguous memory and interrupt (GIC 130, device 34) all work
  from RISC OS.
- **`hevcdec/`** (0.1.4): the HEVC decoder. A stateless
  decoder, as Linux's: the caller parses the stream (FFmpeg's HEVC
  decoder) and gives each picture as the V4L2 stateless HEVC controls
  (`hevc_ctrls.h`) with its slices; hevcdec builds the block's commands
  with `rpivid_h265.c` (unchanged, under a small kernel shim) and runs
  the two phases, polling. Output is NV12 in 128-byte columns, as the
  block writes it, with a conversion to planar 4:2:0. 8-bit only, one
  picture at a time. **`tools/hevctrace`** records what FFmpeg's decoder
  gives a hwaccel (controls, slices, and checksums of each picture) on
  the host; **`tools/hevctest`** (HEVCTest) replays those traces on the
  Pi and checks every picture. Tested on the host against a fake block
  that checks the commands and references and writes FFmpeg's pictures.

- **`ffmpeg/`**: FFmpeg 5.1.10's `h264_vchiq` decoder on top of vcdec, as
  a patch (`0001-avcodec-h264_vchiq.patch`, `--enable-vchiq`), tested in
  FFmpeg itself (libavcodec's API and the `ffmpeg` command) against the
  fakes. **`devkit/`** packs libvcdec, vcdec.h and the patch for
  riscos-ffmpeg (FFmpeg, Reel).

On a Pi 4 (HEVCTest 0.1.2) every picture of every clip comes out
exactly as FFmpeg decodes it, 1080p at 230 pictures a second, now that
the block's scaling factors are always loaded (hevcdec 0.1.2). 0.1.4
makes the output frames cacheable and converts them with NEON (it took
29 ms a 1080p picture from memory that isn't cacheable). Next: output
frames that are quick to read (converting an uncached 1080p frame takes
29 ms), 10-bit and 4K, and `hevc_hwdec` in FFmpeg and Reel.

## Building

    ./build.sh                 # every Pi test zip, into dist/
    vcdec/build.sh OUTDIR      # libvcdec.a and vcdec.h, for RISC OS
    hevcdec/build.sh OUTDIR    # libhevcdec.a and hevcdec.h, for RISC OS
    tools/hevctrace/build.sh .../ffmpeg-5.1.10.tar.xz   # the host ffmpeg that writes traces (HEVC_TRACE=file)
    devkit/build.sh            # dist/riscos-reelhwaccel-devkit-0.1.tgz
    FFMPEG_TARBALL=.../ffmpeg-5.1.10.tar.xz tests/host/run.sh   # the host tests
    ffmpeg/mkpatch.sh .../ffmpeg-5.1.10.tar.xz   # the patch, after vchiqdec.c changes

`build.sh` needs GCCSDK GCC 10 (`CROSS=.../arm-riscos-gnueabihf-`),
`tools/elf2aif` built (`make -C tools/elf2aif GCCSDK_SRC=<gccsdk>`), and
the host's ffmpeg with libx264 for MMALDecode's clips and libx265 for
HEVCTest's (with `FFMPEG_TARBALL` set, for the tracing ffmpeg). The HEVCHW module
is built with `arm-linux-gnueabihf-gcc` (position independent, no GOT).
The host tests run each tool as an arm-linux program under a QEMU that
traps unaligned accesses as RISC OS does (`tests/qemu/build-qemu.sh`),
against fakes of the VCHIQ module, the MMAL firmware, the firmware
mailbox and the HEVC block; the fakes behave as the Pi did in each of
the runs above.

## Licence

The GNU GPL version 2 (see `COPYING`), without "or later": the same
licence as Linux's drivers that ReelHWAccel was made against.

- vcdec is a new implementation over RISC OS's VCHIQ module, with its own
  MMAL client. That client is based on the MMAL message formats as
  Linux's vchiq-mmal driver defines them in its headers, because there's
  no published specification of the protocol. It contains none of the
  driver's code, but because it used the driver as its reference, it is
  treated as derived from it, and so has the driver's licence: GPL
  version 2.
- hevcdec is the same for the HEVC block: its own code over RISC OS,
  with Linux's rpivid driver (GPL version 2) as its reference for the
  block's registers and for what `rpivid_h265.c` expects of the rest of
  the driver. `rpivid_h265.c` itself is included unchanged, under its
  own GPL version 2 or later, which allows its use under version 2.

Reel and FFmpeg (riscos-ffmpeg), which use it, are GPL already (FFmpeg's
build includes x264), and say version 2 for it too.

The build and test helpers come from riscos-ffmpeg: `tools/elf2aif` is
GCCSDK's elf2aif (GPL v2 or later); `tools/mkrozip.py`,
`tests/host/fake/kernel.h` and `tests/qemu` are riscos-ffmpeg's.

What it's built from, and how:

| Source | Licence | Used for |
|---|---|---|
| Raspberry Pi userland, MMAL client (`interface/mmal/vc/mmal_vc_msgs.h`, `mmal_vc_client.c`) | BSD-3-Clause (Broadcom) | MMAL message layouts and how the client handles them, checked against these: no code copied |
| Linux `vchiq-mmal` (`mmal-msg*.h`, `mmal-vchiq.c`) | GPL-2.0 only | the reference for the MMAL message formats and how the client handles them: none of its code, but treated as derived from it (hence GPL version 2) |
| Linux `rpivid` (`rpivid_hw.h`, `rpivid.h`, `rpivid_hw.c`, `rpivid_video.c`) and device tree (`bcm2711*.dtsi`) | GPL-2.0 only | the reference for the HEVC block's registers, addresses, clock and interrupt, and what `rpivid_h265.c` expects of the rest of the driver: none of their code (hevcdec's `rpivid.h`, `rpivid_hw.h` and `rpivid_video.h` are our own), but treated as derived from them |
| Linux `rpivid_h265.c` (rpi-6.6.y) | GPL-2.0-or-later | `hevcdec/rpivid_h265.c`, unchanged: the block's commands for each picture |
| Linux `hevc-ctrls.h` (V4L2 stateless HEVC controls) | BSD-3-Clause or GPL-2.0 (uAPI) | `hevcdec/hevc_ctrls.h`, with plain types |
| Raspberry Pi's FFmpeg (`v4l2_req_hevc_vx.c`) | LGPL-2.1-or-later | `tools/hevctrace/hevc_trace.c`: filling the controls from FFmpeg's HEVC decoder |
| RISC OS's VCHIQ, BCMSound and BCMVideo modules (in the ROM) | - | the VCHIQ SWI conventions, learnt by running and disassembling them |

Code from `mmal-vchiq.c` or rpivid's other files (GPL-2.0 only) is not
to be copied.
