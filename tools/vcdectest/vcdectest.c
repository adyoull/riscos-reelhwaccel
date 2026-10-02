/*
 * vcdectest - the vcdec library (vcdec/) on the Pi: an H.264 MP4 decoded by
 * the VideoCore as a player would drive it, every picture checked.
 *
 * Each turn of the loop is what a Wimp player does on a null event: send
 * access units until vcdec says the input is full (VCDEC_AGAIN), then take
 * every picture that's ready. The pictures are copied into one packed I420
 * picture (the visible size, Y then U then V, as FFmpeg's -f framecrc), so
 * its Adler-32 is FFmpeg's checksum; each is matched to FFmpeg's picture
 * by pts. The VideoCore's chroma can be 1 lower than FFmpeg's here and
 * there: block means within 1 count as "close" (with the .sig file).
 *
 *   vcdectest [-o file] [-v] [-n] [-S] [-g] [-s N | -E] [-t] [-B n] [-c way] [-m memory] [-K] [-Z [-H n] [-R]]
 *             stream.mp4 expected.crc [expected.sig]
 *      -o file   also add the report to file
 *      -v        every MMAL message, and what vcdec does
 *      -n        don't stop at the first wrong picture
 *      -S        wait for each picture's bulk transfer as it arrives (as
 *                MMALDecode did), not later
 *      -g        open even if gpu_mem looks too small
 *      -s N      after N pictures, vcdec_flush, and on from the last
 *                keyframe (a seek before the end: the EOS not yet sent)
 *      -E        decode to the end (EOF), then vcdec_flush (the decoder is
 *                created again) and on from the last keyframe to the end
 *                again (a seek after the end)
 *      -t        time, don't check the pictures
 *      -B n      n output buffers (vcdec's default 3; up to 8)
 *      -c way    copying the pictures out: ldm8 (vcdec's default), ldm4 or neon
 *      -m memory where the pictures arrive: auto (vcdec's default: a cacheable
 *                Physical Memory Pool, or PCI memory if there's none), pci
 *                (PCI_RAMAlloc), pmp (a cacheable pool, nothing else) or
 *                pmpu (a pool not cacheable)
 *      -K        after the decode, one picture's copy timed each way
 *      -Z        zero-copy: pictures held in vcdec's buffers (vcdec_receive_hold),
 *                not copied; checked from there; released at once
 *      -H n      with -Z: up to n pictures held at a time (a player's queue);
 *                the oldest released first, the rest after vcdec_close
 *      -R        with -Z -t: every byte of each held picture read once (as a
 *                player showing it would)
 *   vcdectest -x out stream.mp4   (host check) the Annex B stream as it
 *      would be sent, to out, and each sample's pts, dts and key flag to
 *      out.pts
 *
 * The MP4 reader is MMALDecode 0.17's (tools/mmaldecode).
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "vcdec.h"

#define OS_ReadMonotonicTime 0x42

static FILE *out2;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (out2) {
        va_start(ap, fmt);
        vfprintf(out2, fmt, ap);
        va_end(ap);
    }
}

#ifdef PROBE_TEST
_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r);   /* the host tests' fake RISC OS */
#else
static _kernel_oserror *probe_swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }
#endif

static uint32_t now_cs(void)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof r);
    probe_swi(OS_ReadMonotonicTime, &r);
    return (uint32_t)r.r[0];
}

static uint32_t adler(uint32_t a, const uint8_t *p, size_t n)
{
    uint32_t s1 = a & 0xFFFF, s2 = a >> 16;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { s1 += *p++; s2 += s1; }
        s1 %= 65521; s2 %= 65521;
    }
    return s2 << 16 | s1;
}

/* ---- MP4 input ---- */

/* An MP4's H.264 track, as samples (access units) in decode order, with
   their times in microseconds from the start of the presentation (the
   edit list's media time taken off, as FFmpeg's framecrc of the file). */
typedef struct { uint32_t off, size; int64_t pts, dts; int key; } sample_t;
static sample_t *samples;
static int nsamples, nal_size;
static uint8_t ps[1024];                   /* the avcC's SPS and PPS, as Annex B */
static uint32_t ps_len, mp4_w, mp4_h;

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* the first box of type t in [p, end): its contents, and their end in
   *cend; NULL if there's none (or the boxes don't add up) */
static const uint8_t *box(const uint8_t *p, const uint8_t *end, const char *t, const uint8_t **cend)
{
    while (p && end - p >= 8) {
        uint64_t sz = be32(p);
        uint32_t hdr = 8;
        if (sz == 1) {
            if (end - p < 16) return NULL;
            sz = be64(p + 8);
            hdr = 16;
        } else if (sz == 0) {
            sz = (uint64_t)(end - p);
        }
        if (sz < hdr || sz > (uint64_t)(end - p)) return NULL;
        if (!memcmp(p + 4, t, 4)) { *cend = p + sz; return p + hdr; }
        p += sz;
    }
    return NULL;
}

/* a full box's table: its entry count, the entries in *e, each w bytes
   (after skip bytes); -1 if they don't fit */
static long table(const uint8_t *c, const uint8_t *ce, uint32_t skip, uint32_t w, const uint8_t **e)
{
    uint32_t n;
    if (!c || ce - c < (long)(8 + skip)) return -1;
    n = be32(c + 4 + skip);
    *e = c + 8 + skip;
    if (w && n > (uint32_t)(ce - *e) / w) return -1;
    return (long)n;
}

static int64_t to_us(int64_t t, uint32_t timescale)
{
    int64_t ts = (int64_t)timescale;        /* (in two parts: t * 1000000 could overflow) */
    return t / ts * 1000000 + t % ts * 1000000 / ts;
}

