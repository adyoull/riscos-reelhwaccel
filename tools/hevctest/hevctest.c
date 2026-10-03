/*
 * hevctest - hevcdec (the Pi 4's HEVC block) on a trace from
 * tools/hevctrace: every picture decoded by the block as the trace gives
 * it (parameters, slices, DPB), turned into planar 4:2:0 and checked
 * against the Adler-32s of the picture FFmpeg decoded.
 *
 *   hevctest [-o file] [-v] [-n] [-t] [-c count] [-f frames] trace
 *      -o file   also add the report to file
 *      -v        hevcdec's log (and rpivid's messages)
 *      -n        don't stop at the first wrong picture
 *      -t        time only: pictures not converted or checked
 *      -c count  only the first count pictures
 *      -f frames output frames to use (default 17: the largest DPB, 16, + 1)
 *
 * The trace format: tools/hevctrace/README.md (hevc_trace.c).
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hwhevcdec.h"
#ifndef PROBE_TEST
#include "kernel.h"
static _kernel_oserror *ht_swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }
#include "../../common/contig.h"           /* (the page at &8000, read before and after) */
#endif
#define MAX_FRAMES 32

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

static void log_line(void *h, const char *t) { (void)h; say("  - %s\n", t); }

static uint32_t now_cs(void) { return (uint32_t)(clock() * 100 / CLOCKS_PER_SEC); }

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t adler(const uint8_t *p, int w, int h, int stride)
{
    uint32_t a = 1, b = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *r = p + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            a += r[x];
            if (a >= 65521) a -= 65521;
            b += a;
            if (b >= 65521) b -= 65521;
        }
    }
    return b << 16 | a;
}

typedef struct {
    uint32_t number, width, height, depth, nslices, has_scaling;
    uint32_t left, top, out_w, out_h;      /* the output window */
    int32_t poc;
    /* (copies: in the file they needn't be 8-byte aligned, and they hold 64-bit fields) */
    struct v4l2_ctrl_hevc_sps sps;
    struct v4l2_ctrl_hevc_pps pps;
    struct v4l2_ctrl_hevc_decode_params dec;
    struct v4l2_ctrl_hevc_scaling_matrix scaling;
    struct v4l2_ctrl_hevc_slice_params *params;
    hevcdec_slice *slices;
    uint32_t crc[3];
} pic_t;

static pic_t *pics;
static int npics;
static int shown;                         /* (wrong pictures described: the first ten) */

