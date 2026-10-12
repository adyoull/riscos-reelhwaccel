/*
 * vcenctest - the vcenc encoder (vcdec/vcenc.c, in libvcdec) on the Pi:
 * test pictures encoded to H.264 by the VideoCore, as a program would
 * drive it, then (with -d) the stream decoded again by the VideoCore
 * (vcdec) and each picture compared with what went in (PSNR).
 *
 * Each turn of the loop is what a Wimp program does on a null event: send
 * pictures until vcenc says the input is full (VCENC_AGAIN), then take
 * every packet that's ready. The pictures are made before the loop (a
 * cycle of up to 8, a pattern that moves), so the time is the encoder's.
 *
 *   vcenctest [-o report] [-w stream] [-v] [-d] [-t] [-s WxH] [-n frames] [-r fps] [-b bitrate]
 *             [-g gop] [-p high|main|baseline] [-l level] [-i] [-c] [-k N] [-q min,max] [-B n] [-m pci] [-G]
 *      -o file   also add the report to file
 *      -w file   the stream (H.264 Annex B) to file
 *      -v        every MMAL message, and what vcenc does
 *      -d        decode the stream again with vcdec and compare (PSNR)
 *      -t        a shorter report (no per-packet lines)
 *      -s WxH    the size (default 640x360)
 *      -n N      pictures (default 60)
 *      -r fps    frames a second (default 25)
 *      -b bits   bits a second (default: vcenc's, 10 Mbit/s)
 *      -g N      a keyframe every N pictures (default: the encoder's)
 *      -p name   profile (default high)
 *      -l level  H.264 level times 10, e.g. 41 (default: vcenc's choice)
 *      -i        SPS and PPS again before every keyframe
 *      -c        constant bit rate
 *      -k N      ask for a keyframe at picture N
 *      -q a,b    quantiser limits
 *      -B n      input buffers (vcenc's default 2)
 *      -m pci    buffers in PCI memory (not Physical Memory Pools)
 *      -G        open even if gpu_mem looks too small
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "vcenc.h"
#include "vcdec.h"

#define VCENCTEST_VERSION "0.1"
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

#include "../../common/contig.h"           /* (the page at &8000, read before and after) */

static uint32_t now_cs(void)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof r);
    probe_swi(OS_ReadMonotonicTime, &r);
    return (uint32_t)r.r[0];
}

static void logcb(void *h, const char *t) { (void)h; say("  %s\n", t); }

/* ---- the pictures: a cycle of up to 8 ---- */

static int W, H, CW, CH, ncycle;
static uint8_t *cycle[8];

/* picture j of the cycle: gradients, a fine texture and a square, moving */
static void make_picture(uint8_t *p, int j)
{
    uint8_t *y = p, *u = p + (size_t)W * H, *v = u + (size_t)CW * CH;
    int sx = (W / 4) + j * (W / 32), sy = H / 4 + j * (H / 48), sq = H / 4;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            int val = (c * 160 / W) + (r * 64 / H) + 16 + (((c + j * 4) >> 2 ^ r >> 2) & 1) * 12;
            if (c >= sx && c < sx + sq && r >= sy && r < sy + sq) val = 230 - ((c - sx) * 40 / sq);
            y[(size_t)r * W + c] = (uint8_t)val;
        }
    for (int r = 0; r < CH; r++)
        for (int c = 0; c < CW; c++) {
            int in = 2 * c >= sx && 2 * c < sx + sq && 2 * r >= sy && 2 * r < sy + sq;
            u[(size_t)r * CW + c] = (uint8_t)(in ? 60 : 128 + (c * 64 / CW) - 32);
            v[(size_t)r * CW + c] = (uint8_t)(in ? 200 : 128 + (r * 64 / CH) - 32 + j);
        }
}

/* ---- the packets, kept for the decode ---- */

typedef struct { uint8_t *data; size_t size; int64_t pts; unsigned flags; } pkt_t;
static pkt_t *pkts;
static int npkts;
static uint8_t *hdr;
static size_t hdr_len;

/* the NAL types in an Annex B unit, as a short list ("7 8 5") */
static void nal_types(const uint8_t *d, size_t n, char *out, size_t on)
{
    size_t o = 0;
    out[0] = 0;
    for (size_t i = 0; i + 3 < n && o + 4 < on; i++)
        if (!d[i] && !d[i + 1] && d[i + 2] == 1) {
            o += (size_t)snprintf(out + o, on - o, "%s%d", o ? " " : "", d[i + 3] & 31);
            i += 2;
        }
}

