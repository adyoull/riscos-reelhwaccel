/*
 * hevcdec_test.c - hevcdec (hevcdec/, with Raspberry Pi's rpivid_h265.c)
 * and tools/hevctest against the fake HEVC block (fake_hevc.c).
 *
 *   hevcdec_test TRACE RAW WxH [TRACE2 RAW2 WxH2 ...]
 * Each TRACE comes from tools/hevctrace (FFmpeg's HEVC decoder) and RAW is
 * FFmpeg's decode of the same clip (I420, display order: 8-bit, or for a
 * 10-bit trace yuv420p10le). The fake
 * "decodes" a picture by writing FFmpeg's (found by its slices'
 * bitstream), so hevctest's check passes only if everything between -
 * the slices' bytes, the frames, the references, the 128-byte column
 * format and its conversion - is right.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hwhevcdec.h"
#include "fake_hevc.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int probe_main(int argc, char **argv);          /* tools/hevctest */

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t adler(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return b << 16 | a;
}

/* ---- the pictures, by their slices' bitstream ---- */
#define MAXP 512
static struct { uint32_t hash; const uint8_t *y, *u, *v; } map[MAXP];
static int nmap, map_w, map_h, map_bytes;
static uint8_t *raw;

static int picture(uint32_t hash, const void **y, const void **u, const void **v, int *w, int *h, int *bytes)
{
    for (int i = 0; i < nmap; i++)
        if (map[i].hash == hash) {
            *y = map[i].y; *u = map[i].u; *v = map[i].v; *w = map_w; *h = map_h; *bytes = map_bytes;
            return 0;
        }
    return -1;
}

/* Adler-32 of a w x h rectangle (bytes a row: wb) at p, rows stride apart */
static uint32_t adler_rect(const uint8_t *p, size_t wb, int h, size_t stride)
{
    uint32_t a = 1, b = 0;
    for (int y = 0; y < h; y++, p += stride)
        for (size_t i = 0; i < wb; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return b << 16 | a;
}

/* the trace's pictures matched to RAW's by their Adler-32s over the
   output window (RAW: whole pictures, the coded size, so the fake can
   write them as the block would; for an uncropped clip the window is the
   whole picture); the hash of each picture's slices' data (as phase 1 is
   given it) noted */
static int load(const char *trace, const char *rawname, int w, int h)
{
    FILE *f;
    long n, rn;
    uint8_t *d;
    size_t o = 28, fs, yn, cn;
    size_t s_sps, s_pps, s_sp, s_dec, s_sm;
    int matched = 0, pics = 0, bytes = 1;
    nmap = 0;
    map_w = w; map_h = h;
    free(raw);
    if (!(f = fopen(rawname, "rb"))) return -1;
    fseek(f, 0, SEEK_END); rn = ftell(f); fseek(f, 0, SEEK_SET);
    raw = malloc((size_t)rn);
    if (fread(raw, 1, (size_t)rn, f) != (size_t)rn) { fclose(f); return -1; }
    fclose(f);
    if (!(f = fopen(trace, "rb"))) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -1; }
    fclose(f);
    s_sps = rd32(d + 8); s_pps = rd32(d + 12); s_sp = rd32(d + 16); s_dec = rd32(d + 20); s_sm = rd32(d + 24);
    if (o + 48 <= (size_t)n && rd32(d + o + 20) > 8) bytes = 2;      /* (the first picture's depth) */
    map_bytes = bytes;
    yn = (size_t)w * h * bytes; cn = (size_t)((w + 1) / 2) * ((h + 1) / 2) * bytes; fs = yn + 2 * cn;
    while (o + 48 <= (size_t)n) {
        uint32_t nsl = rd32(d + o + 24), has_sm = rd32(d + o + 28), hash = 2166136261u, crc[3];
        int wl = (int)rd32(d + o + 32), wt = (int)rd32(d + o + 36), ww = (int)rd32(d + o + 40), wh = (int)rd32(d + o + 44);
        o += 48 + s_sps + s_pps + s_dec + (has_sm ? s_sm : 0);
        for (uint32_t i = 0; i < nsl; i++) {
            const uint8_t *sp = d + o;
            uint32_t bits = rd32(sp), off = rd32(sp + 4), len;
            o += s_sp;
            len = rd32(d + o); o += 4;
            hash = (hash ^ fake_hevc_hash(d + o + off, (bits + 7) / 8 - off)) * 16777619u;   /* (every slice's, as the fake's) */
            o += (len + 3) & ~3u;
        }
        for (int k = 0; k < 3; k++) { crc[k] = rd32(d + o); o += 4; }
        pics++;
        for (size_t k = 0; k * fs + fs <= (size_t)rn && nmap < MAXP; k++) {
            const uint8_t *y = raw + k * fs, *u = y + yn, *v = u + cn;
            size_t cs = (size_t)((w + 1) / 2) * bytes, ys = (size_t)w * bytes;
            size_t cwb = (size_t)((ww + 1) / 2) * bytes, coff = (size_t)(wt / 2) * cs + (size_t)(wl / 2) * bytes;
            if (adler_rect(y + (size_t)wt * ys + (size_t)wl * bytes, (size_t)ww * bytes, wh, ys) == crc[0] &&
                adler_rect(u + coff, cwb, (wh + 1) / 2, cs) == crc[1] && adler_rect(v + coff, cwb, (wh + 1) / 2, cs) == crc[2]) {
                map[nmap].hash = hash; map[nmap].y = y; map[nmap].u = u; map[nmap].v = v; nmap++;
                matched++;
                break;
            }
        }
    }
    free(d);
    printf("%s: %d pictures (%d-bit), %d matched in %s\n", trace, pics, bytes == 2 ? 10 : 8, matched, rawname);
    return matched == pics && pics ? 0 : -1;
}