static int parse_mp4(const uint8_t *f, uint32_t n)
{
    const uint8_t *me, *moov = box(f, f + n, "moov", &me), *p, *te, *trak;
    if (!moov) { say("No moov box: not an MP4 this can read\n"); return -1; }
    for (p = moov; (trak = box(p, me, "trak", &te)) != NULL; p = te) {
        const uint8_t *de, *ne, *se, *xe, *he, *ee, *ae, *mdia, *mdhd, *minf, *stbl, *stsd, *entry, *avcc, *edts, *elst;
        const uint8_t *e_stts, *e_ctts = NULL, *e_stss = NULL, *e_stsc, *e_co, *e_sz, *c;
        const uint8_t *ce;
        long n_stts, n_ctts = 0, n_stss = -1, n_stsc, n_co, n_sz;
        uint32_t timescale, fixed_size, i, k;
        int co64 = 0;
        int64_t media_time = 0, t;
        if (!(mdia = box(trak, te, "mdia", &de)) || !(mdhd = box(mdia, de, "mdhd", &he)) ||
            !(minf = box(mdia, de, "minf", &ne)) || !(stbl = box(minf, ne, "stbl", &se)) ||
            !(stsd = box(stbl, se, "stsd", &xe)) || xe - stsd < 16)
            continue;
        entry = stsd + 8;
        if (memcmp(entry + 4, "avc1", 4) && memcmp(entry + 4, "avc3", 4)) continue;
        if (be32(entry) > (uint32_t)(xe - entry) || be32(entry) < 8 + 78) { say("A broken avc1 box\n"); return -1; }
        mp4_w = be16(entry + 8 + 24);
        mp4_h = be16(entry + 8 + 26);
        if (!(avcc = box(entry + 8 + 78, entry + be32(entry), "avcC", &ae)) || ae - avcc < 7) {
            say("No avcC box (the H.264 decoder configuration)\n");
            return -1;
        }
        nal_size = (avcc[4] & 3) + 1;
        ps_len = 0;
        c = avcc + 5;
        for (int set = 0; set < 2; set++) {     /* the SPSs, then the PPSs */
            uint32_t cnt;
            if (c >= ae) { say("A broken avcC box\n"); return -1; }
            cnt = set ? *c++ : *c++ & 31u;
            for (i = 0; i < cnt; i++) {
                uint32_t l;
                if (ae - c < 2 || (l = be16(c)) > (uint32_t)(ae - c - 2) || ps_len + 4 + l > sizeof ps) {
                    say("A broken avcC box\n");
                    return -1;
                }
                ps[ps_len] = ps[ps_len + 1] = ps[ps_len + 2] = 0;
                ps[ps_len + 3] = 1;
                memcpy(ps + ps_len + 4, c + 2, l);
                ps_len += 4 + l;
                c += 2 + l;
            }
        }
        if (he - mdhd < (mdhd[0] == 1 ? 24 : 16)) { say("A broken mdhd box\n"); return -1; }
        timescale = mdhd[0] == 1 ? be32(mdhd + 20) : be32(mdhd + 12);
        if (!timescale) { say("No timescale\n"); return -1; }
        if ((edts = box(trak, te, "edts", &ee)) != NULL && (elst = box(edts, ee, "elst", &ee)) != NULL) {
            long ne2 = table(elst, ee, 0, elst[0] == 1 ? 20 : 12, &c);
            for (long j = 0; j < ne2; j++, c += elst[0] == 1 ? 20 : 12) {
                int64_t mt = elst[0] == 1 ? (int64_t)be64(c + 8) : (int32_t)be32(c + 4);
                if (mt >= 0) { media_time = mt; break; }   /* (-1: an empty edit) */
            }
        }
        if ((n_stts = table(box(stbl, se, "stts", &ce), ce, 0, 8, &e_stts)) < 0) { say("No stts box\n"); return -1; }
        if ((c = box(stbl, se, "ctts", &ce)) != NULL) {
            if ((n_ctts = table(c, ce, 0, 8, &e_ctts)) < 0) { say("A broken ctts box\n"); return -1; }
        }
        if ((c = box(stbl, se, "stss", &ce)) != NULL && (n_stss = table(c, ce, 0, 4, &e_stss)) < 0) {
            say("A broken stss box\n");
            return -1;
        }
        if ((n_stsc = table(box(stbl, se, "stsc", &ce), ce, 0, 12, &e_stsc)) < 1) { say("No stsc box\n"); return -1; }
        if ((c = box(stbl, se, "stco", &ce)) == NULL) { c = box(stbl, se, "co64", &ce); co64 = 1; }
        if ((n_co = table(c, ce, 0, co64 ? 8 : 4, &e_co)) < 0) { say("No stco box\n"); return -1; }
        if (!(c = box(stbl, se, "stsz", &ce)) || ce - c < 12) { say("No stsz box\n"); return -1; }
        fixed_size = be32(c + 4);
        if ((n_sz = table(c, ce, 4, fixed_size ? 0 : 4, &e_sz)) < 1) { say("No samples\n"); return -1; }
        if (fixed_size && (uint32_t)n_sz > n / fixed_size) { say("More samples than the file holds\n"); return -1; }
        free(samples);
        if (!(samples = calloc((size_t)n_sz, sizeof *samples))) { say("Out of memory\n"); return -1; }
        nsamples = (int)n_sz;
        for (k = 0; k < (uint32_t)nsamples; k++) {
            samples[k].size = fixed_size ? fixed_size : be32(e_sz + 4 * k);
            samples[k].key = n_stss < 0;    /* (no stss: every sample is a keyframe) */
        }
        for (long j = 0; j < n_stss; j++) {
            uint32_t s1 = be32(e_stss + 4 * j);
            if (s1 >= 1 && s1 <= (uint32_t)nsamples) samples[s1 - 1].key = 1;
        }
        /* decode times, then the composition offsets */
        for (t = 0, k = 0, i = 0; i < (uint32_t)n_stts; i++)
            for (uint32_t r = be32(e_stts + 8 * i); r && k < (uint32_t)nsamples; r--, k++) {
                samples[k].dts = t;
                t += be32(e_stts + 8 * i + 4);
            }
        for (; k < (uint32_t)nsamples; k++) samples[k].dts = t;
        for (k = 0; k < (uint32_t)nsamples; k++) samples[k].pts = samples[k].dts;
        for (k = 0, i = 0; i < (uint32_t)n_ctts; i++)
            for (uint32_t r = be32(e_ctts + 8 * i); r && k < (uint32_t)nsamples; r--, k++) {
                uint32_t o = be32(e_ctts + 8 * i + 4);
                samples[k].pts += (int32_t)o;   /* (signed in version 0 too, as FFmpeg reads it) */
            }
        for (k = 0; k < (uint32_t)nsamples; k++) {
            samples[k].pts = to_us(samples[k].pts - media_time, timescale);
            samples[k].dts = to_us(samples[k].dts - media_time, timescale);
        }
        /* where each sample is: chunks, so many samples each (stsc runs) */
        for (k = 0, i = 0; i < (uint32_t)n_co && k < (uint32_t)nsamples; i++) {
            uint64_t off = co64 ? be64(e_co + 8 * i) : be32(e_co + 4 * i);
            uint32_t per = 0;
            for (long j = 0; j < n_stsc && be32(e_stsc + 12 * j) <= i + 1; j++) per = be32(e_stsc + 12 * j + 4);
            for (; per && k < (uint32_t)nsamples; per--, k++) {
                if (off > n || samples[k].size > n - off) { say("Sample %u is outside the file\n", (unsigned)k); return -1; }
                samples[k].off = (uint32_t)off;
                off += samples[k].size;
            }
        }
        if (k < (uint32_t)nsamples) { say("Only %u of %d samples are in chunks\n", (unsigned)k, nsamples); return -1; }
        return 0;
    }
    say("No H.264 (avc1) track\n");
    return -1;
}