/* the trace into pics[] (pointing into the loaded file) */
static int read_trace(const char *name, uint8_t **file)
{
    FILE *f = fopen(name, "rb");
    long n;
    uint8_t *d;
    size_t o;
    int cap = 0;
    if (!f) { say("Can't open %s\n", name); return -1; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 28 || !(d = malloc((size_t)n)) || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        say("Can't read %s\n", name);
        return -1;
    }
    fclose(f);
    *file = d;
    if (memcmp(d, "HVTR", 4) || rd32(d + 4) != 1) { say("%s isn't a version 1 trace\n", name); return -1; }
    if (rd32(d + 8) != sizeof(struct v4l2_ctrl_hevc_sps) || rd32(d + 12) != sizeof(struct v4l2_ctrl_hevc_pps) ||
        rd32(d + 16) != sizeof(struct v4l2_ctrl_hevc_slice_params) ||
        rd32(d + 20) != sizeof(struct v4l2_ctrl_hevc_decode_params) ||
        rd32(d + 24) != sizeof(struct v4l2_ctrl_hevc_scaling_matrix)) {
        say("%s: the controls' sizes differ from these (%u %u %u %u %u)\n", name, (unsigned)rd32(d + 8),
            (unsigned)rd32(d + 12), (unsigned)rd32(d + 16), (unsigned)rd32(d + 20), (unsigned)rd32(d + 24));
        return -1;
    }
#define NEED(k) do { if (o + (size_t)(k) > (size_t)n) { say("%s: cut short\n", name); return -1; } } while (0)
    o = 28;
    while (o + 48 <= (size_t)n) {
        pic_t *p;
        if (memcmp(d + o, "PIC1", 4)) { say("%s: a bad record at %u\n", name, (unsigned)o); return -1; }
        if (npics == cap) {
            cap = cap ? cap * 2 : 64;
            if (!(pics = realloc(pics, sizeof *pics * (size_t)cap))) { say("Out of memory\n"); return -1; }
        }
        p = &pics[npics];
        memset(p, 0, sizeof *p);
        p->number = rd32(d + o + 4); p->poc = (int32_t)rd32(d + o + 8); p->width = rd32(d + o + 12);
        p->height = rd32(d + o + 16); p->depth = rd32(d + o + 20); p->nslices = rd32(d + o + 24);
        p->has_scaling = rd32(d + o + 28);
        p->left = rd32(d + o + 32); p->top = rd32(d + o + 36); p->out_w = rd32(d + o + 40); p->out_h = rd32(d + o + 44);
        o += 48;
        NEED(sizeof p->sps + sizeof p->pps + sizeof p->dec + (p->has_scaling ? sizeof p->scaling : 0));
        memcpy(&p->sps, d + o, sizeof p->sps); o += sizeof p->sps;
        memcpy(&p->pps, d + o, sizeof p->pps); o += sizeof p->pps;
        memcpy(&p->dec, d + o, sizeof p->dec); o += sizeof p->dec;
        if (p->has_scaling) { memcpy(&p->scaling, d + o, sizeof p->scaling); o += sizeof p->scaling; }
        if (!p->nslices || p->nslices > 600 || !(p->slices = calloc(p->nslices, sizeof *p->slices)) ||
            !(p->params = calloc(p->nslices, sizeof *p->params))) {
            say("%s: picture %u has %u slices\n", name, (unsigned)p->number, (unsigned)p->nslices);
            return -1;
        }
        for (uint32_t i = 0; i < p->nslices; i++) {
            uint32_t len;
            NEED(sizeof p->params[i] + 4);
            memcpy(&p->params[i], d + o, sizeof p->params[i]); o += sizeof p->params[i];
            p->slices[i].params = &p->params[i];
            len = rd32(d + o); o += 4;
            NEED(len);
            p->slices[i].data = d + o;
            p->slices[i].size = len;
            o += (len + 3) & ~3u;
        }
        NEED(12);
        for (int k = 0; k < 3; k++) { p->crc[k] = rd32(d + o); o += 4; }
        npics++;
    }
    return 0;
}

static uint8_t *planes8[3];                /* 10-bit: the 8-bit conversion, checked against the 16-bit one */
static int strides8[3];
static uint8_t *planesh[3];                /* (0.1.10) the halved conversion, checked against the 1:1 one's 2x2 means */
static int stridesh[3];

/* the halved conversion of f (window w x h from x, y: w/2 x h/2) against
   the 2x2 means of the 1:1 conversion in planes (bps bytes a sample): 0 if
   every sample is right */
static int check_half(hevcdec *d, const hevcdec_frame *f, int x, int y, int w, int h, uint8_t *const planes[3],
                      const int strides[3], int bps)
{
    int ow = w / 2, oh = h / 2, bad = 0;
    if (ow < 1 || oh < 1 || (x & 3) || !planesh[0]) return 0;
    if (hevcdec_frame_to_i420_half(d, f, planesh, stridesh, x, y, ow, oh) != HEVCDEC_OK) return 1;
    for (int k = 0; k < 3 && !bad; k++) {
        int pw = k ? (ow + 1) / 2 : ow, ph = k ? (oh + 1) / 2 : oh, sw = k ? (w + 1) / 2 : w, sh = k ? (h + 1) / 2 : h;
        for (int r = 0; r < ph && !bad; r++)
            for (int c = 0; c < pw; c++) {
                int c1 = 2 * c + 1 < sw ? 2 * c + 1 : 2 * c, r1 = 2 * r + 1 < sh ? 2 * r + 1 : 2 * r;
                unsigned s4 = 0, want;
                if (2 * c + 1 >= sw || 2 * r + 1 >= sh) continue;   /* (an edge without its whole block: not checked) */
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        const uint8_t *row = planes[k] + (size_t)(dy ? r1 : 2 * r) * strides[k];
                        int cc = dx ? c1 : 2 * c;
                        s4 += bps == 2 ? ((const uint16_t *)(const void *)row)[cc] : row[cc];
                    }
                want = bps == 2 ? (s4 + 8) >> 4 : (s4 + 2) >> 2;
                if (planesh[k][(size_t)r * stridesh[k] + c] != want) { bad = 1; break; }
            }
    }
    return bad;
}