static char outbuf[1 << 16];
static char *run_app(int *ret, const char *trace, const char *opts)
{
    char *argv[16], o[256];
    int n = 0;
    FILE *f;
    size_t got;
    argv[n++] = "hevctest"; argv[n++] = "-o"; argv[n++] = "/tmp/hevcdec_test.out";
    snprintf(o, sizeof o, "%s", opts ? opts : "");
    for (char *t = strtok(o, " "); t && n < 12; t = strtok(NULL, " ")) argv[n++] = t;
    argv[n++] = (char *)trace;
    argv[n] = NULL;
    remove("/tmp/hevcdec_test.out");
    *ret = probe_main(n, argv);
    f = fopen("/tmp/hevcdec_test.out", "r");
    got = f ? fread(outbuf, 1, sizeof outbuf - 1, f) : 0;
    outbuf[got] = 0;
    if (f) fclose(f);
    return outbuf;
}

static void cleaned(const char *what)
{
    CHECK(fake_hevc_live() == 0, "%s: %d buffers not freed", what, fake_hevc_live());
    CHECK(fake_hevc.opens == fake_hevc.closes && !fake_hevc.left, "%s: block opened %d, closed %d%s", what,
          fake_hevc.opens, fake_hevc.closes, fake_hevc.left ? ", memory left" : "");
    CHECK(fake_hevc.fails == 0, "%s: %d complaints from the fake block", what, fake_hevc.fails);
}

