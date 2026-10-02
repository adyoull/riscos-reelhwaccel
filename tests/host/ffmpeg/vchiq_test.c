/*
 * FFmpeg's h264_vchiq decoder (ffmpeg/vchiqdec.c, built into FFmpeg
 * 5.1.10 by ffmpeg/0001-*.patch) through libavformat and libavcodec's own
 * API, against vcdec and the fake VCHIQ module and MMAL decoder
 * (fake_vc.c), under qemu:
 *   - the fake MP4 decoded whole: every picture right, its pts, then EOF;
 *   - a seek before the end (avcodec_flush_buffers: FLUSH) and after it
 *     (the decoder created again): every picture from the keyframe;
 *   - streams it refuses with AVERROR(ENOSYS), so a player can use the h264
 *     decoder instead, without touching VCHIQ: High 10, 2048 wide, 1080p
 *     with gpu_mem under 128 MB, no size; and no PCI memory (an error);
 *   - Annex B packets with the SPS and PPS only in extradata: put in front
 *     of the first keyframe, and again after a flush;
 *   - a decoder slower than the packets: its input fills and receive_frame
 *     waits; a seek then (the waiting packet dropped);
 *   - a decoder error: reported, then the packets taken until the end;
 *   - each run: everything closed and freed.
 *
 * Part of riscos-reelhwaccel. GPL version 2 or later (see COPYING).
 */
#include <stdio.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include "fake_vc.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int got[64], ngot, wrong;

/* a frame from the fake: 70x38, Y = value, U +1, V +2 */
static void take_frame(const AVFrame *f, int64_t frames_pts)
{
    int k = (int)frames_pts, v = fake_vc_value(k), ok = f->width == fake_vc_width() && f->height == fake_vc_height() &&
        f->format == AV_PIX_FMT_YUV420P;
    for (int p = 0; p < 3 && ok; p++) {
        int w = p ? (f->width + 1) / 2 : f->width, h = p ? (f->height + 1) / 2 : f->height;
        for (int y = 0; y < h && ok; y++)
            for (int x = 0; x < w && ok; x++) ok = f->data[p][y * f->linesize[p] + x] == (uint8_t)(v + p);
    }
    if (!ok) wrong++;
    if (ngot < 64) got[ngot++] = k;
}

static int got_is(int from, int n)
{
    if (ngot != n) return 0;
    for (int i = 0; i < n; i++) if (got[i] != from + i) return 0;
    return 1;
}

typedef struct { AVFormatContext *fmt; AVCodecContext *dec; AVPacket *pkt; AVFrame *frame; int tb_frame; } run_t;

static int pci_option;                       /* open_file: -pci_memory 1 */
static int open_file(run_t *r)
{
    const AVCodec *c = avcodec_find_decoder_by_name("h264_vchiq");
    int e;
    memset(r, 0, sizeof *r);
    if (!c) { printf("FAIL: no h264_vchiq decoder\n"); fails++; return -1; }
    if ((e = avformat_open_input(&r->fmt, "/tmp/mmaldecode_test.mp4", NULL, NULL)) < 0 ||
        (e = avformat_find_stream_info(r->fmt, NULL)) < 0) return e;
    r->dec = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(r->dec, r->fmt->streams[0]->codecpar);
    r->dec->pkt_timebase = r->fmt->streams[0]->time_base;
    r->tb_frame = r->fmt->streams[0]->time_base.den / 25;
    r->pkt = av_packet_alloc();
    r->frame = av_frame_alloc();
    if (pci_option) av_opt_set_int(r->dec->priv_data, "pci_memory", 1, 0);
    return avcodec_open2(r->dec, c, NULL);
}

static void close_file(run_t *r)
{
    avcodec_free_context(&r->dec);
    avformat_close_input(&r->fmt);
    av_packet_free(&r->pkt);
    av_frame_free(&r->frame);
}

/* reads and decodes; stops after `stop` frames (0: to the end). The last
   avcodec result. */
static int decode(run_t *r, int stop)
{
    int e = 0, eof = 0;
    for (;;) {
        while ((e = avcodec_receive_frame(r->dec, r->frame)) == 0) {
            take_frame(r->frame, r->frame->pts / r->tb_frame);
            av_frame_unref(r->frame);
            if (stop && ngot == stop) return 0;
        }
        if (e == AVERROR_EOF) return e;
        if (e != AVERROR(EAGAIN)) return e;
        if (eof) return -9999;               /* EAGAIN after the end: wrong */
        if (av_read_frame(r->fmt, r->pkt) < 0) {
            eof = 1;
            e = avcodec_send_packet(r->dec, NULL);
        } else {
            e = avcodec_send_packet(r->dec, r->pkt);
            av_packet_unref(r->pkt);
        }
        if (e < 0 && e != AVERROR(EAGAIN)) return e;
    }
}

