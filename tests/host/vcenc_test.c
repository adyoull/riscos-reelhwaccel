/*
 * vcenc (the encoder in libvcdec) against the fake ril.video_encode of
 * fake_vcenc.c (on the fake VCHIQ module of mmaldecode_test.c).
 * Checked: every packet right and in order with its pts, flags and
 * headers; encoders that hold inputs (VCENC_AGAIN), finish receives late,
 * want bigger output buffers, refuse settings (constant bit rate as the
 * Pi), go quiet, report errors; bad calls; everything closed and freed
 * after each run; then VCEncTest (tools/vcenctest) on the fake.
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include "fake_vcenc.c"
/* ---- the pictures ---- */

#define MAXW 200
static uint8_t src_mem[3][MAXW * 2 * 160 + 64];
static const uint8_t *src[3];
static int src_stride[3];

/* picture k (w x h): a pattern that differs at every edge, each row strided past its width */
static void make_picture(int k, int w, int h)
{
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (int p = 0; p < 3; p++) {
        int pw = p ? cw : w, ph = p ? ch : h;
        uint8_t *b = src_mem[p] + 3;          /* (not word aligned) */
        src_stride[p] = pw + 13;
        memset(src_mem[p], 0xC3, sizeof src_mem[p]);
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) b[y * src_stride[p] + x] = (uint8_t)(x * 3 + y * 7 + k * 11 + p * 50 + (x * y & 15));
        src[p] = b;
    }
}

static uint32_t picture_adler(int w, int h)
{
    uint32_t a = 0;
    for (int p = 0; p < 3; p++) {
        int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
        for (int y = 0; y < ph; y++) a = adler0(a, src[p] + y * src_stride[p], (size_t)pw);
    }
    return a;
}

static void logcb(void *h, const char *t) { (void)h; if (getenv("ENCLOG")) printf("  log: %s\n", t); }

typedef struct {
    int frames, w, h, flags, in_buffers, force_at, gop, profile, level, qp_min, qp_max, expect_open;
    unsigned bitrate;
    const char *open_err_has;
    int stop_at_error;                        /* vcenc_send/receive fails part way (and says this) */
    const char *fail_has;
    int expect_rc;                            /* the rate control in the end (0: not checked) */
} run_t;

