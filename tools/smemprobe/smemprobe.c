/*
 * smemprobe - can RISC OS use the VideoCore's shared memory service, so
 * that the decoder writes its pictures straight into our memory (MMAL's
 * zero copy, as Linux's bcm2835-codec does through vc-sm-cma)?
 *
 * Today vcdec and vcenc use MMAL's plain mode: each 1080p picture (3 MB)
 * goes over VCHIQ by bulk transfer, and the firmware keeps its own copy of
 * every buffer in gpu_mem. With zero copy the firmware writes into (and
 * reads from) ARM memory given to it by handle, so nothing is transferred
 * and far less gpu_mem is used. This finds out, step by step, whether that
 * works from RISC OS:
 *
 *   1. open the firmware's "SMEM" service over VCHIQ (Linux's vc-sm-cma
 *      opens it; RISC OS has no client for it);
 *   2. import a Physical Memory Pool's pages into it (by their bus address,
 *      the uncached alias &C0000000 + physical), getting a VideoCore
 *      handle; and free it again;
 *   3. a ril.video_decode with MMAL_PARAMETER_ZERO_COPY on its output port
 *      and imported pools as its output buffers: decode a 360p clip, every
 *      picture checked against FFmpeg's (block means, as VCDecTest), and
 *      say whether the pictures came back zero copy (no bulk transfer);
 *   4. with -t, the 1080p clip decoded that way, timed, then the same clip
 *      through vcdec with bulk transfers (VCDEC_NO_DIRECT) for comparison.
 *
 *   smemprobe [-o file] [-v] small.h264 small.crc small.sig [-t hd.h264 hd.crc]
 *      -o file   also add the report to file
 *      -v        every message
 *   The clips are Annex B with an access unit delimiter before each picture
 *   and no B pictures (made by build.sh); the .crc files are FFmpeg's
 *   framecrc (for the size and the picture count), the .sig its block means.
 *
 * The SMEM service's protocol is vcmmal.c's (vcm_smem_...; until 0.1.1 this
 * had its own copy), as vcdec 0.4.5 uses it; the MMAL layouts are vcmmal.h's.
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcmmal.h"
#include "vcdec.h"

#define VERSION "0.1.1"
#define IN_BUFS 20
#define IN_SIZE (64 * 1024)
#define NOUT 4
#define WAIT_CS 1000                       /* nothing for this long: stalled */

static FILE *out2;
static int verbose;

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

static void log_line(void *h, const char *text) { (void)h; say("  - %s\n", text); }

/* ---- the clips ---- */

typedef struct { const uint8_t *p; uint32_t n; int key; } au_t;
static uint8_t *small_data, *hd_data;
static au_t *small_aus, *hd_aus;
static int small_n, hd_n;

static uint8_t *read_file(const char *name, uint32_t *len)
{
    FILE *f = fopen(name, "rb");
    long n;
    uint8_t *p;
    if (!f) { say("Can't open %s\n", name); return NULL; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || !(p = malloc((size_t)n)) || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        say("Can't read %s\n", name);
        return NULL;
    }
    fclose(f);
    *len = (uint32_t)n;
    return p;
}

/* the access units: from one delimiter (NAL type 9) to the next */
static int split(const uint8_t *p, uint32_t n, au_t **out)
{
    int cap = 64, k = 0;
    uint32_t start = 0;
    au_t *a = malloc(sizeof *a * (size_t)cap);
    if (!a) return -1;
    for (uint32_t i = 0; i + 4 < n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1) continue;
        uint8_t t = p[i + 3] & 31;
        uint32_t sc = i && !p[i - 1] ? i - 1 : i;      /* (a 4-byte start code) */
        if (t == 9 && sc > start) {
            if (k == cap && !(a = realloc(a, sizeof *a * (size_t)(cap *= 2)))) return -1;
            a[k].p = p + start; a[k].n = sc - start; a[k].key = 0; k++;
            start = sc;
        }
        i += 2;
    }
    if (n > start) {
        if (k == cap && !(a = realloc(a, sizeof *a * (size_t)(cap + 1)))) return -1;
        a[k].p = p + start; a[k].n = n - start; a[k].key = 0; k++;
    }
    for (int j = 0; j < k; j++)                         /* keyframes: an IDR slice in them */
        for (uint32_t i = 0; i + 4 < a[j].n; i++)
            if (!a[j].p[i] && !a[j].p[i + 1] && a[j].p[i + 2] == 1 && (a[j].p[i + 3] & 31) == 5) { a[j].key = 1; break; }
    *out = a;
    return k;
}