/* a decoded picture converted (timed), saved (-d) and checked: 0 if it's
   FFmpeg's. 10-bit: as 16-bit samples (checked against FFmpeg's), and as
   8-bit (each sample the 16-bit one's top 8 bits) */
static int check_one(hevcdec *d, const pic_t *p, hevcdec_frame *f, uint8_t *const planes[3], const int strides[3],
                     FILE *dump, int verbose, uint32_t *t_conv)
{
    int cw = ((int)p->out_w + 1) / 2, ch = ((int)p->out_h + 1) / 2, bps = p->depth > 8 ? 2 : 1;
    uint32_t t0 = now_cs(), a[3];
    if (bps == 2) {
        uint16_t *p16[3] = { (uint16_t *)(void *)planes[0], (uint16_t *)(void *)planes[1], (uint16_t *)(void *)planes[2] };
        hevcdec_frame_to_i420_16(d, f, p16, strides, (int)p->left, (int)p->top, (int)p->out_w, (int)p->out_h);
    } else {
        hevcdec_frame_to_i420(d, f, planes, strides, (int)p->left, (int)p->top, (int)p->out_w, (int)p->out_h);
    }
    *t_conv += now_cs() - t0;
    if (bps == 2) {                            /* (the 8-bit conversion of the same frame: the top 8 bits) */
        int bad8 = 0;
        hevcdec_frame_to_i420(d, f, planes8, strides8, (int)p->left, (int)p->top, (int)p->out_w, (int)p->out_h);
        for (int k = 0; k < 3 && !bad8; k++) {
            int pw = k ? cw : (int)p->out_w, ph = k ? ch : (int)p->out_h;
            for (int y = 0; y < ph && !bad8; y++) {
                const uint16_t *r16 = (const uint16_t *)(const void *)(planes[k] + (size_t)y * strides[k]);
                const uint8_t *r8 = planes8[k] + (size_t)y * strides8[k];
                for (int x = 0; x < pw; x++) if (r8[x] != (uint8_t)(r16[x] >> 2)) { bad8 = 1; break; }
            }
        }
        if (bad8) {
            if (++shown <= 10) say("Picture %u (poc %d): the 8-bit conversion WRONG (not the 10-bit one's top bits)\n",
                                   (unsigned)p->number, (int)p->poc);
            return 1;
        }
    }
    if (check_half(d, f, (int)p->left, (int)p->top, (int)p->out_w, (int)p->out_h, planes, strides, bps)) {
        if (++shown <= 10) say("Picture %u (poc %d): the halved conversion WRONG (not the 2x2 means)\n", (unsigned)p->number,
                               (int)p->poc);
        return 1;
    }
    if (dump) {
        for (int y = 0; y < (int)p->out_h; y++) fwrite(planes[0] + (size_t)y * strides[0], 1, (size_t)p->out_w * bps, dump);
        for (int k = 1; k < 3; k++)
            for (int y = 0; y < ch; y++) fwrite(planes[k] + (size_t)y * strides[k], 1, (size_t)cw * bps, dump);
    }
    a[0] = adler(planes[0], (int)p->out_w * bps, (int)p->out_h, strides[0]);
    a[1] = adler(planes[1], cw * bps, ch, strides[1]);
    a[2] = adler(planes[2], cw * bps, ch, strides[2]);
    if (a[0] != p->crc[0] || a[1] != p->crc[1] || a[2] != p->crc[2]) {
        if (++shown <= 10)
            say("Picture %u (poc %d): Y %s, U %s, V %s (%08X %08X %08X, FFmpeg's %08X %08X %08X)\n",
                (unsigned)p->number, (int)p->poc, a[0] == p->crc[0] ? "right" : "WRONG",
                a[1] == p->crc[1] ? "right" : "WRONG", a[2] == p->crc[2] ? "right" : "WRONG", (unsigned)a[0],
                (unsigned)a[1], (unsigned)a[2], (unsigned)p->crc[0], (unsigned)p->crc[1], (unsigned)p->crc[2]);
        return 1;
    }
    if (verbose) say("Picture %u (poc %d): right\n", (unsigned)p->number, (int)p->poc);
    return 0;
}

