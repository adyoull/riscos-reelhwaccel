#!/bin/bash
# ffmpeg/mkpatch-enc.sh FFMPEG_TARBALL - makes 0003-avcodec-h264_vchiq-encoder.patch
# (on top of 0001) from vchiqenc.c: FFmpeg 5.1.10 with 0001 applied, the
# encoder registered (configure: only its deps line, so the patch applies
# before or after 0002; Makefile; allcodecs.c: among the hardware encoders,
# so it's never the default H.264 encoder) and vchiqenc.c put in.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TAR=${1:?usage: ffmpeg/mkpatch-enc.sh ffmpeg-5.1.10.tar.xz}
T=$(mktemp -d)
tar xf "$TAR" -C "$T"
cd "$T"/ffmpeg-5.1.10
git init -q && git add -A && git -c user.name=base -c user.email=base commit -qm base
git -c user.name="Andrew Youll" -c user.email=andrewyoull86@gmail.com am -q "$HERE/0001-avcodec-h264_vchiq.patch"
python3 - <<'PY'
def edit(p, a, b):
    s = open(p).read()
    assert s.count(a) == 1, (p, a)
    open(p, 'w').write(s.replace(a, b))
edit('configure', 'h264_vchiq_decoder_select="h264_mp4toannexb_bsf"\n',
     'h264_vchiq_decoder_select="h264_mp4toannexb_bsf"\nh264_vchiq_encoder_deps="vchiq"\n')
edit('libavcodec/Makefile', 'OBJS-$(CONFIG_H264_VCHIQ_DECODER)      += vchiqdec.o\n',
     'OBJS-$(CONFIG_H264_VCHIQ_DECODER)      += vchiqdec.o\nOBJS-$(CONFIG_H264_VCHIQ_ENCODER)      += vchiqenc.o\n')
edit('libavcodec/allcodecs.c', 'extern const FFCodec ff_h264_omx_encoder;\n',
     'extern const FFCodec ff_h264_omx_encoder;\nextern const FFCodec ff_h264_vchiq_encoder;\n')
PY
cp "$HERE/vchiqenc.c" libavcodec/vchiqenc.c
git add -A
git -c user.name="Andrew Youll" -c user.email=andrewyoull86@gmail.com commit -q -F - <<'MSG'
avcodec: add h264_vchiq encoder (RISC OS, Raspberry Pi VideoCore)

H.264 encoded by the Raspberry Pi's VideoCore on RISC OS, through the
VCHIQ module and the firmware's MMAL service, using vcenc from
riscos-reelhwaccel (in libvcdec, with --enable-vchiq). YUV420P frames in,
Annex B packets out in the same order (no B frames); the SPS and PPS as
extradata with global headers, otherwise in front of the first packet.
Registered among the hardware encoders, so it's chosen only by name
(-c:v h264_vchiq).
MSG
git format-patch -q -1 --stdout > "$HERE/0003-avcodec-h264_vchiq-encoder.patch"
rm -rf "$T"
echo "ffmpeg/0003-avcodec-h264_vchiq-encoder.patch made"