/* FFmpeg's framecrc: the size and the checksums (in order: no B pictures) */
static int read_crc(const char *name, int *w, int *h, uint32_t **crc)
{
    FILE *f = fopen(name, "r");
    char line[256];
    int n = 0, cap = 0;
    uint32_t *c = NULL;
    if (!f) { say("Can't open %s\n", name); return -1; }
    *w = *h = 0;
    while (fgets(line, sizeof line, f)) {
        char *x;
        if (line[0] == '#') { sscanf(line, "#dimensions 0: %dx%d", w, h); continue; }
        if (!(x = strstr(line, "0x"))) continue;
        if (n == cap && !(c = realloc(c, sizeof *c * (size_t)(cap = cap ? cap * 2 : 256)))) { fclose(f); return -1; }
        c[n++] = (uint32_t)strtoul(x, NULL, 16);
    }
    fclose(f);
    if (!*w || !*h || !n) { say("%s has no #dimensions or no frames\n", name); return -1; }
    *crc = c;
    return n;
}

#define SIGN 96
static float (*sig)[SIGN];
static int nsig;

static int read_sig(const char *name)
{
    FILE *f = fopen(name, "r");
    int cap = 0;
    float v[SIGN];
    if (!f) { say("Can't open %s\n", name); return -1; }
    for (;;) {
        int k;
        for (k = 0; k < SIGN; k++) if (fscanf(f, "%f", &v[k]) != 1) break;
        if (k < SIGN) break;
        if (nsig == cap && !(sig = realloc(sig, (size_t)(cap = cap ? cap * 2 : 64) * sizeof *sig))) { fclose(f); return -1; }
        memcpy(sig[nsig++], v, sizeof v);
    }
    fclose(f);
    return 0;
}

static uint32_t adler(const uint8_t *p, size_t n)
{
    uint32_t s1 = 0, s2 = 0;                /* (from 0, as FFmpeg's framecrc) */
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { s1 += *p++; s2 += s1; }
        s1 %= 65521; s2 %= 65521;
    }
    return s2 << 16 | s1;
}

static void grid(const uint8_t *p, int pitch, int w, int h, int n, float *out)
{
    for (int gy = 0; gy < n; gy++)
        for (int gx = 0; gx < n; gx++) {
            int y0 = h * gy / n, y1 = h * (gy + 1) / n, x0 = w * gx / n, x1 = w * (gx + 1) / n;
            double sum = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) sum += p[y * pitch + x];
            *out++ = (float)(sum / (double)((y1 - y0) * (x1 - x0)));
        }
}

/* ---- memory ---- */

typedef struct { uint8_t *p; uint32_t area, size, phys, vch; } buf_t;

static int pool(vcm *m, buf_t *b, uint32_t size)
{
    uint32_t blk[3];
    memset(b, 0, sizeof *b);
    b->size = (size + 4095) & ~4095u;
    if (!(b->p = vcm_pmp_alloc(m, b->size, &b->area, VCM_POOL_RW))) { say("No pool: %s\n", m->why); return -1; }
    blk[0] = 0; blk[1] = (uint32_t)(uintptr_t)b->p; blk[2] = 0;
    if (vcm_swi4(OS_Memory, 0x2200, (uint32_t)(uintptr_t)blk, 1, 0, NULL, NULL)) { say("OS_Memory 0 failed\n"); return -1; }
    b->phys = blk[2];
    vcm_cache_cci(m, b->p, b->size);        /* (no dirty lines left to be written over the VideoCore's) */
    return 0;
}

static void pool_free(vcm *m, buf_t *b)
{
    if (b->p) vcm_pmp_free(m, b->area, b->size >> 12);
    b->p = NULL;
}

/* ---- step 3/4: a decoder with zero copy on its output ---- */

