/*
 * h264_vchiq: H.264 decoded by the Raspberry Pi's VideoCore on RISC OS,
 * through the VCHIQ module and the firmware's MMAL service (vcdec, from
 * riscos-reelhwaccel).
 *
 * The packets go to the VideoCore as Annex B access units (the
 * h264_mp4toannexb filter turns an MP4's avcC packets into those), each
 * with its pts; the pictures come back in display order with their pts
 * and are copied into ordinary YUV420P frames. Nothing is decoded on the
 * ARM.
 *
 * This decoder only waits for the VideoCore when its input is full (about
 * one picture's time) or at the end of the stream; otherwise
 * receive_frame gives AVERROR(EAGAIN) for more input. A flush (a seek)
 * forgets everything sent; after the end of the stream vcdec creates the
 * decoder again, so a seek after the end loses nothing.
 *
 * It refuses (AVERROR(ENOSYS), so the caller can use the h264 decoder
 * instead) streams the VideoCore can't decode: profiles other than
 * Baseline, Main and High, 10-bit or 4:2:2, over 1920x1088, 1080p with
 * gpu_mem under 128 MB, and machines without VCHIQ.
 *
 * Copyright (C) 2026 Andrew Youll
 *
 * This file is part of FFmpeg's RISC OS port.
 *
 * It is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * It is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with it; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <vcdec.h>

#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "internal.h"
#include "profiles.h"

#define WAIT_US 5000000                     /* no progress for this long while waiting: an error */

typedef struct VCHIQContext {
    const AVClass *class;
    vcdec *d;
    AVPacket *pkt;                          /* the packet waiting for room in the decoder */
    uint8_t *ps;                            /* Annex B SPS and PPS from extradata, or NULL: put in */
    int ps_size;                            /* front of each keyframe that hasn't its own (as */
    uint8_t *join;                          /* h264_mp4toannexb does from an avcC) */
    int join_size;
    int draining;                           /* the caller has no more packets */
    int eos_sent;
    int failed;                             /* the AVERROR every call returns from now on */
    int sync_receive;                       /* option */
    int gpu_mem_check;                      /* option */
    int pci_memory;                         /* option */
} VCHIQContext;

static void vchiq_log(void *h, const char *text)
{
    av_log(h, AV_LOG_VERBOSE, "%s\n", text);
}

static int map_result(AVCodecContext *avctx, VCHIQContext *s, int r, const char *what)
{
    if (r == VCDEC_UNSUPPORTED) {
        av_log(avctx, AV_LOG_ERROR, "%s: not for the VideoCore: %s\n", what, vcdec_error(s->d));
        return s->failed = AVERROR(ENOSYS);
    }
    av_log(avctx, AV_LOG_ERROR, "%s: %s\n", what, vcdec_error(s->d));
    return s->failed = AVERROR_EXTERNAL;
}

/* the H.264 profile (from the codec parameters, an avcC or an Annex B
   SPS): 0 if the VideoCore decodes it */
static int check_profile(AVCodecContext *avctx)
{
    const uint8_t *x = avctx->extradata;
    int profile = avctx->profile;
    if (profile == FF_PROFILE_UNKNOWN && x && avctx->extradata_size >= 2 && x[0] == 1)
        profile = x[1];                     /* avcC: AVCProfileIndication */
    if (profile != FF_PROFILE_UNKNOWN) {
        profile &= ~(FF_PROFILE_H264_CONSTRAINED | FF_PROFILE_H264_INTRA);
        if (profile != FF_PROFILE_H264_BASELINE && profile != FF_PROFILE_H264_MAIN &&
            profile != FF_PROFILE_H264_HIGH) {
            av_log(avctx, AV_LOG_VERBOSE, "H.264 profile %d: not for the VideoCore\n", profile);
            return AVERROR(ENOSYS);
        }
    }
    if (x && avctx->extradata_size && x[0] != 1 &&
        vcdec_check_stream(x, avctx->extradata_size) == VCDEC_UNSUPPORTED) {
        av_log(avctx, AV_LOG_VERBOSE, "The SPS's profile isn't for the VideoCore\n");
        return AVERROR(ENOSYS);
    }
    if (avctx->pix_fmt != AV_PIX_FMT_NONE && avctx->pix_fmt != AV_PIX_FMT_YUV420P &&
        avctx->pix_fmt != AV_PIX_FMT_YUVJ420P) {
        av_log(avctx, AV_LOG_VERBOSE, "%s: not for the VideoCore\n", av_get_pix_fmt_name(avctx->pix_fmt));
        return AVERROR(ENOSYS);
    }
    return 0;
}