/* -K: picture p (in frame f) converted each way, BENCH_N times, timed, and
   each way's pictures compared with the default's: 0, or 1 if one differs */
#define BENCH_N 30
static int benchmark(hevcdec *d, hevcdec_frame *f, const pic_t *p, uint8_t *const planes[3], const int strides[3])
{
    static const char *const names[HEVCDEC_CONVERT_WAYS] = { "column by column (the default)", "row by row",
                                                          "column by column, preloading", "through a row buffer (0.1.7)" };
    int bad = 0, cw = ((int)p->out_w + 1) / 2, ch = ((int)p->out_h + 1) / 2;
    say("\n-K: picture %u (%ux%u) converted %d times each way:\n", (unsigned)p->number, (unsigned)p->out_w,
        (unsigned)p->out_h, BENCH_N);
    for (int bits = 8; bits <= 16; bits += 8) {
        uint8_t *const *pl = bits == 16 || p->depth == 8 ? planes : planes8;
        const int *st = bits == 16 || p->depth == 8 ? strides : strides8;
        int bps = bits == 16 ? 2 : 1;
        uint8_t *ref[3] = { NULL, NULL, NULL };
        size_t sz[3];
        if (bits == 16 && p->depth == 8) break;
        sz[0] = (size_t)st[0] * p->out_h; sz[1] = (size_t)st[1] * ch; sz[2] = (size_t)st[2] * ch;
        for (int way = 0; way < HEVCDEC_CONVERT_WAYS; way++) {
            unsigned cs = 0;
            int same = 1;
            void *pv[3] = { pl[0], pl[1], pl[2] };
            for (int k = 0; k < 3; k++) memset(pl[k], 0x5A, sz[k]);
            if (hevcdec_convert_benchmark(d, f, pv, st, bits, (int)p->left, (int)p->top, (int)p->out_w, (int)p->out_h, way,
                                          BENCH_N, &cs) != HEVCDEC_OK)
                continue;
            if (way == 0) {
                for (int k = 0; k < 3; k++) { ref[k] = malloc(sz[k]); if (ref[k]) memcpy(ref[k], pl[k], sz[k]); }
            } else {
                for (int k = 0; k < 3; k++) {
                    int rw = (k ? cw : (int)p->out_w) * bps, rh = k ? ch : (int)p->out_h;
                    for (int y = 0; y < rh && same && ref[k]; y++)
                        same = !memcmp(pl[k] + (size_t)y * st[k], ref[k] + (size_t)y * st[k], (size_t)rw);
                }
            }
            say("  to %d-bit, %-32s %u.%02u ms a picture%s\n", bits == 16 ? 10 : 8, names[way], cs * 10 / BENCH_N,
                cs * 1000 / BENCH_N % 100, same ? "" : " - WRONG (not the default's pictures)");
            bad |= !same;
        }
        for (int k = 0; k < 3; k++) free(ref[k]);
    }
    if (planesh[0] && !(p->left & 3)) {        /* (0.1.10) halved, straight into 8-bit planes a quarter the size */
        uint32_t t0 = now_cs(), cs;
        for (int i = 0; i < BENCH_N; i++)
            hevcdec_frame_to_i420_half(d, f, planesh, stridesh, (int)p->left, (int)p->top, (int)p->out_w / 2,
                                       (int)p->out_h / 2);
        cs = now_cs() - t0;
        say("  halved to 8-bit (%ux%u)%*s %u.%02u ms a picture\n", (unsigned)p->out_w / 2, (unsigned)p->out_h / 2, 14, "",
            (unsigned)(cs * 10 / BENCH_N), (unsigned)(cs * 1000 / BENCH_N % 100));
    }
    return bad;
}