static int first_nal(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) if (!d[i] && !d[i + 1] && d[i + 2] == 1) return d[i + 3] & 31;
    return -1;
}

static double psnr(double sse, double n) { return sse <= 0 ? 99.0 : 10.0 * log10(255.0 * 255.0 * n / sse); }

/* ---- the decode check ---- */

static int decode_check(int frames, unsigned gpu_flag, double *avg_y, double *min_y, int *pictures)
{
    vcdec *d;
    vcdec_config c;
    vcdec_picture pic;
    uint8_t *planes[3], *buf = malloc((size_t)W * H + 2 * (size_t)CW * CH);
    int strides[3] = { W, CW, CW }, sent = 0, eos = 0, got = 0, r, bad = 0;
    double sum = 0;
    uint8_t *tmp = NULL;
    *min_y = 99; *avg_y = 0; *pictures = 0;
    if (!buf) { say("Decode: out of memory\n"); return -1; }
    planes[0] = buf; planes[1] = buf + (size_t)W * H; planes[2] = planes[1] + (size_t)CW * CH;
    vcdec_config_init(&c);
    c.width = W; c.height = H; c.flags = gpu_flag ? VCDEC_NO_GPU_MEM_CHECK : 0;
    if ((r = vcdec_open(&d, &c)) != VCDEC_OK) { say("Decode: vcdec_open: %s\n", vcdec_open_error()); free(buf); return -1; }
    for (uint32_t t0 = now_cs(); now_cs() - t0 < 6000;) {
        while (sent < npkts) {               /* SPS and PPS before every keyframe (vcdec wants them in band) */
            const pkt_t *p = &pkts[sent];
            const void *data = p->data;
            size_t len = p->size;
            if ((p->flags & VCENC_PKT_KEY) && first_nal(p->data, p->size) != 7 && hdr_len) {
                free(tmp);
                tmp = malloc(hdr_len + p->size);
                if (!tmp) break;
                memcpy(tmp, hdr, hdr_len);
                memcpy(tmp + hdr_len, p->data, p->size);
                data = tmp; len = hdr_len + p->size;
            }
            r = vcdec_send(d, data, len, p->pts, p->pts, p->flags & VCENC_PKT_KEY ? VCDEC_KEYFRAME : 0);
            if (r == VCDEC_AGAIN) break;
            if (r != VCDEC_OK) { say("Decode: vcdec_send: %s\n", vcdec_error(d)); bad = 1; break; }
            sent++;
        }
        if (bad) break;
        if (sent == npkts && !eos && vcdec_send_eos(d) == VCDEC_OK) eos = 1;
        while ((r = vcdec_receive(d, &pic, planes, strides)) == VCDEC_OK) {
            int k = (int)(pic.pts / 1000);   /* (pts in ms: the picture's number * 1000) */
            double sse = 0;
            const uint8_t *src;
            if (k < 0 || k >= frames || pic.width != W || pic.height != H) {
                say("Decode: a picture %dx%d with pts %lld (not one sent)\n", pic.width, pic.height, (long long)pic.pts);
                bad = 1;
                continue;
            }
            src = cycle[k % ncycle];
            for (size_t i = 0; i < (size_t)W * H; i++) { int e = buf[i] - src[i]; sse += e * e; }
            {
                double py = psnr(sse, (double)W * H);
                sum += py;
                if (py < *min_y) *min_y = py;
            }
            got++;
        }
        if (r == VCDEC_EOF) break;
        if (r != VCDEC_AGAIN) { say("Decode: vcdec_receive: %s\n", vcdec_error(d)); bad = 1; break; }
    }
    vcdec_close(d);
    free(tmp);
    free(buf);
    *pictures = got;
    *avg_y = got ? sum / got : 0;
    return bad ? -1 : 0;
}

static int parse_size(const char *s, int *w, int *h) { return sscanf(s, "%dx%d", w, h) == 2 ? 0 : -1; }