/* One sample as Annex B, into au: each NAL after a start code instead of
   its length, the avcC's SPS and PPS before an IDR whose sample hasn't
   its own (as FFmpeg's h264_mp4toannexb), then zero bytes to a whole
   number of words (trailing_zero_8bits). au_len 0: an empty sample. */
static uint8_t *au;
static uint32_t au_cap, au_len;

static int to_annexb(const uint8_t *f, const sample_t *sm)
{
    const uint8_t *p = f + sm->off, *end = p + sm->size;
    uint32_t need = sm->size * 4 + ps_len + 8, o = 0;
    int have_ps = 0;
    if (need > au_cap) {
        free(au);
        if (!(au = malloc(need))) { au_cap = 0; say("Out of memory for a %u byte sample\n", (unsigned)sm->size); return -1; }
        au_cap = need;
    }
    while (end - p >= nal_size) {
        uint32_t l = 0, t;
        for (int b = 0; b < nal_size; b++) l = l << 8 | *p++;
        if (!l) continue;
        if (l > (uint32_t)(end - p)) { say("A NAL unit runs past its sample\n"); return -1; }
        t = p[0] & 31u;
        if (t == 7 || t == 8) have_ps = 1;
        if (t == 5 && !have_ps) {
            memcpy(au + o, ps, ps_len);
            o += ps_len;
            have_ps = 1;
        }
        au[o] = au[o + 1] = au[o + 2] = 0;
        au[o + 3] = 1;
        memcpy(au + o + 4, p, l);
        o += 4 + l;
        p += l;
    }
    while (o & 3) au[o++] = 0;
    au_len = o;
    return 0;
}

/* -x: the stream as it would be sent, and each sample's times */
static int export_annexb(const char *in, const char *out)
{
    FILE *f = fopen(in, "rb"), *o, *t;
    uint8_t *data;
    long n;
    char name[300];
    int k;
    if (!f) { say("Can't open %s\n", in); return 1; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (!(data = malloc((size_t)n + 1)) || fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); say("Can't read %s\n", in); return 1; }
    fclose(f);
    if (parse_mp4(data, (uint32_t)n)) return 1;
    snprintf(name, sizeof name, "%s.pts", out);
    if (!(o = fopen(out, "wb")) || !(t = fopen(name, "w"))) { say("Can't write %s\n", out); return 1; }
    for (k = 0; k < nsamples; k++) {
        if (to_annexb(data, &samples[k])) return 1;
        fwrite(au, 1, au_len, o);
        fprintf(t, "%lld %lld %d\n", (long long)samples[k].pts, (long long)samples[k].dts, samples[k].key);
    }
    fclose(o);
    fclose(t);
    say("%d samples, %ux%u, NAL lengths %d bytes, SPS+PPS %u bytes\n", nsamples, (unsigned)mp4_w, (unsigned)mp4_h,
        nal_size, (unsigned)ps_len);
    free(data);
    return 0;
}

/* ---- the expected checksums ---- */

static uint32_t *want;
static int64_t *want_pts;                  /* each picture's pts, microseconds */
static int nwant, want_w, want_h;

/* a whole number at *p (after spaces), *p moved past it and a comma.
   (Not sscanf's long long: UnixLib's filled only the low word - 0.13 on the
   Pi read every pts as -259612551386748682 + k * 40000.) */
static int64_t take_int(const char **p)
{
    const char *s = *p;
    int64_t v = 0;
    int neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    while (*s == ' ') s++;
    if (*s == ',') s++;
    *p = s;
    return neg ? -v : v;
}