typedef struct {
    vcm *m;
    port_info_t in, out;
    int in_on, out_on;
    uint8_t *inb[IN_BUFS];
    int in_busy[IN_BUFS];
    buf_t outb[NOUT];
    int out_vc[NOUT];                      /* with the decoder */
    int ready[NOUT + 1], nready;           /* pictures back, in order */
    uint32_t ready_len[NOUT + 1];
    int efch, eos_back, zc_back, bulk_back, outputs_given;
    uint32_t t_first;
    int zc_param_ok;
} dec_t;


static void on_msg(void *owner, uint32_t got)
{
    dec_t *d = owner;
    hdr_t *h = (hdr_t *)d->m->msg;
    if (h->type == T_BUFFER_TO_HOST) {
        buffer_msg_t *b = (buffer_msg_t *)(h + 1);
        uint32_t k = b->drvbuf.client_context - CTX_BASE;
        if (verbose) say("  rx: buffer back, port %u, buffer %u, length %u, flags &%X, zero copy %d\n",
                         (unsigned)b->drvbuf.port_handle, (unsigned)k, (unsigned)b->length, (unsigned)b->flags,
                         (int)b->is_zero_copy);
        if (b->drvbuf.port_handle == d->in.port_handle && k < IN_BUFS) { d->in_busy[k] = 0; return; }
        if (b->drvbuf.port_handle != d->out.port_handle || k >= NOUT) return;
        d->out_vc[k] = 0;
        if (b->flags & FLAG_EOS) d->eos_back = 1;
        if (!b->length) {
            uint32_t seq;                  /* (an empty EOS without zero copy: its 8 bytes by bulk, as vcdec) */
            if ((b->flags & FLAG_EOS) && !b->is_zero_copy && !vcm_queue_receive(d->m, d->outb[k].p, 8, &seq))
                vcm_wait_received(d->m, seq);
            return;
        }
        if (b->is_zero_copy) d->zc_back++;
        else {                             /* (not zero copy after all: the bytes by bulk transfer) */
            uint32_t seq;
            d->bulk_back++;
            if (vcm_queue_receive(d->m, d->outb[k].p, b->length, &seq) || vcm_wait_received(d->m, seq)) return;
        }
        d->ready[d->nready] = (int)k;
        d->ready_len[d->nready++] = b->length;
        return;
    }
    if (h->type == T_EVENT_TO_HOST) {
        event_msg_t *ev = (event_msg_t *)(h + 1);
        if (!ev->cmd) return;
        if (vcm_event_data(d->m, ev)) return;
        if (ev->cmd == EV_FORMAT_CHANGED) {
            format_changed_t fc;
            memcpy(&fc, ev->data, sizeof fc);
            say("  Format changed: %ux%u (crop %dx%d), buffers of %u bytes\n", (unsigned)fc.video.width,
                (unsigned)fc.video.height, (int)fc.video.crop[2], (int)fc.video.crop[3], (unsigned)fc.buffer_size_min);
            if (fc.video.width != d->out.video.width || fc.video.height != d->out.video.height)
                vcm_fail(d->m, VCM_ERROR, "the decoder wants another output format: not handled by this probe");
            d->efch = 1;
        } else if (ev->cmd == EV_ERROR) {
            vcm_fail(d->m, VCM_ERROR, "the decoder reports an error");
        }
        return;
    }
    (void)got;
}

static int give_outputs(dec_t *d)
{
    for (int i = 0; i < NOUT; i++)
        if (!d->out_vc[i]) {
            int ready = 0;
            for (int j = 0; j < d->nready; j++) ready |= d->ready[j] == i;
            if (ready) continue;
            /* zero copy: the data field is the VideoCore's handle for the buffer (as Linux); refused, the memory */
            if (vcm_buffer_to_vc(d->m, &d->out, "output", i,
                                 d->zc_param_ok ? (const void *)(uintptr_t)d->outb[i].vch : (const void *)d->outb[i].p,
                                 d->outb[i].size, 0, 0, TIME_UNKNOWN, TIME_UNKNOWN))
                return -1;
            d->out_vc[i] = 1;
        }
    return 0;
}

