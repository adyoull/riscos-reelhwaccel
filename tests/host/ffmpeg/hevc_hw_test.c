/*
 * hevc_hw_test.c - hevc_hwdec's output_hw as Reel would use it: frames
 * handed out unconverted (AV_PIX_FMT_HEVCDEC), kept in a queue, and
 * converted when "shown" straight into the caller's planes, 1:1
 * (hevcdec_frame_to_i420, or _16 for 10-bit) and halved
 * (hevcdec_frame_to_i420_half); the last frames kept past
 * avcodec_free_context and converted after it, nothing logged through the
 * freed context (hevcdec closes with the last frame, and says so).
 *
 *   hevc_hw_test IN.mp4 OUT.yuv HALF.yuv [queue [drop_before]]
 * OUT: the 1:1 pictures (yuv420p, or yuv420p10le for 10-bit); HALF: the
 * halved ones (yuv420p, w/2 x h/2). The fake HEVC block (hevc_fake_env.c,
 * from the environment) decodes.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/log.h"
#include <stdarg.h>
#include <hwhevcdec.h>

#define QMAX 16

static AVFrame *q[QMAX];
static int nq, shown, depth;
static FILE *out1, *outh;
static const void *freed_ctx;              /* (the decoder's context, once freed) */
static int logged_freed, logged_after;

static void log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    if (freed_ctx) logged_after++;
    if (freed_ctx && avcl == freed_ctx) logged_freed++;
    if (level <= AV_LOG_ERROR) av_log_default_callback(avcl, level, fmt, vl);
}

static int show(AVFrame *f)
{
    hevcdec_frame *hf = (hevcdec_frame *)f->data[3];
    hevcdec *d = hevcdec_frame_decoder(hf);
    int x = (int)f->crop_left, y = (int)f->crop_top, w = f->width - x, h = f->height - y, bps = depth > 8 ? 2 : 1;
    int cw = (w + 1) / 2, ch = (h + 1) / 2, hw = w / 2, hh = h / 2;
    uint8_t *p[3], *ph[3];
    int st[3] = { w * bps, cw * bps, cw * bps }, sth[3] = { hw, (hw + 1) / 2, (hw + 1) / 2 };
    if (f->format != AV_PIX_FMT_HEVCDEC || !d) { fprintf(stderr, "FAIL: a frame not HEVCDEC (%d) or no decoder\n", f->format); return -1; }
    p[0] = malloc((size_t)st[0] * h); p[1] = malloc((size_t)st[1] * ch); p[2] = malloc((size_t)st[2] * ch);
    ph[0] = malloc((size_t)sth[0] * hh); ph[1] = malloc((size_t)sth[1] * ((hh + 1) / 2)); ph[2] = malloc((size_t)sth[2] * ((hh + 1) / 2));
    if (bps == 2) {
        uint16_t *p16[3] = { (uint16_t *)p[0], (uint16_t *)p[1], (uint16_t *)p[2] };
        if (hevcdec_frame_to_i420_16(d, hf, p16, st, x, y, w, h) != HEVCDEC_OK) { fprintf(stderr, "FAIL: _16\n"); return -1; }
    } else {
        hevcdec_frame_to_i420(d, hf, p, st, x, y, w, h);
    }
    if (hevcdec_frame_to_i420_half(d, hf, ph, sth, x, y, hw, hh) != HEVCDEC_OK) {
        fprintf(stderr, "FAIL: _half: %s\n", hevcdec_error(d));
        return -1;
    }
    fwrite(p[0], 1, (size_t)st[0] * h, out1); fwrite(p[1], 1, (size_t)st[1] * ch, out1); fwrite(p[2], 1, (size_t)st[2] * ch, out1);
    fwrite(ph[0], 1, (size_t)sth[0] * hh, outh);
    fwrite(ph[1], 1, (size_t)sth[1] * ((hh + 1) / 2), outh);
    fwrite(ph[2], 1, (size_t)sth[2] * ((hh + 1) / 2), outh);
    for (int k = 0; k < 3; k++) { free(p[k]); free(ph[k]); }
    shown++;
    return 0;
}

int main(int argc, char **argv)
{
    AVFormatContext *fc = NULL;
    AVCodecContext *c;
    const AVCodec *codec;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f;
    int vs, ret, qn = argc > 4 ? atoi(argv[4]) : 12, eof = 0;
    if (argc < 4 || qn < 1 || qn > QMAX) { fprintf(stderr, "usage: hevc_hw_test in.mp4 out.yuv half.yuv [queue [drop_before]]\n"); return 2; }
    if (!(out1 = fopen(argv[2], "wb")) || !(outh = fopen(argv[3], "wb"))) return 2;
    if (avformat_open_input(&fc, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) return 2;
    vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs < 0 || !(codec = avcodec_find_decoder_by_name("hevc_hwdec"))) { fprintf(stderr, "FAIL: no hevc_hwdec\n"); return 1; }
    c = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(c, fc->streams[vs]->codecpar);
    c->pkt_timebase = fc->streams[vs]->time_base;
    depth = av_pix_fmt_desc_get(fc->streams[vs]->codecpar->format) ?
            av_pix_fmt_desc_get(fc->streams[vs]->codecpar->format)->comp[0].depth : 8;
    av_opt_set_int(c->priv_data, "output_hw", 1, 0);
    if (argc > 5) av_opt_set_int(c->priv_data, "drop_before", strtoll(argv[5], NULL, 10), 0);
    if ((ret = avcodec_open2(c, codec, NULL)) < 0) { fprintf(stderr, "FAIL: open: %d\n", ret); return 1; }
    if (c->pix_fmt != AV_PIX_FMT_HEVCDEC) { fprintf(stderr, "FAIL: pix_fmt %d after open\n", c->pix_fmt); return 1; }
    while (!eof) {
        if (av_read_frame(fc, pkt) < 0) { avcodec_send_packet(c, NULL); eof = 1; }
        else if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
        else { avcodec_send_packet(c, pkt); av_packet_unref(pkt); }
        for (;;) {
            f = av_frame_alloc();
            ret = avcodec_receive_frame(c, f);
            if (ret < 0) { av_frame_free(&f); break; }
            q[nq++] = f;                         /* (kept, as Reel's queue) */
            if (nq == qn) {                      /* the oldest shown: converted now, then freed */
                if (show(q[0])) return 1;
                av_frame_free(&q[0]);
                memmove(q, q + 1, (size_t)--nq * sizeof q[0]);
            }
        }
    }
    /* the decoder closed with frames still held: they stay convertible */
    av_log_set_level(AV_LOG_DEBUG);
    av_log_set_callback(log_cb);
    freed_ctx = c;
    avcodec_free_context(&c);
    for (int i = 0; i < nq; i++) { if (show(q[i])) return 1; av_frame_free(&q[i]); }
    avformat_close_input(&fc);
    av_packet_free(&pkt);
    fclose(out1); fclose(outh);
    if (logged_freed || !logged_after) {        /* (hevcdec's closing message, but not through the freed context) */
        fprintf(stderr, "FAIL: %d messages through the freed context (%d after it was freed)\n", logged_freed, logged_after);
        return 1;
    }
    printf("hevc_hw_test: %d pictures shown, %d-bit\n", shown, depth);
    return 0;
}