static int read_crcs(const char *name)
{
    FILE *f = fopen(name, "r");
    char line[256];
    int cap = 0, tb_num = 1, tb_den = 25;
    if (!f) { say("Can't open %s\n", name); return -1; }
    while (fgets(line, sizeof line, f)) {
        char *x;
        const char *q = line;
        int64_t pts;
        if (line[0] == '#') {
            sscanf(line, "#dimensions 0: %dx%d", &want_w, &want_h);
            sscanf(line, "#tb 0: %d/%d", &tb_num, &tb_den);
            continue;
        }
        if (!(x = strstr(line, "0x"))) continue;
        if (nwant == cap) {
            cap = cap ? cap * 2 : 256;
            want = realloc(want, (size_t)cap * sizeof *want);
            want_pts = realloc(want_pts, (size_t)cap * sizeof *want_pts);
            if (!want || !want_pts) { fclose(f); return -1; }
        }
        want[nwant] = (uint32_t)strtoul(x, NULL, 16);
        take_int(&q);                       /* stream, dts, pts */
        take_int(&q);
        pts = take_int(&q);
        want_pts[nwant] = tb_den > 0 ? pts * 1000000 * tb_num / tb_den : nwant;
        nwant++;
    }
    fclose(f);
    if (!want_w || !want_h || !nwant) { say("%s has no #dimensions or no frames\n", name); return -1; }
    return 0;
}

/* FFmpeg's picture with this pts (within a millisecond), or -1 */
static int want_by_pts(int64_t pts)
{
    for (int j = 0; j < nwant; j++)
        if (want_pts[j] - pts <= 1000 && pts - want_pts[j] <= 1000) return j;
    return -1;
}

/* Signatures: FFmpeg's pictures as block means (an 8x8 grid of Y, 4x4 of U
   and of V: 96 numbers a picture), from build.sh. The VideoCore's H.264
   output isn't always bit-exact with FFmpeg's (the Pi 4: a few chroma
   values 1 lower, in moving areas), so each picture is also compared this
   way: "close" if no block mean differs by more than 1.0; otherwise the
   FFmpeg picture it is closest to is named, which shows pictures out of
   order or missing. */
#define SIGN 96
static float (*sig)[SIGN];
static int nsig;

static int read_sigs(const char *name)
{
    FILE *f = fopen(name, "r");
    int cap = 0;
    float v[SIGN];
    if (!f) { say("Can't open %s\n", name); return -1; }
    for (;;) {
        int k;
        for (k = 0; k < SIGN; k++) if (fscanf(f, "%f", &v[k]) != 1) break;
        if (k < SIGN) break;
        if (nsig == cap) {
            cap = cap ? cap * 2 : 64;
            if (!(sig = realloc(sig, (size_t)cap * sizeof *sig))) { fclose(f); return -1; }
        }
        memcpy(sig[nsig++], v, sizeof v);
    }
    fclose(f);
    return 0;
}

static void grid(const uint8_t *p, uint32_t pitch, uint32_t w, uint32_t h, int n, float *out)
{
    for (int gy = 0; gy < n; gy++)
        for (int gx = 0; gx < n; gx++) {
            uint32_t y0 = h * (uint32_t)gy / (uint32_t)n, y1 = h * (uint32_t)(gy + 1) / (uint32_t)n;
            uint32_t x0 = w * (uint32_t)gx / (uint32_t)n, x1 = w * (uint32_t)(gx + 1) / (uint32_t)n;
            double sum = 0;
            for (uint32_t y = y0; y < y1; y++)
                for (uint32_t x = x0; x < x1; x++) sum += p[y * pitch + x];
            *out++ = (float)(sum / (double)((y1 - y0) * (x1 - x0)));
        }
}

static float sig_diff(const float *a, const float *b)
{
    float m = 0;
    for (int k = 0; k < SIGN; k++) { float d = a[k] > b[k] ? a[k] - b[k] : b[k] - a[k]; if (d > m) m = d; }
    return m;
}

/* ---- checking a picture ---- */

static uint8_t *frame;                     /* the picture, packed I420 at the visible size */
static uint32_t frame_len;
static int close_n, far_n, wrong, disorder, dups, extra, timing, keep_going;
static int have_last, pass;
static int64_t last_pts, seek_pts;
static int from_key;                       /* pictures from the keyframe on (after a seek) */
static uint8_t *seen;                      /* FFmpeg's pictures that have come back (this pass) */

static void frame_sig(float *s)
{
    uint32_t w = (uint32_t)want_w, h = (uint32_t)want_h, cw = (w + 1) / 2, ch = (h + 1) / 2;
    grid(frame, w, w, h, 8, s);
    grid(frame + w * h, cw, cw, ch, 4, s + 64);
    grid(frame + w * h + cw * ch, cw, cw, ch, 4, s + 80);
}