/* a decoder opened for these parameters only (no file) */
static int open_bare(AVCodecContext **dec, int w, int h, int profile, const uint8_t *extra, int extra_size)
{
    const AVCodec *c = avcodec_find_decoder_by_name("h264_vchiq");
    *dec = avcodec_alloc_context3(c);
    (*dec)->width = w;
    (*dec)->height = h;
    (*dec)->profile = profile;
    if (extra) {
        (*dec)->extradata = av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy((*dec)->extradata, extra, extra_size);
        (*dec)->extradata_size = extra_size;
    }
    return avcodec_open2(*dec, c, NULL);
}

/* access unit k of the twelve as Annex B, with no SPS or PPS */
static int make_au(uint8_t *b, int k)
{
    static const int pts[12] = { 0, 3, 1, 2, 5, 4, 6, 9, 7, 8, 11, 10 };
    int n = 0;
    b[n++] = 0; b[n++] = 0; b[n++] = 0; b[n++] = 1;
    b[n++] = k % 6 == 0 ? 0x65 : 0x41;
    b[n++] = (uint8_t)fake_vc_value(pts[k]);
    memset(b + n, 0xAA, 41 + 13 * k);
    return n + 41 + 13 * k;
}

int main(void)
{
    run_t r;
    int e;

    av_log_set_level(getenv("VCHIQ_TEST_VERBOSE") ? AV_LOG_VERBOSE : AV_LOG_QUIET);
    fake_vc_make_mp4();

    /* the whole file */
    fake_vc_reset();
    ngot = wrong = 0;
    e = open_file(&r);
    CHECK(e == 0, "open: %d", e);
    if (!e) {
        e = decode(&r, 0);
        CHECK(e == AVERROR_EOF && got_is(0, 12) && !wrong && *fake_vc_var("aus") == 12,
              "whole file: %d, %d frames, %d wrong", e, ngot, wrong);
        CHECK(avcodec_receive_frame(r.dec, r.frame) == AVERROR_EOF, "EOF again");
        CHECK(*fake_vc_var("pmp_invalidates") >= 12, "whole file: pictures not from a cached pool (%d cleans)",
              *fake_vc_var("pmp_invalidates"));
    }
    close_file(&r);
    CHECK(fake_vc_cleaned("whole file"), "whole file: not cleaned up");

    /* -pci_memory 1: PCI memory, as vcdec 0.3 (no pools) */
    fake_vc_reset();
    ngot = wrong = 0;
    pci_option = 1;
    e = open_file(&r);
    pci_option = 0;
    CHECK(e == 0, "open, pci_memory: %d", e);
    if (!e) {
        e = decode(&r, 0);
        CHECK(e == AVERROR_EOF && got_is(0, 12) && !wrong && !*fake_vc_var("pmp_invalidates") && !*fake_vc_var("pmp_made"),
              "pci_memory: %d, %d frames, %d wrong, %d cleans, %d pools", e, ngot, wrong, *fake_vc_var("pmp_invalidates"),
              *fake_vc_var("pmp_made"));
    }
    close_file(&r);
    CHECK(fake_vc_cleaned("pci_memory"), "pci_memory: not cleaned up");

    /* a seek before the end: three frames, then on from the keyframe at 6 */
    for (int late = 0; late < 2; late++) {
        fake_vc_reset();
        *fake_vc_var("rx_late") = late;
        ngot = wrong = 0;
        if (open_file(&r) == 0) {
            decode(&r, 3);
            CHECK(av_seek_frame(r.fmt, 0, 6 * r.tb_frame, AVSEEK_FLAG_BACKWARD) >= 0, "seek");
            avcodec_flush_buffers(r.dec);
            ngot = 0;
            e = decode(&r, 0);
            CHECK(e == AVERROR_EOF && got_is(6, 6) && !wrong && *fake_vc_var("flushes") == 2 &&
                  !*fake_vc_var("recreated") && !*fake_vc_var("eos_lost"),
                  "seek before the end (late %d): %d, %d frames from %d, %d flushes", late, e, ngot, ngot ? got[0] : -1,
                  *fake_vc_var("flushes"));
        }
        close_file(&r);
        CHECK(fake_vc_cleaned("seek"), "seek: not cleaned up");
    }

    /* a seek after the end: the decoder created again, nothing lost; twice */
    fake_vc_reset();
    ngot = wrong = 0;
    if (open_file(&r) == 0) {
        decode(&r, 0);
        for (int round = 1; round <= 2; round++) {
            av_seek_frame(r.fmt, 0, 6 * r.tb_frame, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(r.dec);
            ngot = 0;
            e = decode(&r, 0);
            CHECK(e == AVERROR_EOF && got_is(6, 6) && !wrong && *fake_vc_var("recreated") == round &&
                  !*fake_vc_var("eos_lost"), "seek after the end (%d): %d, %d frames, %d created again, %d lost", round,
                  e, ngot, *fake_vc_var("recreated"), *fake_vc_var("eos_lost"));
        }
    }
    close_file(&r);
    CHECK(fake_vc_cleaned("seek after the end"), "seek after the end: not cleaned up");

    /* refused, so the caller can use h264: VCHIQ not touched */
    {
        AVCodecContext *d;
        fake_vc_reset();
        e = open_bare(&d, 640, 360, FF_PROFILE_H264_HIGH_10, NULL, 0);
        CHECK(e == AVERROR(ENOSYS), "High 10: %d", e);
        avcodec_free_context(&d);
        e = open_bare(&d, 2048, 1080, FF_PROFILE_UNKNOWN, NULL, 0);
        CHECK(e == AVERROR(ENOSYS), "2048 wide: %d", e);
        avcodec_free_context(&d);
        e = open_bare(&d, 1920, 1080, FF_PROFILE_H264_HIGH, NULL, 0);   /* (the fake has 76 MB) */
        CHECK(e == AVERROR(ENOSYS), "1080p at 76 MB: %d", e);
        avcodec_free_context(&d);
        e = open_bare(&d, 0, 0, FF_PROFILE_UNKNOWN, NULL, 0);
        CHECK(e == AVERROR(EINVAL), "no size: %d", e);
        avcodec_free_context(&d);
        {
            static const uint8_t avcc_high10[] = { 1, 110, 0, 30, 0xFF, 0xE0, 0 };
            e = open_bare(&d, 640, 360, FF_PROFILE_UNKNOWN, avcc_high10, sizeof avcc_high10);
            CHECK(e == AVERROR(ENOSYS), "avcC High 10: %d", e);
            avcodec_free_context(&d);
        }
        CHECK(*fake_vc_var("opens") == 0, "VCHIQ opened for a refused stream");
        *fake_vc_var("no_pci_mem") = 1;
        e = open_bare(&d, 70, 38, FF_PROFILE_UNKNOWN, NULL, 0);
        CHECK(e == AVERROR_EXTERNAL, "no PCI memory: %d", e);
        avcodec_free_context(&d);
        *fake_vc_var("no_pci_mem") = 0;
    }

    /* Annex B packets, the SPS and PPS only in extradata: in front of the
       first keyframe (the fake insists on them before an IDR), and after a
       flush */
    {
        static const uint8_t ps[] = { 0, 0, 0, 1, 0x67, 0x4D, 0x00, 0x1E, 0xAB, 0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80 };
        static const int pts[12] = { 0, 3, 1, 2, 5, 4, 6, 9, 7, 8, 11, 10 };
        AVCodecContext *d;
        AVPacket *p = av_packet_alloc();
        AVFrame *f = av_frame_alloc();
        fake_vc_reset();
        e = open_bare(&d, 70, 38, FF_PROFILE_UNKNOWN, ps, sizeof ps);
        CHECK(e == 0, "Annex B open: %d", e);
        for (int pass = 0; pass < 2 && !e; pass++) {
            int from = pass ? 6 : 0;
            ngot = wrong = 0;
            for (int k = from; k <= 12; k++) {
                if (k < 12) {
                    av_new_packet(p, 200);
                    p->size = make_au(p->data, k);
                    p->pts = pts[k];
                    p->dts = k;
                    p->flags = k % 6 == 0 ? AV_PKT_FLAG_KEY : 0;
                    e = avcodec_send_packet(d, p);
                    av_packet_unref(p);
                } else {
                    e = avcodec_send_packet(d, NULL);
                }
                CHECK(e == 0, "Annex B send %d: %d", k, e);
                while ((e = avcodec_receive_frame(d, f)) == 0) { take_frame(f, f->pts); av_frame_unref(f); }
            }
            CHECK(e == AVERROR_EOF && got_is(from, 12 - from) && !wrong, "Annex B, pass %d: %d, %d frames", pass, e, ngot);
            avcodec_flush_buffers(d);
            e = 0;
        }
        CHECK(fake_vc_fails() == 0, "the fake objected (an IDR without SPS and PPS?)");
        avcodec_free_context(&d);
        av_packet_free(&p);
        av_frame_free(&f);
        CHECK(fake_vc_cleaned("Annex B"), "Annex B: not cleaned up");
    }

    /* a decoder slower than the packets come (its input buffers back late):
       48 access units (four rounds of the twelve), so the input fills and
       receive_frame waits; then a seek with the input full (the packet that
       was waiting for room is dropped: nothing from before the seek after it) */
    for (int at = 18; at <= 23; at++) {   /* (at several points: a packet waits at some) */
        static const uint8_t ps[] = { 0, 0, 0, 1, 0x67, 0x4D, 0x00, 0x1E, 0xAB, 0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80 };
        static const int pts[12] = { 0, 3, 1, 2, 5, 4, 6, 9, 7, 8, 11, 10 };
        AVCodecContext *d;
        AVPacket *p = av_packet_alloc();
        AVFrame *f = av_frame_alloc();
        int i = 0, again = 0, seeked = 0, before = 0;
        fake_vc_reset();
        *fake_vc_var("in_slow") = 40;
        *fake_vc_var("rx_late") = 1;        /* (a picture can finish arriving while the input is full) */
        e = open_bare(&d, 70, 38, FF_PROFILE_UNKNOWN, ps, sizeof ps);
        ngot = wrong = 0;
        while (!e && i <= 48) {
            if (i < 48) {
                int k = i % 12, fp = pts[k] + 12 * (i / 12);
                av_new_packet(p, 200);
                p->size = make_au(p->data, k);
                p->data[5] = (uint8_t)fake_vc_value(fp);
                p->pts = fp;
                p->dts = i;
                p->flags = k % 6 == 0 ? AV_PKT_FLAG_KEY : 0;
                e = avcodec_send_packet(d, p);
                av_packet_unref(p);
            } else {
                e = avcodec_send_packet(d, NULL);
            }
            if (e == AVERROR(EAGAIN)) again++;
            else if (e < 0) break;
            else i++;
            /* (straight after the 20th frame, while a packet still waits for room) */
            while ((seeked || ngot < at) && (e = avcodec_receive_frame(d, f)) == 0) {
                take_frame(f, f->pts);
                av_frame_unref(f);
            }
            if (e == AVERROR_EOF) break;
            e = 0;
            if (!seeked && ngot >= at) {     /* the seek: on from the keyframe at 36 */
                before = ngot;
                avcodec_flush_buffers(d);
                seeked = 1;
                i = 36;
            }
        }
        {
            int ok = before == at && ngot == before + 12 && !wrong;
            for (int j = 0; ok && j < before; j++) ok = got[j] == j;
            for (int j = 0; ok && j < 12; j++) ok = got[before + j] == 36 + j;
            CHECK(e == AVERROR_EOF && ok, "a slow decoder, seek at %d: %d, %d frames before the seek, %d in all, %d wrong",
                  at, e, before, ngot, wrong);
        }
        CHECK(*fake_vc_var("slow_max") >= 20 && *fake_vc_var("idr_dropped") == 0,
              "a slow decoder, seek at %d: its input never full (%d), or old packets after the seek (%d dropped)",
              at, *fake_vc_var("slow_max"), *fake_vc_var("idr_dropped"));
        avcodec_free_context(&d);
        av_packet_free(&p);
        av_frame_free(&f);
        CHECK(fake_vc_cleaned("slow decoder"), "slow decoder: not cleaned up");
        (void)again;
    }

    /* a decoder error: said, then the packets taken to the end */
    fake_vc_reset();
    *fake_vc_var("error_event") = 1;
    ngot = 0;
    if (open_file(&r) == 0) {
        e = decode(&r, 0);
        CHECK(e == AVERROR_EXTERNAL, "error event: %d", e);
        for (int spins = 0; spins < 1000 && (e = avcodec_receive_frame(r.dec, r.frame)) != AVERROR_EOF; spins++) {
            if (e == AVERROR(EAGAIN) || e == AVERROR_EXTERNAL) {
                if (av_read_frame(r.fmt, r.pkt) < 0) avcodec_send_packet(r.dec, NULL);
                else { avcodec_send_packet(r.dec, r.pkt); av_packet_unref(r.pkt); }
            } else break;
        }
        CHECK(e == AVERROR_EOF, "after the error, the end: %d", e);
    }
    close_file(&r);
    *fake_vc_var("error_event") = 0;
    CHECK(fake_vc_cleaned("error"), "error: not cleaned up");

    CHECK(fake_vc_fails() == 0, "the fake reported %d failures", fake_vc_fails());
    printf(fails ? "vchiq_test: %d failures\n" : "vchiq_test: all passed\n", fails);
    return fails != 0;
}
