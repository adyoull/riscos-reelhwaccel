# Changes

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