static int run(const char *name, run_t t)
{
    vcenc *e = NULL;
    vcenc_config c;
    vcenc_packet pk;
    uint32_t want[64];
    int sent = 0, got = 0, configs = 0, againsend = 0, r, keys = 0, eof = 0, failed = 0;
    int f0 = fails;
    vcenc_config_init(&c);
    c.width = t.w ? t.w : 70; c.height = t.h ? t.h : 38;
    c.fps_num = 25; c.fps_den = 1; c.bitrate = t.bitrate; c.flags = (unsigned)t.flags | VCENC_LOG_MESSAGES;
    c.in_buffers = t.in_buffers; c.gop = t.gop; c.profile = t.profile; c.level = t.level; c.qp_min = t.qp_min; c.qp_max = t.qp_max;
    c.log = logcb;
    r = vcenc_open(&e, &c);
    if (r != t.expect_open) {
        CHECK(0, "%s: open gave %d (%s), expected %d", name, r, vcenc_open_error(), t.expect_open);
        if (e) vcenc_close(e);
        return 0;
    }
    if (r != VCENC_OK) {
        CHECK(!e && (!t.open_err_has || strstr(vcenc_open_error(), t.open_err_has)), "%s: open error \"%s\"", name,
              vcenc_open_error());
        closed_ok(name);
        printf("%s: open refused (%s)\n", name, vcenc_open_error());
        return fails == f0;
    }
    {
        const uint8_t *hd; size_t hn;
        CHECK(vcenc_headers(e, &hd, &hn) == VCENC_AGAIN, "%s: headers before any came", name);
    }
    while (!eof && !failed) {
        if (sent < t.frames) {
            make_picture(sent, c.width, c.height);
            want[sent] = picture_adler(c.width, c.height);
            r = vcenc_send(e, src, src_stride, 1000 + 40 * sent, sent == t.force_at ? VCENC_FORCE_KEY : 0);
            if (r == VCENC_OK) sent++;
            else if (r == VCENC_AGAIN) againsend++;
            else { failed = r; break; }
        } else if (sent == t.frames) {
            r = vcenc_send_eos(e);
            if (r == VCENC_OK) sent++;
            else if (r != VCENC_AGAIN) { failed = r; break; }
        }
        while ((r = vcenc_receive(e, &pk)) == VCENC_OK) {
            if (pk.flags & VCENC_PKT_CONFIG) {
                const param_t *pr = param(P_PROFILE);
                configs++;
                CHECK(got == 0 && configs == 1, "%s: a CONFIG packet after %d pictures", name, got);
                CHECK(pk.size == 17 && pk.data[4] == 0x67 && pr && pk.data[5] == pr->v[0] && pk.data[7] == pr->v[1] &&
                      pk.data[8] == 1 && pk.data[13] == 0x68, "%s: the CONFIG packet (%u bytes)", name, (unsigned)pk.size);
                continue;
            }
            {
                const uint8_t *d = pk.data;
                size_t o = 0;
                int k, key, inl = 0;
                uint32_t a, n0;
                CHECK(configs == 1, "%s: a picture before the CONFIG packet", name);
                if (pk.size > 17 && d[4] == 0x67) { inl = 1; o = 17; }
                CHECK(pk.size >= o + 13 && !d[o] && !d[o + 1] && !d[o + 2] && d[o + 3] == 1, "%s: packet %d isn't Annex B", name, got);
                key = d[o + 4] == 0x65;
                memcpy(&k, d + o + 5, 4); memcpy(&a, d + o + 9, 4);
                n0 = got == e_big_at ? 100000 : 20 + 37u * (uint32_t)got;
                CHECK(k == got, "%s: packet %d holds picture %d", name, got, k);
                CHECK(got < 64 && a == want[got], "%s: picture %d's data (Adler %08X, sent %08X)", name, got, a,
                      got < 64 ? want[got] : 0);
                CHECK(pk.size == o + (n0 > 13 ? n0 : 13), "%s: packet %d is %u bytes", name, got, (unsigned)pk.size);
                CHECK(pk.pts == 1000 + 40 * got && pk.dts == pk.pts, "%s: packet %d pts %lld", name, got, (long long)pk.pts);
                CHECK(!!(pk.flags & VCENC_PKT_KEY) == key, "%s: packet %d's key flag", name, got);
                CHECK(inl == (key && (t.flags & VCENC_INLINE_HEADERS)), "%s: packet %d: headers %d", name, got, inl);
                if (key) keys++;
                if (t.gop) CHECK(key == (got % t.gop == 0 || got == t.force_at), "%s: packet %d key %d", name, got, key);
                else CHECK(key == (got == 0 || got == t.force_at), "%s: packet %d key %d", name, got, key);
                for (size_t i = o + 13; i < pk.size; i++)
                    if (d[i] != (uint8_t)(0xA5 ^ got)) { CHECK(0, "%s: packet %d byte %u", name, got, (unsigned)i); break; }
                got++;
            }
        }
        if (r == VCENC_EOF) eof = 1;
        else if (r != VCENC_AGAIN) failed = r;
    }
    if (failed) {
        CHECK(t.stop_at_error && failed == VCENC_ERROR && (!t.fail_has || strstr(vcenc_error(e), t.fail_has)),
              "%s: failed (%d) after %d pictures: %s", name, failed, got, vcenc_error(e));
    } else {
        vcenc_stats s;
        const uint8_t *hd; size_t hn;
        CHECK(!t.stop_at_error, "%s: didn't fail", name);
        CHECK(got == t.frames && configs == 1 && e_frames == t.frames, "%s: %d of %d packets (%d encoded)", name, got, t.frames,
              e_frames);
        CHECK(vcenc_headers(e, &hd, &hn) == VCENC_OK && hn == 17 && hd[4] == 0x67, "%s: vcenc_headers", name);
        CHECK(vcenc_receive(e, &pk) == VCENC_EOF, "%s: EOF again", name);
        CHECK(vcenc_send(e, src, src_stride, 0, 0) == VCENC_EINVAL, "%s: a picture after the EOS", name);
        vcenc_get_stats(e, &s);
        CHECK(s.pictures == (unsigned)t.frames && s.packets == (unsigned)t.frames + 1 && s.config_packets == 1 &&
              s.keyframes == (unsigned)keys, "%s: stats %u %u %u %u", name, s.pictures, s.packets, s.config_packets, s.keyframes);
        CHECK(!!(t.flags & VCENC_PCI) == !s.in_pools && !!(t.flags & VCENC_PCI) == !s.out_pools, "%s: pools %d %d", name,
              s.in_pools, s.out_pools);
        CHECK(!t.expect_rc || s.rate_control == t.expect_rc, "%s: rate control %d", name, s.rate_control);
        CHECK(s.headers_repeated == (t.flags & VCENC_INLINE_HEADERS ? (unsigned)keys : 0), "%s: %u keyframes given headers",
              name, s.headers_repeated);
        CHECK(!e_pad_bad, "%s: %d rows' padding not the edge repeated", name, e_pad_bad);
        CHECK(!(t.flags & VCENC_PCI) || !pmp_invalidates, "%s: cache maintenance on PCI memory", name);
        printf("%s: %d pictures, %d keyframes, %d receives, %d times input full\n", name, got, keys, e_receives, againsend);
    }
    vcenc_close(e);
    closed_ok(name);
    return fails == f0;
}