static int dec_open(dec_t *d, vcm *m, int w, int h)
{
    uint32_t size = ((uint32_t)w + 31) / 32 * 32 * (((uint32_t)h + 15) / 16 * 16) * 3 / 2;
    memset(d, 0, sizeof *d);
    d->m = m;
    m->on_msg = on_msg;
    m->owner = d;
    for (int i = 0; i < IN_BUFS; i++)
        if (!(d->inb[i] = vcm_pci_alloc(m, IN_SIZE))) { say("No PCI memory\n"); return -1; }
    for (int i = 0; i < NOUT; i++) {
        if (pool(m, &d->outb[i], size)) return -1;
        if (!(d->outb[i].vch = vcm_smem_import(m, d->outb[i].p, d->outb[i].size, "smemprobe"))) {
            say("SMEM import of output buffer %d: %s\n", i, m->why);
            return -1;
        }
    }
    if (vcm_create(m, "ril.video_decode") || vcm_port_get(m, PORT_INPUT, &d->in, "Input port") ||
        vcm_port_get(m, PORT_OUTPUT, &d->out, "Output port"))
        return -1;
    d->in.format.type = ES_VIDEO; d->in.format.encoding = ENC_H264;
    d->in.video.width = (uint32_t)w; d->in.video.height = (uint32_t)h;
    d->in.port.buffer_num = IN_BUFS; d->in.port.buffer_size = IN_SIZE;
    if (vcm_port_set(m, PORT_INPUT, &d->in, "Input format")) return -1;
    d->out.format.type = ES_VIDEO; d->out.format.encoding = ENC_I420;
    d->out.video.width = ((uint32_t)w + 31) & ~31u; d->out.video.height = ((uint32_t)h + 15) & ~15u;
    d->out.video.crop[0] = d->out.video.crop[1] = 0; d->out.video.crop[2] = w; d->out.video.crop[3] = h;
    d->out.port.buffer_num = NOUT; d->out.port.buffer_size = d->outb[0].size;
    if (vcm_port_set(m, PORT_OUTPUT, &d->out, "Output format")) return -1;
    {
        int r = vcm_param_set(m, &d->out, P_ZERO_COPY, 1, "Zero copy on the output");
        if (r < 0) return -1;
        if (r) say("%s\n", m->why);
        d->zc_param_ok = !r;
    }
    say("  MMAL_PARAMETER_ZERO_COPY on the output port: %s\n", d->zc_param_ok ? "accepted" : "refused");
    if (vcm_simple(m, T_COMPONENT_ENABLE, "Enable")) return -1;
    m->enabled = 1;
    if (vcm_port_action(m, &d->in, ACTION_ENABLE, "Input enable")) return -1;
    d->in_on = 1;
    if (vcm_port_action(m, &d->out, ACTION_ENABLE, "Output enable")) return -1;
    d->out_on = 1;
    return 0;
}

static void dec_close(dec_t *d)
{
    vcm *m = d->m;
    vcm_destroy(m, &d->in, &d->in_on, &d->out, &d->out_on, 1);
    m->failed = 0;
    for (int i = 0; i < NOUT; i++) {
        vcm_smem_free(m, d->outb[i].vch);
        pool_free(m, &d->outb[i]);
    }
    for (int i = 0; i < IN_BUFS; i++) vcm_pci_free(m, d->inb[i]);
}

static int poll_dec(dec_t *d)
{
    uint32_t got;
    while ((got = vcm_dequeue(d->m)) != 0) {
        on_msg(d, got);
        if (d->m->failed) return -1;
    }
    if (!d->outputs_given && (d->efch || (d->t_first && vcm_now_cs() - d->t_first > 200))) d->outputs_given = 1;
    if (d->outputs_given && !d->eos_back && give_outputs(d)) return -1;
    return 0;
}