/* picture k (in frame[], with this pts) against FFmpeg's: 0, or -1 to stop */
static int check_picture(int k, const vcdec_picture *pic)
{
    int j;
    uint32_t crc;
    if (pic->pts == VCDEC_NOPTS) {
        wrong++;
        if (wrong <= 5) say("Picture %d: no pts\n", k);
        return keep_going || timing ? 0 : -1;
    }
    if (have_last && pic->pts <= last_pts) {
        disorder++;
        if (disorder <= 5) say("Picture %d: pts %lld after %lld: not in display order\n", k, (long long)pic->pts, (long long)last_pts);
    }
    last_pts = pic->pts;
    have_last = 1;
    if (pic->pts >= seek_pts) from_key++;
    if ((j = want_by_pts(pic->pts)) < 0) {
        wrong++;
        if (wrong <= 5) say("Picture %d: pts %lld isn't one of FFmpeg's pictures\n", k, (long long)pic->pts);
        return keep_going || timing ? 0 : -1;
    }
    if (seen[j]) {
        dups++;
        if (dups <= 5) say("Picture %d: FFmpeg's picture %d again\n", k, j);
    }
    seen[j] = 1;
    if (timing) return 0;
    crc = adler(0, frame, frame_len);
    if (crc != want[j]) {
        int is_close = 0;
        if (j < nsig) {
            float s[SIGN], d, best = 1e9f;
            int bj = -1;
            frame_sig(s);
            d = sig_diff(s, sig[j]);
            for (int q = 0; q < nsig; q++) {
                float dq = sig_diff(s, sig[q]);
                if (dq < best) { best = dq; bj = q; }
            }
            if (d <= 1.0f) { is_close = 1; close_n++; }
            else {
                far_n++;
                if (far_n <= 5) say("Picture %d: not close to FFmpeg's picture %d (%.1f); closest is FFmpeg's %d (%.1f)\n",
                                    k, j, (double)d, bj, (double)best);
            }
        }
        if (!is_close) {
            if (wrong < 5 && j >= nsig) say("Picture %d: checksum &%08X, FFmpeg's &%08X\n", k, (unsigned)crc, (unsigned)want[j]);
            wrong++;
            if (!keep_going) return -1;
        }
    }
    return 0;
}

static void log_line(void *h, const char *text)
{
    (void)h;
    say("  - %s\n", text);
}

/* FFmpeg's pictures from pts p on (or all) that haven't come back */
static int count_missing(int64_t p, int show)
{
    int missing = 0;
    for (int i = 0; i < nwant; i++)
        if (!seen[i] && want_pts[i] >= p) {
            if (++missing <= 5 && show) say("FFmpeg's picture %d (pts %lld) never came back\n", i, (long long)want_pts[i]);
        }
    if (missing > 5 && show) say("(%d of FFmpeg's pictures never came back)\n", missing);
    return missing;
}

/* ---- main ---- */

