/*
 * vcdec (the library) against the fake VCHIQ module and MMAL decoder of
 * mmaldecode_test.c (included, without its main), which behaves as the Pi
 * did for MMALDecode 0.12 to 0.17. The real copy routines run
 * (vcdec_copy.S, built with VCDEC_HOST: the SWIs that enter and leave SVC
 * mode open and close the fake's privileged PCI memory instead).
 *
 * The stream: the fake MP4's twelve access units (two closed GOPs, I P B B
 * P B, the first over 64 KB), made here as Annex B. Each picture is
 * received into the caller's planes and checked byte by byte, with guard
 * bytes around each row:
 *   - a whole decode in display order with its pts, then EOF; with the
 *     receives finishing late, waited for one by one, into unaligned planes;
 *   - a seek before the end (FLUSH: nothing lost) and after it (the decoder
 *     created again: nothing lost; a flush alone loses two, as on the Pi);
 *   - pictures not taken when flushing; input full (VCDEC_AGAIN, nothing
 *     half sent); bad calls; an unsupported profile; sizes and gpu_mem;
 *   - a decoder that never answers, reports an error, changes format,
 *     waits for buffers before its format change, sends a long event;
 *   - after every run: everything closed and freed.
 * Then VCDecTest (tools/vcdectest) on the fake's MP4: a whole decode, a
 * seek before the end and after it, -S, -t, a picture within rounding,
 * pictures out of order, a seek that can't happen.
 */
#define FAKE_ONLY
#include "mmaldecode_test.c"
#include "../../vcdec/vcdec.h"

void vcdec_host_enter_os(void) { pci_open(1); }
void vcdec_host_leave_os(void) { pci_open(0); }

#define CW ((W + 1) / 2)
#define CH ((H + 1) / 2)
#define GUARD 0xE7

static uint8_t aubuf[80000];

/* access unit k (decode order) of the twelve, as Annex B */
static size_t make_au(int k, int profile)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    size_t n = 0, fill = k == 0 ? 70000 : 41 + 13 * (size_t)k;   /* (most not whole words) */
    if (k % 6 == 0) {
        const uint8_t sps[] = { 0x67, (uint8_t)profile, 0x00, 0x1E, 0xAB }, pps[] = { 0x68, 0xCE, 0x3C, 0x80 };
        memcpy(aubuf + n, sc, 4); n += 4; memcpy(aubuf + n, sps, sizeof sps); n += sizeof sps;
        memcpy(aubuf + n, sc, 4); n += 4; memcpy(aubuf + n, pps, sizeof pps); n += sizeof pps;
    }
    memcpy(aubuf + n, sc, 4); n += 4;
    aubuf[n++] = k % 6 == 0 ? 0x65 : 0x41;
    aubuf[n++] = mp4_val(mp4_pts[k]);
    memset(aubuf + n, 0xAA, fill); n += fill;
    return n;
}

/* the caller's picture: three planes with guard bytes around every row */
static uint8_t pic_mem[4 * 64 * 64];
static uint8_t *planes[3];
static int strides[3], plane_rows[3], plane_w[3];

static void setup_planes(int skew, int extra)
{
    uint8_t *p = pic_mem + 16;
    memset(pic_mem, GUARD, sizeof pic_mem);
    for (int i = 0; i < 3; i++) {
        plane_w[i] = i ? CW : W;
        plane_rows[i] = i ? CH : H;
        strides[i] = plane_w[i] + 8 + extra;
        planes[i] = p + skew;
        p += strides[i] * plane_rows[i] + 32;
    }
}