int probe_main(int argc, char **argv)
{
    const char *name = NULL;
    int verbose = 0, keep_going = 0, timing = 0, count = 0, nframes = 17, r, i, wrong = 0, done = 0, fatal = 0;
    int flat = 0;                          /* -s: flat scaling lists given where the stream has none */
    int overran = 0;                       /* the block wrote past a buffer's end */
    int uncached = 0;                      /* -u: output frames not cacheable (as before 0.1.4) */
    int quick = 0;                         /* -q: start, write a line, stop (no decoding: the hardware untouched) */
    int pipelined = 0;                     /* -p: pictures given to the block without waiting; each checked
                                              while the block decodes the next */
    int lag = 1;                           /* -P n: n pictures given and not yet checked (-p: 1) */
    int pend[MAX_FRAMES], pend_slot[MAX_FRAMES], npend = 0;
    uint32_t t_all = 0;                    /* the whole run, decoding and checking */
    FILE *dump = NULL;                     /* -d: every picture decoded, 4:2:0 (10-bit: 16-bit LE), in decoding order */
    int spoil = 0;                         /* (host tests: -x N spoils picture N's second slice) */
    int no_wait = 0;                       /* (host tests: -w, the picture converted without waiting first) */
    int force_depth = 0;                   /* (host tests: -D n, a decoder for n-bit whatever the trace) */
    int bench = 0;                         /* -K: the last picture converted each way, timed */
    int last_fs = -1;                      /* (the last picture decoded: its frame and picture) */
    const pic_t *last_p = NULL;
    int bench_bad = 0;                     /* -K: a way gave other pictures than the default's */
    uint8_t *file = NULL, *planes[3] = { NULL, NULL, NULL };
    int strides[3];
    hevcdec *d = NULL;
    hevcdec_config c;
    hevcdec_frame *frames[MAX_FRAMES];
    uint64_t holds[MAX_FRAMES];
    uint32_t t0, t_dec = 0, t_conv = 0;
    hevcdec_stats st;
    int page_moved = 0;                    /* the program's page at &8000 moved (ARMEABISupport loses it) */
#ifndef PROBE_TEST
    contig_app_page page0 = { 0, 0, 0, 0 };
#endif

    out2 = NULL; pics = NULL; npics = 0; shown = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out2 = fopen(argv[++i], "a");
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-n")) keep_going = 1;
        else if (!strcmp(argv[i], "-t")) timing = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) nframes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s")) flat = 1;
        else if (!strcmp(argv[i], "-u")) uncached = 1;
        else if (!strcmp(argv[i], "-q")) quick = 1;
        else if (!strcmp(argv[i], "-K")) bench = 1;
        else if (!strcmp(argv[i], "-p")) pipelined = 1;
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) pipelined = 1, lag = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            if (!(dump = fopen(argv[++i], "wb"))) { printf("Can't write %s\n", argv[i]); return 1; }
        }
#ifdef PROBE_TEST
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) spoil = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-D") && i + 1 < argc) force_depth = atoi(argv[++i]);   /* (host tests: the decoder's depth) */
        else if (!strcmp(argv[i], "-w")) no_wait = 1;   /* (host tests: converting waits by itself) */
        else if (!strcmp(argv[i], "-z")) flat = 2;   /* (host tests: the flag without the lists) */
