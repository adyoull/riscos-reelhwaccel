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

static int out_buffers;                    /* (vcdec_config.out_buffers for open70) */
static int pmp_svc_logged;
static void svc_log(void *h, const char *t) { (void)h; if (strstr(t, "isn't readable in USR mode")) pmp_svc_logged = 1; }
static char logbuf[4096];
static void cap_log(void *h, const char *t)
{
    (void)h;
    if (strlen(logbuf) + strlen(t) + 2 < sizeof logbuf) { strcat(logbuf, t); strcat(logbuf, "\n"); }
}
static vcdec *open70(unsigned flags)
{
    vcdec_config c;
    vcdec *d = NULL;
    int r;
    vcdec_config_init(&c);
    c.width = W; c.height = H; c.flags = flags; c.out_buffers = out_buffers;
    c.log = cap_log;
    logbuf[0] = 0;
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

/* ---- zero-copy (0.4.1) ---- */
static vcdec_hold *hq[16];
static uint8_t *hq_y[16];
static int hq_val[16], nhq, hold_max = 1, hold_refused, hold_copied;

/* a held picture as the fake made it */
static int held_right(uint8_t *const hp[3], const int hs[3], uint8_t val)
{
    for (int i = 0; i < 3; i++)
        for (int y = 0; y < (i ? CH : H); y++)
            for (int x = 0; x < (i ? CW : W); x++)
                if (hp[i][y * hs[i] + x] != (uint8_t)(val + i)) return 0;
    return 1;
}

/* every picture ready, held (up to hold_max at once: at that, the oldest
   released before the next is taken; each one still held checked
   unchanged); refused ones copied */
static int take_hold(vcdec *d)
{
    vcdec_picture p;
    uint8_t *hp[3];
    int hs[3], r;
    vcdec_hold *h;
    for (;;) {
        if ((r = vcdec_peek(d, &p)) != VCDEC_OK) return r;
        if (nhq == hold_max) {                   /* (the oldest given back first) */
            vcdec_release(hq[0]);
            memmove(hq, hq + 1, sizeof hq[0] * (size_t)(nhq - 1));
            memmove(hq_y, hq_y + 1, sizeof hq_y[0] * (size_t)(nhq - 1));
            memmove(hq_val, hq_val + 1, sizeof hq_val[0] * (size_t)(nhq - 1));
            nhq--;
        }
        r = vcdec_receive_hold(d, &p, hp, hs, &h);
        if (r == VCDEC_UNSUPPORTED) {
            hold_refused++;
            if ((r = vcdec_receive(d, &p, planes, strides)) != VCDEC_OK) return r;
            if (!picture_right(mp4_val((int)(p.pts / 40000)))) wrong_pics++;
            hold_copied++;
            if (ngot < 64) got[ngot++] = (int)(p.pts / 40000);
            continue;
        }
        if (r != VCDEC_OK) return r;
        {
            int f = (int)(p.pts / 40000);
            if (!h || p.width != W || p.height != H || !held_right(hp, hs, mp4_val(f))) wrong_pics++;
            if (ngot < 64) got[ngot++] = f;
            hq[nhq] = h; hq_y[nhq] = hp[0]; hq_val[nhq] = mp4_val(f); nhq++;
            for (int i = 0; i < nhq; i++)        /* (the ones still held: not written over) */
                if (hq_y[i][0] != (uint8_t)hq_val[i] || hq_y[i][W - 1] != (uint8_t)hq_val[i]) wrong_pics++;
        }
    }
}

static void release_all(void)
{
    for (int i = 0; i < nhq; i++) vcdec_release(hq[i]);
    nhq = 0;
}

static int feed_hold(vcdec *d, int from, int to, int eos)
{
    int k = from, r = VCDEC_OK, spins = 0;
    while (k < to && spins < 10000) {
        size_t n = make_au(k, 100);
        r = vcdec_send(d, aubuf, n, (int64_t)mp4_pts[k] * 40000, (int64_t)k * 40000, k % 6 == 0 ? VCDEC_KEYFRAME : 0);
        if (r == VCDEC_OK) k++;
        else if (r != VCDEC_AGAIN) return r;
        if ((r = take_hold(d)) < 0) return r;
        spins++;
    }
    if (!eos) return VCDEC_OK;
    while ((r = vcdec_send_eos(d)) == VCDEC_AGAIN && spins++ < 10000) take_hold(d);
    if (r != VCDEC_OK) return r;
    while ((r = take_hold(d)) == VCDEC_AGAIN && spins++ < 10000) {}
    return r;
}

static void hold_tests(void)
{
    vcdec *d;
    vcdec_stats s;
    int r;
    /* one held at a time, 3 buffers: every picture held, none copied */
    d = open70(0);
    setup_planes(0, 0);
    ngot = wrong_pics = hold_refused = hold_copied = nhq = 0; hold_max = 1;
    r = feed_hold(d, 0, 12, 1);
    vcdec_get_stats(d, &s);
    CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics && !hold_refused && s.holds == 12 && s.held_now == 1 &&
          s.pictures == 12 && pmp_invalidates >= 12, "hold one at a time: %d, %d pictures (%d wrong), %d refused, %u held, %u now",
          r, ngot, wrong_pics, hold_refused, s.holds, s.held_now);
    release_all();
    close70(d, "hold one");

    /* three held at once with 3 buffers: only one can be (2 left to the decoder), the rest copied */
    d = open70(0);
    setup_planes(0, 0);
    ngot = wrong_pics = hold_refused = hold_copied = nhq = 0; hold_max = 3;
    r = feed_hold(d, 0, 12, 1);
    vcdec_get_stats(d, &s);
    CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics && hold_copied == 11 && s.holds == 1 && s.held_now == 1,
          "hold three of three buffers: %d, %d pictures (%d wrong), %d copied, %u held", r, ngot, wrong_pics, hold_copied, s.holds);
    release_all();
    close70(d, "hold too many");

    /* six buffers, four held at once, receives late: all held; a seek before the end and after it with
       pictures held (their buffers not the decoder's), then close with them still held */
    out_buffers = 6;
    d = open70(0);
    out_buffers = 0;
    rx_late = 1;
    setup_planes(0, 0);
    ngot = wrong_pics = hold_refused = hold_copied = nhq = 0; hold_max = 4;
    r = feed_hold(d, 0, 6, 0);
    for (int i = 0; i < 30 && ngot < 3; i++) take_hold(d);
    CHECK(vcdec_flush(d) == VCDEC_OK && nhq >= 1, "hold: flush with %d held", nhq);
    ngot = 0;
    r = feed_hold(d, 6, 12, 1);
    CHECK(r == VCDEC_EOF && got_is(6, 6) && !wrong_pics && !hold_refused, "hold, seek before the end: %d, %d pictures (%d wrong), %d refused",
          r, ngot, wrong_pics, hold_refused);
    CHECK(vcdec_flush(d) == VCDEC_OK && recreated == 1, "hold: flush after the end (%d held)", nhq);
    ngot = 0;
    r = feed_hold(d, 6, 12, 1);
    vcdec_get_stats(d, &s);
    CHECK(r == VCDEC_EOF && got_is(6, 6) && !wrong_pics && !hold_refused && s.held_now == 4,
          "hold, seek after the end: %d, %d pictures (%d wrong), %d refused, %u held now", r, ngot, wrong_pics, hold_refused,
          s.held_now);
    rx_late = 0;
    vcdec_close(d);
    CHECK(pmp_live == 4 && freed == 0 && opens == closes, "closed with 4 held: %d pools left, stub freed %d", pmp_live, freed);
    for (int i = 0; i < nhq; i++)
        if (hq_y[i][0] != (uint8_t)hq_val[i]) wrong_pics++;
    CHECK(!wrong_pics, "pictures held past vcdec_close changed");
    release_all();
    mp4_mode = 0;
    cleaned("hold past close");

    /* PCI memory: nothing can be held; every picture copied */
    d = open70(VCDEC_OUT_PCI);
    setup_planes(0, 0);
    ngot = wrong_pics = hold_refused = hold_copied = nhq = 0; hold_max = 1;
    r = feed_hold(d, 0, 12, 1);
    CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics && hold_copied == 12, "hold from PCI memory: %d, %d copied", r, hold_copied);
    close70(d, "hold PCI");

    /* a release is what gives the buffer back: not released, the decoder runs out */
    d = open70(0);
    setup_planes(0, 0);
    {
        vcdec_picture p;
        uint8_t *hp[3];
        int hs[3];
        vcdec_hold *h = NULL, *h2 = NULL;
        for (int k = 0; k < 6; k++) vcdec_send(d, aubuf, make_au(k, 100), (int64_t)mp4_pts[k] * 40000, 0, k ? 0 : 1);
        for (int i = 0; i < 50 && (r = vcdec_receive_hold(d, &p, hp, hs, &h)) == VCDEC_AGAIN; i++) {}
        CHECK(r == VCDEC_OK && h && held_right(hp, hs, mp4_val(0)), "hold: first picture %d", r);
        for (int i = 0; i < 50 && (r = vcdec_receive_hold(d, &p, hp, hs, &h2)) == VCDEC_AGAIN; i++) {}
        CHECK(r == VCDEC_UNSUPPORTED && !h2, "hold: a second with 3 buffers refused (%d)", r);
        CHECK(vcdec_receive(d, &p, planes, strides) == VCDEC_OK && p.pts == 40000 && picture_right(mp4_val(1)),
              "hold: the refused one taken by copying");
        vcdec_release(h);
        vcdec_release(NULL);
        vcdec_get_stats(d, &s);
        CHECK(s.held_now == 0, "released: %u still held", s.held_now);
    }
    close70(d, "hold release");
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