/* the picture as the fake made it: Y val, U val+1, V val+2, guards intact */
static int picture_right(uint8_t val)
{
    for (int i = 0; i < 3; i++)
        for (int y = 0; y < plane_rows[i]; y++) {
            const uint8_t *r = planes[i] + y * strides[i];
            if (r[-1] != GUARD || r[plane_w[i]] != GUARD) return 0;
            for (int x = 0; x < plane_w[i]; x++) if (r[x] != (uint8_t)(val + i)) return 0;
            for (int x = plane_w[i]; x < strides[i]; x++) if (r[x] != GUARD) return 0;
        }
    for (int i = 0; i < 3; i++) {                /* and nothing written to the next one */
        memset(planes[i], GUARD, (size_t)(strides[i] * plane_rows[i]) - 1);
        planes[i][-1] = GUARD;
    }
    return 1;
}

static vcdec *open70(unsigned flags)
{
    vcdec_config c;
    vcdec *d = NULL;
    int r;
    vcdec_config_init(&c);
    c.width = W; c.height = H; c.flags = flags;
    reset_fake();
    mp4_mode = 1;
    r = vcdec_open(&d, &c);
    CHECK(r == VCDEC_OK && d, "open: %d %s", r, vcdec_open_error());
    return d;
}

static void close70(vcdec *d, const char *what)
{
    vcdec_close(d);
    mp4_mode = 0;
    cleaned(what);
}

/* receives every picture that's ready; their pts (in frames) appended to
   got[]; -1 if one is wrong. The result of the last receive. */
static int got[64], ngot, wrong_pics;
static int take(vcdec *d)
{
    vcdec_picture p;
    int r;
    while ((r = vcdec_receive(d, &p, planes, strides)) == VCDEC_OK) {
        int f = (int)(p.pts / 40000);
        CHECK(p.width == W && p.height == H, "picture %dx%d", p.width, p.height);
        if (!picture_right(mp4_val(f))) wrong_pics++;
        if (ngot < 64) got[ngot++] = f;
    }
    return r;
}

/* sends access units from..to-1 (as many as go), taking pictures as they
   come; then EOS if asked, and everything to EOF */
static int feed(vcdec *d, int from, int to, int eos)
{
    int k = from, r = VCDEC_OK, spins = 0;
    while (k < to && spins < 10000) {
        size_t n = make_au(k, 100);
        r = vcdec_send(d, aubuf, n, (int64_t)mp4_pts[k] * 40000, (int64_t)k * 40000, k % 6 == 0 ? VCDEC_KEYFRAME : 0);
        if (r == VCDEC_OK) k++;
        else if (r != VCDEC_AGAIN) return r;
        r = take(d);
        if (r < 0) return r;
        spins++;
    }
    if (!eos) return VCDEC_OK;
    while ((r = vcdec_send_eos(d)) == VCDEC_AGAIN && spins++ < 10000) take(d);
    if (r != VCDEC_OK) return r;
    while ((r = take(d)) == VCDEC_AGAIN && spins++ < 10000) {}
    return r;
}

static int got_is(int from, int n)
{
    if (ngot != n) return 0;
    for (int i = 0; i < n; i++) if (got[i] != from + i) return 0;
    return 1;
}

static void whole(unsigned flags, int late, int skew, int extra, const char *what)
{
    vcdec *d = open70(flags);
    vcdec_stats s;
    int r;
    rx_late = late;
    setup_planes(skew, extra);
    ngot = wrong_pics = 0;
    r = feed(d, 0, 12, 1);
    vcdec_get_stats(d, &s);
    CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics && s.pictures == 12 && s.sent == 12,
          "%s: %d, %d pictures (%d wrong), stats %u/%u", what, r, ngot, wrong_pics, s.pictures, s.sent);
    CHECK(aus == 12 && au_pieces_max == 2 && bulks_rx == 13, "%s: %d access units, %d pieces, %d receives", what, aus,
          au_pieces_max, bulks_rx);
    CHECK(vcdec_receive(d, NULL, planes, strides) == VCDEC_EOF, "%s: EOF again", what);
    close70(d, what);
    rx_late = 0;
}

int probe_main(int argc, char **argv);        /* tools/vcdectest */