static void trace_tests(const char *trace, const char *rawname, const char *size)
{
    int w = 0, h = 0, ret;
    char *o;
    sscanf(size, "%dx%d", &w, &h);
    if (load(trace, rawname, w, h)) { CHECK(0, "%s: the pictures don't match %s", trace, rawname); return; }
    fake_hevc_set_pictures(picture);

    if (w >= 3840) {                            /* 4K: frames of 12-17 MB, so a few of them, and the main runs only */
        static const char *const opts[] = { "-f 6", "-p -f 6" };
        for (int k = 0; k < 2; k++) {
            fake_hevc_reset();
            fake_hevc.p2_ticks = k ? 4 : 1;
            o = run_app(&ret, trace, opts[k]);
            printf("%s", o);
            CHECK(ret == 0 && strstr(o, "Result: OK - every picture exactly") && !fake_hevc.ref_errors &&
                  !fake_hevc.unknown_pictures && fake_hevc.phase2s == nmap && (!k || fake_hevc.overlaps > 0),
                  "hevctest %s %s (%d): %d references wrong, %d unknown, %d phase 2s", opts[k], trace, ret,
                  fake_hevc.ref_errors, fake_hevc.unknown_pictures, fake_hevc.phase2s);
            cleaned(trace);
        }
        return;
    }

    fake_hevc_reset();
    o = run_app(&ret, trace, NULL);
    printf("%s", o);
    CHECK(ret == 0 && strstr(o, "Result: OK - every picture exactly") && !fake_hevc.ref_errors && !fake_hevc.factors_wrong &&
          strstr(o, "hevcdec: nothing written past any buffer's end") && strstr(o, "hevcdec: output frames cacheable (") &&
          fake_hevc.cached_allocs > 0 && fake_hevc.cache_ops > 0 && !fake_hevc.evictions &&
          !fake_hevc.unknown_pictures && fake_hevc.phase2s == nmap,
          "hevctest %s (%d): %d references wrong, %d pictures unknown, %d phase 2s", trace, ret, fake_hevc.ref_errors,
          fake_hevc.unknown_pictures, fake_hevc.phase2s);
    cleaned(trace);

    fake_hevc_reset();                          /* the fewest frames: a reference would be overwritten */
    fake_hevc.quiet = 1;
    o = run_app(&ret, trace, "-f 2 -n");
    CHECK(ret == 1 && (strstr(o, "no free frame") || fake_hevc.ref_errors), "hevctest -f 2 %s (%d):\n%s", trace, ret, o);
    fake_hevc.fails = 0;
    cleaned("-f 2");

    fake_hevc_reset();                          /* phase 1 runs out of PU buffer once: grown, run again */
    fake_hevc.p1_exhaust = 1;
    o = run_app(&ret, trace, "-c 3");
    CHECK(ret == 0 && strstr(o, "run again (buffers grown) 1 times") && strstr(o, "Result: OK"),
          "hevctest, PU exhausted once (%d):\n%s", ret, o);
    cleaned("PU exhausted");

    fake_hevc_reset();                          /* phase 2 never finishes: said, not hung */
    fake_hevc.p2_hang = 1;                      /* (then nothing more given to the block, its memory left) */
    o = run_app(&ret, trace, "-c 3 -n");
    CHECK(ret == 1 && strstr(o, "Phase 2 didn't finish") && strstr(o, "restart the machine before decoding again\n") &&
          fake_hevc.phase1s == 1 && fake_hevc.left && fake_hevc_live() > 0,
          "hevctest, phase 2 hangs (%d, %d phase 1s, left %d, %d buffers):\n%s", ret, fake_hevc.phase1s, fake_hevc.left,
          fake_hevc_live(), o);

    fake_hevc_reset();
    fake_hevc.p1_hang = 1;
    o = run_app(&ret, trace, "-c 2");
    CHECK(ret == 1 && strstr(o, "Phase 1 didn't finish") && fake_hevc.left, "hevctest, phase 1 hangs (%d):\n%s", ret, o);
    fake_hevc_reset();

    if (strstr(trace, "slices")) {              /* a picture's second slice refused: only that picture lost */
        fake_hevc_reset();
        int ndone = -1, nall = -1;
        const char *q;
        fake_hevc.quiet = 1;
        o = run_app(&ret, trace, "-n -x 2");
        q = strstr(o, " pictures decoded in");
        if (q) { while (q > o && q[-1] != '\n') q--; sscanf(q, "%d of %d", &ndone, &nall); }
        /* (the pictures that use it as their collocated reference are refused too, as by Linux's
           driver, up to the next IRAP picture; then all is well again) */
        CHECK(ret == 1 && strstr(o, "Picture 2 (poc") && strstr(o, "Slice 1:") && !strstr(o, "free decode env") &&
              nall == 12 && ndone >= 12 - 5, "hevctest, a slice refused (%d, %d of %d):\n%s", ret, ndone, nall, o);
        fake_hevc.fails = 0;
        cleaned("a slice refused");
    }

    if (strstr(trace, "odd")) {                 /* no scaling lists: hevcdec loads flat factors, as -s does */
        int plain, plain_nf;
        fake_hevc_reset();
        o = run_app(&ret, trace, "-n");
        plain = fake_hevc.scaling_writes; plain_nf = fake_hevc.scaling_not_flat;
        CHECK(ret == 0 && plain == 8 * 4064 / 4 && plain_nf == 0 && !fake_hevc.factors_wrong,
              "odd: %d scaling writes, %d not flat, %d pictures with stale factors:\n%s", plain, plain_nf,
              fake_hevc.factors_wrong, o);
        fake_hevc_reset();
        o = run_app(&ret, trace, "-s");
        CHECK(ret == 0 && strstr(o, "Flat scaling lists given to 8 pictures (-s)") &&
              strstr(o, "Result: OK - every picture exactly") &&
              fake_hevc.scaling_writes == 8 * 4064 / 4 && fake_hevc.scaling_not_flat == 0,
              "hevctest -s (%d, %d then %d scaling writes, %d not flat):\n%s", ret, plain, fake_hevc.scaling_writes,
              fake_hevc.scaling_not_flat, o);
        cleaned("-s");
        fake_hevc_reset();                      /* the flag without the lists: refused, not a crash */
        o = run_app(&ret, trace, "-z");
        CHECK(ret == 1 && strstr(o, "Scaling lists enabled but none given") && fake_hevc.phase1s == 0,
              "scaling lists enabled without lists (%d):\n%s", ret, o);
        cleaned("-z");
    }
    if (strstr(trace, "nowpp")) {               /* -s leaves a clip's own lists alone */
        fake_hevc_reset();
        o = run_app(&ret, trace, "-s");
        CHECK(ret == 0 && strstr(o, "Flat scaling lists given to 0 pictures") && fake_hevc.scaling_writes > 0 &&
              fake_hevc.scaling_not_flat > 0, "hevctest -s, nowpp (%d):\n%s", ret, o);
        cleaned("-s nowpp");
    }

    fake_hevc_reset();                          /* frames not cacheable: asked for (-u), or no cache maintenance */
    o = run_app(&ret, trace, "-u");
    CHECK(ret == 0 && strstr(o, "Result: OK - every picture exactly") && strstr(o, "output frames not cacheable (-u)") &&
          fake_hevc.cached_allocs == 0, "hevctest -u (%d, %d cached):\n%s", ret, fake_hevc.cached_allocs, o);
    cleaned("-u");
    fake_hevc_reset();
    fake_hevc.no_cache = 1;
    o = run_app(&ret, trace, "");
    CHECK(ret == 0 && strstr(o, "Result: OK - every picture exactly") &&
          strstr(o, "output frames not cacheable (no cache maintenance)") && fake_hevc.cached_allocs == 0,
          "no cache maintenance (%d):\n%s", ret, o);
    cleaned("no cache maintenance");

    fake_hevc_reset();                          /* the block writing past a frame: caught by its guard, reported */
    fake_hevc.overrun = 2048;
    o = run_app(&ret, trace, "-c 3");
    CHECK(ret == 1 && strstr(o, "hevcdec: the block wrote past the end of 3 buffers, at most 2048 bytes past an output frame") &&
          strstr(o, "Result: FAILED") && strstr(o, "3 checked against FFmpeg: 0 wrong"),
          "a block writing past the frames (%d):\n%s", ret, o);
    cleaned("overrun");

    fake_hevc_reset();                          /* the stream's depth isn't the decoder's: refused, said */
    fake_hevc.quiet = 1;
    o = run_app(&ret, trace, map_bytes == 2 ? "-c 2 -D 8" : "-c 2 -D 10");
    CHECK(ret == 1 && strstr(o, map_bytes == 2 ? "10-bit (chroma 10-bit): this decoder is for 8-bit" :
                                                 "8-bit (chroma 8-bit): this decoder is for 10-bit") &&
          fake_hevc.phase1s == 0, "hevctest, %d-bit trace, the other depth's decoder (%d):\n%s", map_bytes == 2 ? 10 : 8,
          ret, o);
    fake_hevc.fails = 0;
    cleaned("other depth");

    if (!strstr(trace, "crop")) {
    fake_hevc_reset();                          /* -d: the pictures as decoded, in decoding order */
    remove("/tmp/hevcdec_test.dump");
    o = run_app(&ret, trace, "-c 3 -d /tmp/hevcdec_test.dump");
    {
        FILE *f = fopen("/tmp/hevcdec_test.dump", "rb");
        size_t fs = ((size_t)w * h + 2 * (size_t)((w + 1) / 2) * ((h + 1) / 2)) * map_bytes, got = 0, ok = 0;
        uint8_t *b = malloc(3 * fs + 1);
        if (f && b) { got = fread(b, 1, 3 * fs + 1, f); fclose(f); }
        if (got == 3 * fs && nmap) ok = !memcmp(b, map[0].y, (size_t)w * h * map_bytes);   /* picture 1: FFmpeg's first */
        CHECK(ret == 0 && got == 3 * fs && ok, "hevctest -d: %zu bytes (%zu a picture), first picture %s:\n%s", got, fs,
              ok ? "right" : "wrong", o);
        free(b);
        remove("/tmp/hevcdec_test.dump");
    }
    cleaned("-d");
    }

    /* pipelined (-p): pictures given without waiting, phase 1 of one
       alongside phase 2 of another (phase 2 made slow here) */
    fake_hevc_reset();
    fake_hevc.p2_ticks = 6;
    o = run_app(&ret, trace, "-p");
    CHECK(ret == 0 && strstr(o, "Result: OK - every picture exactly") && strstr(o, "(pipelined: -p)") &&
          !fake_hevc.ref_errors && !fake_hevc.unknown_pictures && fake_hevc.phase2s == nmap && fake_hevc.overlaps > 0 &&
          !fake_hevc.evictions, "hevctest -p %s (%d): %d references wrong, %d unknown, %d phase 2s, %d overlaps:\n%s",
          trace, ret, fake_hevc.ref_errors, fake_hevc.unknown_pictures, fake_hevc.phase2s, fake_hevc.overlaps, o);
    cleaned("-p");
    fake_hevc_reset();                          /* five pictures given ahead, phase 2 slow: more pictures */
    fake_hevc.p2_ticks = 9;                     /* through phase 1 than PU/coefficient buffer sets */
    o = run_app(&ret, trace, "-P 5");
    CHECK(ret == 0 && strstr(o, "Result: OK - every picture exactly") && !fake_hevc.ref_errors &&
          fake_hevc.phase2s == nmap && fake_hevc.overlaps > 0,
          "hevctest -P 5 %s (%d): %d references wrong, %d phase 2s, %d overlaps:\n%s", trace, ret, fake_hevc.ref_errors,
          fake_hevc.phase2s, fake_hevc.overlaps, o);
    cleaned("-P 5");
    fake_hevc_reset();                          /* converting a picture still being decoded waits for it */
    fake_hevc.p2_ticks = 6;
    o = run_app(&ret, trace, "-p -w");
    CHECK(ret == 0 && strstr(o, "Result: OK - every picture exactly"), "hevctest -p -w (%d):\n%s", ret, o);
    cleaned("-p -w");
    fake_hevc_reset();                          /* and phase 1 slow */
    fake_hevc.p1_ticks = 5;
    fake_hevc.p2_ticks = 2;
    o = run_app(&ret, trace, "-p -t");
    CHECK(ret == 0 && strstr(o, "OK - timed") && fake_hevc.phase2s == nmap, "hevctest -p -t (%d, %d phase 2s):\n%s", ret,
          fake_hevc.phase2s, o);
    cleaned("-p -t");
    fake_hevc_reset();                          /* phase 1 runs out of buffer once, pipelined */
    fake_hevc.p1_exhaust = 1;
    fake_hevc.p2_ticks = 4;
    o = run_app(&ret, trace, "-p -c 5");
    CHECK(ret == 0 && strstr(o, "run again (buffers grown) 1 times") && strstr(o, "Result: OK"),
          "hevctest -p, PU exhausted once (%d):\n%s", ret, o);
    cleaned("-p PU exhausted");
    fake_hevc_reset();                          /* phase 2 never finishes, pipelined: said, memory left */
    fake_hevc.p2_hang = 1;
    o = run_app(&ret, trace, "-p -c 4 -n");
    CHECK(ret == 1 && strstr(o, "Phase 2 didn't finish") && fake_hevc.left && fake_hevc_live() > 0,
          "hevctest -p, phase 2 hangs (%d, left %d):\n%s", ret, fake_hevc.left, o);
    fake_hevc_reset();
    fake_hevc.p2_ticks = 6;                     /* blocking, with slow phases: as before */
    fake_hevc.p1_ticks = 3;
    o = run_app(&ret, trace, "-c 6");
    CHECK(ret == 0 && strstr(o, "Result: OK - every picture exactly") && fake_hevc.overlaps == 0,
          "hevctest, slow phases, not pipelined (%d, %d overlaps):\n%s", ret, fake_hevc.overlaps, o);
    cleaned("slow phases");
    if (strstr(trace, "slices")) {              /* a slice refused, pipelined */
        int ndone = -1, nall = -1;
        const char *q;
        fake_hevc_reset();
        fake_hevc.quiet = 1;
        fake_hevc.p2_ticks = 4;
        o = run_app(&ret, trace, "-p -n -x 2");
        q = strstr(o, " pictures decoded in");
        if (q) { while (q > o && q[-1] != '\n') q--; sscanf(q, "%d of %d", &ndone, &nall); }
        CHECK(ret == 1 && strstr(o, "Picture 2 (poc") && strstr(o, "Slice 1:") && !strstr(o, "free decode env") &&
              nall == 12 && ndone >= 12 - 5, "hevctest -p, a slice refused (%d, %d of %d):\n%s", ret, ndone, nall, o);
        fake_hevc.fails = 0;
        cleaned("-p a slice refused");
        /* (0.1.10) and with phase 1 slow, so two pictures wait in phase 1 when one is refused: the
           refused one takes no slice buffer, so the next can't take one phase 1 has still to read */
        fake_hevc_reset();
        fake_hevc.quiet = 1;
        fake_hevc.p1_ticks = 40;
        fake_hevc.p2_ticks = 2;
        o = run_app(&ret, trace, "-P 4 -n -x 4");
        CHECK(ret == 1 && strstr(o, "Picture 4 (poc 1, 4 slices): Slice 1:") && !strstr(o, "WRONG") &&
              strstr(o, "11 checked against FFmpeg: 1 wrong"),
              "hevctest -P 4, a slice refused with phase 1 slow (%d; the refused picture's slice buffer taken from "
              "a picture still in phase 1?):\n%s", ret, o);
        fake_hevc.fails = 0;
        cleaned("-p a slice refused, phase 1 slow");
    }

    {                                           /* (0.1.8) the block kept busy while a picture is converted: */
        unsigned waits = 99, cs = 0;            /* the next one decoded meanwhile, so hardly ever waited for */
        const char *q;
        fake_hevc_reset();
        fake_hevc.p1_ticks = 2;
        fake_hevc.p2_ticks = 3;
        o = run_app(&ret, trace, "-p");
        if ((q = strstr(o, "the program waited for the block "))) sscanf(q, "the program waited for the block %u cs (%u times)", &cs, &waits);
        CHECK(ret == 0 && strstr(o, "Result: OK - every picture exactly") && waits <= 2 && fake_hevc.overlaps > 0,
              "hevctest -p %s, phases noticed while converting: waited %u times (%d pictures):\n%s", trace, waits, nmap, o);
        cleaned("-p busy while converting");
    }

    fake_hevc_reset();                          /* (0.1.8) -K: every way of converting gives the same pictures */
    o = run_app(&ret, trace, "-K -c 4");
    {
        int lines = 0;
        for (const char *q = o; (q = strstr(q, " ms a picture")); q++) lines++;
        CHECK(ret == 0 && strstr(o, "Result: OK - every picture exactly") && !strstr(o, "WRONG") &&
              lines == (map_bytes == 2 ? 8 : 3) + 1 && strstr(o, "halved to 8-bit") && strstr(o, "column by column (the default)") && strstr(o, "row by row") &&
              strstr(o, "column by column, preloading") && (map_bytes == 1 || strstr(o, "through a row buffer")),
              "hevctest -K %s (%d, %d ways timed):\n%s", trace, ret, lines, o);
    }
    cleaned("-K");

    fake_hevc_reset();                          /* -q: starts and stops, the block untouched */
    o = run_app(&ret, trace, "-q");
    CHECK(ret == 0 && strstr(o, "started and wrote this (-q") && !strstr(o, "Trace:") && fake_hevc.opens == 0,
          "hevctest -q (%d, %d opens):\n%s", ret, fake_hevc.opens, o);

    fake_hevc_reset();
    o = run_app(&ret, trace, "-t");
    CHECK(ret == 0 && strstr(o, "OK - timed"), "hevctest -t (%d):\n%s", ret, o);
    cleaned("-t");
}