static int app_move_next_run;                 /* the next VCDecTest run: every pool claim moves the page at &8000 */

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
    move_on_claim = app_move_next_run; app_move_next_run = 0;
    remove("/tmp/vcdec_test.out");
    *ret = probe_main(n, argv);
    mp4_mode = 0;
    f = fopen("/tmp/vcdec_test.out", "r");
    got_n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got_n] = 0;
    if (f) fclose(f);
    return buf;
}

/* VCDecTest with options in one string (split at spaces) */
static char *run_apps(int *ret, const char *opts)
{
    static char buf[16384], o[256];
    char *argv[24];
    int n = 0;
    FILE *f;
    size_t got_n;
    argv[n++] = "vcdectest"; argv[n++] = "-o"; argv[n++] = "/tmp/vcdec_test.out";
    snprintf(o, sizeof o, "%s", opts);
    for (char *t = strtok(o, " "); t && n < 18; t = strtok(NULL, " ")) argv[n++] = t;
    argv[n++] = "/tmp/mmaldecode_test.mp4"; argv[n++] = "/tmp/mmaldecode_test4.crc"; argv[n++] = "/tmp/mmaldecode_test4.sig";
    argv[n] = NULL;
    reset_fake();
    mp4_mode = 1;
    move_on_claim = app_move_next_run; app_move_next_run = 0;
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
    CHECK(strstr(o, "page at &8000") && strstr(o, ": not moved"), "VCDecTest: the page at &8000 not reported:\n%s", o);
    app_move_next_run = 1;                    /* (0.4.2) a moved page at &8000 is a failure, said */
    o = run_app(&ret, "-m", "pmp", NULL);
    CHECK(ret != 0 && strstr(o, "MOVED - ARMEABISupport") && strstr(o, "claiming pools moved the program's page at &8000 3 times") &&
          strstr(o, "Result: the program's page at &8000 moved"), "VCDecTest, the page at &8000 moved (%d):\n%s", ret, o);
    cleaned("app, page moved");
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

    /* 0.2: buffers, ways, memory, the copy timed */
    o = run_apps(&ret, "-m pmp -K");
    printf("%s", o);
    CHECK(ret == 0 && strstr(o, "a Physical Memory Pool, cacheable") && strstr(o, "RAM disc's dynamic area flags: &100180 (bit 20 set)") &&
          strstr(o, "(pmp, LDM 4 words)") && strstr(o, "(pmp, LDM 8 words)") && strstr(o, "(pmp, NEON 64 bytes)") &&
          strstr(o, "Result: OK") && pmp_invalidates >= 12 + 3 * 10 && !strstr(o, "isn't readable in USR mode"),
          "VCDecTest -m pmp -K (%d, %d cleans):\n%s", ret, pmp_invalidates, o);
    cleaned("app pmp");
    o = run_apps(&ret, "-m pmpu -c neon -B 6 -t -K");
    CHECK(ret == 0 && strstr(o, "Output buffers: 6; pictures copied out by neon") && strstr(o, "not cacheable") &&
          nout_max == 6 && !pmp_invalidates && strstr(o, "OK - timed"), "VCDecTest -m pmpu -c neon -B 6 (%d):\n%s", ret, o);
    cleaned("app pmpu");
    o = run_apps(&ret, "-m pci -c ldm4 -E");
    CHECK(ret == 0 && strstr(o, "by ldm4; they arrive in PCI_RAMAlloc memory") && !strstr(o, "RAM disc") && strstr(o, "Result: OK") &&
          strstr(o, "0 in pools, 3 in PCI memory") && !pmp_invalidates, "VCDecTest -m pci -c ldm4 -E (%d):\n%s", ret, o);
    cleaned("app pci");
    o = run_apps(&ret, "-t -K");
    printf("%s", o);
    CHECK(ret == 0 && strstr(o, "by ldm8; they arrive in a cacheable Physical Memory Pool if there is one") &&
          strstr(o, "RAM disc") && strstr(o, "3 in pools, 0 in PCI memory") && strstr(o, "Inside vcdec (cs in all): messages") &&
          strstr(o, "(pmp, NEON 64 bytes)") && strstr(o, "OK - timed"), "VCDecTest -t -K, the defaults (%d):\n%s", ret, o);
    cleaned("app defaults");
    no_pmp = 1;
    o = run_apps(&ret, "-t -K");
    CHECK(ret == 0 && strstr(o, "0 in pools, 3 in PCI memory") && strstr(o, "No picture pool") && strstr(o, "(pci, LDM 8 words)") &&
          strstr(o, "NEON isn't used for PCI") && strstr(o, "OK - timed"), "VCDecTest -t -K, no pools (%d):\n%s", ret, o);
    cleaned("app no pools");
    no_pmp = 0;
    o = run_apps(&ret, "-Z");
    CHECK(ret == 0 && strstr(o, "Zero-copy: pictures held") && strstr(o, "Zero-copy: 12 pictures held, 0 copied") &&
          strstr(o, "Result: OK - every picture exactly"), "VCDecTest -Z (%d):\n%s", ret, o);
    cleaned("app -Z");
    o = run_apps(&ret, "-H 3 -B 5 -E");
    printf("%s", o);
    CHECK(ret == 0 && strstr(o, "up to 3 at a time") && strstr(o, "Zero-copy: 18 pictures held, 0 copied") &&
          strstr(o, "3 pictures still held at vcdec_close") && strstr(o, "Result: OK"), "VCDecTest -H 3 -B 5 -E (%d):\n%s", ret, o);
    cleaned("app -H 3");
    o = run_apps(&ret, "-Z -m pci");
    CHECK(ret == 0 && strstr(o, "Zero-copy: 0 pictures held, 12 copied") && strstr(o, "Result: OK"), "VCDecTest -Z -m pci (%d):\n%s", ret, o);
    cleaned("app -Z pci");
    o = run_apps(&ret, "-t -Z -R");
    CHECK(ret == 0 && strstr(o, "each read once") && strstr(o, "12 pictures held, 0 copied instead (not holdable); all read") &&
          strstr(o, "OK - timed"), "VCDecTest -t -Z -R (%d):\n%s", ret, o);
    cleaned("app -Z -R");
    o = run_apps(&ret, "-H 17");
    CHECK(ret == 1 && !opens, "VCDecTest -H 17: refused (%d)", ret);
    o = run_apps(&ret, "-c fast");
    CHECK(ret == 1 && !opens, "VCDecTest -c fast: refused (%d)", ret);
    npci_blocks = 0;
}

