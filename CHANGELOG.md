# Changes

## MMALDecode 0.17 (test zip, 2026-10-02)

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