static char logged[4096];
static void log_to(void *h, const char *t)
{
    (void)h;
    if (strlen(logged) + strlen(t) + 2 < sizeof logged) { strcat(logged, t); strcat(logged, "\n"); }
}

int main(int argc, char **argv)
{
    hevcdec *d;
    hevcdec_config c;
    int r, ret;

    /* refusals: not a Pi 4, sizes, depth, no memory */
    fake_hevc_reset();
    fake_hevc.no_block = 1;
    hevcdec_config_init(&c);
    c.width = 352; c.height = 288;
    CHECK(hevcdec_open(&d, &c) == HEVCDEC_ERROR && strstr(hevcdec_open_error(), "can't be mapped"), "no block: %s",
          hevcdec_open_error());
    fake_hevc_reset();
    c.width = 8192;
    CHECK(hevcdec_open(&d, &c) == HEVCDEC_UNSUPPORTED, "8192 wide");
    c.width = 352; c.bit_depth = 12;
    CHECK(hevcdec_open(&d, &c) == HEVCDEC_UNSUPPORTED && strstr(hevcdec_open_error(), "12-bit"), "12-bit: %s",
          hevcdec_open_error());
    c.bit_depth = 10;                           /* (0.1.7) 10-bit: frames 3 samples a word, 96 a column */
    c.log = log_to; logged[0] = 0;
    r = hevcdec_open(&d, &c);
    c.log = NULL;
    CHECK(r == HEVCDEC_OK, "10-bit: %s", hevcdec_open_error());
    if (r == HEVCDEC_OK) {
        /* 352 wide: 4 columns of 96 samples (384); 288 high: columns of 432 rows; 4 * 128 * 432 bytes */
        CHECK(strstr(logged, "frames 384x288 (10-bit NV12, 3 samples a word, 128-byte columns of 432 lines), 221184 bytes"),
              "10-bit frames: %s", logged);
        CHECK(hevcdec_frame_new(d) != NULL, "a 10-bit frame");
        hevcdec_close(d);
        d = NULL;
    }
    cleaned("10-bit open");
    fake_hevc_reset();
    c.bit_depth = 8;
    r = hevcdec_open(&d, &c);
    if (r == HEVCDEC_OK) {                      /* 16-bit samples: a 10-bit decoder's only */
        uint16_t buf[8], *p16[3] = { buf, buf, buf };
        int st[3] = { 0, 0, 0 };
        hevcdec_frame *f = hevcdec_frame_new(d);
        CHECK(f && hevcdec_frame_to_i420_16(d, f, p16, st, 0, 0, 2, 2) == HEVCDEC_UNSUPPORTED, "16-bit from an 8-bit decoder");
        if (f) {                                /* (0.1.10) windows checked (the frame: 384x288) */
            static uint8_t pl[3][384 * 288];
            uint8_t *p8[3] = { pl[0], pl[1], pl[2] };
            int s8[3] = { 384, 192, 192 };
            CHECK(hevcdec_frame_to_i420(d, f, p8, s8, 0, 0, 384, 288) == HEVCDEC_OK, "the whole frame: %s", hevcdec_error(d));
            CHECK(hevcdec_frame_to_i420(d, f, p8, s8, 2, 0, 384, 2) == HEVCDEC_UNSUPPORTED, "converting past the right");
            CHECK(hevcdec_frame_to_i420(d, f, p8, s8, 0, 2, 2, 288) == HEVCDEC_UNSUPPORTED, "converting past the bottom");
            CHECK(hevcdec_frame_to_i420(d, f, p8, s8, -2, 0, 2, 2) == HEVCDEC_UNSUPPORTED, "converting from x -2");
            CHECK(hevcdec_frame_to_i420(d, f, p8, s8, 1, 0, 2, 2) == HEVCDEC_UNSUPPORTED, "converting from an odd x");
            CHECK(hevcdec_frame_to_i420_half(d, f, p8, s8, 4, 0, 0x7FFFFFFF, 2) == HEVCDEC_UNSUPPORTED, "halving, w huge");
            CHECK(hevcdec_frame_to_i420_half(d, f, p8, s8, 0, 2, 2, 144) == HEVCDEC_UNSUPPORTED, "halving past the bottom");
            CHECK(hevcdec_frame_to_i420_half(d, f, p8, s8, 0, 0, 192, 144) == HEVCDEC_OK, "halving the whole frame");
        }
        {                                       /* (0.1.10) pictures rpivid would index past its arrays with */
            static struct v4l2_ctrl_hevc_sps sps;
            static struct v4l2_ctrl_hevc_pps pps;
            static struct v4l2_ctrl_hevc_decode_params dec;
            static struct v4l2_ctrl_hevc_slice_params sp;
            static uint8_t data[64];
            hevcdec_slice sl = { &sp, data, sizeof data };
            hevcdec_picture pic = { &sps, &pps, &dec, NULL, 1, &sl };
            sps.chroma_format_idc = 1;
            sps.pic_width_in_luma_samples = 352;
            sps.pic_height_in_luma_samples = 288;
            dec.num_active_dpb_entries = 17;
            CHECK(f && hevcdec_decode(d, &pic, f, 1) == HEVCDEC_ERROR && strstr(hevcdec_error(d), "17 references"),
                  "17 DPB entries: %s", hevcdec_error(d));
            dec.num_active_dpb_entries = 2;
            sp.slice_type = V4L2_HEVC_SLICE_TYPE_P;
            sp.num_ref_idx_l0_active_minus1 = 16;
            CHECK(f && hevcdec_decode(d, &pic, f, 1) == HEVCDEC_ERROR && strstr(hevcdec_error(d), "17 and 1 references"),
                  "17 L0 references: %s", hevcdec_error(d));
            sp.num_ref_idx_l0_active_minus1 = 1;
            sp.ref_idx_l0[1] = 2;
            CHECK(f && hevcdec_decode(d, &pic, f, 1) == HEVCDEC_ERROR && strstr(hevcdec_error(d), "not among the 2"),
                  "a reference past the DPB: %s", hevcdec_error(d));
            sp.ref_idx_l0[1] = 1;
            sp.bit_size = 8 * 65;
            CHECK(f && hevcdec_decode(d, &pic, f, 1) == HEVCDEC_ERROR && strstr(hevcdec_error(d), "520 bits in 64 bytes"),
                  "a slice longer than its data: %s", hevcdec_error(d));
        }
        hevcdec_close(d);
        d = NULL;
    }
    cleaned("16-bit refused");
    fake_hevc_reset();
    fake_hevc.no_memory = 1;
    r = hevcdec_open(&d, &c);
    CHECK(r == HEVCDEC_ERROR && !d, "no memory: %d %s", r, hevcdec_open_error());
    fake_hevc.no_memory = 0;
    cleaned("no memory");
    fake_hevc_reset();
    r = hevcdec_open(&d, &c);
    CHECK(r == HEVCDEC_OK && d, "open: %s", hevcdec_open_error());
    {
        hevcdec *d2;
        CHECK(hevcdec_open(&d2, &c) == HEVCDEC_ERROR && !d2 && strstr(hevcdec_open_error(), "in use"), "opened twice");
    }
    if (r == HEVCDEC_OK) {
        hevcdec_frame *f = NULL;
        for (int i = 0; i < 40 && (f = hevcdec_frame_new(d)); i++) {}
        CHECK(!f && strstr(hevcdec_error(d), "More than"), "frames without end");
    }
    hevcdec_close(d);
    cleaned("open/close");
    {
        char *o = run_app(&ret, "/nonexistent", NULL);
        CHECK(ret == 1 && strstr(o, "Can't open"), "hevctest, no trace");
        o = run_app(&ret, "/tmp/hevcdec_test.out", NULL);
        CHECK(ret == 1, "hevctest, not a trace");
    }

    for (int i = 1; i + 2 < argc; i += 3) trace_tests(argv[i], argv[i + 1], argv[i + 2]);
    if (argc < 4) printf("hevcdec_test: no traces given (only the refusals tested)\n");

    printf("hevcdec_test: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