#endif
        else if (!name) name = argv[i];
        else name = NULL, i = argc;
    }
    if (quick) {
        say("hevctest %s: started and wrote this (-q: nothing decoded, the HEVC block untouched)\n", HEVCDEC_VERSION);
        if (out2) fclose(out2);
        if (dump) fclose(dump);
        return 0;
    }
    if (!name || nframes < 2 || nframes > MAX_FRAMES || count < 0 || lag < 1 || lag >= nframes) {
        printf("Usage: hevctest [-o file] [-v] [-n] [-t] [-s] [-u] [-p | -P n] [-q] [-d file] [-c count] [-f frames] trace\n");
        if (dump) fclose(dump);
        return 1;
    }
    say("hevctest: %s through hevcdec %s\n", name, HEVCDEC_VERSION);
    if (read_trace(name, &file)) goto out;
    if (!npics) { say("No pictures in the trace\n"); goto out; }
    if (count && count < npics) npics = count;
    if (spoil > 0 && spoil <= npics && pics[spoil - 1].nslices > 1)
        pics[spoil - 1].params[1].data_byte_offset = pics[spoil - 1].params[1].bit_size;   /* (refused by setup) */
    if (flat) {                            /* the same pictures: flat lists are what "no scaling lists" means */
        int n = 0;
        for (i = 0; i < npics; i++) {
            if (pics[i].sps.flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED) continue;
            pics[i].sps.flags |= V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED;
            memset(&pics[i].scaling, 16, sizeof pics[i].scaling);
            pics[i].has_scaling = flat == 1;
            n++;
        }
        say("Flat scaling lists given to %d pictures (-s)\n", n);
    }
    say("Trace: %d pictures, %ux%u (shown %ux%u) %u-bit\n", npics, (unsigned)pics[0].width, (unsigned)pics[0].height,
        (unsigned)pics[0].out_w, (unsigned)pics[0].out_h, (unsigned)pics[0].depth);
    hevcdec_config_init(&c);
    c.width = 0; c.height = 0;
    for (i = 0; i < npics; i++) {
        if ((int)pics[i].width > c.width) c.width = (int)pics[i].width;
        if ((int)pics[i].height > c.height) c.height = (int)pics[i].height;
    }
    c.bit_depth = force_depth ? force_depth : (int)pics[0].depth;
    if (uncached) c.cached_frames = 0;
    c.pipelined = pipelined;
    if (verbose) c.log = log_line;
#ifndef PROBE_TEST
    page0 = contig_read_app_page(ht_swi);