/* one access unit, in pieces of up to 64 KB: 0, 1 if no input buffer is free, -1 failed */
static int send_au(dec_t *d, const au_t *a, int64_t pts)
{
    int free_n = 0;
    uint32_t pieces = (a->n + IN_SIZE - 1) / IN_SIZE, off = 0;
    for (int i = 0; i < IN_BUFS; i++) free_n += !d->in_busy[i];
    if ((uint32_t)free_n < pieces) return 1;
    for (uint32_t k = 0; k < pieces; k++) {
        uint32_t n = a->n - off > IN_SIZE ? IN_SIZE : a->n - off, flags = 0;
        int i;
        for (i = 0; d->in_busy[i]; i++) {}
        vcdec_svc_copy(d->inb[i], a->p + off, n & ~3u);
        if (n & 3) {
            uint32_t tail = 0;
            memcpy(&tail, a->p + off + (n & ~3u), n & 3);
            vcdec_svc_copy(d->inb[i] + (n & ~3u), &tail, 4);
        }
        if (!k) flags |= FLAG_FRAME_START | (a->key ? FLAG_KEYFRAME : 0);
        if (k + 1 == pieces) flags |= FLAG_FRAME_END;
        if (vcm_buffer_to_vc(d->m, &d->in, "input", i, d->inb[i], IN_SIZE, (n + 3) & ~3u, flags,   /* (zero-padded */
                                                                                                     /* to whole words, as vcdec) */
                             k ? TIME_UNKNOWN : (uint64_t)pts, k ? TIME_UNKNOWN : (uint64_t)pts))
            return -1;
        d->in_busy[i] = 1;
        off += n;
    }
    if (!d->t_first) d->t_first = vcm_now_cs();
    return 0;
}

static int send_eos(dec_t *d)
{
    for (int i = 0; i < IN_BUFS; i++)
        if (!d->in_busy[i]) {
            if (vcm_buffer_to_vc(d->m, &d->in, "input", i, d->inb[i], IN_SIZE, 0, FLAG_EOS, TIME_UNKNOWN, TIME_UNKNOWN))
                return -1;
            d->in_busy[i] = 1;
            return 0;
        }
    return 1;
}

/* the whole clip through the zero-copy decoder: pictures taken; checked
   against crc/sig if given (wrong and close counted) */
static int decode_zc(vcm *m, const au_t *aus, int n, int w, int h, const uint32_t *crc, int ncrc,
                     int *wrong, int *close_n, uint32_t *cs, int *zc, int *bulk)
{
    dec_t d;
    int cur = 0, eos_sent = 0, pics = 0;
    uint32_t t0, last;
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    uint8_t *frame = malloc((size_t)(w * h + 2 * cw * ch));
    *wrong = *close_n = 0;
    if (!frame) return -1;
    if (dec_open(&d, m, w, h)) { say("  The decoder: %s\n", m->err[0] ? m->err : "(see above)"); dec_close(&d); free(frame); return -1; }
    t0 = last = vcm_now_cs();
    while (!d.eos_back || d.nready) {
        if (poll_dec(&d)) break;
        while (cur < n) {
            int r = send_au(&d, &aus[cur], cur);
            if (r < 0) break;
            if (r) break;
            cur++;
        }
        if (m->failed) break;
        if (cur == n && !eos_sent) { int r = send_eos(&d); if (r < 0) break; if (!r) eos_sent = 1; }
        while (d.nready) {                 /* each picture: read where it is, then back to the decoder */
            int k = d.ready[0];
            const uint8_t *b = d.outb[k].p;
            uint32_t pw = d.out.video.width, ph = d.out.video.height;
            vcm_cache_cci(m, b, d.ready_len[0]);   /* (written behind the cache: invalidated) */
            if (crc) {
                uint8_t *o = frame;
                for (int y = 0; y < h; y++, o += w) memcpy(o, b + (uint32_t)y * pw, (size_t)w);
                for (int pl = 0; pl < 2; pl++)
                    for (int y = 0; y < ch; y++, o += cw)
                        memcpy(o, b + pw * ph + (uint32_t)pl * (pw / 2) * (ph / 2) + (uint32_t)y * (pw / 2), (size_t)cw);
                if (pics >= ncrc) (*wrong)++;
                else if (adler(frame, (size_t)(w * h + 2 * cw * ch)) != crc[pics]) {
                    float sg[SIGN], dmax = 0;
                    grid(frame, w, w, h, 8, sg);
                    grid(frame + w * h, cw, cw, ch, 4, sg + 64);
                    grid(frame + w * h + cw * ch, cw, cw, ch, 4, sg + 80);
                    for (int q = 0; q < SIGN && pics < nsig; q++) {
                        float dd = sg[q] > sig[pics][q] ? sg[q] - sig[pics][q] : sig[pics][q] - sg[q];
                        if (dd > dmax) dmax = dd;
                    }
                    if (pics < nsig && dmax <= 1.0f) (*close_n)++;
                    else {
                        if (*wrong < 3) say("  Picture %d: not FFmpeg's (block means up to %.1f apart)\n", pics, (double)dmax);
                        (*wrong)++;
                    }
                }
            }
            pics++;
            last = vcm_now_cs();
            memmove(d.ready, d.ready + 1, sizeof d.ready[0] * (size_t)(d.nready - 1));
            memmove(d.ready_len, d.ready_len + 1, sizeof d.ready_len[0] * (size_t)(d.nready - 1));
            d.nready--;
        }
        if (vcm_now_cs() - last > WAIT_CS) { say("  Nothing for 10 s (%d pictures, %d of %d sent)\n", pics, cur, n); break; }
    }
    *cs = vcm_now_cs() - t0;
    if (m->failed) say("  The decoder: %s\n", m->err);
    *zc = d.zc_back;
    *bulk = d.bulk_back;
    dec_close(&d);
    free(frame);
    return pics;
}