/* VCDecTest on the fake's MP4, with up to three options */
static char *run_app(int *ret, const char *o1, const char *o2, const char *o3)
{
    static char buf[16384];
    char *argv[12];
    int n = 0;
    FILE *f;
    size_t got_n;
    argv[n++] = "vcdectest"; argv[n++] = "-o"; argv[n++] = "/tmp/vcdec_test.out";
    if (o1) argv[n++] = (char *)o1;
    if (o2) argv[n++] = (char *)o2;
    if (o3) argv[n++] = (char *)o3;
    argv[n++] = "/tmp/mmaldecode_test.mp4"; argv[n++] = "/tmp/mmaldecode_test4.crc"; argv[n++] = "/tmp/mmaldecode_test4.sig";
    argv[n] = NULL;
    reset_fake();
    mp4_mode = 1;
    remove("/tmp/vcdec_test.out");
    *ret = probe_main(n, argv);
    mp4_mode = 0;
    f = fopen("/tmp/vcdec_test.out", "r");
    got_n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got_n] = 0;
    if (f) fclose(f);
    return buf;
}

static void app_tests(void)
{
    int ret;
    char *o;
    make_mp4();
    o = run_app(&ret, NULL, NULL, NULL);
    printf("%s", o);
    CHECK(ret == 0 && strstr(o, "MP4: 12 samples, 70x38") && strstr(o, "12 pictures in") &&
          strstr(o, "12 checked against FFmpeg: 0 wrong") && strstr(o, "Result: OK - every picture exactly"),
          "VCDecTest (%d):\n%s", ret, o);
    cleaned("app");
    rx_late = 1;
    o = run_app(&ret, "-S", NULL, NULL);
    CHECK(ret == 0 && strstr(o, "-S: each bulk") && strstr(o, "Result: OK"), "VCDecTest -S (%d):\n%s", ret, o);
    cleaned("app -S");
    o = run_app(&ret, "-s", "3", NULL);
    printf("%s", o);
    CHECK(ret == 0 && flushes == 2 && !recreated && !eos_lost && strstr(o, "the EOS not") && strstr(o, "sample 6, pts 240000") &&
          strstr(o, "from the keyframe on, 6 (") && strstr(o, "all of them") && strstr(o, "Result: OK"),
          "VCDecTest -s 3 (%d, %d flushes):\n%s", ret, flushes, o);
    cleaned("app -s");
    o = run_app(&ret, "-E", NULL, NULL);
    printf("%s", o);
    CHECK(ret == 0 && recreated == 1 && !eos_lost && strstr(o, "Pass 1: 12 pictures to the end, all right") &&
          strstr(o, "1 created again") && strstr(o, "Pass 2 (after the end): 6 pictures from the keyframe on") &&
          strstr(o, "all of them") && strstr(o, "Result: OK"), "VCDecTest -E (%d, %d recreated):\n%s", ret, recreated, o);
    cleaned("app -E");
    rx_late = 0;
    corrupt_at = 3;
    o = run_app(&ret, "-t", NULL, NULL);
    CHECK(ret == 0 && strstr(o, "OK - timed") && strstr(o, "Copying the pictures out"), "VCDecTest -t (%d):\n%s", ret, o);
    cleaned("app -t");
    o = run_app(&ret, "-n", NULL, NULL);
    CHECK(ret == 0 && strstr(o, "within 1): 1; not close: 0") && strstr(o, "some within rounding"),
          "VCDecTest, a picture 1 off (%d):\n%s", ret, o);
    cleaned("app close");
    corrupt_at = -1;
    pts_fifo = 1;
    o = run_app(&ret, "-n", NULL, NULL);
    CHECK(ret == 1 && strstr(o, "out of display order"), "VCDecTest, pts in decode order (%d):\n%s", ret, o);
    cleaned("app pts FIFO");
    pts_fifo = 0;
    o = run_app(&ret, "-s", "9", NULL);
    CHECK(ret == 1 && strstr(o, "has been passed") && !flushes, "VCDecTest -s 9 (%d):\n%s", ret, o);
    cleaned("app passed");
    o = run_app(&ret, "-s", "50", NULL);
    CHECK(ret == 1 && strstr(o, "never happened"), "VCDecTest -s 50 (%d):\n%s", ret, o);
    cleaned("app no seek");
}