#endif
    t0 = now_cs();
    if ((r = hevcdec_open(&d, &c)) != HEVCDEC_OK) {
        say("hevcdec_open: %s%s\n", r == HEVCDEC_UNSUPPORTED ? "not for the block: " : "", hevcdec_open_error());
        goto out;
    }
    say("hevcdec_open: %u cs\n", (unsigned)(now_cs() - t0));
    for (i = 0; i < nframes; i++) {
        if (!(frames[i] = hevcdec_frame_new(d))) {
            say("Frame %d: %s\n", i, hevcdec_error(d));
            goto out;
        }
        holds[i] = 0;
    }
    {
        int bps = c.bit_depth > 8 ? 2 : 1;
        strides[0] = c.width * bps;
        strides[1] = strides[2] = (c.width + 1) / 2 * bps;
        planes[0] = malloc((size_t)strides[0] * c.height);
        planes[1] = malloc((size_t)strides[1] * ((c.height + 1) / 2));
        planes[2] = malloc((size_t)strides[2] * ((c.height + 1) / 2));
        if (!planes[0] || !planes[1] || !planes[2]) { say("Out of memory\n"); goto out; }
        stridesh[0] = c.width / 2 + 16;
        stridesh[1] = stridesh[2] = c.width / 4 + 16;
        planesh[0] = malloc((size_t)stridesh[0] * (c.height / 2 + 1));
        planesh[1] = malloc((size_t)stridesh[1] * (c.height / 4 + 1));
        planesh[2] = malloc((size_t)stridesh[2] * (c.height / 4 + 1));
        if (!planesh[0] || !planesh[1] || !planesh[2]) { say("Out of memory\n"); goto out; }
        if (bps == 2) {
            strides8[0] = c.width;
            strides8[1] = strides8[2] = (c.width + 1) / 2;
            planes8[0] = malloc((size_t)c.width * c.height);
            planes8[1] = malloc((size_t)strides8[1] * ((c.height + 1) / 2));
            planes8[2] = malloc((size_t)strides8[2] * ((c.height + 1) / 2));
            if (!planes8[0] || !planes8[1] || !planes8[2]) { say("Out of memory\n"); goto out; }
        }
    }

    t_all = now_cs();
    for (i = 0; i <= npics && !fatal; i++) {
        int given = -1;                    /* this picture's frame, if it was given to the block */
        if (i < npics) {
            pic_t *p = &pics[i];
            hevcdec_picture hp;
            int k, slot = -1;
            /* a frame no reference of this picture is in (nor the picture
               given last, still to be checked: -p) */
            for (k = 0; k < nframes && slot < 0; k++) {
                int in_dpb = 0;
                for (int e = 0; e < p->dec.num_active_dpb_entries; e++) in_dpb |= p->dec.dpb[e].timestamp == holds[k];
                int pending = 0;
                for (int q = 0; q < npend; q++) pending |= pend_slot[q] == k;
                if ((!in_dpb || !holds[k]) && !pending) slot = k;
            }
            if (slot < 0) { say("Picture %u: no free frame (%d in use)\n", (unsigned)p->number, nframes); fatal = 1; break; }
            hp.sps = &p->sps; hp.pps = &p->pps; hp.dec = &p->dec; hp.scaling = p->has_scaling ? &p->scaling : NULL;
            hp.nslices = p->nslices; hp.slices = p->slices;
            t0 = now_cs();
            r = hevcdec_decode(d, &hp, frames[slot], p->number);
            t_dec += now_cs() - t0;
            holds[slot] = p->number;
            if (r != HEVCDEC_OK) {
                say("Picture %u (poc %d, %u slices): %s\n", (unsigned)p->number, (int)p->poc, (unsigned)p->nslices,
                    hevcdec_error(d));
                wrong++;
                if (!keep_going) fatal = 1;
            } else if (!pipelined) {
                done++;
                last_fs = slot; last_p = p;
                if (!timing && check_one(d, p, frames[slot], planes, strides, dump, verbose, &t_conv)) {
                    wrong++;
                    if (!keep_going) fatal = 1;
                }
            } else {
                given = slot;
            }
        }
        if (given >= 0) { pend[npend] = i; pend_slot[npend] = given; npend++; }
        while (npend > (i < npics ? lag : 0)) {   /* the oldest picture given, waited for and checked */
            pic_t *p = &pics[pend[0]];
            int fs = pend_slot[0];
            for (int q = 1; q < npend; q++) { pend[q - 1] = pend[q]; pend_slot[q - 1] = pend_slot[q]; }
            npend--;
            t0 = now_cs();
            r = no_wait ? HEVCDEC_OK : hevcdec_frame_wait(d, frames[fs]);
            t_dec += now_cs() - t0;
            if (r != HEVCDEC_OK) {
                say("Picture %u (poc %d, %u slices): %s\n", (unsigned)p->number, (int)p->poc, (unsigned)p->nslices,
                    hevcdec_error(d));
                wrong++;
                if (!keep_going) fatal = 1;
            } else {
                done++;
                last_fs = fs; last_p = p;
                if (!timing && check_one(d, p, frames[fs], planes, strides, dump, verbose, &t_conv)) {
                    wrong++;
                    if (!keep_going) fatal = 1;
                }
            }
        }
    }
    t_all = now_cs() - t_all;
    if (bench && last_fs >= 0 && !fatal) bench_bad = benchmark(d, frames[last_fs], last_p, planes, strides);
    hevcdec_get_stats(d, &st);
    say("\n%d of %d pictures decoded in %u.%02u s (%u.%u a second); converting them %u cs\n", done, npics,
        (unsigned)(t_dec / 100), (unsigned)(t_dec % 100), t_dec ? (unsigned)(done * 100 / t_dec) : 0,
        t_dec ? (unsigned)(done * 1000 / t_dec % 10) : 0, (unsigned)t_conv);
    say("In all %u.%02u s%s; the program waited for the block %u cs (%u times)\n", (unsigned)(t_all / 100),
        (unsigned)(t_all % 100), pipelined ? " (pipelined: -p)" : "", st.cs_wait, st.waits);
    say("hevcdec: phase 1 ran %u cs, phase 2 %u cs; phase 1 run again (buffers grown) %u times\n", st.cs_phase1,
        st.cs_phase2, st.phase1_retries);
    say("hevcdec: output frames %s%s", st.cached_frames ? "cacheable" : "not cacheable",
        st.cached_frames ? "" : uncached ? " (-u)\n" : " (no cache maintenance)\n");
    if (st.cached_frames) say(" (cleaning and invalidating them before converting: %u cs of that)\n", st.cs_cache);
    if (!timing) say("%d checked against FFmpeg: %d wrong\n", done, wrong);
    if (st.overruns) {
        say("hevcdec: the block wrote past the end of %u buffers, at most %u bytes past %s (into their guard areas)\n",
            st.overruns, st.overrun_max, st.overrun_what ? st.overrun_what : "one");
        overran = 1;
    } else {
        say("hevcdec: nothing written past any buffer's end\n");
    }
    if (st.app_page_moves) {
        say("hevcdec: claiming memory moved the program's page at &8000 %u times\n", st.app_page_moves);
        page_moved = 1;
    }