/* the same clip through vcdec with bulk transfers: pictures, time */
static int decode_vcdec(const au_t *aus, int n, int w, int h, uint32_t *cs)
{
    vcdec_config c;
    vcdec *d = NULL;
    int cur = 0, pics = 0, r;
    uint32_t t0, last;
    vcdec_config_init(&c);
    c.width = w; c.height = h; c.out_buffers = 6; c.flags = VCDEC_NO_DIRECT;
    if (vcdec_open(&d, &c) != VCDEC_OK) { say("  vcdec_open: %s\n", vcdec_open_error()); return -1; }
    t0 = last = vcm_now_cs();
    for (;;) {
        vcdec_picture p;
        uint8_t *pl[3];
        int st[3];
        vcdec_hold *hold;
        while (cur < n && (r = vcdec_send(d, aus[cur].p, aus[cur].n, cur, cur, aus[cur].key ? VCDEC_KEYFRAME : 0)) == VCDEC_OK) cur++;
        if (cur == n) vcdec_send_eos(d);
        while ((r = vcdec_receive_hold(d, &p, pl, st, &hold)) == VCDEC_OK) { vcdec_release(hold); pics++; last = vcm_now_cs(); }
        if (r == VCDEC_UNSUPPORTED) { if (vcdec_receive(d, &p, NULL, NULL) == VCDEC_OK) pics++; continue; }
        if (r == VCDEC_EOF) break;
        if (r < 0) { say("  vcdec: %s\n", vcdec_error(d)); break; }
        if (vcm_now_cs() - last > WAIT_CS) { say("  vcdec: nothing for 10 s\n"); break; }
    }
    *cs = vcm_now_cs() - t0;
    vcdec_close(d);
    return pics;
}

static void rate(const char *what, int pics, uint32_t cs)
{
    if (!cs) cs = 1;
    say("%s: %d pictures in %u.%02u s, %u.%u a second\n", what, pics, (unsigned)(cs / 100), (unsigned)(cs % 100),
        (unsigned)(pics * 100 / cs), (unsigned)(pics * 1000 / cs % 10));
}

/* ---- main ---- */