int probe_main(int argc, char **argv)
{
    vcenc_config c;
    vcenc *e = NULL;
    vcenc_packet pk;
    vcenc_stats st;
    const char *report = NULL, *stream = NULL;
    int verbose = 0, check = 0, terse = 0, frames = 60, force_at = -1, r, sent = 0, eos_sent = 0, got = 0, again = 0;
    int keys = 0, configs = 0, wrong = 0, eof = 0, fatal = 0, first_config = -1, inline_bad = 0, moved;
    FILE *sf = NULL;
    uint32_t t0, t_open, t_loop, t_close;
    contig_app_page page0;
    char nt[64];
    vcenc_config_init(&c);
    W = 640; H = 360;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *n = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-o") && n) { report = n; i++; }
        else if (!strcmp(a, "-w") && n) { stream = n; i++; }
        else if (!strcmp(a, "-v")) verbose = 1;
        else if (!strcmp(a, "-d")) check = 1;
        else if (!strcmp(a, "-t")) terse = 1;
        else if (!strcmp(a, "-s") && n && !parse_size(n, &W, &H)) i++;
        else if (!strcmp(a, "-n") && n) { frames = atoi(n); i++; }
        else if (!strcmp(a, "-r") && n) { c.fps_num = atoi(n); c.fps_den = 1; i++; }
        else if (!strcmp(a, "-b") && n) { c.bitrate = (unsigned)strtoul(n, NULL, 10); i++; }
        else if (!strcmp(a, "-g") && n) { c.gop = atoi(n); i++; }
        else if (!strcmp(a, "-p") && n) {
            c.profile = !strcmp(n, "main") ? VCENC_PROFILE_MAIN : !strcmp(n, "baseline") ? VCENC_PROFILE_BASELINE : VCENC_PROFILE_HIGH;
            i++;
        }
        else if (!strcmp(a, "-l") && n) { c.level = atoi(n); i++; }
        else if (!strcmp(a, "-i")) c.flags |= VCENC_INLINE_HEADERS;
        else if (!strcmp(a, "-c")) c.flags |= VCENC_CBR;
        else if (!strcmp(a, "-k") && n) { force_at = atoi(n); i++; }
        else if (!strcmp(a, "-q") && n) { sscanf(n, "%d,%d", &c.qp_min, &c.qp_max); i++; }
        else if (!strcmp(a, "-B") && n) { c.in_buffers = atoi(n); i++; }
        else if (!strcmp(a, "-m") && n && !strcmp(n, "pci")) { c.flags |= VCENC_PCI; i++; }
        else if (!strcmp(a, "-G")) c.flags |= VCENC_NO_GPU_MEM_CHECK;
        else {
            printf("vcenctest %s: unknown or incomplete option %s (see the ReadMe)\n", VCENCTEST_VERSION, a);
            return 2;
        }
    }
    if (report && !(out2 = fopen(report, "a"))) printf("Can't open %s\n", report);
    say("\n==== VCEncTest %s (vcenc %s, vcdec %s): %dx%d, %d pictures", VCENCTEST_VERSION, VCENC_VERSION, VCDEC_VERSION, W, H,
        frames);
    say("%s%s%s%s%s\n", c.flags & VCENC_INLINE_HEADERS ? ", -i" : "", c.flags & VCENC_CBR ? ", -c" : "",
        c.flags & VCENC_PCI ? ", PCI memory" : "", check ? ", decoded again" : "", force_at >= 0 ? ", a keyframe asked for" : "");
    {
        char lv[16], br[24], gp[16];
        snprintf(lv, sizeof lv, c.level ? "%d.%d" : "vcenc's", c.level / 10, c.level % 10);
        snprintf(br, sizeof br, c.bitrate ? "%u bit/s" : "vcenc's bit rate", c.bitrate);
        snprintf(gp, sizeof gp, c.gop ? "every %d" : "the encoder's", c.gop);
        say("Profile %s, level %s, %s, keyframes %s, %d a second; the VideoCore has %u MB (gpu_mem)\n",
            c.profile == VCENC_PROFILE_MAIN ? "main" : c.profile == VCENC_PROFILE_BASELINE ? "baseline" : "high", lv, br, gp,
            c.fps_num ? c.fps_num : 25, vcdec_gpu_mem());
    }
    if (W < 2 || H < 2 || W > 1920 || H > 1088 || frames < 1 || frames > 100000) { say("Bad size or count\n"); return 2; }
    CW = (W + 1) / 2; CH = (H + 1) / 2;
    c.width = W; c.height = H;
    ncycle = frames < 8 ? frames : (size_t)W * H > 1280 * 720 ? 4 : 8;
    for (int j = 0; j < ncycle; j++) {
        if (!(cycle[j] = malloc((size_t)W * H + 2 * (size_t)CW * CH))) { say("Out of memory for the pictures\n"); return 1; }
        make_picture(cycle[j], j);
    }
    if (!(pkts = calloc((size_t)frames + 2, sizeof *pkts))) { say("Out of memory\n"); return 1; }
    if (stream && !(sf = fopen(stream, "wb"))) say("Can't write %s\n", stream);
    if (verbose) c.flags |= VCENC_LOG_MESSAGES;
    c.log = logcb;
    page0 = contig_read_app_page(probe_swi);
    t0 = now_cs();
    r = vcenc_open(&e, &c);
    t_open = now_cs() - t0;
    if (r != VCENC_OK) {
        say("vcenc_open: %s (%d)\n\nResult: the encoder didn't open\n", vcenc_open_error(), r);
        if (out2) fclose(out2);
        return 1;
    }
    say("vcenc_open: %u cs\n", (unsigned)t_open);
    t0 = now_cs();
    while (!eof && !fatal && now_cs() - t0 < 100 * 60 * 5) {
        while (sent < frames) {              /* send until the input is full */
            const uint8_t *src = cycle[sent % ncycle];
            const uint8_t *planes[3] = { src, src + (size_t)W * H, src + (size_t)W * H + (size_t)CW * CH };
            int strides[3] = { W, CW, CW };
            r = vcenc_send(e, planes, strides, (int64_t)sent * 1000, sent == force_at ? VCENC_FORCE_KEY : 0);
            if (r == VCENC_AGAIN) { again++; break; }
            if (r != VCENC_OK) { say("vcenc_send: %s (%d)\n", vcenc_error(e), r); fatal = 1; break; }
            sent++;
        }
        if (fatal) break;
        if (sent == frames && !eos_sent) {
            r = vcenc_send_eos(e);
            if (r == VCENC_OK) eos_sent = 1;
            else if (r != VCENC_AGAIN) { say("vcenc_send_eos: %s\n", vcenc_error(e)); fatal = 1; break; }
        }
        while ((r = vcenc_receive(e, &pk)) == VCENC_OK) {
            int k = (int)(pk.pts / 1000);
            nal_types(pk.data, pk.size, nt, sizeof nt);
            if (sf) fwrite(pk.data, 1, pk.size, sf);
            if (pk.flags & VCENC_PKT_CONFIG) {
                configs++;
                if (first_config < 0) first_config = npkts;   /* (packets before it) */
                if (!(hdr = malloc(pk.size))) { fatal = 1; break; }
                memcpy(hdr, pk.data, pk.size);
                hdr_len = pk.size;
                {
                    int sps = -1;
                    for (size_t i = 0; i + 7 < pk.size; i++)
                        if (!pk.data[i] && !pk.data[i + 1] && pk.data[i + 2] == 1 && (pk.data[i + 3] & 31) == 7) { sps = (int)i + 4; break; }
                    say("CONFIG packet: %u bytes, NAL types %s", (unsigned)pk.size, nt);
                    if (sps >= 0) say("; SPS profile_idc %u, level_idc %u", pk.data[sps], pk.data[sps + 2]);
                    say("\n");
                }
                continue;
            }
            if (npkts == 0 && !configs) say("The first packet isn't CONFIG (NAL types %s)\n", nt);
            if (pk.pts == VCENC_NOPTS || k != got) {
                if (wrong < 5) say("Packet %d: pts %lld, expected %d000\n", got, (long long)pk.pts, got);
                wrong++;
            }
            if (pk.flags & VCENC_PKT_KEY) {
                keys++;
                if ((c.flags & VCENC_INLINE_HEADERS) && first_nal(pk.data, pk.size) != 7) inline_bad++;
            }
            if (!terse && (got < 3 || (pk.flags & VCENC_PKT_KEY) || got == frames - 1))
                say("Packet %d: %u bytes, pts %lld%s, NAL types %s\n", got, (unsigned)pk.size, (long long)pk.pts,
                    pk.flags & VCENC_PKT_KEY ? ", KEY" : "", nt);
            if (npkts < frames) {
                pkt_t *p = &pkts[npkts];
                if (check) {
                    if (!(p->data = malloc(pk.size))) { say("Out of memory for the stream\n"); fatal = 1; break; }
                    memcpy(p->data, pk.data, pk.size);
                }
                p->size = pk.size; p->pts = pk.pts; p->flags = pk.flags;
                npkts++;
            } else {
                say("More packets than pictures\n");
                wrong++;
            }
            got++;
        }
        if (r == VCENC_EOF) eof = 1;
        else if (r != VCENC_AGAIN && !fatal) { say("vcenc_receive: %s (%d)\n", vcenc_error(e), r); fatal = 1; }
    }
    t_loop = now_cs() - t0;
    vcenc_get_stats(e, &st);
    t0 = now_cs();
    vcenc_close(e);
    t_close = now_cs() - t0;
    if (sf) fclose(sf);
    if (!eof && !fatal) say("The encode didn't finish in 5 minutes\n");
    {
        double secs = t_loop / 100.0, fps = c.fps_num ? (double)c.fps_num / (c.fps_den ? c.fps_den : 1) : 25.0;
        say("Encoded %d pictures in %u cs: %.1f a second; input full %d times\n", got, (unsigned)t_loop,
            secs > 0 ? got / secs : 0.0, again);
        say("Stream: %llu bytes (%llu bit/s at %.0f a second), %u keyframes, %u CONFIG packets\n", st.bytes,
            got ? (unsigned long long)(st.bytes * 8.0 * fps / got) : 0ull, fps, st.keyframes, st.config_packets);
        say("Copying pictures in: %u cs in all; buffers: %d+%d in pools, in %u bytes, out %u x %u bytes\n", st.cs_copy,
            st.in_pools, st.out_pools, st.in_size, st.out_buffers, st.out_size);
        say("vcenc_close: %u cs\n", (unsigned)t_close);
        if (stream && sf) say("The stream is in %s\n", stream);
    }
    if (inline_bad) say("%d keyframes without SPS and PPS before them (-i)\n", inline_bad);
    if (force_at >= 0 && force_at < npkts) say("Picture %d (a keyframe asked for): %s\n", force_at,
                                               pkts[force_at].flags & VCENC_PKT_KEY ? "a keyframe" : "NOT a keyframe");
    {
        int dec_ok = 1, pictures = 0;
        double avg = 0, mn = 0;
        if (check && eof && !fatal) {
            t0 = now_cs();
            if (decode_check(frames, c.flags & VCENC_NO_GPU_MEM_CHECK, &avg, &mn, &pictures)) dec_ok = 0;
            say("Decoded again: %d of %d pictures in %u cs; luma PSNR %.1f dB on average, %.1f the lowest\n", pictures,
                frames, (unsigned)(now_cs() - t0), avg, mn);
            if (pictures != frames || mn < 25.0) dec_ok = 0;
        }
        moved = contig_app_page_moved(probe_swi, &page0);
        if (page0.have_pn || page0.have_pa) {
            contig_app_page p1 = contig_read_app_page(probe_swi);
            say("The program's page at &8000: &%08X (page %u) at the start, &%08X (page %u) at the end: %s\n",
                (unsigned)page0.pa, (unsigned)page0.pn, (unsigned)p1.pa, (unsigned)p1.pn,
                moved ? "MOVED - ARMEABISupport has lost this program (reboot before the next test)" : "not moved");
        }
        {
            int ok = eof && !fatal && !wrong && got == frames && configs >= 1 && first_config == 0 && !inline_bad && dec_ok &&
                     !moved && (force_at < 0 || force_at >= npkts || (pkts[force_at].flags & VCENC_PKT_KEY));
            say("\nResult: %s\n", ok && check ? "OK - encoded, and decoded again close to the pictures sent" :
                                  ok ? "OK - encoded (not decoded again)" :
                                  fatal ? "the encoder failed (see above)" :
                                  !eof ? "the encode didn't finish" :
                                  got != frames ? "pictures are missing" :
                                  wrong ? "packets out of order or without their pts" :
                                  first_config != 0 ? "the SPS and PPS didn't come first" :
                                  !dec_ok ? "the stream didn't decode back to the pictures (see above)" :
                                  moved ? "the program's page at &8000 moved (see above)" : "something's wrong (see above)");
            if (out2) fclose(out2);
            for (int k = 0; k < npkts; k++) free(pkts[k].data);
            free(pkts);
            free(hdr);
            hdr = NULL; hdr_len = 0; pkts = NULL; npkts = 0;
            for (int j = 0; j < ncycle; j++) free(cycle[j]);
            out2 = NULL;
            return ok ? 0 : 1;
        }
    }
}

#ifndef PROBE_TEST
int main(int argc, char **argv) { return probe_main(argc, argv); }
#endif
