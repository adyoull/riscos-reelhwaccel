# ReelHWAccel - hardware video decoding and encoding for RISC OS on the Raspberry Pi

ReelHWAccel decodes and encodes video with the Raspberry Pi's own hardware
instead of the ARM, for Reel and for FFmpeg on RISC OS
(github.com/adyoull/riscos-ffmpeg, where it started). It has three
libraries (vcdec and vcenc both in `libvcdec.a`), each with an FFmpeg
codec on top:

| Library | What it drives | Codecs | Boards | FFmpeg codec |
|---|---|---|---|---|
| `vcdec` (0.4.2) | the VideoCore's decoder, through RISC OS's VCHIQ module and the firmware's MMAL service | H.264 (8-bit 4:2:0, up to 1080p), Motion JPEG | Pi 4 (tested); Pi 2, 3 (untested) | `h264_vchiq` |
| `vcenc` (0.1.3, in libvcdec) | the VideoCore's encoder, the same way | H.264 encoding (8-bit 4:2:0, up to 1080p) | Pi 4 (tested) | `h264_encvchiq` |
| `hevcdec` (0.1.13) | the Pi 4's HEVC block ("rpivid" on Linux) directly: its registers, clock and memory from RISC OS, its commands built by `rpivid_h265.c` from the Raspberry Pi Linux kernel, unmodified | HEVC (H.265), 8-bit and 10-bit 4:2:0, up to 4096x4096 (4K) | Pi 4, 400, CM4 | `hevc_hwdec` |

Reel and FFmpeg use them through this project's devkit (0.2.15): the
libraries, their headers and the three FFmpeg 5.1.10 patches: two
decoders and one encoder (`devkit/`, and `devkit/README.md` for how to
use them). The decoders are `h264_vchiq` and `hevc_hwdec`, not
`h264_mmal`, which is FFmpeg's existing decoder for Linux's MMAL library;
the encoder is `h264_encvchiq`. Each refuses what its hardware can't do
(AVERROR(ENOSYS)), so a program falls back to FFmpeg's software codecs.

## Decoding on a Raspberry Pi 4

Every picture is exactly as FFmpeg's software decoder gives it (H.264:
within the VideoCore's rounding of chroma). Checked on a Pi 4 with the
test programs below.

| | Pictures a second | ARM time a picture |
|---|---|---|
| H.264 1080p (vcdec, zero-copy) | 85.7 | 0.13 ms |
| HEVC 1080p 8-bit (hevcdec, pipelined) | 353 | 4.0 ms converting (2.0 halved) |
| HEVC 1080p 10-bit | 316 | 4.7 ms to 8-bit, 7.7 ms to 16-bit |
| HEVC 4K 8-bit | 83 | 16.3 ms converting, 7.7 ms halved |
| HEVC 4K 10-bit | 79 | 18.0 ms to 8-bit, 10.0 ms halved |

In Reel (riscos-ffmpeg) this plays 1080p60 H.264, and 1080p HEVC at
35-45% of the ARM (Reel 0.1.23 test builds, which convert each picture
from the block straight into the video overlay, halving 4K).

## Encoding H.264 on the VideoCore

`h264_encvchiq` (vcenc underneath) encodes on the VideoCore. It's never
FFmpeg's default H.264 encoder, so ask for it by name:

    ffmpeg -c:v h264_vchiq -i in.mp4 -c:v h264_encvchiq -profile:v main -b:v 5M -c:a aac out.mp4

