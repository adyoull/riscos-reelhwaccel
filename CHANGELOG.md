# Changes

## Release 0.2.17 (2026-10-21)

The GitHub release `v0.2.17`: devkit 0.2.17 (vcdec 0.4.4, vcenc 0.1.5,
hevcdec 0.1.14) and VCDecTest 0.3.4. Everything since v0.2.15 is below:
the VideoCore's memory given back (0.2.16), the shared MMAL connection
(0.4.4 / 0.1.5), and the Multi and Exit tests. The text is
`release-notes/v0.2.17.md`.

## vcdec 0.4.4, vcenc 0.1.5: one MMAL connection for both (2026-10-21)

Done for maintainability, at the owner's request (the hot-path review).
Nothing they do changes.

- **What moved:** vcenc began as a copy of vcdec's VCHIQ/MMAL code, and
  the two files shared 517 identical lines. Every fix had to be made
  twice: the exit clean-up was the latest, and copies had begun to drift
  (vcenc's `pmp_free` handled an empty pool, vcdec's didn't). That code
  is now `vcdec/vcmmal.c` and `vcmmal.h`, used by both:
  - the SWIs and the time;
  - the log and failing;
  - PCI memory and Physical Memory Pools;
  - the RMA callback, connecting and closing;
  - messages, replies and bulk transfers, events with long data;
  - the component and its ports.
- **What stayed:** each file keeps what is its own (vcdec: the pictures,
  zero copy, flushes, the end of the stream; vcenc: the encoder's settings,
  pictures in, packets out), and holds a `vcm` for its connection.
  The public API (`vcdec.h`, `vcenc.h`) is unchanged. vcdec.c went from
  1447 to 884 lines and vcenc.c from 1238 to 722; vcmmal.c is 552.
- **What differed** is now a parameter: the component's name in
  messages, the pools' name, whether a pool must be user-writable (vcenc)
  or may be read in SVC mode (vcdec), whether pools are logged, and
  whether they're cacheable.
- **Checked:** vcdec_test's and vcenc_test's whole outputs (every log
  line, through the fake VideoCore) are identical before and after, and
  every host test passes. `libvcdec.a` now holds vcmmal.o too; the build
  scripts, the devkit's check and the host tests' builds include it.

## devkit 0.2.16, vcdec 0.4.3, vcenc 0.1.4, hevcdec 0.1.14, VCDecTest 0.3.4: VideoCore memory given back (2026-10-20)

VCDecTest 0.3.3's Multi on a Pi 4 (gpu_mem=128): a decoder takes its share
of the VideoCore's memory as its stream starts, and one 1080p decoder
fills it. A second opens without an error and then sends nothing until
the first is closed. With four open none started; at six the VideoCore
stopped answering and the machine locked up. Earlier the same day, every
decoder was refused (ENOMEM): memory still held by decoders a program
had left open, which on RISC OS nothing frees until a restart.

- **vcdec 0.4.3, free at the end:** once the end of the stream is taken
  (VCDEC_EOF), the decoder is destroyed, so its memory is free while the
  caller keeps vcdec open. A flush creates it again, as after any EOS
  (`stats.freed_at_end`). An EOS with nothing before it frees nothing (no
  decoder memory was taken).
- **Closed at exit:** vcdec, vcenc and hevcdec register an `atexit`
  handler at their first open that closes whatever is still open, with
  pictures still held. The handlers are `vcdec_close_all`,
  `vcenc_close_all` and `hevcdec_close_all`. They are public, so a
  program can call them from its own handler for the signals that end
  it: UnixLib runs no `atexit` handlers then (Escape, a crash).
- **vcdec's stall message:** with gpu_mem of 128 or more it now says the
  VideoCore has no memory left for the decoder (another decoder open, or
  one left behind). It no longer says "1080p needs gpu_mem=128".
- **VCDecTest 0.3.4:**
  - `-X n` exits after n pictures without closing. The Exit Obey file
    (ResultX) runs three of those, then a whole decode.
  - Multi goes to 3 decoders at 1080p and 4 at 360p (6 at 1080p wedged
    the VideoCore).
- **The fake VideoCore:**
  - With multi, each decoder takes its memory at its format change, and
    `multi_room` have it. One that starts with none left keeps its input
    and sends nothing until one of those is destroyed, as on the Pi.
  - `fake_gpu_mb` sets what the firmware reports.
  - Blocks: 160, closer together in multi.
- **Tests:**
  - Whole decodes check the decoder was destroyed at its end.
  - Multi with room for two (the third starts when the first ends, all
    right).
  - Multi with room for one and four decoders (the fourth waits past 5 s
    and says why).
  - `--exit` runs of vcdec_test (two decoders open, a picture held),
    vcenc_test and hevcdec_test, each checked after the libraries'
    handler.
  - `--exit-app`: VCDecTest `-X`.
  - 8 mutants, all caught.

## VCDecTest 0.3.3: several decoders at once (2026-10-20)

Reel Edit (MLT) keeps several clips open, each with its own `h264_vchiq`
decoder, and asked how many the Pi 4 can have open at once and what each
costs. It has never been measured: every Pi run so far had one decoder.

- **VCDecTest `-M n`:** n decoders open at once, each decoding the whole
  clip: on each turn, each is fed until its input is full and one
  picture is taken from it, held and given back (zero copy, 6 output
  buffers, as `h264_vchiq`). For each decoder: whether it opened, the
  pictures right, wrong and missing, its pools (MB). Then the pictures a
  second together. A decoder that fails is closed, and the others go on.
- **The Multi Obey file** (ResultMulti): 1, 2, 3, 4 and 6 decoders on
  the 1080p clip, then 4 and 8 on the 360p one.
- **The fake VideoCore** (`tests/host/mmaldecode_test.c`) can now be
  several decoders: with `multi`, each RMA stub claimed starts a
  decoder's state, and the SWIs that name a service handle or a stub
  switch to it. The memory, PCI blocks and pools stay shared.
  `multi_room` decoders fit. The ones after either keep their input and
  send nothing (as one 1080p decoder at gpu_mem=64 on the Pi), or have
  their create refused (`multi_full_way`). Which of these the Pi does is
  what Multi will show.
- **Tests:** -M 3, -M 1, the third stalling (closed after vcdec's 5 s,
  the other two right), the third refused at open, and a picture within
  rounding in each of two decoders. The decoders are checked to have
  overlapped. Every mutant was caught: one picture a turn, release,
  checksum, the close on failure, the fake's switch, the fake's full
  decoder, the result.
- vcdec and the devkit are unchanged.

## Built on GitHub with riscos-crossdev (2026-10-17)

- **`.riscos-deps`** pins riscos-crossdev v1.8.1, whose container image
  GitHub builds in.
- **`.github/workflows/riscos-build.yml`** (from riscos-crossdev's `ci/`):
  every push and pull request runs `ci/build.sh` and keeps `dist/`.
- **`.github/workflows/riscos-deps-update.yml`:** once a day, a pull
  request when riscos-crossdev has a newer release, built with it.
- **`ci/build.sh`:** `./build.sh`, then riscos-crossdev's checks on every
  program linked (the tool build scripts keep their ELFs in `KEEP_ELF`)
  and on libvcdec.a and libhevcdec.a, and its unaligned-cast check on our
  sources (`ci/unaligned-safe` lists the casts checked by hand).
- **`common/toolchain.sh`:** every build script finds the RISC OS
  compiler and elf2aif there: `$RISCOS_TOOLCHAIN` (riscos-crossdev), else
  `CROSS`/`ELF2AIF` as before, else `~/gccsdk/env`. No path is assumed.
- **`build.sh`** also builds the devkit, and skips (saying so) the zips
  whose clips need what the image lacks: MMALDecode and VCDecTest
  (libx264), HEVCTest (libx265, `FFMPEG_TARBALL`).

## devkit 0.2.15, hevcdec 0.1.13, HEVCTest 0.1.14: HEVC frames given back to RISC OS (2026-10-13)

Users have been running out of logical address space ("Unable to allocate
logical address space"; see AddrSpace). ReelHWAccel has no heap areas of
its own: the test programs' heaps are in their Wimp slots, and each
hardware buffer is a Physical Memory Pool reserving exactly its size. The
big consumer was `hevc_hwdec`'s frame pool at 4K. Measured on the fake
block (FFmpeg + hevc_hwdec, 4-picture clips), 4K 10-bit is 19 pools and
116 MB: about 50 MB of working buffers, plus 16.6 MB per frame. The pool
made a frame whenever none was free and kept them all until the decoder
closed (up to 32), so a player holding pictures decoded ahead kept its
peak for the whole film: 14 frames at 4K 10-bit is about 280 MB.

- **hevcdec 0.1.13: `hevcdec_frame_free`.** A frame no longer wanted goes
  back to RISC OS without waiting: at once if the block has finished every
  picture given so far, otherwise once it has (counted: pictures given,
  pictures finished), since the picture given last may still read it as a
  reference. Checked at hevcdec_decode, _frame_new, _frame_done and
  _finish. A dead decoder keeps its memory, as at close. `hevcdec_stats`
  gains `frames`, `frames_freed` and `frames_bytes` (rebuild with the new
  header: the struct is bigger).
- **hevc_hwdec (patch 0002): `spare_frames`** (default 2). A frame FFmpeg
  and the caller have let go of goes back to RISC OS when more than that
  many are free; 32 keeps them all, as before. The pool reuses the freed
  places.
- **HEVCTest 0.1.14: `-G`** gives every frame back as soon as no picture
  to come refers to it, before the next picture is given (as FFmpeg
  frees). The GiveBack Obey file (ResultG) does it pipelined, every
  picture checked, at 1080p and 4K, for the Pi.
- **Tests:**
  - The fake block now checks that the references are still there, and
    still the same pictures, when phase 2 finishes, not just when it
    starts (the block reads them throughout).
  - hevcdec_test: `-p -G` and `-P 5 -G` on every trace with a slow
    phase 2.
  - FFmpeg: `-spare_frames 32` against `0` (every picture exact, frames
    remade, no higher peak).
  - hevc_hw_test (output_hw, a 12-picture queue): hevcdec's frames drop
    from 15 to 7 once the queue empties.
  - The fake reports allocations and peak KB.
  - Mutants caught: freeing at once (79 failures), waiting one picture
    too few (18), hevc_hwdec never giving back (6).

## v0.2.14: devkit 0.2.14, vcenc 0.1.3, h264_encvchiq, VCEncTest 0.1.3 (2026-10-12)

riscos-ffmpeg released the encoder in riscos21 / Reel 0.1.28 (their
FFmpeg/handoffs/2026-10-12-reelhwaccel-encoder-released-riscos21). On the
owner's Pi 4, the Convert window converted a 1280x544 trailer with AAC at
3.4x real time with h264_vchiq decoding and the encoder, 2.2x with ARM
decoding, and 0.66x with libx264 veryfast. Every output played.

VCEncTest 0.1.2 on the Pi (ResultC): constant bit rate is refused every
way, at the enables (2 and 4, on new encoders) and on the running encoder
(EINVAL). So `-c` gives variable, the same streams byte for byte. Variable
holds the rate on real content: 1080p30 at 8 Mbit/s gave 8.04, and 720p at
3 Mbit/s gave 3.09.

- The encoder is renamed **`h264_encvchiq`** (riscos-ffmpeg's request; the
  decoder stays `h264_vchiq`): the codec's name, `ff_h264_encvchiq_encoder`,
  `h264_encvchiq_encoder_deps`, CONFIG_H264_ENCVCHIQ_ENCODER and the class.
  The patch is now `0003-avcodec-h264_encvchiq.patch`.
- **Profiles by name:** FFmpeg 5.1's `profile` option names none for H.264
  (`-profile:v main` failed), so the encoder has its own: `baseline`,
  `constrained_baseline`, `main`, `high`, or the numbers.
- **vcenc 0.1.3:** with VCENC_CBR, one encoder: variable at the enables,
  constant (2, else 4) tried on the running encoder. 0.1.2 made three
  encoders first, about 6 cs at open, for nothing on the Pi 4.
- devkit 0.2.14 carries all of this. VCEncTest 0.1.3 links vcenc 0.1.3.
- Tests: the fake refuses rate control on the running encoder by default,
  as the Pi did. The FFmpeg tests use the new name, `-profile:v main` on
  the command line (checked in the SPS the fake made), and
  `profile=constrained_baseline` through libavcodec. Mutant caught: the
  private profile ignored (High came out where Main was asked for).

## vcenc 0.1.2, devkit 0.2.13, VCEncTest 0.1.2: constant bit rate made again each way (2026-10-12)

VCEncTest 0.1.1's Options on the Pi 4: `-i` passes (every keyframe given
its SPS and PPS, 4 of 4), and 2 Mbit/s on its own is fine. `-c` still
failed: after the input's enable was refused with rate control 2, the
encoder refused every enable after it, 4 and then even 1 (variable),
which opens normally on a new encoder.

- vcenc 0.1.2: with VCENC_CBR, each way is tried on a new encoder
  (destroyed and made again after a refused enable): 2 before the
  enables, 4 before them, then 1 before them and 2 (else 4) set once the
  ports are running. `rate_control` says which took, and
  `rate_control_running` whether it was set while running. Without
  VCENC_CBR nothing changes.
- devkit 0.2.13: vcenc 0.1.2 (the patches are as in 0.2.12, apart from
  the wording of rc_constant's help).
- VCEncTest 0.1.2: the report says "set once running"; a CBR Obey file
  (ResultC) runs constant at 360p, 720p and 1080p beside variable at the
  same bit rates, to see whether constant holds the rate.
- Tests: the fake encoder now refuses every enable after a refused one,
  until it's made again (as the Pi), and refuses 4 at the enable too.
  Scenarios: three encoders and constant once running, 4 taken on the
  second encoder, constant refused every way. Mutants caught: no
  fallback, constant never set while running, 4 never tried, the encoder
  not made again.

## vcenc 0.1.1, devkit 0.2.12 (the h264_vchiq encoder), VCEncTest 0.1.1 (2026-10-12)

VCEncTest 0.1 on the Pi 4: every run in Test, Speed and Verbose passed.
Each stream decoded back with vcdec at a luma PSNR of 33 to 52 dB, and
FFmpeg's own decoder plays Stream360 and Stream1080 (High; levels 3.1 and
4.0). 1080p30 encodes at 72.6 frames a second and 720p at 156; copying a
1080p frame in takes 3.6 ms from a pool and 12 ms from PCI memory. The
SPS and PPS come as soon as the output has buffers (CONFIG, with
FRAME_END and pts 0). Two runs in Options failed:

- vcenc 0.1.1: with INLINE_HEADER the firmware sends the SPS and PPS as a
  CONFIG buffer of their own before each later keyframe. vcenc now keeps
  them, and with VCENC_INLINE_HEADERS puts them at the start of every
  keyframe's packet (the first one included); `headers_repeated` counts
  them.
- vcenc 0.1.1: constant bit rate (RATECONTROL 2) made the input's enable
  fail with EINVAL. vcenc now falls back to 4 (constant, skipping frames),
  then 1 (variable); `rate_control` says which it got.
- The h264_vchiq encoder (`ffmpeg/vchiqenc.c`, patch 0003, applied after
  0001, before or after 0002): YUV420P frames to Annex B packets with
  their pts (dts = pts, no B frames). The SPS and PPS are the extradata
  with global headers, otherwise at the start of the first packet. pict_type
  I forces a keyframe. b and g default to 0 (vcenc's 10 Mbit/s and the
  encoder's interval); profile, level, rc_constant, repeat_headers, qp_min,
  qp_max, in_buffers, pci_memory and gpu_mem_check are supported.
  Registered among the hardware encoders, so it's never the default.
- devkit 0.2.12: vcenc in libvcdec.a, vcenc.h, patch 0003, and how to use
  them in the README.
- VCEncTest 0.1.1: reports the rate control it got. Options now runs
  constant and 2 Mbit/s each on their own too, and constant at 720p.
- Tests: the fake encoder (now `tests/host/fake_vcenc.c`, and a library
  for FFmpeg's tests) behaves as the Pi did: CONFIG at the first output
  buffer, again on its own before later keyframes, constant refused at
  the input's enable. `tests/host/ffmpeg/enc_run.sh` builds FFmpeg with
  0001 and 0003. `vchiqenc_test` covers packets, pts, keyframes, headers
  both ways, settings, a slow encoder, an error, a quiet encoder (open
  fails) and sizes refused. The ffmpeg command writes raw YUV to .h264
  (headers first) and to MP4 (an avcC, no start codes). Mutants caught:
  no CBR fallback, or straight to variable; a repeated CONFIG given out;
  headers not put in front; in FFmpeg, a frame dropped when the input is
  full, no headers in front, pict_type ignored, no dts, no extradata, g
  ignored.

## vcenc 0.1, VCEncTest 0.1: H.264 encoding on the VideoCore (2026-10-12)

The next step after decoding: the VideoCore's H.264 encoder, for FFmpeg
(and so for recording or converting video on RISC OS). Not yet tried on a
Pi: VCEncTest 0.1 is its first run there.

- vcenc (`vcdec/vcenc.c`, `vcenc.h`, in libvcdec): the firmware's
  ril.video_encode over VCHIQ and MMAL, with vcdec's Pi-proven transport
  (buffers numbered from 1, each receive queued as its message arrives,
  contiguous cacheable pools or PCI memory, data of up to 128 bytes in the
  message). Set up as Linux's bcm2835-codec sets the encoder: I420 input
  in whole rows (width to 32, height to 16, the edges repeated into the
  padding, the picture's size as the crop), profile and level (the level
  chosen from the size, rate and bit rate unless given), bit rate,
  variable or constant, keyframe period, quantiser limits, SPS and PPS
  before keyframes, timing in the SPS. A program polled from null events
  sends pictures until the input is full and takes packets: whole access
  units in order with their pts, the SPS and PPS first on their own
  (CONFIG), keyframes flagged, a keyframe on request. Up to 1920x1088;
  1080p needs gpu_mem=128.
- VCEncTest 0.1 (`tools/vcenctest`): test pictures encoded as a program
  would, the stream checked (SPS and PPS first, order, pts, keyframes),
  kept with -w, and with -d decoded again by vcdec and compared (luma
  PSNR). Obey files: Verbose (every MMAL message), Test (360p, 720p,
  1080p), Options (each setting), Speed.
- Tests: `tests/host/vcenc_test.c`, a fake ril.video_encode on the fake
  VCHIQ (which now models a write-back cache: a picture not cleaned out
  of a cached pool isn't what the VideoCore reads): every packet's
  picture checked by its Adler-32, edges in the padding, pts, keyframes
  and headers; a slow encoder that holds inputs (input full), late
  receives, units in two pieces with the pts on either, bigger output
  buffers, settings refused, an encoder that goes quiet or reports an
  error, bad calls; VCEncTest on the fake. Mutants caught: no cache clean
  before a transmit, padding not repeated, short data ignored, a packet
  per buffer, pts or key flag lost, no keyframe request, no inline
  headers, no buffer growth, input not marked busy.
- devkit/build.sh (for 0.2.12, not built yet): libvcdec.a with vcenc,
  and vcenc.h.

## hevcdec 0.1.12, HEVCTest 0.1.13: converting into each kind of memory, timed (2026-10-12)

Reel's halved 4K 10-bit conversion straight into its VideoOverlay buffer
takes about 18 ms (riscos-ffmpeg's opt11/opt12 logs, block busy or
idle); HEVCTest -K's into cached memory, 9.3 ms. The overlay's memory is
probably not cacheable, and the conversion writes it column by column:
64-byte pieces of every output row in turn.

- hevcdec_conv_half: row by row too (HEVCDEC_CONV_ROWS: each output row
  written left to right across the columns), and hevcdec_convert_benchmark
  bits 0 for the halved conversion, either way. hevcdec_frame_to_i420_half
  is unchanged (column by column).
- HEVCTest -O (the Memory Obey file, ResultO): the last picture converted
  30 times into cached memory, a dynamic area that isn't cacheable but is
  bufferable, one that's neither, and a VideoOverlay buffer (YV12, as
  Reel makes it; HEVCTest becomes a Wimp task for it): 1:1 to 8-bit (up
  to 1920x1088) and halved, column by column and row by row, and a plain
  copy of the halved picture from cached memory; each checked against the
  cached column-by-column result. -q's line names hevcdec's version as
  such.
- Tests: hevcdec_conv_test now covers the halved row order (random
  windows); hevctest -O on the host (cached, the two areas as ordinary
  memory, no overlay), every way the same pictures. Mutant caught (the
  last chroma row not written row by row).

## devkit 0.2.11: hevc_hwdec honours skip_frame (2026-10-06)

riscos-ffmpeg asked (FFmpeg/handoffs/2026-10-06-reelhwaccel-skip-nonref-request):
at 4K 10-bit 60 fps and about 40 Mbit/s the block only just keeps up, so
once Reel falls behind it can't catch up; drop_before saves the ARM's
conversion but not the block's decoding.

- hevc_hwdec: the caller's skip_frame is passed to the inner HEVC
  decoder before each packet (it wasn't at all: one of the audit's low
  items). At AVDISCARD_NONREF FFmpeg 5.1's decoder drops non-reference
  NAL units (ff_hevc_nal_is_nonref) before parsing, so those pictures
  never reach the block.
- The count: the read-only option "skipped" (non-reference pictures'
  first slices in the packets decoded while skip_frame >= NONREF), and
  "N non-reference pictures skipped (skip_frame)" at close (verbose).
- hevcdec unchanged (0.1.11); HEVCTest unchanged (0.1.12).
- Tests: -skip_frame noref on small and small10: exactly the hevc
  decoder's pictures with the same skip_frame, the fake given only
  those, the count logged; set part way through output_hw (as a player
  behind would): every picture shown or skipped, the count read through
  the option. hevc_hw_test's freed-context check now starts once the
  context is gone (its own closing messages are fine). Mutants caught:
  skip_frame not passed, not counted.

## hevcdec 0.1.11, HEVCTest 0.1.12, devkit 0.2.10: once a picture, and without waiting (2026-10-05)

riscos-ffmpeg asked (FFmpeg/handoffs/2026-10-05-reelhwaccel-cache-invalidate-per-call):
Reel with the stats panel made two or three conversion calls a picture,
each cleaning and invalidating the whole frame; and at 4K 10-bit 60 fps,
once behind, every picture it converted was still in the block.

- hevcdec: a frame is cleaned and invalidated once after the block has
  written it (a flag, cleared when the frame is given to the block
  again), not on every conversion call. The program only reads frames,
  so nothing it caches between conversions goes stale.
- hevcdec_frame_done: whether a picture is decoded, without waiting
  (polling the phases as the other calls do).
- hevcdec_stats: convert_waits and cs_convert_wait (conversions that
  waited for their picture), cache_cleans.
- HEVCTest 0.1.12: says how many cleans and conversion waits there were;
  pipelined runs ask hevcdec_frame_done before each wait and count the
  pictures not yet decoded (-Q, for the host tests: checks a picture it
  called done is then not waited for).
- Host tests: one clean per picture decoded in a checked run (1:1,
  halved, 8-bit); frame_done with slow phases (some not done, none
  waited for after done) and quick ones (some done). Mutants caught: the
  flag not cleared on resubmission (stale pictures through the fake's
  caches), a clean on every call, frame_done always 1, always 0.

## hevcdec 0.1.10, HEVCTest 0.1.11, devkit 0.2.9: fixes from a code audit (2026-10-04)

A code audit of the HEVC side (also reported by riscos-ffmpeg's own
audit: FFmpeg/handoffs/2026-10-04-reelhwaccel-hevc_hwdec-audit) found:

- hevc_hwdec: hevcdec logged through the outer AVCodecContext, which is
  freed before hevcdec closes when a caller keeps output_hw frames (as
  Reel does): a use-after-free on every such close. hevcdec's messages
  now go through the shared object, without a context once the decoder
  is closed.
- hevc_hwdec: fill_dpb_entries took every one of FFmpeg's 32 DPB places
  marked as a reference into V4L2's 16-entry list; a damaged stream could
  overrun it into the decoder's own pointers. More than 16 now refuses
  the picture (AVERROR_INVALIDDATA).
- hevc_hwdec: the slices of a picture whose header failed after
  start_frame stayed for the next end_frame, which could send a freed
  packet's slices with another picture. Slices and end_frame are now
  taken only for start_frame's own picture (by its number); otherwise
  the picture is refused and FFmpeg lets go of its frame. A frame never
  given to the block is flagged, not given out as decoded. Flush resets
  the picture in hand.
- hevc_hwdec: weighted prediction tables filled for 16 references (15).
- hevcdec: rpivid reuses its motion-vector buffers (per frame slot) and
  bitstream copies at the size they were made for; a stream getting
  smaller then bigger again (with the decoder kept: its frames fit) had
  the block write past them. A size change now restarts rpivid after
  the pictures in flight finish, as Linux's driver does.
- hevcdec: the slice buffer ring moved on before a picture was
  accepted, so a picture refused in pipelined mode let the next one take
  the buffer of a picture phase 1 had still to read.
- hevcdec: pictures are checked as Linux's control core would (at most
  16 DPB entries and references, references inside the DPB, slices no
  longer than their data) before rpivid indexes its arrays with them.
- hevcdec: hevcdec_frame_to_i420 (now returning a result) and _16 check
  their window as _half does; _half's check can't overflow; a frame the
  block may still be writing (the decoder stopped) isn't converted.
- hevcdec_conv: halving 10-bit, a 2x2 sum of 4088 or more rounded to
  256 and wrapped to 0 (the brightest pixels black): saturated now
  (VQRSHRN, and a clamp). Halving with an odd height to the frame's last
  chroma row read the row below it (past the frame in the last column):
  that row is used twice.
- Tests: hevcdec_conv_test (from the audit's harness: random windows
  against a reference, NEON under qemu and plain C, frames ending at an
  unreadable page, guard bytes); the fake identifies pictures by all
  their slices and writes the motion vectors phase 2 would; hevcdec_test
  with a slow phase 1 and a refused picture, window and picture checks;
  FFmpeg: a size change part way (new tiny clip), damaged streams, and
  nothing logged through a freed context. Each caught its mutant (the
  old code for the ring and the size change; NEON and scalar wrapping;
  the row below; the log context kept).

## Release 0.2.8 (2026-10-04)

The first release on GitHub: devkit 0.2.8 (vcdec 0.4.2, hevcdec 0.1.9,
patches 0001 and 0002 for FFmpeg 5.1.10), with the Pi test programs
HEVCTest 0.1.10 and VCDecTest 0.3.2. The code is as below; the README
now describes where it has got to, with the Pi 4 figures.

## hevcdec 0.1.9, HEVCTest 0.1.10, devkit 0.2.8: one copy instead of two (output_hw) (2026-10-03)

Agreed with riscos-ffmpeg (FFmpeg/handoffs/2026-10-03-reelhwaccel-devkit-0.2.7-adopted):
Reel converts each picture when it shows it, straight into the overlay,
instead of hevc_hwdec converting it into an AVFrame that Reel copies again
(and halves, at 4K).

- hevc_hwdec `output_hw`: frames given out unconverted (AV_PIX_FMT_HEVCDEC,
  data[3] the hevcdec_frame, the window in the crop fields), not waited
  for. hevcdec and its frames are now reference counted (one reference
  the decoder's, one each frame in use): a caller's frames outlive
  avcodec_free_context, hevcdec closing with the last.
- hevcdec_frame_decoder(f), and hevcdec_frame_to_i420_half: the window
  halved into the caller's 8-bit planes, each sample the rounded mean of a
  2x2 block (as reelcore_halve_plane), 10-bit means rounded to 8 bits;
  NEON (VPADDL/VPADAL for 8-bit luma, VLD4 for chroma's U and V, two
  unpacked rows for 10-bit); polling between columns as the others.
- HEVCTest 0.1.10: every picture's halved conversion checked against the
  2x2 means of its 1:1 one; -K times the halved conversion too.
- Host tests: hevc_hw_test (libavformat and hevc_hwdec with output_hw, a
  queue of 12 frames as Reel's, each converted 1:1 and halved when shown,
  the last after the decoder is closed): 8-bit, 10-bit and 10-bit cropped
  on the left, exactly FFmpeg's pictures and their 2x2 means, nothing
  left. Mutations caught: rounding dropped, U/V mixed (8-bit and
  10-bit), the second row not read, hevcdec closed with frames still
  held, frames never letting go.

## devkit 0.2.7: hevc_hwdec's drop_before (2026-10-03)

riscos-ffmpeg asked (handoff 2026-10-03-reelhwaccel-hevc_hwdec-drop_before-request):
at 4K, Reel fell behind and set skip_frame, so the block was given only
keyframes, when the block wasn't the limit; the ARM's conversion of
pictures that would be thrown away as late was.

- hevc_hwdec: `drop_before` (int64 pts in pkt_timebase, default
  INT64_MIN), as their h264_vchiq patch 0022: a picture with an earlier
  pts is decoded by the block but not converted and not given out (also
  a held pipelined picture that has become late, and at the end of the
  stream); the count is logged at close (verbose).
- Host test: -drop_before at the 8th pts of small: the 13 later pictures
  given out, exactly the hevc decoder's last 13, all 20 decoded by the
  (fake) block, the count logged, pipelined and not; the option ignored
  (mutation) fails it.
- hevcdec unchanged (0.1.8).

## hevcdec 0.1.8, HEVCTest 0.1.9, devkit 0.2.6: the block kept busy during conversion (test zip, 2026-10-03)

The hot paths, looked at (project doc reelhwaccel-hevc-hotpaths.md):
hevcdec saw a phase finish only when the program next called it, so in
a player (hevc_hwdec, pipelined) phase 1 of the next picture finished
early in the conversion of this one, phase 2 didn't start until after
it, and the program then waited all of phase 2: each picture cost the
conversion plus phase 2 rather than the longer of the two. HEVCTest
0.1.8's pipelined checked runs showed it (phase times of 112/126 cs per
60 1080p 10-bit pictures against 11/19 cs unchecked).

- hevcdec: the conversions call back between columns (or every 64
  rows), and hevcdec polls the block there and once before, so the next
  phase starts during the copy. `hevcdec_stats.waits`: how many times
  the program had to wait.
- hevcdec_conv.c rewritten as one function with an order: column by
  column (the default), row by row, column by column with PLD 8 rows
  ahead, or (10-bit) through a row buffer (0.1.7's). Whole 10-bit rows
  are unpacked straight into the planes: luma to 8-bit with VST3 of the
  narrowed fields; chroma with VLD2 of the word pairs (U = a0 a2 b1, V =
  a1 b0 b2) and VST3 into U and V, 16-bit or 8-bit.
- `hevcdec_convert_benchmark` and HEVCTest -K: a picture converted 30
  times each way, timed, each way's output compared with the default's;
  the Convert Obey file (1080p and 4K, 8-bit and 10-bit; ResultC).
  HEVCTest prints how many times the program waited.
- Host tests: pipelined with phases of 2 and 3 polls, the program waits
  at most twice a clip (without the polling, every picture); -K on every
  clip (cropped ones included) gives the same pictures every way.
  Mutations caught: no polling during conversion, V's fields in the
  wrong order, 8-bit luma or chroma narrowed wrongly, row order reading
  the wrong row.

## hevcdec 0.1.7, HEVCTest 0.1.8, devkit 0.2.5: 10-bit and 4K (test zip, 2026-10-03)

- hevcdec: `config.bit_depth` 10 (Main 10). Frames are
  V4L2_PIX_FMT_NV12_10_COL128, as rpivid_video.c sizes them: the same
  128-byte columns, each row 32 little-endian words of three 10-bit
  samples (96 luma, or 48 U,V pairs), the width up to whole columns of 96.
  `hevcdec_frame_to_i420_16` gives 16-bit samples (FFmpeg's YUV420P10);
  `hevcdec_frame_to_i420` from a 10-bit decoder gives each sample's top 8
  bits. NEON: 8 words into 24 samples with VST3, U and V split with VLD2.
  A picture whose depth isn't the decoder's is refused (UNSUPPORTED).
- hevc_hwdec (patch 0002): 10-bit 4:2:0 too (FFmpeg's HEVC get_format
  offers AV_PIX_FMT_HEVCDEC for YUV420P10), out as YUV420P10, or YUV420P
  with `-output_8bit 1`. A depth change part way is refused (ENOSYS),
  said once. 4:2:2, 4:4:4 and 12-bit are still refused.
- 4K: nothing new in the code (sizes up to 4096x4096 were accepted);
  now tested, on the host and in HEVCTest's UHD Obey file.
- The fake HEVC block writes 10-bit frames when CONFIG2 says 10-bit, and
  checks CONFIG2's luma and chroma depths agree. Host clips small10,
  slices10, odd10 (four and a half columns), crop10 (cropped on the left
  by 16: samples split across words), uhd and uhd10 (3840x2160); in
  FFmpeg the 10-bit clips, a 10-bit cropped clip, `-output_8bit 1`
  against FFmpeg's samples' top bits, a depth change part way, 4K, and
  4:2:2 and 12-bit refused. hevcdec_test matches pictures over the
  trace's output window, so cropped clips can be fed whole pictures.
  Mutations caught: the word's samples in the wrong order, U and V
  swapped, the luma or chroma column offset wrong, the 8-bit shift wrong,
  8-bit columns for 10-bit frames, the last chroma row missed, 8-bit V
  from U, 10-bit given out as 8-bit, the depth not compared on reopening.
- HEVCTest 0.1.8: clips small10, slices10, odd10, crop10, hd10 (Test,
  Pipelined, Speed, Dump), and uhd and uhd10 (the new UHD Obey file:
  checked, timed, timed pipelined, ten frames). 10-bit pictures are
  checked as 16-bit against FFmpeg's, and their 8-bit conversion against
  the 16-bit one.

## vcdec 0.4.2, hevcdec 0.1.6, devkit 0.2.4: never the program's page at &8000 (test zips, 2026-10-03)

riscos-unixlib traced the "code 6" EMT trap to our contiguous memory:
OS_Memory 12 may recommend pages an application slot is using, the
running program's own page at &8000 included. Claiming them makes the
kernel copy the program to another page, so its page at &8000 changes
physical address. ARMEABISupport 1.08 finds programs by that address and
has no service handler (it never sees Service_PagesSafe), so it loses
the program: its stacks are never freed, and a later program given the
old page has no abort handlers and traps at its first stack extension.

- `common/contig.h` (new, shared): reads the page at &8000 (OS_Memory 0:
  page number and physical address), and asks OS_Memory 12 for pages
  wholly below it, else wholly above it (R4-R7). A RISC OS before 5.29
  ignores R4-R7, so the recommendation is also checked by page number,
  and refused if it holds the program's page.
- vcdec 0.4.2 (pools) and hevcdec 0.1.6 (all its buffers) use it, and
  count claims after which the page at &8000 moved anyway
  (`vcdec_stats.app_page_moves`, `hevcdec_stats.app_page_moves`: should
  be 0). hevcdec logs why contiguous memory couldn't be had.
- HEVCTest 0.1.7 and VCDecTest 0.3.2 report the program's page at &8000
  at the start and the end of each run; a move is a failure.
- Host tests: the fake RISC OS has an application page that OS_Memory 12
  will recommend and that a claim moves (with page numbers and addresses
  that differ, so mixing them up shows), honours R4-R7 or (as before
  5.29) ignores them. vcdec_test: the page in the way, near the top
  (room only below), an old RISC OS with it in the way (refused) and out
  of the way, and a move counted; VCDecTest fails on a move. Mutations
  caught: one range only, above only, the page-number check
  dropped, above off by one, the count dropped, the move not a failure.

## devkit 0.2.3: hevcdec's header is hwhevcdec.h (2026-10-03)

- `hevcdec.h` renamed `hwhevcdec.h` (guard HWHEVCDEC_H), so it no longer
  shares a name with FFmpeg's `libavcodec/hevcdec.h`. Patch 0002's
  configure check and `hevc_hwdec.c`'s include follow. Nothing else
  changes: the library is still `libhevcdec.a`, the API still `hevcdec_*`,
  hevcdec still 0.1.5.

## hevc_hwdec in FFmpeg 5.1.10, devkit 0.2.2 (2026-10-03)

HEVCTest 0.1.6 on a Pi 4: every picture right; pipelined, 1080p at 352.9
pictures a second (phase 2 bound, about 2.8 ms a picture), against
222-231 waiting for each.

- `ffmpeg/0002-avcodec-hevc_hwdec.patch` (`ffmpeg/hevc_hwdec.c`,
  `--enable-libhevcdec`): the hwaccel `hevc_hwdec` on FFmpeg's HEVC
  decoder (AV_PIX_FMT_HEVCDEC; FFmpeg parses and keeps the DPB, each
  picture goes to hevcdec as V4L2 stateless controls into a frame of
  hevcdec's), and the decoder `hevc_hwdec` around it giving YUV420P with
  the cropping applied. Refused with ENOSYS (so the caller can use `hevc`)
  for other than 8-bit 4:2:0, over 4096x4096, a size growing part way, or
  no block: at open when the extradata has the SPS, else at the first
  picture. A picture the block fails on is flagged as corrupt and
  decoding goes on. Options `pipelined` and `cached_frames` (both on).
- Tests: `tests/host/ffmpeg/hevc_run.sh` builds FFmpeg with 0001 and 0002
  over the fake HEVC block and checks every picture of each clip against
  `hevc` (pipelined and not, cached and not, -ss, a raw stream, a cropped
  stream, a failed picture, 10-bit and no block refused). It found the top
  and left crop applied twice. 10 of 10 mutations caught.
- The fake HEVC block can fail a chosen picture (HEVC_FAKE_FAIL), and its
  picture hash ignores trailing zero bytes (as raw streams' slices end).
- devkit 0.2.2: adds libhevcdec.a, hevcdec.h, hevc_ctrls.h and the 0002
  patch, link-tested.

## hevcdec 0.1.5 and HEVCTest 0.1.6: the two phases overlapped (test zip, 2026-10-02)

HEVCTest 0.1.5 on a Pi 4: all right. The EMT trap on a quick start from
the Filer happens with NoHW too (no HEVC block, no hevcdec): it's
UnixLib's / ARMEABISupport's stack-guard handling at start-up (the abort
is the stack probe in __riscosify), reported to riscos-unixlib.

- hevcdec: the block's phase 1 of one picture can run alongside phase 2
  of another, as rpivid_h265.c is written for. Claims are now queued and
  granted by hevcdec (one phase 1 and one phase 2 at a time, in order;
  phase 1 only while one of the 3 PU/coefficient buffer sets is free,
  phase 2 giving one back), and the polling moves whichever phase
  finished on to the next claim.
- hevcdec_config.pipelined: hevcdec_decode returns once the picture is
  given to the block; hevcdec_frame_wait for a picture, hevcdec_finish
  for all; converting a picture still being decoded waits for it.
  Without it, as before. hevcdec_stats.cs_wait.
- Each picture's phase 1 command list now has its own buffer until rpivid
  unmaps it, and the slices go through a ring of 3 buffers (phase 1
  reads a picture's last slice from there): with one of each, the next
  picture overwrote them while phase 1 still read them.
- HEVCTest: -p (each picture checked while the block decodes the next),
  -P n (n pictures ahead); Obey file Pipelined (ResultP).
- The fake HEVC block runs its phases as the Pi does: started by their
  last register write, finished later, one of each at a time, reading
  its command list and bitstream as it finishes, and checking that no
  phase 1 writes buffers a phase 2 is still to read. Mutations caught:
  no phase 1 allowance, a phase started while busy, one slice buffer,
  one command list, no limit on pictures given, pipelining ignored,
  converting without waiting.

## HEVCTest 0.1.5: whose is the EMT trap? (test zip, 2026-10-02)

HEVCTest 0.1.4 on a Pi 4: every picture right; converting the 60 1080p
pictures 14 cs with cacheable frames against 174 before (2.3 ms a
picture; uncached, with the NEON conversion, 40 cs); 1080p decoding at
222-261 pictures a second; Repeat fine again. But hevctest started from
the Filer straight after another Obey file stopped once more with
"EMT trap", never within one Obey file.

- hevctest -q: starts, writes a line, stops, without the HEVC block; Obey
  file NoHW (ResultN). The ReadMe asks for NoHW after NoHW, NoHW after
  Speed, Speed after Speed: if NoHW after NoHW fails too, it isn't
  hevcdec's.
- On an exception, hevctest prints UnixLib's __ul_errbuf (RISC OS's
  error number, message and pc) before the backtrace.
- hevcdec is unchanged (0.1.4).

## hevcdec 0.1.4 and HEVCTest 0.1.4: cacheable output frames, NEON conversion (test zip, 2026-10-02)

HEVCTest 0.1.3 on a Pi 4: every picture of every clip right; nothing
written past any buffer's end (so the block doesn't overrun them); the
block idle at close (interrupt control &44); Repeat, 13 runs back to
back, without the start-up crash. 1080p decoded at 222-231 pictures a
second, but converting each picture out of the block's format took
29 ms (174 cs for 60): memory that isn't cacheable, read byte by byte for
chroma.

- hevcdec: output frames cacheable (hevcdec_config.cached_frames, on by
  default), as vcdec's pictures: cleaned after they're zeroed and after
  their guard is written, cleaned and invalidated (the kernel's
  Cache_CleanInvalidateRange ARMop, from OS_MMUControl 2, in SVC mode)
  before hevcdec_frame_to_i420 reads one and before a guard is checked.
  Without the ARMop, not cacheable, as before. hevcdec_stats: cs_cache,
  cached_frames.
- hevcdec_conv.c: the conversion column by column (the frame read in
  order), luma 64 and 16 bytes at a time, chroma split by NEON's VLD2
  (built for NEON: the block is only on the Pi 4).
- HEVCTest: -u (frames not cacheable); says which; Obey file Uncached
  (ResultU) to compare the conversion time.
- The fake HEVC block models the caches: the program's view and the
  block's memory apart, brought together only by the cache maintenance;
  what the program wrote and didn't clean is written back after phase 2.
  A missing invalidate or clean fails the tests.

## hevcdec 0.1.3 and HEVCTest 0.1.3: guard areas (test zip, 2026-10-02)

HEVCTest 0.1.2 on a Pi 4: every picture of every clip exactly right
(Test), 1080p at 230 pictures a second. But hevctest run again straight
after itself sometimes stopped at once: "Fatal signal received: EMT
trap" (a RISC OS exception; ARMEABISupport's code 6 is "not one of
mine"), inside UnixLib's filename translation for fopen at start-up,
before hevcdec runs. Code that runs fine every other time faulting
there points at the new program's memory being changed: the previous
run's buffer pages go back to RISC OS and may be given straight to the
next program, so the block writing anything after it's finished, or
past a buffer's end, would show this way.

- hevcdec: every buffer the block uses (frames, the slice buffer, the
  command list, rpivid's PU/coefficient and collocated buffers) has 16K
  of pattern after it, checked as it goes and by hevcdec_get_stats
  (overruns, overrun_max, overrun_what); written into, it's logged.
  hevcdec_close gives the block 2 cs before its memory goes, and logs
  its interrupt control register.
- HEVCTest: says whether the block wrote past any buffer (a FAILED
  result if so); on an exception, prints RISC OS's error before
  UnixLib's backtrace; new Obey file Repeat (ResultR): hevctest 13
  times back to back.
- Host tests: the fake block can write past its frames; caught and
  reported, the pictures still right.

## Licence: GPL version 2, as Linux's drivers (2026-10-02)

At the owner's request (as riscos-ffmpeg already says it): ReelHWAccel is
under the GNU GPL version 2, without "or later", the licence of Linux's
vchiq-mmal driver. vcdec is a new implementation over RISC OS's VCHIQ
module with its own MMAL client, based on the MMAL message formats as
that driver's headers define them (there's no published specification);
it contains none of the driver's code, but having used the driver as its
reference it is treated as derived from it. hevcdec the same with
Linux's rpivid driver; rpivid_h265.c, included unchanged, keeps its own
"version 2 or later".

- Every file's notice, the ReadMes, the README (with the reasoning) and
  the devkit's README now say GPL version 2; h264_vchiq's vchiqdec.c and
  its patch too.
- tools/hevctrace's patch remade: it was missing hevc_trace.c's clearing
  of DPB slots let go of (in the source since hevcdec 0.1). The traces
  it makes for HEVCTest's clips are unchanged.

## hevcdec 0.1.2 and HEVCTest 0.1.2: pictures without scaling lists right (test zip, 2026-10-02)

HEVCTest 0.1.1 on a Pi 4: with flat scaling lists given (Flat, -s) every
picture of every clip was exactly as FFmpeg decodes it, 1080p included
(240 pictures a second). Test without them still failed, the same way it
had after nowpp's lists were loaded; Dump and Verbose, run after Flat,
were right without -s. So the block keeps its scaling factors from one
decoder to the next and uses them for streams without scaling lists too:
SPS1's scaling bit is set anyway by the PCM fields overflowing into it
(FFmpeg gives 255 and 253 without PCM, as rpi-ffmpeg does for Linux), and
rpivid_h265.c only loads the factors when the SPS enables scaling lists.

- hevcdec: a picture without scaling lists is given flat ones (all 16)
  with a copy of its SPS enabling them, so the factors are loaded for
  every picture.
- The fake HEVC block now behaves as the Pi did: a picture whose phase 1
  doesn't load the factors comes out wrong. (Without the fix it fails
  small, slices and odd and passes nowpp, as HEVCTest 0.1 did.)
- HEVCTest: Flat removed (hevcdec does it now); -s and Dump kept.

## hevcdec 0.1.1 and HEVCTest 0.1.1: why the pictures are wrong (test zip, 2026-10-02)

HEVCTest 0.1 on a Pi 4: the block ran every picture through both phases
with no errors or retries, and 1080p went through at 230 pictures a
second (unchecked). The clip with scaling lists (nowpp) was exactly
right, all 12 pictures. Every clip without them (small, slices, odd,
hd) was wrong from its first picture, and differently from run to run.
odd has no WPP either, so the difference is the scaling lists: with
none, rpivid_h265.c doesn't load the block's scaling factors, and the
block may be reading whatever its factor memory holds.

- HEVCTest: -s gives flat scaling lists (all 16, which is what "no
  scaling lists" means) to pictures without any, so the factors are
  loaded; the same pictures should come out. -d file saves every
  picture decoded (8-bit 4:2:0, decoding order) to compare with FFmpeg's.
  New Obey files Flat (ResultS: every clip with -s) and Dump (ResultD;
  DumpSmall twice and DumpOdd). The clips are the same as 0.1's.
- hevcdec: a picture with scaling lists enabled but none given is
  refused (rpivid_h265.c would read through a null pointer).
- Host tests: -s loads flat factors for every picture of odd and leaves
  nowpp's own; -d writes FFmpeg's pictures; the refusal.

## hevcdec 0.1 and HEVCTest 0.1: HEVC on the Pi 4's HEVC block (test zip, 2026-10-02)

The first HEVC decoder, for the first test on a Pi 4.

- hevcdec: a stateless HEVC decoder on the Pi 4's HEVC block. Each
  picture comes as the V4L2 stateless HEVC controls and its slices;
  Raspberry Pi's `rpivid_h265.c` (unchanged, GPL-2.0-or-later, under a
  kernel shim) builds phase 1's command list and phase 2's registers;
  hevcdec runs both, polling the interrupt status (phase 1 run again
  with bigger PU/coefficient buffers when they run out). The block's
  registers mapped with OS_Memory 13 and written in SVC; its clock on at
  its maximum through the firmware (BCMSupport); its memory physically
  contiguous, uncached, from Physical Memory Pools below &FC000000 (bus
  address = physical address on the Pi 4). A phase that never finishes
  is reported and the decoder refuses to go on, its memory left alone.
  8-bit 4:2:0 only, one picture at a time, output NV12 in 128-byte
  columns (hevcdec_frame_to_i420 converts any window of it).
- tools/hevctrace: a patch for FFmpeg 5.1.10's HEVC decoder that writes,
  with HEVC_TRACE=file, each picture's controls and slices (filled as
  Raspberry Pi's FFmpeg fills them for Linux) and Adler-32s of its planes.
  hvtdump.py prints and checks traces.
- HEVCTest 0.1 (tools/hevctest): replays traces through hevcdec on the Pi
  and checks every picture. Test (Result): five clips, 352x288 (WPP,
  B-pictures), 416x240 in 4 slices, 416x240 without WPP and with the
  default scaling lists, 426x240 (cropped), and 1080p 60 pictures.
  Speed (ResultT): the 1080p clip timed without checks. Verbose
  (ResultV): the first 3 pictures of the small clip with every step.
- Host tests: a fake HEVC block that reads the command list (register
  and table addresses, the bitstream, the slices' references), checks
  each reference holds the right picture, and writes FFmpeg's picture in
  128-byte columns; with phases that run out of buffer, never finish,
  and a slice refused mid-picture.

## devkit 0.2.1: h264_vchiq zero-copy (2026-10-02)

VCDecTest 0.3.1 on a Pi 4: every run OK. Every picture was right held in
place (-Z) for small, odd and hd, and for hd holding 4 at a time in 6
buffers through a seek after the end and a close with 4 still held. The
copying runs (Test, Memory, Sync, Seek, SeekEnd, Verbose) were also right.
1080p without checks: copied 82.4 pictures a second with 9.9 ms of each
picture inside vcdec_receive; zero-copy 85.7 with 0.13 ms; zero-copy with
each picture read once (as a player showing it) 84.7, 4 buffers 84.7,
4 held of 6 84.7. So the decoder and its transfers now set the rate
(about 85 a second) and the ARM is left almost all its time.

- devkit 0.2.1: vcdec 0.4.1 and the patch with zero_copy (on by default)
  and out_buffers.

## vcdec 0.4.1 and VCDecTest 0.3.1: zero-copy (test zip, 2026-10-02)

At 82 pictures a second the copy out of vcdec's buffer took 9 ms of each
1080p picture's 12 (1.4 ms on its own): it competes with the VideoCore's
decode and transfers. So the caller can now have the buffer itself.

- vcdec_receive_hold: the next picture's planes in vcdec's own buffer
  (cache cleaned and invalidated first), read only, until vcdec_release.
  VCDEC_UNSUPPORTED (the picture left for vcdec_receive) for PCI memory,
  an SVC-only pool, or when holding it would leave the decoder fewer than
  2 buffers, so a caller holding too many falls back to copies instead of
  stalling the decoder. Held buffers are left alone by flushes, the
  decoder created again, and format changes (grown when released).
- vcdec_close with pictures held: everything but their buffers goes; the
  last vcdec_release frees those, the RMA block and the rest.
- Up to 16 output buffers (was 8). vcdec_stats: holds, held_now.
- h264_vchiq: zero_copy (default on): frames wrap the held buffer
  (av_buffer_create, read only), released when freed, copies when a
  picture can't be held; out_buffers (default 6 with zero_copy, else 3).
  Frames can outlive the decoder.

VCDecTest 0.3.1: -Z (held, checked in place), -H n (n held at a time),
-R (each held picture read once). New Obey file ZeroCopy (ResultZ); Speed
compares the copy with zero-copy.

## devkit 0.2: vcdec 0.4 for FFmpeg, h264_vchiq's pci_memory (2026-10-02)

VCDecTest 0.3 on a Pi 4: every run OK. Every picture right with the
defaults (small, odd, hd; Sync, Seek, SeekEnd and Verbose too) and in PCI
memory copied by LDM 8 (the fallback). 1080p without checks: 82.4 pictures
a second (defaults), 64.6 (0.3's defaults), 66.6 (PCI, LDM 8), 82.8 with
6 buffers, 80.2 with -S.

Where the time goes at 82 a second (12.1 ms a picture): the copy, 9.1 ms
during a decode against 1.4 ms alone; the cache 0.4 ms; messages, queueing
and handing back about nothing; vcdec_send 0.9 ms. With -S the waits for
transfers take 6.9 ms and the copy 4.9 ms. So the copy is slowed by the
VideoCore's own traffic (its decode and the 3 MB bulk transfer of the next
picture, about 7 ms) running at the same time.

- h264_vchiq: `pci_memory` option (default off) for 0.3's PCI memory.
- devkit 0.2: vcdec 0.4 and the patch with that option.
- Host tests: the FFmpeg tests check the pool is used by default and not
  with pci_memory (the fake counts pools made).

## vcdec 0.4 and VCDecTest 0.3 (test zip, 2026-10-02)

VCDecTest 0.2 on a Pi 4 (1080p, no checks): PCI memory with LDM 4 gave 64
pictures a second; PCI with LDM 8, 72; a cacheable pool, 82, every
picture right in the checked runs (so the cache maintenance is right). The
copy on its own: 6.2 ms (PCI, LDM 4), 4.3 (PCI, LDM 8), 1.9 (pool, LDM 4),
1.4 (pool, LDM 8 or NEON); an uncached pool is no better than PCI memory.
More output buffers (4, 6) made no difference. The RAM disc's flags,
&100122, confirm bit 20 as RISC OS 5's PMP flag.

- The defaults are now a cacheable pool and LDM 8. If a pool can't be had
  (no OS_MMUControl 2, pools refused, pages not contiguous below 1 GB),
  PCI memory instead, logged; from then on, for every buffer made. Each
  buffer remembers which it is (freed and copied accordingly).
  VCDEC_OUT_PMP now means a pool or nothing; VCDEC_OUT_PCI and
  VCDEC_COPY_LDM4 ask for 0.3's defaults.
- vcdec_stats: pool_buffers and pci_buffers, and centiseconds spent on
  messages, queueing transfers, handing buffers back, the cache and the
  copy (at 82 a second the copy is under 2 ms of each picture's 12).

VCDecTest 0.3: -m auto (the default), -c ldm8 the default; reports where
the time inside vcdec went and how long vcdec_send took. Memory now checks
PCI memory with LDM 8 (the fallback); Speed compares the defaults with
0.3's.

## vcdec 0.3 and VCDecTest 0.2 (test zip, 2026-10-02)

Three ways that might make vcdec faster, each to be timed on the Pi
before it is used (the defaults are unchanged):

- vcdec_config.out_buffers: 1 to 8 output buffers (3 so far).
- VCDEC_COPY_LDM8 and VCDEC_COPY_NEON: pictures copied out by LDM/STM of
  8 words, or by NEON 64 bytes a loop, instead of 4 words. NEON only in
  USR mode (from a pool): in SVC mode an IRQ could leave the VFP context
  lazily switched off; there it copies by LDM 8.
- VCDEC_OUT_PMP (and VCDEC_OUT_UNCACHED: not cacheable, bufferable):
  pictures arrive in Physical Memory Pools (OS_Memory 12 pages for DMA,
  contiguous below 1 GB, claimed straight after with OS_DynamicArea 21 and
  mapped with 22) instead of PCI_RAMAlloc memory, which the PRM says is
  uncachable and privileged. The pools are removed before the RMA block
  holding their handler is freed (kept if a pool won't go). The pool's pages are checked
  contiguous (OS_Memory 0) and readable from USR mode (OS_Memory 24; if
  not, copied in SVC mode); a cacheable pool is cleaned and invalidated
  over each picture (ARMop 21, Cache_CleanInvalidateRange) before it's read.
- vcdec_copy_benchmark: one picture's copy timed, each way.
- received() reads both callback counters in one SVC copy.

VCDecTest 0.2: -B n, -c ldm4|ldm8|neon, -m pci|pmp|pmpu, -K; the RAM
disc's area flags shown (to confirm RISC OS 5's PMP flag, bit 20). New
Obey file Memory (every picture checked in both pools); Speed times hd
one change at a time.

Host tests: the copy routines on random data (each way, USR and SVC, every
alignment); the fake now has pools, OS_Memory 0/12/24, the ARMop, a cache
that holds stale pictures until it's cleaned, privileged pools, and
aborted receives.

## h264_vchiq for FFmpeg 5.1.10, and devkit 0.1 (2026-10-02)

- ffmpeg/vchiqdec.c and ffmpeg/0001-avcodec-h264_vchiq.patch: FFmpeg's
  h264_vchiq decoder on vcdec (`--enable-vchiq`, GPL; asked for by name).
  Packets through h264_mp4toannexb; the SPS and PPS of Annex B extradata
  put in front of keyframes that haven't their own; pictures as YUV420P
  frames with their pts. receive_frame waits only while the VideoCore's
  input is full or at the end; flush is a seek. Streams it can't take
  give AVERROR(ENOSYS) at open (profile, bit depth, size, gpu_mem, no
  VCHIQ), so a player can open h264 instead; a failure part way gives
  AVERROR_EXTERNAL, then the packets are taken to the end.
- vcdec 0.2: an EOS with nothing sent before it is the end at once (the
  VideoCore isn't told, and a flush after it needs no new decoder).
- tests/host/ffmpeg: FFmpeg 5.1.10 built for arm-linux with the patch and
  linked with vcdec and the fake (tests/host/fake_vc.c, the fake as a
  library); libavcodec's API (whole, seeks before and after the end, a
  slow decoder with its input full, refusals, Annex B extradata, an
  error) and the ffmpeg command (framecrc, whole and with -ss). Run when
  FFMPEG_TARBALL is set. The fake gained in_slow (input buffers taken one
  every so many polls) and an mvhd and tkhd in its MP4.
- devkit/build.sh: riscos-reelhwaccel-devkit-0.1.tgz (libvcdec.a,
  vcdec.h, the patch, README).

## vcdec 0.1 and VCDecTest 0.1 (test zip, 2026-10-02)

On a Pi 4 (gpu_mem=128) every run was OK: every picture of the three
clips right (within rounding), in display order with its own pts.

- Test: 640x360 400 pictures a second, 426x240 273, 1080p 45.3 (each
  picture copied out and checksummed). Sync (each receive waited for):
  375, 250, 41.6, so letting the receives run on is worth about 10%.
- Speed (no checksums): 1080p 64.6 pictures a second with each copied
  into the caller's planes (58.3 with -S; MMALDecode 0.17: 58-59).
- Seek: flush in 3 cs, the 3 pictures already decoded dropped, all 20
  from the keyframe on.
- SeekEnd: after the EOS the decoder is created again (1-2 cs) and all
  20 (small) and 50 (hd) pictures from the keyframe come back; the new
  component sends its format change again. The fake's fresh decoder is
  the Pi's.
- open and close: 0-1 cs.

vcdec, the library Reel and FFmpeg will use for H.264 on the VideoCore,
built on what MMALDecode 0.12 to 0.17 found on a Pi 4:

- vcdec_open (checks the size, up to 1920x1088, and gpu_mem: 1080p needs
  128 MB), vcdec_send (one Annex B access unit with its pts and dts;
  VCDEC_AGAIN when the 20 input buffers are full, never half sent; the
  SPS's profile checked: Baseline, Main or High), vcdec_send_eos,
  vcdec_peek and vcdec_receive (the next picture in display order,
  copied by LDM/STM into the caller's three planes), vcdec_flush,
  vcdec_close, vcdec_poll. VCDEC_UNSUPPORTED says "decode it in
  software".
- The pictures' bulk transfers are queued as their messages arrive and
  run on while vcdec carries on, counted by the RMA callback;
  VCDEC_SYNC_RECEIVE waits for each instead, as MMALDecode did.
- A flush before the end of the stream is FLUSH of both ports (0.17's
  Seek); after the end (EOS sent) the component is destroyed and created
  again, since a flush alone loses the last pictures at the next EOS.
- A format change to another size is acted on between calls; pictures
  not yet taken keep their layout, and their buffers grow when taken.
- A decoder that says nothing after the first input (gpu_mem too small)
  or after the EOS is reported, not waited for.
- The copy routines (vcdec_copy.S) run in SVC mode, as PCI memory needs:
  32 bytes a loop by LDM/STM, then words, then bytes, exact lengths, any
  alignment.

VCDecTest drives it as a player would (send until full, take what's
ready) and checks every picture against FFmpeg's: Test, Sync, Seek,
SeekEnd (a seek after the end: the decoder created again), Speed,
Verbose. The clips are MMALDecode 0.17's.

Host tests: tests/host/vcdec_test.c against the same fake, which now
also refuses output buffers before its format change, can finish
receives late, makes a fresh decoder when the component is created
again, and can change format
part way or during a flush.

## MMALDecode 0.17 (test zip, 2026-10-02)

On a Pi 4: Seek and SeekD (EOS held back) kept all 60 pictures, the
EOS buffer's pts 2400000; SeekE (EOS before the flush) lost the last
two. So a flush is clean as long as no EOS reached the decoder before
it; after an EOS, the decoder doesn't drain at the next one.

0.16 on a Pi 4: DISCONTINUITY (echoed back on the first picture, flags
&1C), a component disable/enable, and mmaldec's disable-flush-enable all
lost the same two pictures. In every seek run so far the whole clip and
its EOS had been sent before the flush (60 samples, 20 buffers ahead):
the decoder lost its last pictures at the second EOS. Without a seek,
EOS always drained.

- -s holds EOS back until after the seek (as a player seeking before the
  end), when the seek point is at least 8 samples from the end; -E sends
  it early as before. Obey files Seek, SeekD (-F) and SeekE (-E);
  SeekDC, SeekC and SeekM removed (the options stay).
- The fake loses the waiting pictures only after a flush that followed
  an EOS, which fits every Pi run; a flush before any EOS is assumed to
  drain (Seek checks it).

## MMALDecode 0.16 (test zip, 2026-10-02)

0.15 on a Pi 4: every MP4 check OK; 1080p 36.5 pictures a second with
checksums, 59.2 timed (one LDM/STM copy each). EOS on its own buffer
didn't save Seek: after a flush or a disable, EOS (either way) comes
back without the last two pictures (the decoder's reorder delay), its
pts the last picture's plus one frame. SeekE lost the same two.

- Seek variants: -D marks the first buffer after the seek DISCONTINUITY;
  -C disables and enables the component after the flush; -M does what
  FFmpeg's mmaldec does (ports disabled, flushed, enabled). Obey files
  SeekDC, SeekC and SeekM; SeekD and SeekE (settled) removed.
- The fake loses the waiting pictures after any flush, either EOS, as
  the Pi; 0.15 fails against it.

## MMALDecode 0.15 (test zip, 2026-10-02)

0.14 on a Pi 4: every MP4 check OK (pts back with each picture, in
display order; 1080p 22.3 pictures a second with checksums). The
LDM/STM copy out of PCI memory runs at 506-559 MB/s, 4x the word copy
(1080p: 6.2 ms a picture, not 23.8). Seek and SeekD lost the last two
pictures: after a flush, EOS on the last access unit's buffer came back
at once, with that unit's pts.

- MP4: EOS goes on its own empty buffer after the last access unit, as
  FFmpeg's mmaldec sends it; -e keeps 0.14's way (new Obey file SeekE,
  to confirm the cause).
- Each picture is copied out of PCI memory by LDM/STM.
- The fake decoder loses the pictures still waiting when EOS comes on an
  access unit's buffer after a flush, as the Pi: 0.14 fails against it.

## MMALDecode 0.14 (test zip, 2026-10-02)

0.13 on a Pi 4 (gpu_mem=128): every picture came back with its own pts,
in display order, within rounding of FFmpeg's (checked from the dumps);
FLUSH and disable/enable both seek cleanly (no new format change after
them); 1080p from MP4 doesn't stall. But every MP4 run failed: FFmpeg's
pts were read with sscanf's %lld, and UnixLib fills only its low word.

- FFmpeg's list read without sscanf (run.sh now rejects a scanf %ll).
- -t never stops at a picture; it times the copy out of PCI memory word
  by word (0.13: 130 MB/s, 24 ms for a 1080p picture) and by LDM/STM.
- The fake decoder sets FRAME_END, and KEYFRAME on an IDR's picture, and
  gives the EOS buffer a pts, as the Pi does.

## MMALDecode 0.13 (test zip, 2026-10-02)

- MP4 input: the H.264 track's samples (avcC, stts/ctts/elst, stss,
  stsz/stsc/stco or co64) sent one access unit a buffer, converted to
  Annex B as FFmpeg's h264_mp4toannexb does, each with its pts and dts
  (microseconds), FRAME_START/FRAME_END and KEYFRAME; padded with zero
  bytes to whole words.
- Each picture from an MP4 is matched to FFmpeg's by its pts, so the
  report says whether the pts come back with their pictures, in display
  order. Raw H.264 is checked in order, as before.
- `-s N` flushes both ports after N pictures (`-F`: disable and enable
  them instead) and goes on from the last keyframe; every picture from
  there must come back.
- `-t` times without checksums: receive time, copy time, and one copy
  timed on its own.
- `-x` (host check) writes the Annex B stream and sample times;
  tests/host/mp4_check.sh compares them with FFmpeg's for five x264 MP4s.
- New Obey files: Seek, SeekD, Speed. Test adds the MP4s. The clips are
  encoded to MP4 (small has a keyframe every 20), and the raw streams
  come from the same bitstream.
- The fake decoder learns MP4 access units, a DPB that gives pictures out
  in pts order, FLUSH, and needing an IDR after a flush (to be checked
  against the Pi).
- hevchw: the module build takes MODULE_CROSS, so the top-level build's
  CROSS (GCCSDK's) no longer reaches it.

## Unreleased: moved out of riscos-ffmpeg (2026-10-01)

ReelHWAccel began in riscos-ffmpeg (`reelhwaccel/` and `tools/`), and
moves here with its history: the HEVCHW module (`hevchw/`, earlier
`hwhevc/`), the test tools VCHIQProbe, MMALProbe, MMALDecode and
HEVCProbe, and their host tests. Each tool's history is in `git log`.

Test zips so far (riscos-ffmpeg's `dist/`): HEVCProbe 0.1, HEVCHW 0.1
(module 0.01), VCHIQProbe 0.1, MMALProbe 0.1, MMALDecode 0.1 to 0.12.
Their versions carry on here.