static av_cold int vchiq_close(AVCodecContext *avctx)
{
    VCHIQContext *s = avctx->priv_data;
    vcdec_close(s->d);
    s->d = NULL;
    av_packet_free(&s->pkt);
    av_freep(&s->ps);
    av_freep(&s->join);
    return 0;
}

static av_cold int vchiq_init(AVCodecContext *avctx)
{
    VCHIQContext *s = avctx->priv_data;
    vcdec_config c;
    int r, w = avctx->coded_width ? avctx->coded_width : avctx->width;
    int h = avctx->coded_height ? avctx->coded_height : avctx->height;

    if ((r = check_profile(avctx)) < 0)
        return r;
    if (w <= 0 || h <= 0) {
        av_log(avctx, AV_LOG_ERROR, "The stream's size is needed to open the VideoCore's decoder\n");
        return AVERROR(EINVAL);
    }
    if (!(s->pkt = av_packet_alloc()))
        return AVERROR(ENOMEM);
    /* SPS and PPS as Annex B (not avcC, which h264_mp4toannexb puts in the
       packets itself), for keyframes that haven't them in band */
    if (avctx->extradata_size > 4 && avctx->extradata[0] != 1) {
        if (!(s->ps = av_memdup(avctx->extradata, avctx->extradata_size)))
            return AVERROR(ENOMEM);
        s->ps_size = avctx->extradata_size;
    }

    vcdec_config_init(&c);
    c.width = w;
    c.height = h;
    c.flags = (s->sync_receive ? VCDEC_SYNC_RECEIVE : 0) | (s->gpu_mem_check ? 0 : VCDEC_NO_GPU_MEM_CHECK) |
              (s->pci_memory ? VCDEC_OUT_PCI : 0) | (av_log_get_level() >= AV_LOG_TRACE ? VCDEC_LOG_MESSAGES : 0);
    c.log = vchiq_log;
    c.log_handle = avctx;
    r = vcdec_open(&s->d, &c);
    if (r != VCDEC_OK) {
        av_log(avctx, r == VCDEC_UNSUPPORTED ? AV_LOG_VERBOSE : AV_LOG_ERROR, "The VideoCore's decoder: %s\n",
               vcdec_open_error());
        return r == VCDEC_UNSUPPORTED || r == VCDEC_EINVAL ? AVERROR(ENOSYS) : AVERROR_EXTERNAL;
    }
    avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    av_log(avctx, AV_LOG_VERBOSE, "H.264 on the VideoCore (vcdec %s), %dx%d, gpu_mem %u MB%s\n", VCDEC_VERSION, w, h,
           vcdec_gpu_mem(), s->pci_memory ? ", pictures in PCI memory" : "");
    return 0;
}

/* has this access unit an SPS (NAL type 7)? */
static int has_sps(const uint8_t *p, int n)
{
    for (int i = 0; i + 3 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1) {
            if ((p[i + 3] & 31) == 7)
                return 1;
            i += 2;
        }
    return 0;
}

/* the waiting packet to the decoder: 1 sent (or dropped), 0 no room, <0 error */
static int send_pkt(AVCodecContext *avctx, VCHIQContext *s)
{
    const uint8_t *data = s->pkt->data;
    int size = s->pkt->size, r;
    unsigned flags = s->pkt->flags & AV_PKT_FLAG_KEY ? VCDEC_KEYFRAME : 0;

    if (s->ps && (flags & VCDEC_KEYFRAME) && !has_sps(data, size)) {
        if (s->join_size < s->ps_size + size) {
            av_freep(&s->join);
            s->join_size = 0;
            if (!(s->join = av_malloc(s->ps_size + size)))
                return AVERROR(ENOMEM);
            s->join_size = s->ps_size + size;
        }
        memcpy(s->join, s->ps, s->ps_size);
        memcpy(s->join + s->ps_size, data, size);
        data = s->join;
        size += s->ps_size;
    }
    r = vcdec_send(s->d, data, size, s->pkt->pts, s->pkt->dts, flags);
    if (r == VCDEC_AGAIN)
        return 0;
    if (r == VCDEC_EINVAL) {                /* (an access unit too big for the decoder: dropped) */
        av_log(avctx, AV_LOG_WARNING, "A packet dropped: %s\n", vcdec_error(s->d));
    } else if (r != VCDEC_OK) {
        return map_result(avctx, s, r, "Sending");
    }
    av_packet_unref(s->pkt);
    return 1;
}