/* vcdec_copy.S on its own: each way, in USR and "SVC" mode, every
   alignment, widths around the loop sizes; random bytes, so a word or a
   half put in the wrong place shows; nothing written past a row */
void vcdec_copy_rows(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride, int width, int rows, int mode);
static void copy_tests(void)
{
    static const int widths[] = { 0, 1, 3, 4, 7, 31, 32, 33, 63, 64, 65, 96, 127, 128, 129, 200, 1000 };
    static uint8_t src[4 * 1100 + 64], dst[4 * 1100 + 64];
    int bad = 0;
    srand(7);
    for (unsigned i = 0; i < sizeof src; i++) src[i] = (uint8_t)rand();
    for (int mode = 0; mode < 8; mode++) {
        if ((mode & 3) == 3) continue;
        for (int so = 0; so < 4; so++)
            for (int doff = 0; doff < 4; doff++)
                for (unsigned w = 0; w < sizeof widths / sizeof widths[0]; w++) {
                    int wd = widths[w], ss = 1040 + so, ds = 1036;
                    memset(dst, 0x5A, sizeof dst);
                    vcdec_copy_rows(dst + doff, ds, src + so, ss, wd, 3, mode);
                    for (int r = 0; r < 3; r++) {
                        if (memcmp(dst + doff + r * ds, src + so + r * ss, (size_t)wd)) bad++;
                        for (int x = wd; x < wd + 8; x++)
                            if (dst[doff + r * ds + x] != 0x5A) { bad++; break; }
                    }
                    if (doff && dst[doff - 1] != 0x5A) bad++;
                }
    }
    CHECK(bad == 0, "vcdec_copy_rows: %d wrong", bad);
}

