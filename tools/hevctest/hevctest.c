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
#include "hevcdec.h"
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

int probe_main(int argc, char **argv)
{
    const char *name = NULL;
    int verbose = 0, keep_going = 0, timing = 0, count = 0, nframes = 17, r, i, wrong = 0, done = 0, fatal = 0;
    int flat = 0;                          /* -s: flat scaling lists given where the stream has none */
    int overran = 0;
    FILE *dump = NULL;                     /* -d: every picture decoded, 8-bit 4:2:0, in decoding order */
    int spoil = 0;                         /* (host tests: -x N spoils picture N's second slice) */
    uint8_t *file = NULL, *planes[3] = { NULL, NULL, NULL };
    int strides[3];
    hevcdec *d = NULL;
    hevcdec_config c;
    hevcdec_frame *frames[MAX_FRAMES];
    uint64_t holds[MAX_FRAMES];
    uint32_t t0, t_dec = 0, t_conv = 0;
    hevcdec_stats st;

    out2 = NULL; pics = NULL; npics = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out2 = fopen(argv[++i], "a");
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-n")) keep_going = 1;
        else if (!strcmp(argv[i], "-t")) timing = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) nframes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s")) flat = 1;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            if (!(dump = fopen(argv[++i], "wb"))) { printf("Can't write %s\n", argv[i]); return 1; }
        }
#ifdef PROBE_TEST
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) spoil = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-z")) flat = 2;   /* (host tests: the flag without the lists) */
#endif
        else if (!name) name = argv[i];
        else name = NULL, i = argc;
    }
    if (!name || nframes < 2 || nframes > MAX_FRAMES || count < 0) {
        printf("Usage: hevctest [-o file] [-v] [-n] [-t] [-s] [-d file] [-c count] [-f frames] trace\n");
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
    c.bit_depth = (int)pics[0].depth;
    if (verbose) c.log = log_line;
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
    strides[0] = c.width;
    strides[1] = strides[2] = (c.width + 1) / 2;
    planes[0] = malloc((size_t)c.width * c.height);
    planes[1] = malloc((size_t)strides[1] * ((c.height + 1) / 2));
    planes[2] = malloc((size_t)strides[2] * ((c.height + 1) / 2));
    if (!planes[0] || !planes[1] || !planes[2]) { say("Out of memory\n"); goto out; }

    for (i = 0; i < npics && !fatal; i++) {
        pic_t *p = &pics[i];
        hevcdec_picture hp;
        int k, slot = -1;
        /* a frame no reference of this picture is in */
        for (k = 0; k < nframes && slot < 0; k++) {
            int in_dpb = 0;
            for (int e = 0; e < p->dec.num_active_dpb_entries; e++) in_dpb |= p->dec.dpb[e].timestamp == holds[k];
            if (!in_dpb || !holds[k]) slot = k;
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
            continue;
        }
        done++;
        if (timing) continue;
        t0 = now_cs();
        hevcdec_frame_to_i420(d, frames[slot], planes, strides, (int)p->left, (int)p->top, (int)p->out_w, (int)p->out_h);
        t_conv += now_cs() - t0;
        if (dump) {
            int cw = ((int)p->out_w + 1) / 2, ch = ((int)p->out_h + 1) / 2, y;
            for (y = 0; y < (int)p->out_h; y++) fwrite(planes[0] + (size_t)y * strides[0], 1, p->out_w, dump);
            for (k = 1; k < 3; k++)
                for (y = 0; y < ch; y++) fwrite(planes[k] + (size_t)y * strides[k], 1, (size_t)cw, dump);
        }
        {
            int cw = ((int)p->out_w + 1) / 2, ch = ((int)p->out_h + 1) / 2;
            uint32_t a[3] = { adler(planes[0], (int)p->out_w, (int)p->out_h, strides[0]),
                              adler(planes[1], cw, ch, strides[1]), adler(planes[2], cw, ch, strides[2]) };
            if (a[0] != p->crc[0] || a[1] != p->crc[1] || a[2] != p->crc[2]) {
                if (++wrong <= 10)
                    say("Picture %u (poc %d): Y %s, U %s, V %s (%08X %08X %08X, FFmpeg's %08X %08X %08X)\n",
                        (unsigned)p->number, (int)p->poc, a[0] == p->crc[0] ? "right" : "WRONG",
                        a[1] == p->crc[1] ? "right" : "WRONG", a[2] == p->crc[2] ? "right" : "WRONG", (unsigned)a[0],
                        (unsigned)a[1], (unsigned)a[2], (unsigned)p->crc[0], (unsigned)p->crc[1], (unsigned)p->crc[2]);
                if (!keep_going) fatal = 1;
            } else if (verbose) {
                say("Picture %u (poc %d): right\n", (unsigned)p->number, (int)p->poc);
            }
        }
    }
    hevcdec_get_stats(d, &st);
    say("\n%d of %d pictures decoded in %u.%02u s (%u.%u a second); converting them %u cs\n", done, npics,
        (unsigned)(t_dec / 100), (unsigned)(t_dec % 100), t_dec ? (unsigned)(done * 100 / t_dec) : 0,
        t_dec ? (unsigned)(done * 1000 / t_dec % 10) : 0, (unsigned)t_conv);
    say("hevcdec: phase 1 waited %u cs, phase 2 %u cs; phase 1 run again (buffers grown) %u times\n", st.cs_phase1,
        st.cs_phase2, st.phase1_retries);
    if (!timing) say("%d checked against FFmpeg: %d wrong\n", done, wrong);
    if (st.overruns) {
        say("hevcdec: the block wrote past the end of %u buffers, at most %u bytes past %s (into their guard areas)\n",
            st.overruns, st.overrun_max, st.overrun_what ? st.overrun_what : "one");
        overran = 1;
    } else {
        say("hevcdec: nothing written past any buffer's end\n");
    }
out:
    if (d) hevcdec_close(d);
    {
        int ok = d && !fatal && !wrong && !overran && done == npics && npics > 0;
        say("\nResult: %s\n", ok ? (timing ? "OK - timed (the pictures weren't checked)" :
                                    "OK - every picture exactly as FFmpeg decodes it")
                                 : "FAILED");
        for (i = 0; i < npics; i++) { free(pics[i].slices); free(pics[i].params); }
        free(pics);
        free(file);
        free(planes[0]); free(planes[1]); free(planes[2]);
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
   said, before UnixLib's backtrace */
static void on_fatal(int sig)
{
    const _kernel_oserror *e = _kernel_last_oserror();
    printf("\nhevctest: signal %d", sig);
    if (e) printf(": error &%X, %s", (unsigned)e->errnum, e->errmess);
    printf("\n");
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