static void params_check(const char *name, uint32_t profile, uint32_t level, uint32_t bitrate, uint32_t rc, int gop, int qmin,
                         int qmax, int inl)
{
    const param_t *p;
    CHECK((p = param(P_PROFILE)) && p->n == 2 && p->v[0] == profile && p->v[1] == level, "%s: profile %u level &%X", name,
          p ? p->v[0] : 0, p ? p->v[1] : 0);
    CHECK((p = param(P_BITRATE)) && p->v[0] == bitrate, "%s: bit rate %u", name, p ? p->v[0] : 0);
    CHECK((p = param(P_RATECONTROL)) && p->v[0] == rc, "%s: rate control %u", name, p ? p->v[0] : 0);
    CHECK(gop ? (p = param(P_INTRAPERIOD)) && p->v[0] == (uint32_t)gop : !param(P_INTRAPERIOD), "%s: intra period", name);
    CHECK(qmin ? (p = param(P_MINQ)) && p->v[0] == (uint32_t)qmin : !param(P_MINQ), "%s: min quant", name);
    CHECK(qmax ? (p = param(P_MAXQ)) && p->v[0] == (uint32_t)qmax : !param(P_MAXQ), "%s: max quant", name);
    CHECK((p = param(P_INLINE)) && p->v[0] == (uint32_t)inl, "%s: inline header", name);
    CHECK((p = param(P_SPS_TIMING)) && p->v[0] == 1, "%s: SPS timing", name);
    CHECK((p = param(P_MINFRAG)) && p->port == EP_CTL && p->v[0] == 1, "%s: minimise fragmentation", name);
    CHECK(e_in_enc == 0x30323449u && e_out_enc == 0x34363248u, "%s: encodings %08X %08X", name, e_in_enc, e_out_enc);
}