int probe_main(int argc, char **argv)
{
    const char *stream_name = NULL, *crc_name = NULL, *sig_name = NULL, *x_out = NULL;
    uint8_t *stream = NULL;
    uint32_t file_len = 0, t_start, t_end = 0, recv_cs = 0, send_cs = 0, last_picture, ticks = 0, again_in = 0;
    int verbose = 0, sync = 0, gpu_any = 0, seek_after = 0, seek_end = 0, seek_s = -1, seeked = 0;
    int out_buffers = 0, bench = 0, zero = 0, hold_n = 1, read_all = 0, nholds = 0, unholdable = 0;
    vcdec_hold *holds[16];
    uint32_t read_sum = 0;
    unsigned way = 0, memory = 0;
    const char *way_name = "ldm8", *memory_name = "auto";
    int frames = 0, pass_frames = 0, before_flush = 0, fatal = 0, eof = 0, i, r, cur = 0, eos_sent = 0, au_ready = 0;
    int missing = 0, missing2 = 0, pass1_frames = 0, pass1_ok = 0;
    vcdec *d = NULL;
    vcdec_config cfg;
    vcdec_stats stats;
    uint8_t *planes[3];
    int strides[3];

    memset(&stats, 0, sizeof stats);
    /* (a fresh start each time: the host tests call this more than once) */
    free(want); want = NULL; free(want_pts); want_pts = NULL; nwant = want_w = want_h = 0; nsig = 0;
    out2 = NULL; nsamples = 0; au_len = 0; close_n = far_n = wrong = disorder = dups = extra = timing = keep_going = 0;
    have_last = 0; seek_pts = INT64_MIN; from_key = 0; free(seen); seen = NULL; pass = 1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out2 = fopen(argv[++i], "a");
        else if (!strcmp(argv[i], "-n")) keep_going = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-S")) sync = 1;
        else if (!strcmp(argv[i], "-g")) gpu_any = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seek_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-E")) seek_end = 1;
        else if (!strcmp(argv[i], "-t")) timing = 1;
        else if (!strcmp(argv[i], "-B") && i + 1 < argc) out_buffers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-K")) bench = 1;
        else if (!strcmp(argv[i], "-Z")) zero = 1;
        else if (!strcmp(argv[i], "-R")) read_all = 1;
        else if (!strcmp(argv[i], "-H") && i + 1 < argc) { zero = 1; hold_n = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            way_name = argv[++i];
            way = !strcmp(way_name, "ldm8") ? VCDEC_COPY_LDM8 : !strcmp(way_name, "neon") ? VCDEC_COPY_NEON :
                  !strcmp(way_name, "ldm4") ? VCDEC_COPY_LDM4 : 0;
            if (!way) stream_name = crc_name = NULL, i = argc;
        } else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            memory_name = argv[++i];
            memory = !strcmp(memory_name, "pmp") ? VCDEC_OUT_PMP :
                     !strcmp(memory_name, "pmpu") ? VCDEC_OUT_PMP | VCDEC_OUT_UNCACHED :
                     !strcmp(memory_name, "pci") ? VCDEC_OUT_PCI : 0;
            if (!memory && strcmp(memory_name, "auto")) stream_name = crc_name = NULL, i = argc;
        }
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) x_out = argv[++i];
        else if (!stream_name) stream_name = argv[i];
        else if (!crc_name) crc_name = argv[i];
        else if (!sig_name) sig_name = argv[i];
        else stream_name = NULL;
    }
    if (x_out && stream_name && !crc_name) return export_annexb(stream_name, x_out);
    if (!stream_name || !crc_name || seek_after < 0 || (seek_after && seek_end) || hold_n < 1 || hold_n > 16) {
        printf("Usage: vcdectest [-o file] [-v] [-n] [-S] [-g] [-s N | -E] [-t] [-B n] [-c ldm8|ldm4|neon] "
               "[-m auto|pci|pmp|pmpu] [-K] [-Z [-H n] [-R]] stream.mp4 expected.crc [expected.sig]\n");
        return 1;
    }
    say("vcdectest: %s through vcdec %s\n", stream_name, VCDEC_VERSION);
    if (read_crcs(crc_name)) goto done;
    if (sig_name && read_sigs(sig_name)) goto done;
    if (!(seen = calloc((size_t)nwant, 1))) goto done;
    {
        FILE *f = fopen(stream_name, "rb");
        long n;
        if (!f) { say("Can't open %s\n", stream_name); goto done; }
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        stream = malloc((size_t)n + 4);
        if (!stream || fread(stream, 1, (size_t)n, f) != (size_t)n) { fclose(f); say("Can't read %s\n", stream_name); goto done; }
        fclose(f);
        file_len = (uint32_t)n;
    }
    if (file_len < 8 || (memcmp(stream + 4, "ftyp", 4) && memcmp(stream + 4, "moov", 4))) {
        say("%s isn't an MP4\n", stream_name);
        goto done;
    }
    if (parse_mp4(stream, file_len)) goto done;
    if (seek_after || seek_end) {
        for (i = nsamples - 1; i > 0 && seek_s < 0; i--) if (samples[i].key) seek_s = i;
        if (seek_s < 0) { say("No keyframe after the first to seek to\n"); goto done; }
    }
    say("MP4: %d samples, %ux%u; %d of FFmpeg's pictures expected\n", nsamples, (unsigned)mp4_w, (unsigned)mp4_h, nwant);
    say("The VideoCore has %u MB of memory (gpu_mem)\n", vcdec_gpu_mem());
    say("Each turn: access units sent until the input is full, then every picture ready taken%s\n\n",
        sync ? " (-S: each bulk transfer waited for as it arrives)" : "");
    frame_len = (uint32_t)(want_w * want_h + 2 * ((want_w + 1) / 2) * ((want_h + 1) / 2));
    if (!(frame = realloc(frame, frame_len))) goto done;
    planes[0] = frame;
    planes[1] = frame + want_w * want_h;
    planes[2] = planes[1] + ((want_w + 1) / 2) * ((want_h + 1) / 2);
    strides[0] = want_w;
    strides[1] = strides[2] = (want_w + 1) / 2;

    vcdec_config_init(&cfg);
    cfg.width = (int)mp4_w;
    cfg.height = (int)mp4_h;
    cfg.flags = (sync ? VCDEC_SYNC_RECEIVE : 0) | (gpu_any ? VCDEC_NO_GPU_MEM_CHECK : 0) | (verbose ? VCDEC_LOG_MESSAGES : 0) |
                way | memory;
    cfg.out_buffers = out_buffers;
    if (zero) say("Zero-copy: pictures held in vcdec's buffers (up to %d at a time)%s\n", hold_n,
                  read_all ? ", each read once" : "");
    say("Output buffers: %d; pictures copied out by %s; they arrive in %s\n", out_buffers ? out_buffers : 3, way_name,
        memory == VCDEC_OUT_PCI ? "PCI_RAMAlloc memory" : memory & VCDEC_OUT_UNCACHED ? "a Physical Memory Pool, not cacheable" :
        memory ? "a Physical Memory Pool, cacheable" : "a cacheable Physical Memory Pool if there is one, else PCI memory");
    if (memory != VCDEC_OUT_PCI) {                          /* (the RAM disc is a PMP on RISC OS 5: its flags show the PMP bit) */
        _kernel_swi_regs r;
        memset(&r, 0, sizeof r);
        r.r[0] = 2; r.r[1] = 5;
        if (!probe_swi(0x66, &r))
            say("The RAM disc's dynamic area flags: &%X (bit 20 %s)\n", (unsigned)r.r[4], r.r[4] & 1 << 20 ? "set" : "clear");
        else
            say("(the RAM disc's dynamic area can't be read)\n");
    }
    cfg.log = log_line;
    {
        uint32_t t0 = now_cs();
        if ((r = vcdec_open(&d, &cfg)) != VCDEC_OK) {
            say("vcdec_open: %s%s\n", r == VCDEC_UNSUPPORTED ? "not for the VideoCore: " : "", vcdec_open_error());
            goto done;
        }
        say("vcdec_open: %u cs\n", (unsigned)(now_cs() - t0));
    }

    t_start = last_picture = now_cs();
    while (!fatal && !eof) {
        vcdec_picture pic;
        ticks++;
        /* send until the input is full; EOS after the last (with -s, not
           before the seek: a player seeking before the end hasn't reached
           it. Unless the seek point is too near the end to be reached without
           the EOS: the decoder keeps its last pictures until then) */
        while (cur < nsamples) {
            if (!au_ready) {
                if (to_annexb(stream, &samples[cur])) { fatal = 1; break; }
                if (!au_len) { cur++; continue; }   /* (an empty sample) */
                au_ready = 1;
            }
            {
                uint32_t t0 = now_cs();
                r = vcdec_send(d, au, au_len, samples[cur].pts, samples[cur].dts, samples[cur].key ? VCDEC_KEYFRAME : 0);
                send_cs += now_cs() - t0;
            }
            if (r == VCDEC_AGAIN) { again_in++; break; }
            if (r != VCDEC_OK) { say("vcdec_send: %s\n", vcdec_error(d)); fatal = 1; break; }
            au_ready = 0;
            cur++;
        }
        if (fatal) break;
        if (cur == nsamples && !eos_sent && !(seek_after && !seeked && seek_after + 8 <= nsamples)) {
            if ((r = vcdec_send_eos(d)) == VCDEC_OK) eos_sent = 1;
            else if (r != VCDEC_AGAIN) { say("vcdec_send_eos: %s\n", vcdec_error(d)); fatal = 1; break; }
        }
        /* every picture ready (one of another size dropped: it wouldn't fit) */
        for (;;) {
            uint32_t t0 = now_cs();
            if ((r = vcdec_peek(d, &pic)) != VCDEC_OK) break;
            if (pic.width != want_w || pic.height != want_h) {
                if (++wrong <= 5) say("Picture %d: %dx%d, not %dx%d\n", frames, pic.width, pic.height, want_w, want_h);
                vcdec_receive(d, &pic, NULL, NULL);
                if (!keep_going) { fatal = 1; break; }
                continue;
            }
            if (zero) {
                uint8_t *hp[3];
                int hs[3];
                vcdec_hold *h = NULL;
                if (nholds == hold_n) {          /* the oldest given back first */
                    vcdec_release(holds[0]);
                    memmove(holds, holds + 1, sizeof holds[0] * (size_t)(hold_n - 1));
                    nholds--;
                }
                r = vcdec_receive_hold(d, &pic, hp, hs, &h);
                if (r == VCDEC_UNSUPPORTED) {   /* (PCI memory, or too many held: copied instead) */
                    unholdable++;
                    r = vcdec_receive(d, &pic, planes, strides);
                } else if (r == VCDEC_OK) {
                    if (!timing)                /* (packed for the checksum) */
                        for (int pl = 0; pl < 3; pl++) {
                            int pw = pl ? (want_w + 1) / 2 : want_w, ph = pl ? (want_h + 1) / 2 : want_h;
                            for (int y = 0; y < ph; y++) memcpy(planes[pl] + y * strides[pl], hp[pl] + y * hs[pl], (size_t)pw);
                        }
                    else if (read_all)
                        for (int pl = 0; pl < 3; pl++) {
                            int pw = pl ? (want_w + 1) / 2 : want_w, ph = pl ? (want_h + 1) / 2 : want_h;
                            for (int y = 0; y < ph; y++) {
                                const uint32_t *w4 = (const uint32_t *)(hp[pl] + y * hs[pl]);
                                for (int x = 0; x < pw / 4; x++) read_sum += w4[x];
                            }
                        }
                    holds[nholds++] = h;
                }
            } else {
                r = vcdec_receive(d, &pic, planes, strides);
            }
            if (r != VCDEC_OK) break;
            recv_cs += now_cs() - t0;
            last_picture = now_cs();
            if (verbose) say("  picture %d: pts %lld%s\n", frames, (long long)pic.pts, pic.flags & VCDEC_PIC_KEYFRAME ? ", keyframe" : "");
            if (check_picture(frames, &pic)) { fatal = 1; break; }
            frames++;
            pass_frames++;
            if (seek_after && !seeked && pass_frames == seek_after) break;
        }
        if (fatal) break;
        if (r < 0) { say("vcdec_receive: %s\n", vcdec_error(d)); fatal = 1; break; }
        if (r == VCDEC_EOF) {
            t_end = now_cs();
            if (seek_end && pass == 1) {
                /* -E: the whole clip out, then a seek after the end */
                uint32_t t0 = now_cs(), cs;
                pass1_frames = frames;
                missing = count_missing(INT64_MIN, 1);
                pass1_ok = !missing && !wrong && !disorder && !dups;
                say("Pass 1: %d pictures to the end%s; then vcdec_flush (after the EOS: the decoder created again) and on from "
                    "the last keyframe (sample %d, pts %lld)\n", frames, pass1_ok ? ", all right" : "", seek_s, (long long)samples[seek_s].pts);
                if ((r = vcdec_flush(d)) != VCDEC_OK) { say("vcdec_flush: %s\n", vcdec_error(d)); fatal = 1; break; }
                cs = now_cs() - t0;
                say("  done in %u cs\n\n", (unsigned)cs);
                pass = 2;
                memset(seen, 0, (size_t)nwant);
                seek_pts = samples[seek_s].pts;
                have_last = 0; from_key = 0; pass_frames = 0;
                cur = seek_s; au_ready = 0; eos_sent = 0;
                continue;
            }
            eof = 1;
            break;
        }
        /* -s: a seek before the end */
        if (seek_after && !seeked && pass_frames == seek_after) {
            uint32_t t0 = now_cs();
            seek_pts = samples[seek_s].pts;
            if (have_last && seek_pts <= last_pts) {
                say("\nAfter %d pictures the last keyframe (pts %lld) has been passed: nothing to seek to\n", frames,
                    (long long)seek_pts);
                fatal = 1;
                break;
            }
            say("\nAfter %d pictures (%d of %d samples sent, the EOS not): vcdec_flush, then on from the last keyframe "
                "(sample %d, pts %lld)\n", frames, cur, nsamples, seek_s, (long long)seek_pts);
            if ((r = vcdec_flush(d)) != VCDEC_OK) { say("vcdec_flush: %s\n", vcdec_error(d)); fatal = 1; break; }
            say("  done in %u cs\n\n", (unsigned)(now_cs() - t0));
            before_flush = frames;
            from_key = 0;
            seeked = 1;
            cur = seek_s; au_ready = 0; eos_sent = 0;   /* (if the EOS had gone, vcdec created the decoder again) */
        }
        if (now_cs() - last_picture > 1000) {
            say("No picture for 10 s (%d so far, %d of %d samples sent)\n", frames, cur, nsamples);
            fatal = 1;
        }
    }
    vcdec_get_stats(d, &stats);
    if (eof) {
        uint32_t cs = t_end - t_start ? t_end - t_start : 1;
        say("\n%d pictures in %u.%02u s: %u.%u pictures a second (loop turns: %u; input full: %u)\n", frames,
            (unsigned)(cs / 100), (unsigned)(cs % 100), (unsigned)(frames * 100 / cs), (unsigned)(frames * 1000 / cs % 10),
            (unsigned)ticks, (unsigned)again_in);
        say("Copying the pictures out (vcdec_receive): %u cs in all, %u.%02u ms a picture\n", (unsigned)recv_cs,
            frames ? (unsigned)(recv_cs * 10 / (unsigned)frames) : 0, frames ? (unsigned)(recv_cs * 1000 / (unsigned)frames % 100) : 0);
        /* where the time went, in cs over the whole run (each part timed to the centisecond: totals only) */
        say("Inside vcdec (cs in all): messages taken in %u (the pictures' transfers queued: %u), buffers handed back %u, "
            "cache %u, copying %u; vcdec_send: %u\n", stats.cs_messages, stats.cs_queue, stats.cs_give, stats.cs_cache,
            stats.cs_copy, (unsigned)send_cs);
    }
    say("Output buffers at the end: %u in pools, %u in PCI memory\n", stats.pool_buffers, stats.pci_buffers);
    if (bench && eof) {                    /* -K: one picture's copy, each way */
        static const struct { unsigned way; const char *name; } ways[3] = {
            { VCDEC_COPY_LDM4, "LDM 4 words" }, { VCDEC_COPY_LDM8, "LDM 8 words" }, { VCDEC_COPY_NEON, "NEON 64 bytes" } };
        uint32_t bytes = frame_len;
        int reps = (int)((64u << 20) / (bytes ? bytes : 1));
        if (reps < 10) reps = 10;
        if (reps > 2000) reps = 2000;
        for (int k = 0; k < 3; k++) {
            unsigned cs = 0;
            if (k == 2 && !stats.pool_buffers) { say("(NEON isn't used for PCI memory: it's copied in SVC mode)\n"); break; }
            if (vcdec_copy_benchmark(d, ways[k].way, reps, planes, strides, &cs) != VCDEC_OK) {
                say("Copy timing: %s\n", vcdec_error(d));
                break;
            }
            if (!cs) cs = 1;
            say("One %ux%u picture copied out (%s, %s): %u.%02u ms (%u MB/s; %d copies in %u cs)\n", (unsigned)want_w,
                (unsigned)want_h, stats.pool_buffers ? (stats.pci_buffers ? "pool+pci" : memory & VCDEC_OUT_UNCACHED ? "pmpu" : "pmp") : "pci",
                ways[k].name, (unsigned)(cs * 1000u / (unsigned)reps / 100),
                (unsigned)(cs * 1000u / (unsigned)reps % 100), (unsigned)((uint64_t)bytes * (unsigned)reps * 100 / cs / 1000000),
                reps, cs);
        }
    }
    say("vcdec: %u access units sent, %u pictures taken, %u dropped, %u flushes, %u created again, %u format changes\n",
        stats.sent, stats.pictures, stats.discarded, stats.flushes, stats.recreated, stats.format_changes);
    if (seek_end) {
        missing2 = count_missing(seek_pts, 1);
        say("Pass 2 (after the end): %d pictures from the keyframe on, FFmpeg's from pts %lld on: %s\n", from_key,
            (long long)seek_pts, missing2 ? "some missing" : "all of them");
    } else {
        missing = count_missing(seeked ? seek_pts : INT64_MIN, 1);
        if (seeked)
            say("Before the flush: %d pictures; from the keyframe on, %d (FFmpeg's from pts %lld on: %s)\n", before_flush,
                from_key, (long long)seek_pts, missing ? "some missing" : "all of them");
    }
    if (disorder) say("%d pictures came back out of display order\n", disorder);
    if (!timing) say("%d checked against FFmpeg: %d wrong\n", frames, wrong);