out:
    if (d) hevcdec_close(d);
#ifndef PROBE_TEST
    if (page0.have_pn || page0.have_pa) {
        contig_app_page p1 = contig_read_app_page(ht_swi);
        int moved = contig_app_page_moved(ht_swi, &page0);
        say("The program's page at &8000: &%08X (page %u) at the start, &%08X (page %u) at the end: %s\n",
            (unsigned)page0.pa, (unsigned)page0.pn, (unsigned)p1.pa, (unsigned)p1.pn,
            moved ? "MOVED - ARMEABISupport has lost this program (reboot before the next test)" : "not moved");
        if (moved) page_moved = 1;
    }
#endif
    {
        int ok = d && !fatal && !wrong && !overran && !page_moved && !bench_bad && done == npics && npics > 0;
        say("\nResult: %s\n", ok ? (timing ? "OK - timed (the pictures weren't checked)" :
                                    "OK - every picture exactly as FFmpeg decodes it")
                                 : "FAILED");
        for (i = 0; i < npics; i++) { free(pics[i].slices); free(pics[i].params); }
        free(pics);
        free(file);
        free(planes[0]); free(planes[1]); free(planes[2]);
        free(planes8[0]); free(planes8[1]); free(planes8[2]);
        planes8[0] = planes8[1] = planes8[2] = NULL;
        free(planesh[0]); free(planesh[1]); free(planesh[2]);
        planesh[0] = planesh[1] = planesh[2] = NULL;
        if (out2) fclose(out2);
        if (dump) fclose(dump);
        return ok ? 0 : 1;
    }
}

#ifndef PROBE_TEST
#include <signal.h>
#include <unistd.h>
#include <kernel.h>
/* a hardware exception (an abort, an undefined instruction): what RISC OS
   said, before UnixLib's backtrace. (UnixLib's error handler keeps it in
   __ul_errbuf: the pc, the error number and message.) */
extern struct { void *pc; int errnum; char errmess[252]; } __ul_errbuf;
static void on_fatal(int sig)
{
    const _kernel_oserror *e = _kernel_last_oserror();
    printf("\nhevctest: signal %d: RISC OS error &%X at pc &%08X: %.200s\n", sig, (unsigned)__ul_errbuf.errnum,
           (unsigned)(uintptr_t)__ul_errbuf.pc, __ul_errbuf.errmess);
    if (e) printf("  (last error: &%X, %s)\n", (unsigned)e->errnum, e->errmess);
    fflush(stdout);
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char **argv)
{
    signal(SIGEMT, on_fatal);
    signal(SIGSEGV, on_fatal);
    signal(SIGBUS, on_fatal);
    signal(SIGILL, on_fatal);
    return probe_main(argc, argv);
}
#endif