int main(void)
{
    run_t t;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* a whole encode: pools, the first units in the message, the rest by bulk, one in two pieces */
    enc_reset(); e_big_at = 6;
    memset(&t, 0, sizeof t); t.frames = 12; t.gop = 5; t.bitrate = 2000000;
    run("pools", t);
    params_check("pools", 28, 0x1A, 2000000, 1, 5, 0, 0, 0);
    CHECK(e_in_stride == 96 && e_in_rows == 48 && e_crop_w == 70 && e_crop_h == 38, "input format %ux%u %ux%u", e_in_stride,
          e_in_rows, e_crop_w, e_crop_h);
    CHECK(pmp_invalidates > 0, "pools: no cache maintenance");

    /* PCI memory; receives finishing late; a keyframe asked for; headers before keyframes; main, level 4.1, CBR */
    enc_reset(); e_rx_late = 1; e_big_at = 3; pts_late = 1;
    memset(&t, 0, sizeof t); t.frames = 10; t.gop = 4; t.flags = VCENC_PCI | VCENC_INLINE_HEADERS | VCENC_CBR; t.force_at = 6;
    t.profile = VCENC_PROFILE_MAIN; t.level = 41; t.qp_min = 20; t.qp_max = 40; t.expect_rc = 4;
    run("pci", t);
    params_check("pci", 26, 0x1D, 10000000, 4, 4, 20, 40, 1);

    /* constant bit rate refused both ways: variable */
    enc_reset(); e_refuse_rc4 = 1;
    memset(&t, 0, sizeof t); t.frames = 3; t.flags = VCENC_CBR; t.expect_rc = 1;
    run("cbr refused", t);
    params_check("cbr refused", 28, 0x1A, 10000000, 1, 0, 0, 0, 0);

    /* a slow encoder that holds each input until its unit is out (input full), one input buffer; baseline */
    enc_reset(); e_hold_in = 1; e_pim = 0; e_rx_late = 1; e_slow = 5;
    memset(&t, 0, sizeof t); t.frames = 8; t.in_buffers = 1; t.profile = VCENC_PROFILE_BASELINE; t.w = 64; t.h = 32;
    run("held", t);
    params_check("held", 32, 0x1A, 10000000, 1, 0, 0, 0, 0);
    CHECK(e_in_stride == 64 && e_in_rows == 32, "held: input %ux%u", e_in_stride, e_in_rows);

    /* an encoder that wants bigger output buffers than 512 KB */
    enc_reset(); e_out_min = 700000;
    memset(&t, 0, sizeof t); t.frames = 3; t.w = 2; t.h = 2;
    run("bigger output", t);
    CHECK(e_out_size_set >= 700000, "bigger output: %u", e_out_size_set);

    /* a setting refused: carried on without it; a needed one: open fails */
    enc_reset(); e_refuse_id = P_RATECONTROL;
    memset(&t, 0, sizeof t); t.frames = 2;
    run("refused rate control", t);
    enc_reset(); e_refuse_id = P_PROFILE;
    memset(&t, 0, sizeof t); t.expect_open = VCENC_ERROR; t.open_err_has = "Profile and level: ENOSYS";
    run("refused profile", t);

    /* an encoder that goes quiet; one that reports an error part way */
    enc_reset(); e_quiet = 1;
    memset(&t, 0, sizeof t); t.frames = 4; t.stop_at_error = 1; t.fail_has = "Nothing from the encoder";
    run("quiet", t);
    enc_reset(); e_error_at = 2;
    memset(&t, 0, sizeof t); t.frames = 5; t.stop_at_error = 1; t.fail_has = "reports an error";
    run("error event", t);

    /* refusals before anything's made */
    enc_reset();
    memset(&t, 0, sizeof t); t.w = 71; t.expect_open = VCENC_EINVAL;
    run("odd width", t);
    memset(&t, 0, sizeof t); t.w = 1922; t.h = 1080; t.expect_open = VCENC_UNSUPPORTED;
    run("too wide", t);
    memset(&t, 0, sizeof t); t.w = 1920; t.h = 1080; t.expect_open = VCENC_UNSUPPORTED; t.open_err_has = "gpu_mem=128";
    run("gpu_mem", t);                       /* (the fake has 76 MB) */
    memset(&t, 0, sizeof t); t.level = 33; t.expect_open = VCENC_EINVAL;
    run("bad level", t);
    memset(&t, 0, sizeof t); t.in_buffers = 5; t.expect_open = VCENC_EINVAL;
    run("five buffers", t);

    /* 1080p (gpu_mem check off): sizes and level */
    enc_reset();
    memset(&t, 0, sizeof t); t.frames = 0; t.w = 1920; t.h = 1080; t.flags = VCENC_NO_GPU_MEM_CHECK;
    {
        vcenc *e; vcenc_config c;
        vcenc_config_init(&c);
        c.width = 1920; c.height = 1080; c.flags = VCENC_NO_GPU_MEM_CHECK; c.fps_num = 30; c.fps_den = 1;
        CHECK(vcenc_open(&e, &c) == VCENC_OK, "1080p open: %s", vcenc_open_error());
        CHECK(e_in_stride == 1920 && e_in_rows == 1088 && e_crop_h == 1080, "1080p input %ux%u", e_in_stride, e_in_rows);
        CHECK(param(P_PROFILE) && param(P_PROFILE)->v[1] == 0x1C, "1080p30 at 10 Mbit/s: level 4.0 (&%X)",
              param(P_PROFILE) ? param(P_PROFILE)->v[1] : 0);
        {
            const uint8_t *pl[3] = { NULL, NULL, NULL }; int st[3] = { 1920, 960, 960 };
            CHECK(vcenc_send(e, pl, st, 0, 0) == VCENC_EINVAL, "no planes");
        }
        CHECK(vcenc_send_eos(e) == VCENC_OK, "an EOS with nothing sent");
        {   /* (the SPS and PPS came at open, as on the Pi: then the end) */
            vcenc_packet pk;
            int rr, n = 0;
            while ((rr = vcenc_receive(e, &pk)) == VCENC_OK || (rr == VCENC_AGAIN && n < 1000)) n += rr == VCENC_OK ? 1 : 0;
            CHECK(rr == VCENC_EOF && n == 1 && (pk.flags & VCENC_PKT_CONFIG), "nothing sent: %d, %d packets", rr, n);
        }
        vcenc_close(e);
        closed_ok("1080p");
        printf("1080p: opened and closed\n");
    }
    /* short strides refused, the encoder carries on */
    enc_reset();
    {
        vcenc *e; vcenc_config c; int st[3];
        vcenc_config_init(&c);
        c.width = 70; c.height = 38;
        CHECK(vcenc_open(&e, &c) == VCENC_OK, "open: %s", vcenc_open_error());
        make_picture(0, 70, 38);
        st[0] = 69; st[1] = st[2] = 35;
        CHECK(vcenc_send(e, src, st, 0, 0) == VCENC_EINVAL && strstr(vcenc_error(e), "strides"), "short strides");
        CHECK(vcenc_send(e, src, src_stride, 0, 0) == VCENC_OK, "a picture after the refusal");
        vcenc_close(e);                      /* (closed with a picture in) */
        closed_ok("short strides");
        printf("short strides: refused; closed with a picture in\n");
    }

    /* VCEncTest (tools/vcenctest) on the fake: its checks, report and stream file */
    {
        static const char *runs[][20] = {
            { "vcenctest", "-s", "64x32", "-n", "20", "-g", "5", "-i", "-k", "7", "-w", "/tmp/vcenctest.h264", "-o",
              "/tmp/vcenctest.out", NULL },
            { "vcenctest", "-s", "128x72", "-n", "9", "-m", "pci", "-p", "baseline", "-c", "-t", "-B", "1", "-o",
              "/tmp/vcenctest.out", NULL },
        };
        remove("/tmp/vcenctest.out");
        for (int i = 0; i < 2; i++) {
            int argc = 0, rr;
            enc_reset();
            while (runs[i][argc]) argc++;
            rr = probe_main(argc, (char **)runs[i]);
            CHECK(rr == 0, "VCEncTest run %d: %d", i, rr);
            closed_ok("VCEncTest");
        }
        {
            FILE *f = fopen("/tmp/vcenctest.h264", "rb");
            static uint8_t b[65536];
            size_t n = f ? fread(b, 1, sizeof b, f) : 0, want = 17;
            int keys = 0;
            if (f) fclose(f);
            for (int k = 0; k < 20; k++) {    /* (the fake's units: 13 bytes or 20+37k, headers before keys) */
                int key = k % 5 == 0 || k == 7;
                size_t s0 = 20 + 37u * (size_t)k;
                want += (key ? 17 : 0) + (s0 > 13 ? s0 : 13);
                keys += key;
            }
            CHECK(n == want && b[4] == 0x67 && b[17 + 4] == 0x67, "VCEncTest's stream: %u bytes (%u expected)", (unsigned)n,
                  (unsigned)want);
            printf("VCEncTest: the stream file is %u bytes, %d keyframes\n", (unsigned)n, keys);
        }
        {
            FILE *f = fopen("/tmp/vcenctest.out", "r");
            char line[256];
            int oks = 0;
            while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "Result: OK", 10)) oks++;
            if (f) fclose(f);
            CHECK(oks == 2, "VCEncTest's report: %d OK results", oks);
        }
    }

    swi_hook = NULL;
    printf("vcenc_test: %s (%d failures)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
