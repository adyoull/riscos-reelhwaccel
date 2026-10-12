/*
 * The h264_encvchiq encoder (ffmpeg/0003) through libavcodec's API, on vcenc
 * and the fake ril.video_encode (fake_vcenc_lib.c), under qemu:
 *   - frames from av_frame_get_buffer (rows longer than the picture), each
 *     packet the fake's unit for that frame: its number and the Adler-32
 *     of the visible picture the fake read; pts as sent, dts = pts;
 *     keyframes every g and on pict_type I;
 *   - the SPS and PPS: the extradata with global headers (and not in the
 *     packets), else in front of the first packet; with repeat_headers in
 *     front of every keyframe;
 *   - the settings as vcenc's parameters (b and g 0: the encoder's; b,
 *     g, profile, level, rc_constant falling back as on the Pi, qp);
 *   - a slow encoder (EAGAIN, frames sent and packets taken in turn), an
 *     encoder that goes quiet or reports an error, sizes refused;
 *   - after every run: everything closed and freed.
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdio.h>
#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
#include "fake_vcenc.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define P_PROFILE 0x20002u
#define P_INTRAPERIOD 0x20003u
#define P_RATECONTROL 0x20004u
#define P_BITRATE 0x2000Eu
#define P_MINQ 0x20010u
#define P_MAXQ 0x20011u
#define P_INLINE 0x2002Au

static uint32_t adler(uint32_t a, const uint8_t *p, size_t n)
{
    uint32_t s1 = a & 0xFFFF, s2 = a >> 16;
    while (n--) { s1 = (s1 + *p++) % 65521; s2 = (s2 + s1) % 65521; }
    return s2 << 16 | s1;
}

static int sps_at(const uint8_t *d, int n) { return n >= 5 && !d[0] && !d[1] && !d[2] && d[3] == 1 && (d[4] & 31) == 7; }

typedef struct {
    int w, h, frames, global, force_at, slow, expect_open, stop_err, quiet, error_at;
    const char *opts;                       /* "key=value:key=value" for the encoder */
    int64_t b;
    int g, profile, level;
} run_t;

static int run(const char *name, run_t t)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_encvchiq");
    AVCodecContext *c = avcodec_alloc_context3(codec);
    AVDictionary *d = NULL;
    AVFrame *f = av_frame_alloc();
    AVPacket *pk = av_packet_alloc();
    uint32_t want[64];
    int r, sent = 0, got = 0, eof = 0, err = 0, again = 0, keys = 0, f0 = fails, repeat;
    fake_vcenc_reset();
    *fake_vcenc_var("slow") = t.slow;
    *fake_vcenc_var("quiet") = t.quiet;
    *fake_vcenc_var("error_at") = t.error_at ? t.error_at : -1;
    c->width = t.w ? t.w : 70;
    c->height = t.h ? t.h : 38;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->time_base = (AVRational){ 1, 25 };
    c->framerate = (AVRational){ 25, 1 };
    if (t.b >= 0) c->bit_rate = t.b;
    if (t.g) c->gop_size = t.g;
    if (t.profile) c->profile = t.profile;
    if (t.level) c->level = t.level;
    if (t.global) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (t.opts) av_dict_parse_string(&d, t.opts, "=", ":", 0);
    repeat = t.opts && strstr(t.opts, "repeat_headers=1");
    r = avcodec_open2(c, codec, &d);
    av_dict_free(&d);
    if (r != t.expect_open) {
        CHECK(0, "%s: open gave %d, expected %d", name, r, t.expect_open);
        goto out;
    }
    if (r < 0) {
        printf("%s: open refused (%d)\n", name, r);
        goto out;
    }
    if (t.global)
        CHECK(c->extradata_size == 17 && sps_at(c->extradata, 17) && c->extradata[13] == 0x68, "%s: extradata %d bytes", name,
              c->extradata_size);
    else
        CHECK(!c->extradata_size, "%s: extradata without global headers", name);
    while (!eof && !err) {
        if (sent < t.frames) {               /* a frame: a pattern, rows longer than the picture */
            av_frame_unref(f);
            f->width = c->width; f->height = c->height; f->format = c->pix_fmt;
            av_frame_get_buffer(f, 64);
            CHECK(f->linesize[0] > c->width, "%s: the frames' rows aren't longer than the picture", name);
            want[sent] = 0;
            for (int p = 0; p < 3; p++) {
                int pw = p ? c->width / 2 : c->width, ph = p ? c->height / 2 : c->height;
                for (int y = 0; y < ph; y++) {
                    for (int x = 0; x < pw; x++) f->data[p][y * f->linesize[p] + x] = (uint8_t)(x * 5 + y * 3 + sent * 17 + p * 40);
                    want[sent] = adler(want[sent], f->data[p] + y * f->linesize[p], (size_t)pw);
                }
            }
            f->pts = 7 + 3 * sent;
            f->pict_type = sent == t.force_at ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
            r = avcodec_send_frame(c, f);
            if (r == AVERROR(EAGAIN)) again++;
            else if (r < 0) { err = r; break; }
            else sent++;
        } else if (sent == t.frames) {
            r = avcodec_send_frame(c, NULL);
            if (r == 0) sent++;
            else if (r != AVERROR(EAGAIN)) { err = r; break; }
        }
        while ((r = avcodec_receive_packet(c, pk)) == 0) {
            const uint8_t *p = pk->data;
            int o = 0, k, key = !!(pk->flags & AV_PKT_FLAG_KEY), hdr;
            uint32_t a;
            hdr = sps_at(p, pk->size);
            if (hdr) o = 17;
            CHECK(hdr == ((!t.global && got == 0) || (repeat && key)), "%s: packet %d: headers %d", name, got, hdr);
            CHECK(pk->size >= o + 13 && p[o + 3] == 1, "%s: packet %d: %d bytes", name, got, pk->size);
            memcpy(&k, p + o + 5, 4); memcpy(&a, p + o + 9, 4);
            CHECK(k == got && got < 64 && a == want[got] && a == fake_vcenc_adler(got), "%s: packet %d holds picture %d (%08X, sent %08X)",
                  name, got, k, a, got < 64 ? want[got] : 0);
            CHECK(pk->pts == 7 + 3 * got && pk->dts == pk->pts, "%s: packet %d pts %lld dts %lld", name, got, (long long)pk->pts,
                  (long long)pk->dts);
            CHECK(key == (p[o + 4] == 0x65), "%s: packet %d's key flag", name, got);
            CHECK(key == (got == 0 || got == t.force_at || (t.g > 0 && got % t.g == 0)), "%s: packet %d key %d", name, got, key);
            keys += key;
            got++;
            av_packet_unref(pk);
        }
        if (r == AVERROR_EOF) eof = 1;
        else if (r != AVERROR(EAGAIN)) err = r;
    }
    if (t.stop_err) {
        CHECK(err == AVERROR_EXTERNAL, "%s: ended with %d after %d packets (an error expected)", name, err, got);
        printf("%s: stopped with an error after %d packets\n", name, got);
    } else {
        CHECK(!err && got == t.frames, "%s: %d of %d packets, error %d", name, got, t.frames, err);
        CHECK(!*fake_vcenc_var("pad_bad"), "%s: padding", name);
        printf("%s: %d packets, %d keyframes, %d times input full\n", name, got, keys, again);
    }