int probe_main(int argc, char **argv)
{
    const char *names[3] = { NULL, NULL, NULL }, *hd_name = NULL, *hd_crc = NULL;
    int nn = 0, ok_open = 0, ok_import = 0, ok_zc = 0, ok_speed = -1;
    uint32_t len;
    vcm m;
    memset(&m, 0, sizeof m);
    out2 = NULL; verbose = 0; nsig = 0; free(sig); sig = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out2 = fopen(argv[++i], "a");
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-t") && i + 2 < argc) { hd_name = argv[++i]; hd_crc = argv[++i]; }
        else if (nn < 3) names[nn++] = argv[i];
    }
    if (nn < 3) {
        printf("Usage: smemprobe [-o file] [-v] small.h264 small.crc small.sig [-t hd.h264 hd.crc]\n");
        return 1;
    }
    say("smemprobe %s (vcdec %s): the VideoCore's shared memory service, and zero copy\n", VERSION, VCDEC_VERSION);
    say("The VideoCore has %u MB of memory (gpu_mem)\n\n", vcm_gpu_mem());

    m.name = "decoder";
    m.pool_name = "smemprobe";
    m.log = log_line;
    m.log_messages = verbose;
    m.waiting = "";
    m.on_msg = on_msg;
    if (vcm_claim_stub(&m) || vcm_connect(&m)) { say("VCHIQ: %s\n", m.err); goto done; }
    if (vcm_find_cci(&m)) say("(no Cache_CleanInvalidateRange: RISC OS 5.23 or later is needed)\n");

    /* 1 */
    say("1. The \"SMEM\" service\n");
    if (vcm_smem_open(&m)) { say("%s\n", m.why); goto done; }
    ok_open = 1;
    say("  Opened (handle &%X)\n\n", (unsigned)m.smem_handle);

    /* 2 */
    say("2. A pool's pages imported\n");
    {
        buf_t b;
        if (pool(&m, &b, 65536) == 0) {
            uint32_t h = vcm_smem_import(&m, b.p, b.size, "smemprobe test");
            say("  64 KB at physical &%08X (bus &%08X): ", (unsigned)b.phys, (unsigned)(b.phys | 0xC0000000u));
            if (h) say("a VideoCore handle, &%08X\n", (unsigned)h);
            else say("refused (%s)\n", m.why);
            if (h) { ok_import = 1; vcm_smem_free(&m, h); say("  Freed\n"); }
            pool_free(&m, &b);
        }
        say("\n");
    }
    if (!ok_import) goto done;

    /* 3 */
    say("3. Zero-copy decoding: %s\n", names[0]);
    {
        uint32_t *crc = NULL, cs;
        int w, h, ncrc, wrong, close_n, zc, bulk, pics;
        if (!(small_data = read_file(names[0], &len)) || (small_n = split(small_data, len, &small_aus)) <= 0 ||
            (ncrc = read_crc(names[1], &w, &h, &crc)) < 0 || read_sig(names[2]))
            goto done;
        say("  %d access units, %dx%d; %d of FFmpeg's pictures\n", small_n, w, h, ncrc);
        pics = decode_zc(&m, small_aus, small_n, w, h, crc, ncrc, &wrong, &close_n, &cs, &zc, &bulk);
        if (pics >= 0) {
            say("  %d pictures: %d came back zero copy, %d by bulk transfer; %d exact, %d within rounding, %d wrong\n",
                pics, zc, bulk, pics - wrong - close_n, close_n, wrong);
            ok_zc = pics == ncrc && !wrong && zc == pics && !bulk;
        }
        free(crc);
        say("\n");
    }

    /* 4 */
    if (hd_name) {
        uint32_t *crc = NULL, cs1 = 0, cs2 = 0;
        int w, h, n1, n2, wrong, close_n, zc, bulk;
        say("4. Speed: %s, zero copy, then through vcdec (bulk transfers)\n", hd_name);
        if (!(hd_data = read_file(hd_name, &len)) || (hd_n = split(hd_data, len, &hd_aus)) <= 0 ||
            read_crc(hd_crc, &w, &h, &crc) < 0)
            goto done;
        n1 = decode_zc(&m, hd_aus, hd_n, w, h, NULL, 0, &wrong, &close_n, &cs1, &zc, &bulk);
        if (n1 >= 0) rate("  Zero copy", n1, cs1);
        n2 = decode_vcdec(hd_aus, hd_n, w, h, &cs2);
        if (n2 >= 0) rate("  vcdec", n2, cs2);
        ok_speed = n1 == hd_n && n2 == hd_n;
        free(crc);
        say("\n");
    }

done:
    vcm_smem_close(&m);
    vcm_service_close(&m, "the callback is left in the RMA");
    vcm_disconnect(&m);
    vcm_free_stub(&m);
    say("Result: %s\n", !ok_open ? "the SMEM service can't be opened from RISC OS (see above)" :
                        !ok_import ? "SMEM opens, but importing memory failed (see above)" :
                        !ok_zc ? "SMEM import works, but zero-copy decoding didn't (see above)" :
                        ok_speed == 0 ? "zero-copy decoding works; the speed test didn't finish (see above)" :
                        "OK - zero-copy decoding works from RISC OS");
    if (out2) fclose(out2);
    free(small_data); free(small_aus); free(hd_data); free(hd_aus);
    small_data = hd_data = NULL; small_aus = hd_aus = NULL;
    return ok_zc && ok_speed != 0 ? 0 : 1;
}

#ifndef PROBE_TEST
int main(int argc, char **argv) { return probe_main(argc, argv); }
#endif