static int output_frame(AVCodecContext *avctx, VCHIQContext *s, AVFrame *frame, const vcdec_picture *pic)
{
    int r;
    if (pic->width != avctx->width || pic->height != avctx->height) {
        if ((r = ff_set_dimensions(avctx, pic->width, pic->height)) < 0)
            return r;
    }
    avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    if ((r = ff_get_buffer(avctx, frame, 0)) < 0)
        return r;
    r = vcdec_receive(s->d, NULL, frame->data, frame->linesize);
    if (r != VCDEC_OK) {
        av_frame_unref(frame);
        return r < 0 ? map_result(avctx, s, r, "Receiving") : AVERROR_BUG;
    }
    frame->pts = pic->pts;
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->key_frame = !!(pic->flags & VCDEC_PIC_KEYFRAME);
    frame->pict_type = frame->key_frame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    return 0;
}

static int vchiq_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    VCHIQContext *s = avctx->priv_data;
    int64_t t0 = 0;
    int r;

    if (s->failed) {
        /* (the packets taken and dropped, so a caller that carries on
           after an error, as ffmpeg does, reaches the end) */
        av_packet_unref(s->pkt);
        r = ff_decode_get_packet(avctx, s->pkt);
        av_packet_unref(s->pkt);
        return r == AVERROR_EOF ? AVERROR_EOF : r == AVERROR(EAGAIN) ? s->failed : r < 0 ? r : s->failed;
    }
    for (;;) {
        vcdec_picture pic;
        int progress = 0;
        /* a picture ready: out */
        r = vcdec_peek(s->d, &pic);
        if (r == VCDEC_OK)
            return output_frame(avctx, s, frame, &pic);
        if (r == VCDEC_EOF)
            return AVERROR_EOF;
        if (r < 0)
            return map_result(avctx, s, r, "Decoding");
        /* input: the waiting packet, or the next */
        if (!s->pkt->size && !s->draining) {
            r = ff_decode_get_packet(avctx, s->pkt);
            if (r == AVERROR_EOF) {
                s->draining = 1;
            } else if (r == AVERROR(EAGAIN)) {
                return AVERROR(EAGAIN);     /* (the caller sends more) */
            } else if (r < 0) {
                return r;
            }
        }
        if (s->pkt->size) {
            if ((r = send_pkt(avctx, s)) < 0)
                return r;
            progress = r;
        } else if (s->draining && !s->eos_sent) {
            r = vcdec_send_eos(s->d);
            if (r == VCDEC_OK) {
                s->eos_sent = progress = 1;
            } else if (r != VCDEC_AGAIN) {
                return map_result(avctx, s, r, "Sending the end of the stream");
            }
        }
        /* the input full, or the end sent: wait for the VideoCore */
        if (progress) {
            t0 = 0;
        } else if (!t0) {
            t0 = av_gettime_relative();
        } else if (av_gettime_relative() - t0 > WAIT_US) {
            av_log(avctx, AV_LOG_ERROR, "No picture from the VideoCore in %d s\n", WAIT_US / 1000000);
            return s->failed = AVERROR_EXTERNAL;
        }
    }
}

static void vchiq_flush(AVCodecContext *avctx)
{
    VCHIQContext *s = avctx->priv_data;
    int r;
    av_packet_unref(s->pkt);
    s->draining = s->eos_sent = 0;
    if (!s->failed && (r = vcdec_flush(s->d)) != VCDEC_OK)
        map_result(avctx, s, r, "Flushing");
}

#define OFFSET(x) offsetof(VCHIQContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "sync_receive", "wait for each picture's transfer as it arrives", OFFSET(sync_receive), AV_OPT_TYPE_BOOL,
      { .i64 = 0 }, 0, 1, VD },
    { "gpu_mem_check", "refuse 1080p when gpu_mem is under 128 MB", OFFSET(gpu_mem_check), AV_OPT_TYPE_BOOL,
      { .i64 = 1 }, 0, 1, VD },
    { "pci_memory", "pictures arrive in PCI memory (vcdec 0.3's way), not a cacheable pool", OFFSET(pci_memory),
      AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VD },
    { NULL }
};

static const AVClass vchiq_class = {
    .class_name = "h264_vchiq",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_h264_vchiq_decoder = {
    .p.name         = "h264_vchiq",
    .p.long_name    = NULL_IF_CONFIG_SMALL("H.264 on the VideoCore (RISC OS, VCHIQ)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(VCHIQContext),
    .init           = vchiq_init,
    .close          = vchiq_close,
    FF_CODEC_RECEIVE_FRAME_CB(vchiq_receive_frame),
    .flush          = vchiq_flush,
    .bsfs           = "h264_mp4toannexb",
    .p.priv_class   = &vchiq_class,
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE,
    .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS | FF_CODEC_CAP_INIT_CLEANUP,
    .p.pix_fmts     = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE },
    .p.profiles     = NULL_IF_CONFIG_SMALL(ff_h264_profiles),
    .p.wrapper_name = "vchiq",
};