out:
    avcodec_free_context(&c);
    av_frame_free(&f);
    av_packet_free(&pk);
    CHECK(fake_vcenc_cleaned(name), "%s: not cleaned up", name);
    return fails == f0;
}

int main(void)
{
    run_t t;
    setvbuf(stdout, NULL, _IONBF, 0);
    av_log_set_level(getenv("ENCLOG") ? AV_LOG_DEBUG : AV_LOG_QUIET);

    /* defaults: b and g 0 (the encoder's), the headers in front of the first packet */
    memset(&t, 0, sizeof t); t.frames = 12; t.b = -1;
    run("defaults", t);
    CHECK(fake_vcenc_param(P_BITRATE, 0) == 10000000 && fake_vcenc_param(P_INTRAPERIOD, 0) == -1 &&
          fake_vcenc_param(P_PROFILE, 0) == 28 && fake_vcenc_param(P_RATECONTROL, 0) == 1 && fake_vcenc_param(P_INLINE, 0) == 0,
          "defaults: bit rate %lld, intra period %lld, profile %lld", fake_vcenc_param(P_BITRATE, 0),
          fake_vcenc_param(P_INTRAPERIOD, 0), fake_vcenc_param(P_PROFILE, 0));

    /* global headers (as for MP4); b, g, main, level 4.1, constant (refused as on the Pi: 4), qp */
    memset(&t, 0, sizeof t); t.frames = 20; t.global = 1; t.b = 3000000; t.g = 5; t.profile = FF_PROFILE_H264_MAIN;
    t.level = 41; t.opts = "rc_constant=1:qp_min=18:qp_max=38:pci_memory=1";
    run("global headers", t);
    CHECK(fake_vcenc_param(P_BITRATE, 0) == 3000000 && fake_vcenc_param(P_INTRAPERIOD, 0) == 5 &&
          fake_vcenc_param(P_PROFILE, 0) == 26 && fake_vcenc_param(P_PROFILE, 1) == 0x1D &&
          fake_vcenc_param(P_RATECONTROL, 0) == 1 && fake_vcenc_param(P_MINQ, 0) == 18 && fake_vcenc_param(P_MAXQ, 0) == 38,
          "global headers: the settings");

    /* repeat_headers; a frame with pict_type I; baseline */
    memset(&t, 0, sizeof t); t.frames = 16; t.g = 6; t.force_at = 9; t.b = -1;
    t.opts = "repeat_headers=1:profile=constrained_baseline";   /* (the profile by name: the private option) */
    run("repeat headers", t);
    CHECK(fake_vcenc_param(P_PROFILE, 0) == 32 && fake_vcenc_param(P_INLINE, 0) == 1, "repeat headers: the settings");

    /* a slow encoder: the input fills, frames and packets in turn */
    memset(&t, 0, sizeof t); t.frames = 10; t.slow = 40; t.b = -1; t.opts = "in_buffers=1";
    run("slow", t);

    /* misbehaving encoders: quiet (open waits 5 s for the SPS and PPS), an error event part way */
    memset(&t, 0, sizeof t); t.frames = 6; t.b = -1; t.quiet = 1; t.expect_open = AVERROR_EXTERNAL;
    run("quiet", t);                         /* (no SPS and PPS at open) */
    memset(&t, 0, sizeof t); t.frames = 8; t.b = -1; t.stop_err = 1; t.error_at = 3;
    run("error event", t);

    /* sizes refused */
    memset(&t, 0, sizeof t); t.w = 66; t.h = 47; t.b = -1; t.expect_open = AVERROR(EINVAL);
    run("odd height", t);
    memset(&t, 0, sizeof t); t.w = 1922; t.h = 1080; t.b = -1; t.expect_open = AVERROR(ENOSYS);
    run("too wide", t);
    memset(&t, 0, sizeof t); t.b = -1; t.profile = FF_PROFILE_H264_HIGH_10; t.expect_open = AVERROR(ENOSYS);
    run("high 10", t);

    printf("vchiqenc_test: %s (%d failures here, %d in the fake)\n", fails || fake_vcenc_fails() ? "FAILED" : "all passed",
           fails, fake_vcenc_fails());
    return fails || fake_vcenc_fails();
}