int main(int argc, char **argv)
{
    vcdec *d;
    vcdec_config c;
    vcdec_picture p;
    vcdec_stats s;
    int r, before;

    if (argc > 1) return probe_main(argc, argv);   /* (run.sh's -x check of real MP4s) */

    copy_tests();
    hold_tests();

    /* ---- a whole decode ---- */
    whole(0, 0, 0, 0, "whole");
    whole(0, 1, 0, 0, "whole, receives late");
    whole(VCDEC_SYNC_RECEIVE, 1, 0, 0, "whole, receives waited for");
    whole(0, 1, 1, 3, "whole, unaligned planes");
    whole(0, 0, 2, 1, "whole, odd strides");

    /* ---- 0.4's defaults: a cached pool, LDM 8; PCI memory and LDM 4 asked for ---- */
    d = open70(0);
    setup_planes(0, 0);
    ngot = wrong_pics = 0;
    r = feed(d, 0, 12, 1);
    vcdec_get_stats(d, &s);
    CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics && s.pool_buffers == 3 && !s.pci_buffers && pmp_invalidates >= 12 &&
          strstr(logbuf, "copied out by LDM 8") && !strstr(logbuf, "No picture pool"),
          "the defaults: %d, %d pictures, %u in pools, %u PCI, %d cleans; log:\n%s", r, ngot, s.pool_buffers, s.pci_buffers,
          pmp_invalidates, logbuf);
    close70(d, "defaults");
    whole(VCDEC_OUT_PCI, 0, 0, 0, "PCI memory");
    CHECK(!pmp_live && !pmp_invalidates, "PCI memory asked for: a pool made (%d) or cleaned (%d)", pmp_live, pmp_invalidates);
    whole(VCDEC_OUT_PCI | VCDEC_COPY_LDM4, 1, 1, 3, "PCI memory, LDM 4, unaligned planes");
    whole(VCDEC_COPY_LDM4, 0, 2, 1, "a pool, LDM 4, odd strides");
    whole(VCDEC_OUT_UNCACHED, 0, 0, 0, "an uncached pool (falling back allowed)");
    CHECK(pmp_invalidates == 0, "an uncached pool cleaned (%d)", pmp_invalidates);

    /* no pool to be had: PCI memory instead, said; with VCDEC_OUT_PMP, a failure */
    for (int k = 0; k < 3; k++) {
        static const char *const what[3] = { "pools refused", "a scattered pool", "no Cache_CleanInvalidateRange" };
        vcdec_config cc;
        vcdec *dd = NULL;
        vcdec_config_init(&cc);
        reset_fake();
        mp4_mode = 1;
        no_pmp = k == 0; pmp_scattered = k == 1; no_armop = k == 2;
        cc.width = W; cc.height = H; cc.log = cap_log;
        logbuf[0] = 0;
        r = vcdec_open(&dd, &cc);
        CHECK(r == VCDEC_OK && strstr(logbuf, "No picture pool") && strstr(logbuf, "PCI memory instead") && !pmp_live,
              "%s: open %d (%s), %d pools left; log:\n%s", what[k], r, vcdec_open_error(), pmp_live, logbuf);
        if (dd) {
            setup_planes(0, 0);
            ngot = wrong_pics = 0;
            r = feed(dd, 0, 12, 1);
            vcdec_get_stats(dd, &s);
            CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics && s.pci_buffers == 3 && !s.pool_buffers && !pmp_live &&
                  !pmp_invalidates, "%s: PCI memory instead: %d, %d pictures (%d wrong), %u PCI", what[k], r, ngot, wrong_pics,
                  s.pci_buffers);
            vcdec_close(dd);
        }
        mp4_mode = 0;
        cleaned(what[k]);
        npci_blocks = 0;
        no_pmp = pmp_scattered = 0;
    }
    no_armop = 1;                             /* (no_pmp and pmp_scattered with VCDEC_OUT_PMP: below) */
    vcdec_config_init(&c);
    reset_fake();
    no_armop = 1;
    c.width = W; c.height = H; c.flags = VCDEC_OUT_PMP;
    r = vcdec_open(&d, &c);
    CHECK(r == VCDEC_ERROR && strstr(vcdec_open_error(), "OS_MMUControl") && !pmp_live && !pci_live,
          "no Cache_CleanInvalidateRange, a pool or nothing: %d %s", r, vcdec_open_error());
    npci_blocks = 0;
    no_armop = 0;

    /* pools refused part way (a bigger format): those buffers made again in PCI memory */
    big_efch_at = 3;
    d = open70(0);
    no_pmp = 1;
    setup_planes(0, 0);
    ngot = wrong_pics = 0;
    r = feed(d, 0, 12, 1);
    vcdec_get_stats(d, &s);
    CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics && s.pci_buffers == 3 && !s.pool_buffers &&
          strstr(logbuf, "PCI memory instead"), "pools refused part way: %d, %d pictures (%d wrong), %u pool %u PCI; log:\n%s",
          r, ngot, wrong_pics, s.pool_buffers, s.pci_buffers, logbuf);
    close70(d, "pools refused part way");
    no_pmp = 0;
    big_efch_at = 0;

    /* ---- the ways of copying, the number of buffers, the memory (0.3) ---- */
    whole(VCDEC_COPY_LDM8, 0, 0, 0, "LDM 8");
    whole(VCDEC_COPY_LDM8, 1, 1, 3, "LDM 8, unaligned planes");
    whole(VCDEC_COPY_NEON, 0, 0, 0, "NEON");
    whole(VCDEC_COPY_NEON, 1, 2, 1, "NEON, odd strides");
    for (int n = 1; n <= 8; n += n < 4 ? 2 : 4) {
        out_buffers = n;
        whole(0, n & 1, 0, 0, "output buffers");
        CHECK(nout_max == n, "%d output buffers asked for, the decoder had up to %d", n, nout_max);
        out_buffers = 0;
    }
    out_buffers = 16;                         /* (0.4.1: up to 16) */
    whole(0, 1, 0, 0, "16 output buffers");
    CHECK(nout_max == 16, "16 output buffers asked for, the decoder had up to %d", nout_max);
    out_buffers = 40;
    whole(0, 0, 0, 0, "40 output buffers asked for");
    CHECK(nout_max == 16, "40 output buffers asked for: the decoder had up to %d (16 the most)", nout_max);
    out_buffers = 0;
    whole(VCDEC_OUT_PMP, 0, 0, 0, "a cached pool");
    CHECK(pmp_invalidates >= 12, "a cached pool: %d cache cleans (12 pictures)", pmp_invalidates);
    whole(VCDEC_OUT_PMP, 1, 1, 3, "a cached pool, late, unaligned planes");
    whole(VCDEC_OUT_PMP | VCDEC_COPY_NEON, 1, 0, 0, "a cached pool, NEON");
    whole(VCDEC_OUT_PMP | VCDEC_COPY_LDM8, 0, 0, 0, "a cached pool, LDM 8");
    whole(VCDEC_OUT_PMP | VCDEC_OUT_UNCACHED, 0, 0, 0, "an uncached pool");
    CHECK(pmp_invalidates == 0, "an uncached pool cleaned (%d)", pmp_invalidates);
    {                                         /* a pool the kernel doesn't make user readable */
        vcdec_config cc;
        vcdec *dd = NULL;
        vcdec_config_init(&cc);
        reset_fake();
        mp4_mode = 1;
        pmp_privileged = 1;
        cc.width = W; cc.height = H; cc.flags = VCDEC_OUT_PMP | VCDEC_COPY_NEON;
        cc.log = svc_log;
        pmp_svc_logged = 0;
        CHECK(vcdec_open(&dd, &cc) == VCDEC_OK, "a privileged pool: open");
        setup_planes(0, 0);
        ngot = wrong_pics = 0;
        r = feed(dd, 0, 12, 1);
        CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics, "a pool not readable in USR mode: %d, %d, %d wrong", r, ngot,
              wrong_pics);
        CHECK(pmp_svc_logged, "a pool not readable in USR mode: not said");
        close70(dd, "privileged pool");
        pmp_privileged = 0;
    }
    out_buffers = 6;
    whole(VCDEC_OUT_PMP | VCDEC_COPY_NEON, 1, 2, 1, "a cached pool, 6 buffers, NEON, odd strides");
    out_buffers = 0;

    /* a pool: a format change part way (pools freed and made again bigger),
       a seek after the end; the copy timed */
    for (int pool = 0; pool < 2; pool++) {
        unsigned cs = 0;
        int k;
        big_efch_at = 3;
        d = open70(pool ? VCDEC_OUT_PMP : VCDEC_OUT_PCI);
        setup_planes(0, 0);
        for (k = 0; k < 6; k++) vcdec_send(d, aubuf, make_au(k, 100), (int64_t)mp4_pts[k] * 40000, 0, k ? 0 : 1);
        for (int i = 0; i < 30; i++) vcdec_poll(d);
        ngot = wrong_pics = 0;
        r = feed(d, 6, 12, 1);
        CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics, "format change part way (pool %d): %d, %d, %d wrong", pool, r, ngot,
              wrong_pics);
        CHECK(vcdec_copy_benchmark(d, VCDEC_COPY_NEON, 5, planes, strides, &cs) == VCDEC_OK && picture_right(mp4_val(11)),
              "the copy timed (pool %d)", pool);
        CHECK(vcdec_flush(d) == VCDEC_OK && recreated == 1, "flush after the end (pool %d)", pool);
        ngot = wrong_pics = 0;
        r = feed(d, 6, 12, 1);
        CHECK(r == VCDEC_EOF && got_is(6, 6) && !wrong_pics, "seek after the end (pool %d): %d, %d", pool, r, ngot);
        close70(d, "pool format change");
        big_efch_at = 0;
    }
    d = open70(0);
    {
        unsigned cs;
        setup_planes(0, 0);
        CHECK(vcdec_copy_benchmark(d, 0, 1, planes, strides, &cs) == VCDEC_EINVAL, "timing before a picture");
    }
    close70(d, "benchmark early");

    {                                         /* a pool ending at the last page below 1 GB: fine */
        vcdec_config cc;
        vcdec *dd = NULL;
        vcdec_config_init(&cc);
        reset_fake();
        mp4_mode = 1;
        pmp_top = 1;
        cc.width = W; cc.height = H; cc.flags = VCDEC_OUT_PMP;
        r = vcdec_open(&dd, &cc);
        CHECK(r == VCDEC_OK, "a pool at the top of the first 1 GB: %d %s", r, vcdec_open_error());
        if (dd) {
            setup_planes(0, 0);
            ngot = 0;
            CHECK(feed(dd, 0, 12, 1) == VCDEC_EOF && got_is(0, 12), "a pool at the top: decode");
            close70(dd, "pool at the top");
        }
        pmp_top = 0;
    }

    /* (0.4.2) the program's page at &8000 never among a pool's pages: below
       it, above it, or (a RISC OS ignoring OS_Memory 12's R4-R7) refused */
    for (int k = 0; k < 5; k++) {
        static const char *what[] = { "in the way", "near the top (only room below)", "in the way, an old RISC OS",
                                      "out of the way, an old RISC OS", "moved anyway (counted)" };
        vcdec_config cc;
        vcdec *dd = NULL;
        vcdec_stats st;
        vcdec_config_init(&cc);
        reset_fake();
        mp4_mode = 1;
        app_pn = k == 1 ? 0x3FEFE : k == 3 ? 0x100 : pmp_next_page + 1;
        old_os_12 = k == 2 || k == 3;
        move_on_claim = k == 4;
        cc.width = W; cc.height = H; cc.flags = VCDEC_OUT_PMP;
        r = vcdec_open(&dd, &cc);
        if (k == 2) {
            CHECK(r == VCDEC_ERROR && strstr(vcdec_open_error(), "page at &8000") && !pmp_live && !app_moves,
                  "the page at &8000 %s: %d %s (%d moves)", what[k], r, vcdec_open_error(), app_moves);
            if (dd) vcdec_close(dd);
            npci_blocks = 0;
            continue;
        }
        CHECK(r == VCDEC_OK, "the page at &8000 %s: %d %s", what[k], r, vcdec_open_error());
        if (!dd) continue;
        setup_planes(0, 0);
        ngot = 0;
        CHECK(feed(dd, 0, 12, 1) == VCDEC_EOF && got_is(0, 12), "the page at &8000 %s: decode", what[k]);
        vcdec_get_stats(dd, &st);
        CHECK(st.pool_buffers > 0 && rec_calls > 0, "the page at &8000 %s: %u pools", what[k], st.pool_buffers);
        CHECK(k == 4 ? app_moves > 0 && st.app_page_moves == (unsigned)app_moves : !app_moves && !st.app_page_moves,
              "the page at &8000 %s: moved %d times (vcdec says %u)", what[k], app_moves, st.app_page_moves);
        close70(dd, what[k]);
    }
    move_on_claim = old_os_12 = 0; app_pn = 0x100;

    /* pools refused, or not contiguous: said, nothing left */
    no_pmp = 1;
    vcdec_config_init(&c);
    reset_fake();
    c.width = W; c.height = H; c.flags = VCDEC_OUT_PMP;
    r = vcdec_open(&d, &c);
    CHECK(r == VCDEC_ERROR && strstr(vcdec_open_error(), "Physical Memory Pool") && !pmp_live && !pci_live, "no pools: %d %s",
          r, vcdec_open_error());
    npci_blocks = 0;
    no_pmp = 0;
    pmp_scattered = 1;
    reset_fake();
    r = vcdec_open(&d, &c);
    CHECK(r == VCDEC_ERROR && strstr(vcdec_open_error(), "contiguous") && !pmp_live && !pci_live, "a scattered pool: %d %s", r,
          vcdec_open_error());
    npci_blocks = 0;
    pmp_scattered = 0;
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

    /* an EOS with nothing before it: the end at once, the decoder not told;
       a flush then needs no new decoder */
    d = open70(0);
    setup_planes(0, 0);
    ngot = wrong_pics = 0;
    CHECK(vcdec_send_eos(d) == VCDEC_OK && vcdec_receive(d, &p, planes, strides) == VCDEC_EOF && !aus,
          "an empty stream's end");
    CHECK(vcdec_flush(d) == VCDEC_OK && !recreated && created == 1, "a flush after an empty stream: %d created again",
          recreated);
    r = feed(d, 0, 12, 1);
    CHECK(r == VCDEC_EOF && got_is(0, 12) && !wrong_pics, "a decode after an empty stream: %d, %d", r, ngot);
    close70(d, "empty stream");

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

    d = open70(0);
    rx_abort = 3;                             /* a receive aborted: an error, not a picture */
    setup_planes(0, 0);
    ngot = wrong_pics = 0;
    r = feed(d, 0, 12, 1);
    CHECK(r == VCDEC_ERROR && strstr(vcdec_error(d), "aborted") && ngot == 2 && !wrong_pics, "an aborted receive: %d, %d, %s",
          r, ngot, vcdec_error(d));
    close70(d, "aborted receive");
    rx_abort = 0;

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