done:
    if (d) {
        uint32_t t0 = now_cs();
        vcdec_close(d);
        say("vcdec_close: %u cs\n", (unsigned)(now_cs() - t0));
        if (nholds) say("%d pictures still held at vcdec_close: released after it\n", nholds);
        for (int k = 0; k < nholds; k++) vcdec_release(holds[k]);
        nholds = 0;
    }
    if (zero) say("Zero-copy: %u pictures held, %d copied instead (not holdable)%s\n", stats.holds, unholdable,
                  read_all ? (read_sum ? "; all read" : "; all read (sum 0)") : "");
    {
        int complete = seek_end ? pass1_ok && !missing2 : !missing;
        int ok = eof && !fatal && !wrong && complete && !disorder && !dups && (!seek_after || seeked);
        (void)pass1_frames; (void)extra;
        if (seek_after && !seeked && eof) say("The flush and seek never happened (the clip ended first)\n");
        if (nsig && !timing) say("Not bit-exact but close to FFmpeg's (block means within 1): %d; not close: %d\n", close_n, far_n);
        say("\nResult: %s\n", ok && timing ? "OK - timed (the pictures weren't checked)" :
                              ok && !close_n ? "OK - every picture exactly as FFmpeg decodes it" :
                              ok ? "OK - every picture matches FFmpeg's, some within rounding (see above)" :
                              wrong ? "pictures differ from FFmpeg's" :
                              eof && disorder ? "pictures came back out of display order" :
                              eof && dups ? "a picture came back twice" :
                              eof && !complete ? "the decoder finished, but pictures are missing" :
                              eof && seek_after && !seeked ? "the seek wasn't tried" : "the decode didn't finish (see above)");
        if (out2) fclose(out2);
        free(stream);
        return ok ? 0 : 1;
    }
}

#ifndef PROBE_TEST
int main(int argc, char **argv) { return probe_main(argc, argv); }
#endif