int main(int argc, char **argv)
{
    vcdec *d;
    vcdec_config c;
    vcdec_picture p;
    vcdec_stats s;
    int r, before;

    if (argc > 1) return probe_main(argc, argv);   /* (run.sh's -x check of real MP4s) */

    /* ---- a whole decode ---- */
    whole(0, 0, 0, 0, "whole");
    whole(0, 1, 0, 0, "whole, receives late");
    whole(VCDEC_SYNC_RECEIVE, 1, 0, 0, "whole, receives waited for");
    whole(0, 1, 1, 3, "whole, unaligned planes");
    whole(0, 0, 2, 1, "whole, odd strides");
    CHECK(recreated == 0, "created again without a flush");

    /* the pts back as sent, in display order; peek doesn't take */
    d = open70(0);
    setup_planes(0, 0);
    for (int k = 0; k < 6; k++) { size_t n = make_au(k, 100); vcdec_send(d, aubuf, n, (int64_t)mp4_pts[k] * 40000 + 7, 0, k ? 0 : 1); }
    for (int i = 0; i < 50 && (r = vcdec_peek(d, &p)) == VCDEC_AGAIN; i++) {}
    CHECK(r == VCDEC_OK && p.pts == 7 && (p.flags & VCDEC_PIC_KEYFRAME) && vcdec_peek(d, &p) == VCDEC_OK && p.pts == 7,
          "peek: %d, pts %lld, flags %u", r, (long long)p.pts, p.flags);
    CHECK(vcdec_receive(d, &p, planes, strides) == VCDEC_OK && p.pts == 7 && picture_right(mp4_val(0)), "receive after peek");
    CHECK(vcdec_receive(d, &p, planes, strides) == VCDEC_OK && p.pts == 40007 && !(p.flags & VCDEC_PIC_KEYFRAME),
          "the next: pts %lld", (long long)p.pts);
    close70(d, "peek");

    /* ---- seeking ---- */
    /* before the end: the first GOP, three pictures, FLUSH, the second GOP */
    for (int late = 0; late < 2; late++) {
        d = open70(0);
        rx_late = late;
        setup_planes(0, 0);
        ngot = wrong_pics = 0;
        feed(d, 0, 6, 0);
        for (int i = 0; i < 50 && ngot < 3; i++) take(d);
        before = ngot;
        r = vcdec_flush(d);
        CHECK(r == VCDEC_OK && flushes == 2 && !recreated, "flush: %d, %d flushes, %d recreated", r, flushes, recreated);
        CHECK(before >= 3 && before < 6 && take(d) == VCDEC_AGAIN && ngot == before,
              "pictures from before the flush after it: %d, then %d", before, ngot);
        ngot = 0;
        r = feed(d, 6, 12, 1);
        vcdec_get_stats(d, &s);
        CHECK(r == VCDEC_EOF && got_is(6, 6) && !wrong_pics && !eos_lost && s.flushes == 1,
              "seek before the end (late %d): %d, %d pictures, %d lost", late, r, ngot, eos_lost);
        close70(d, "seek");
        rx_late = 0;
    }

    /* pictures not taken when it flushes: dropped */
    d = open70(0);
    setup_planes(0, 0);
    for (int k = 0; k < 6; k++) { size_t n = make_au(k, 100); vcdec_send(d, aubuf, n, (int64_t)mp4_pts[k] * 40000, 0, k ? 0 : 1); }
    for (int i = 0; i < 50 && vcdec_peek(d, &p) == VCDEC_AGAIN; i++) {}
    CHECK(vcdec_flush(d) == VCDEC_OK, "flush with pictures held");
    vcdec_get_stats(d, &s);
    CHECK(s.discarded >= 1 && s.pictures == 0, "held pictures dropped: %u", s.discarded);
    ngot = wrong_pics = 0;
    r = feed(d, 6, 12, 1);
    CHECK(r == VCDEC_EOF && got_is(6, 6) && !wrong_pics, "after dropping: %d, %d pictures", r, ngot);
    close70(d, "flush held");

    /* after the end (EOS reached the decoder): created again, nothing lost */
    for (int late = 0; late < 2; late++) {
        d = open70(0);
        rx_late = late;
        setup_planes(0, 0);
        ngot = 0;
        r = feed(d, 0, 12, 1);
        CHECK(r == VCDEC_EOF && got_is(0, 12), "to the end: %d, %d", r, ngot);
        CHECK(vcdec_send(d, aubuf, make_au(6, 100), 6 * 40000, 0, 1) == VCDEC_EINVAL && strstr(vcdec_error(d), "after the EOS"),
              "a send after the EOS");
        r = vcdec_flush(d);
        vcdec_get_stats(d, &s);
        CHECK(r == VCDEC_OK && recreated == 1 && created == 2 && destroyed == 1 && s.recreated == 1,
              "flush after the EOS: %d, created %d, destroyed %d", r, created, destroyed);
        ngot = wrong_pics = 0;
        r = feed(d, 6, 12, 1);
        CHECK(r == VCDEC_EOF && got_is(6, 6) && !wrong_pics && !eos_lost,
              "seek after the end (late %d): %d, %d pictures, %d lost", late, r, ngot, eos_lost);
        /* and again, from the start */
        CHECK(vcdec_flush(d) == VCDEC_OK && recreated == 2, "a second flush after the EOS");
        ngot = 0;
        r = feed(d, 0, 12, 1);
        CHECK(r == VCDEC_EOF && got_is(0, 12) && !eos_lost, "replay from the start: %d, %d", r, ngot);
        close70(d, "seek after the end");
        rx_late = 0;
    }

    /* DISCONTINUITY passed on */
    d = open70(0);
    setup_planes(0, 0);
    feed(d, 0, 6, 0);
    vcdec_flush(d);
    r = vcdec_send(d, aubuf, make_au(6, 100), 6 * 40000, 6 * 40000, VCDEC_KEYFRAME | VCDEC_DISCONTINUITY);
    CHECK(r == VCDEC_OK && disc_seen == 1, "DISCONTINUITY: %d, %d seen", r, disc_seen);
    close70(d, "discontinuity");

    /* ---- the input full: VCDEC_AGAIN, an access unit never half sent ---- */
    hold_inputs = 100;                        /* (the fake keeps every input buffer until the EOS) */
    d = open70(0);
    setup_planes(0, 0);
    {
        /* access unit 0 (two pieces), seventeen of one piece; then with one
           buffer free, access unit 0 again: refused whole */
        int k;
        r = VCDEC_OK;
        for (k = 0; k < 18 && r == VCDEC_OK; k++) {
            int j = k ? 1 + (k - 1) % 5 : 0;
            r = vcdec_send(d, aubuf, make_au(j, 100), (int64_t)k * 40000, 0, !j);
        }
        CHECK(r == VCDEC_OK && bulks_tx == 19, "input nearly full: %d, %d pieces", r, bulks_tx);
        r = vcdec_send(d, aubuf, make_au(0, 100), 18 * 40000, 0, 1);
        CHECK(r == VCDEC_AGAIN && bulks_tx == 19, "two pieces, one buffer: %d, %d pieces", r, bulks_tx);
        r = vcdec_send(d, aubuf, make_au(1, 100), 18 * 40000, 0, 0);
        CHECK(r == VCDEC_OK && bulks_tx == 20, "one piece, one buffer: %d, %d pieces", r, bulks_tx);
        CHECK(vcdec_send(d, aubuf, make_au(1, 100), 19 * 40000, 0, 0) == VCDEC_AGAIN, "input full");
        CHECK(vcdec_send_eos(d) == VCDEC_AGAIN, "EOS with the input full");
    }
    close70(d, "input full");
    hold_inputs = 0;

    /* ---- bad calls, unsupported streams and sizes ---- */
    d = open70(0);
    CHECK(vcdec_send(d, aubuf, 0, 0, 0, 0) == VCDEC_EINVAL, "an empty send");
    CHECK(vcdec_send(d, aubuf, (size_t)21 * 65536, 0, 0, 0) == VCDEC_EINVAL, "a send too big");
    r = vcdec_send(d, aubuf, make_au(0, 110), 0, 0, 1);   /* High 10 */
    CHECK(r == VCDEC_UNSUPPORTED && strstr(vcdec_error(d), "profile") && !aus, "High 10: %d", r);
    CHECK(vcdec_send(d, aubuf, make_au(0, 100), 0, 0, 1) == VCDEC_UNSUPPORTED, "unsupported stays");
    close70(d, "unsupported");
    CHECK(vcdec_check_stream(aubuf, make_au(0, 66)) == VCDEC_OK && vcdec_check_stream(aubuf, make_au(0, 77)) == VCDEC_OK &&
          vcdec_check_stream(aubuf, make_au(0, 244)) == VCDEC_UNSUPPORTED && vcdec_check_stream(aubuf, make_au(1, 244)) == VCDEC_OK,
          "vcdec_check_stream");

    CHECK(vcdec_gpu_mem() == 76, "gpu_mem %u", vcdec_gpu_mem());
    vcdec_config_init(&c);
    reset_fake();
    c.width = 1920; c.height = 1080;          /* the fake has 76 MB */
    r = vcdec_open(&d, &c);
    CHECK(r == VCDEC_UNSUPPORTED && !d && strstr(vcdec_open_error(), "gpu_mem=128") && !opens && !pci_live,
          "1080p at 76 MB: %d %s", r, vcdec_open_error());
    c.width = 2048; c.height = 1080; c.flags = VCDEC_NO_GPU_MEM_CHECK;
    CHECK(vcdec_open(&d, &c) == VCDEC_UNSUPPORTED && !d, "2048 wide");
    c.width = 0;
    CHECK(vcdec_open(&d, &c) == VCDEC_EINVAL && !d, "no size");
    no_pci_mem = 1;
    c.width = W; c.height = H;
    r = vcdec_open(&d, &c);
    CHECK(r == VCDEC_ERROR && !d && strstr(vcdec_open_error(), "physically contiguous") && !opens && !pci_live,
          "no PCI memory: %d %s", r, vcdec_open_error());
    no_pci_mem = 0;
    npci_blocks = 0;

    /* ---- decoders that misbehave ---- */
    go_quiet = 1;                             /* never a word: gpu_mem too small for the size */
    d = open70(0);
    setup_planes(0, 0);
    vcdec_send(d, aubuf, make_au(0, 100), 0, 0, 1);
    for (int i = 0; i < 2000 && (r = vcdec_receive(d, &p, planes, strides)) == VCDEC_AGAIN; i++) {}
    CHECK(r == VCDEC_ERROR && strstr(vcdec_error(d), "Nothing from the decoder") && strstr(vcdec_error(d), "76 MB"),
          "quiet: %d %s", r, vcdec_error(d));
    CHECK(vcdec_send(d, aubuf, make_au(1, 100), 0, 0, 0) == VCDEC_ERROR, "failed stays failed");
    close70(d, "quiet");
    go_quiet = 0;

    error_event = 1;
    d = open70(0);
    setup_planes(0, 0);
    ngot = 0;
    r = feed(d, 0, 12, 1);
    CHECK(r == VCDEC_ERROR && strstr(vcdec_error(d), "reports an error"), "error event: %d %s", r, vcdec_error(d));
    close70(d, "error event");
    error_event = 0;

    big_efch = 1;                             /* a format change to a bigger size */
    d = open70(0);
    setup_planes(0, 0);
    ngot = wrong_pics = 0;
    r = feed(d, 0, 12, 1);
    vcdec_get_stats(d, &s);
    CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics && disables_out == 1 && s.format_changes == 1,
          "bigger format: %d, %d pictures, %d wrong, %d disables", r, ngot, wrong_pics, disables_out);
    close70(d, "bigger format");
    big_efch = 0;

    /* a format change part way (bigger), with pictures held by the caller:
       theirs in the old layout, their buffers grown before they go back */
    for (int late = 0; late < 2; late++) {
        big_efch_at = 3;
        rx_late = late;
        d = open70(0);
        setup_planes(0, 0);
        for (int k = 0; k < 6; k++) vcdec_send(d, aubuf, make_au(k, 100), (int64_t)mp4_pts[k] * 40000, 0, k ? 0 : 1);
        for (int i = 0; i < 30; i++) vcdec_poll(d);
        CHECK(efch_sent == 2 && nout == 0, "the second format change came with every buffer held (%d, %d)", efch_sent, nout);
        ngot = wrong_pics = 0;
        r = feed(d, 6, 12, 1);
        vcdec_get_stats(d, &s);
        CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics && s.format_changes == 2 && disables_out == 1,
              "format change part way (late %d): %d, %d pictures, %d wrong, %u changes", late, r, ngot, wrong_pics,
              s.format_changes);
        close70(d, "format change part way");
        big_efch_at = 0;
        rx_late = 0;
    }

    /* a format change during a flush: acted on, the decoder waiting for it */
    d = open70(0);
    setup_planes(0, 0);
    ngot = wrong_pics = 0;
    feed(d, 0, 6, 0);
    efch_on_flush = 1;
    r = vcdec_flush(d);
    vcdec_get_stats(d, &s);
    CHECK(r == VCDEC_OK && !awaiting_reformat && s.format_changes == 2 && nout == 3, "format change in a flush: %d, %u, %d",
          r, s.format_changes, nout);
    ngot = 0;
    r = feed(d, 6, 12, 1);
    CHECK(r == VCDEC_EOF && got_is(6, 6) && !wrong_pics, "after a format change in a flush: %d, %d", r, ngot);
    close70(d, "format change in a flush");

    /* closing after an error, with a step that fails: the rest still done */
    error_event = 1;
    d = open70(0);
    setup_planes(0, 0);
    feed(d, 0, 12, 1);
    fail_out_disable = 1;
    close70(d, "close with a failed step");
    CHECK(!fail_out_disable && destroyed == 1 && ports_on_at_destroy == 2,
          "the failed disable wasn't tried, or not everything after it (%d destroyed, ports on %d)", destroyed, ports_on_at_destroy);
    error_event = 0;

    crop_too_wide = 1;                        /* a visible width wider than the rows: refused, not read past */
    d = open70(0);
    setup_planes(0, 0);
    r = feed(d, 0, 12, 1);
    CHECK(r == VCDEC_ERROR && strstr(vcdec_error(d), "too small for 104x38"), "crop wider than the pitch: %d %s", r, vcdec_error(d));
    close70(d, "crop too wide");
    crop_too_wide = 0;

    efch_late = 1;                            /* buffers wanted before the format change */
    d = open70(0);
    setup_planes(0, 0);
    ngot = 0;
    r = feed(d, 0, 12, 1);
    CHECK(r == VCDEC_EOF && got_is(0, 12), "late format change: %d, %d", r, ngot);
    close70(d, "late EFCH");
    efch_late = 0;

    long_event = 1;                           /* an event's data by bulk */
    d = open70(0);
    setup_planes(0, 0);
    ngot = 0;
    r = feed(d, 0, 12, 1);
    CHECK(r == VCDEC_EOF && got_is(0, 12) && long_event == 2 && bulks_rx == 14, "long event: %d, %d, %d receives", r, ngot, bulks_rx);
    close70(d, "long event");
    long_event = 0;

    app_tests();

    printf(fails ? "vcdec_test: %d failures\n" : "vcdec_test: all passed\n", fails);
    return fails != 0;
}