| On a Raspberry Pi 4 | Speed |
|---|---|
| 1080p30 (VCEncTest, pictures already made) | 72 frames a second |
| 720p | 155 frames a second |
| Converting a 1280x544 trailer with AAC (riscos-ffmpeg's Convert window), `h264_vchiq` decoding + `h264_encvchiq` | 3.4x real time |
| The same with ARM decoding + `h264_encvchiq` | 2.2x real time |
| The same with libx264 (veryfast) on the ARM | 0.66x real time |

- **Input:** YUV420P frames, even sizes up to 1920x1088 (1080p needs `gpu_mem=128`).
- **Output:** H.264 Baseline, Main or High, Annex B, one access unit a packet, in order (no B frames, so dts = pts).
- **Headers:** the SPS and PPS are the extradata for MP4 and Matroska; otherwise they're in front of the first packet. `repeat_headers` puts them in front of every keyframe.
- **Settings:**
  - `b` (default 10 Mbit/s);
  - `g` (the keyframe interval; the encoder's default is about 60);
  - `profile` by name (`baseline`, `main`, `high`) or number;
  - `level`, `qp_min`, `qp_max`;
  - keyframes on request (a frame with pict_type I).
- **Quality:** every stream VCEncTest made decoded back close to the pictures sent (luma PSNR 33 to 52 dB). The 360p and 1080p streams it kept also play in FFmpeg's software decoder.
- **Constant bit rate:** the Pi 4's firmware refuses it every way, so `rc_constant` gives variable. Variable keeps to the asked rate on real material: 1080p30 at 8 Mbit/s came out at 8.04.

From a program of your own, `vcdec/vcenc.h` is the same without FFmpeg: open, send pictures until the input is full, take packets, send the end, close.

## The test programs

The Pi test zips (`./build.sh`, into `dist/`) are what found out how to
drive the hardware from RISC OS, and check it:

- **VCDecTest** (`tools/vcdectest`): vcdec as a player drives it: every
  picture checked, seeks before and after the end of the stream, zero
  copy, memory, speed, and several decoders open at once (Multi).
- **VCEncTest** (`tools/vcenctest`): vcenc as a program drives it: test
  pictures encoded, the stream checked and decoded again by vcdec
  (PSNR), each setting in turn, speed.
- **HEVCTest** (`tools/hevctest`): replays traces of FFmpeg's HEVC
  decoder (made on the host by `tools/hevctrace`) through hevcdec and
  checks every picture, 1:1, halved and 10-bit, blocking and pipelined,
  1080p and 4K; `-K` times the conversions.
- Earlier probes: **VCHIQProbe** (the VCHIQ module's SWIs), **MMALProbe**
  and **MMALDecode** (the firmware's video decoder through VCHIQ and
  MMAL), **HEVCProbe** and the **HEVCHW** module (the HEVC block's
  registers, clock, interrupt and memory).

How it got here, version by version, is in `CHANGELOG.md`. Some of what
it took:

- **H.264:** physically contiguous buffers numbered from 1, output
  buffers only after the decoder's format change, every receive queued
  as its message arrives, and `gpu_mem=128` for 1080p. Pictures arrive
  in a cacheable Physical Memory Pool and are handed out without a copy.
- **HEVC:** the block's scaling-factor RAM persists and is used even
  when the stream has no scaling lists, so the factors are always
  loaded. The block's two phases overlap, decoding while the program
  works, and hevcdec polls the block between the columns of a
  conversion, so the next picture is decoded during the copy. Frames
  are cacheable and converted with NEON from the block's 128-byte
  columns (three 10-bit samples a word at 10-bit).
- **Encoding:** the encoder is set up as Linux's bcm2835-codec driver
  sets it, over the same transport as the decoder. The firmware sends
  the SPS and PPS as soon as the output has buffers, and again as a
  buffer of their own before each later keyframe, which vcenc puts at
  the start of the keyframe's packet. It refuses constant bit rate, and
  once it has refused an enable it refuses every later one on that
  encoder, so vcenc tries constant only on the running encoder.
- **Memory:** both libraries' contiguous memory (`common/contig.h`)
  avoids the running program's page at &8000. Taking it moves the
  program to another page, which ARMEABISupport doesn't notice (the
  "EMT trap" seen after hardware runs).

## Building

    ./build.sh                 # every Pi test zip and the devkit, into dist/
    ci/build.sh                # ./build.sh, then riscos-crossdev's checks (as GitHub runs it)
    vcdec/build.sh OUTDIR      # libvcdec.a (vcdec, vcenc and the MMAL connection they share), vcdec.h and vcenc.h
    hevcdec/build.sh OUTDIR    # libhevcdec.a and hwhevcdec.h, for RISC OS
    tools/hevctrace/build.sh .../ffmpeg-5.1.10.tar.xz   # the host ffmpeg that writes traces (HEVC_TRACE=file)
    devkit/build.sh            # dist/riscos-reelhwaccel-devkit-V.tgz (V=0.2.15)
    FFMPEG_TARBALL=.../ffmpeg-5.1.10.tar.xz tests/host/run.sh   # the host tests
    ffmpeg/mkpatch.sh .../ffmpeg-5.1.10.tar.xz   # patch 0001, after vchiqdec.c changes
    ffmpeg/mkpatch-hevc.sh .../ffmpeg-5.1.10.tar.xz   # patch 0002, after hevc_hwdec.c changes
    ffmpeg/mkpatch-enc.sh .../ffmpeg-5.1.10.tar.xz    # patch 0003, after vchiqenc.c changes

**The toolchain** (`common/toolchain.sh`) is
[riscos-crossdev](https://github.com/adyoull/riscos-crossdev)'s, at
`$RISCOS_TOOLCHAIN` (set in its container image and by its `env.sh`), or a
GCCSDK GCC 10 environment (`CROSS=.../arm-riscos-gnueabihf-`, with
`tools/elf2aif` built: `make -C tools/elf2aif GCCSDK_SRC=<gccsdk>`). The
HEVCHW module is built with `arm-linux-gnueabihf-gcc` (position
independent, no GOT).

**Test zips with clips** need more, and `build.sh` skips them (saying so)
without it: MMALDecode and VCDecTest need the host's ffmpeg with libx264,
HEVCTest needs libx265 and `FFMPEG_TARBALL` (for the tracing ffmpeg).

**On GitHub,** every push builds with `ci/build.sh` in riscos-crossdev's
container image, the version named in `.riscos-deps`, and keeps `dist/` as
the run's artifact. `ci/build.sh` runs riscos-crossdev's checks on each
program and on both libraries: static linking, stack probes, no
VFPv4-only instructions (so they run on VFPv3 machines), UnixLib
consistency, and casts that could load unaligned. Once a day a second
workflow looks for a newer riscos-crossdev release and opens a pull
request moving `.riscos-deps` to it, saying whether the build passed.
Neither runs anything on RISC OS, and neither releases.

**The host tests** run each tool as an arm-linux program under a QEMU that
traps unaligned accesses as RISC OS does (`tests/qemu/build-qemu.sh`, or
riscos-crossdev's: `QEMU=$RISCOS_TOOLCHAIN/bin/qemu-arm-aligntrap`),
against fakes of the VCHIQ module, the MMAL firmware, the firmware
mailbox and the HEVC block; the fakes behave as the Pi did in each of
the runs above. They need `FFMPEG_TARBALL`, libx264 and libx265, so they
are run before each release rather than on GitHub.

## Licence

The GNU GPL version 2 (see `COPYING`), without "or later": the same
licence as Linux's drivers that ReelHWAccel was made against.

- vcdec is a new implementation over RISC OS's VCHIQ module, with its own
  MMAL client. That client is based on the MMAL message formats as
  Linux's vchiq-mmal driver defines them in its headers, because there's
  no published specification of the protocol. It contains none of the
  driver's code, but because it used the driver as its reference, it is
  treated as derived from it, and so has the driver's licence: GPL
  version 2. vcenc, its encoder, is the same, with Linux's
  bcm2835-codec driver as its reference for how the encoder is set up.
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
| Linux `bcm2835-codec` (`bcm2835-v4l2-codec.c`) and userland's `mmal_parameters_video.h` | GPL-2.0 only; BSD-3-Clause | vcenc's encoder setup (the parameters, and the order Linux sets them in) and their numbers: no code copied |
| Linux `rpivid` (`rpivid_hw.h`, `rpivid.h`, `rpivid_hw.c`, `rpivid_video.c`) and device tree (`bcm2711*.dtsi`) | GPL-2.0 only | the reference for the HEVC block's registers, addresses, clock and interrupt, and what `rpivid_h265.c` expects of the rest of the driver: none of their code (hevcdec's `rpivid.h`, `rpivid_hw.h` and `rpivid_video.h` are our own), but treated as derived from them |
| Linux `rpivid_h265.c` (rpi-6.6.y) | GPL-2.0-or-later | `hevcdec/rpivid_h265.c`, unchanged: the block's commands for each picture |
| Linux `hevc-ctrls.h` (V4L2 stateless HEVC controls) | BSD-3-Clause or GPL-2.0 (uAPI) | `hevcdec/hevc_ctrls.h`, with plain types |
| Raspberry Pi's FFmpeg (`v4l2_req_hevc_vx.c`) | LGPL-2.1-or-later | `tools/hevctrace/hevc_trace.c` and `ffmpeg/hevc_hwdec.c`: filling the controls from FFmpeg's HEVC decoder |
| RISC OS's VCHIQ, BCMSound and BCMVideo modules (in the ROM) | - | the VCHIQ SWI conventions, learnt by running and disassembling them |

Code from `mmal-vchiq.c` or rpivid's other files (GPL-2.0 only) is not
to be copied.
