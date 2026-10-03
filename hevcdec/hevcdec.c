/*
 * hevcdec.c - hevcdec's glue: the decoder, its frames and memory around
 * Raspberry Pi's rpivid_h265.c (kept as it is), and the polling that
 * stands in for the block's interrupts. See hwhevcdec.h.
 *
 * rpivid_h265.c is driven as the V4L2 stateless request API would: for
 * each slice, setup (the slice's commands added) then trigger; after the
 * last slice of a picture, trigger starts phase 1 at once (claims are
 * granted at once here). Phase 1's and phase 2's completions are seen in
 * the interrupt control register (polled), and the callbacks rpivid_h265.c
 * left run then: phase 1's starts phase 2, phase 2's marks the frame done.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hwhevcdec.h"
#include "rpivid.h"
#include "rpivid_hw.h"
#include "hevcdec_conv.h"

#define MAX_FRAMES  VB2_MAX_FRAME
#define WAIT_CS     100                    /* a phase not done in this: an error */
#define GUARD       16384                  /* bytes of pattern after each buffer the block uses */
#define GUARD_BYTE  0xA5
#define MAX_GUARDED 120

struct hevcdec_frame {
    struct vb2_v4l2_buffer vb;           /* (first: rpivid_h265.c sees this) */
    int used;
    int cached;                          /* cacheable: invalidated before it's read */
    int submitted;                       /* given to the block: decoded when vb.state is set */
    char err[160];                       /* why the block failed it */
};

struct hevcdec {
    hevcdec_config cfg;
    hevcdec_hw *hw;
    struct rpivid_dev dev;
    struct rpivid_ctx ctx;
    struct v4l2_m2m_ctx m2m;
    struct vb2_queue vq;
    int started;
    int dead;                            /* a phase never finished: the block may still be at work */
    /* For a stream without scaling lists. The block keeps its scaling
       factors from one picture (and one decoder) to the next, and uses
       them: SPS1's scaling bit is set anyway when FFmpeg's PCM fields
       (255, 253 without PCM) overflow into it, and rpivid_h265.c loads the
       factors only when the SPS enables scaling lists. So such pictures
       came out with whatever factors were left (HEVCTest 0.1 on a Pi 4);
       given flat lists (all 16, what "no scaling lists" means) with the
       SPS copied to enable them, every picture was right (0.1.1). */
    struct { void *p; size_t size; const char *what; int reported, cached; } guarded[MAX_GUARDED];
    int cached_frames;                   /* frames cacheable (config, and the machine can) */
    unsigned overruns, overrun_max;     /* buffers the block wrote past, and by how much at most */
    const char *overrun_what;
    struct v4l2_ctrl_hevc_sps sps_flat;
    struct v4l2_ctrl_hevc_scaling_matrix flat;
    char err[256];
    hevcdec_frame *frames[MAX_FRAMES];
    /* the slices sent, contiguous copies: a picture's last slice is read by
       phase 1 from here, so a ring (as rpivid's bitstream copies), one a
       picture, each free again once that picture's phase 1 is done */
    struct vb2_v4l2_buffer srcs[RPIVID_P1BUF_COUNT];
    size_t src_caps[RPIVID_P1BUF_COUNT];
    unsigned src_next;
    struct vb2_v4l2_buffer *cur_src, *cur_dst;
    int job_state;                       /* trigger's verdict on the last slice sent */
    /* phase 1's command lists, each picture's copied for the block and kept
       until rpivid unmaps it (its decode environment gone: phase 1 done) */
    struct { void *p; uint64_t bus; size_t cap; int mapped; } cmds[RPIVID_DEC_ENV_COUNT];
    hevcdec_stats stats;
};

static char open_err[256];
static hevcdec *cur;                     /* (one block, one decoder: for kshim's calls) */

/* ---- log ---- */

static void logf_(hevcdec *d, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    if (!d || !d->cfg.log) return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    d->cfg.log(d->cfg.log_handle, b);
}

void hevcdec_klog(int level, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    size_t n;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    n = strlen(b);
    while (n && b[n - 1] == '\n') b[--n] = 0;
    if (level == 0 && cur && !cur->err[0]) snprintf(cur->err, sizeof cur->err, "%s", b);
    if (cur) logf_(cur, "%s%s", level == 0 ? "rpivid error: " : level == 1 ? "rpivid: " : "rpivid: ", b);
}

static int fail(hevcdec *d, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->err, sizeof d->err, fmt, ap);
    va_end(ap);
    logf_(d, "%s", d->err);
    return HEVCDEC_ERROR;
}

/* ---- kshim's DMA memory ---- */

/* Every buffer the block uses has GUARD bytes of a pattern after it: the
   block writing past a buffer's end (into whatever memory follows) shows,
   and does no harm. Checked as each buffer goes and by hevcdec_get_stats. */
static void guard_check(hevcdec *d, int k)
{
    const uint8_t *g = (const uint8_t *)d->guarded[k].p + d->guarded[k].size;
    size_t n = GUARD;
    if (d->guarded[k].cached) hevcdec_hw_cache_clean_inv(d->hw, g, GUARD);   /* (what's in memory) */
    while (n && g[n - 1] == GUARD_BYTE) n--;
    if (n && !d->guarded[k].reported) {
        d->guarded[k].reported = 1;
        d->overruns++;
        if (n > d->overrun_max) { d->overrun_max = (unsigned)n; d->overrun_what = d->guarded[k].what; }
        logf_(d, "The block wrote past the end of %s (%u bytes; up to %u bytes after it)", d->guarded[k].what,
              (unsigned)d->guarded[k].size, (unsigned)n);
    }
}

static void *dma_get(hevcdec *d, size_t size, uint64_t *bus, const char *what, int cached)
{
    int k;
    void *p;
    for (k = 0; k < MAX_GUARDED && d->guarded[k].p; k++) {}
    if (k == MAX_GUARDED) return NULL;
    size = (size + 63) & ~(size_t)63;
    if (!(p = hevcdec_hw_alloc(d->hw, size + GUARD, bus, cached))) {
        logf_(d, "No contiguous memory for %s (%u bytes): %s", what, (unsigned)size, hevcdec_hw_why(d->hw));
        return NULL;
    }
    memset((uint8_t *)p + size, GUARD_BYTE, GUARD);
    if (cached) hevcdec_hw_cache_clean_inv(d->hw, (uint8_t *)p + size, GUARD);   /* (out to memory) */
    d->guarded[k].p = p; d->guarded[k].size = size; d->guarded[k].what = what; d->guarded[k].reported = 0;
    d->guarded[k].cached = cached;
    return p;
}

static void dma_put(hevcdec *d, void *p)
{
    if (!p) return;
    for (int k = 0; k < MAX_GUARDED; k++)
        if (d->guarded[k].p == p) { guard_check(d, k); d->guarded[k].p = NULL; break; }
    hevcdec_hw_free(d->hw, p);
}

void *hevcdec_dma_alloc(size_t size, dma_addr_t *addr)
{
    uint64_t bus = 0;
    void *p = cur ? dma_get(cur, size, &bus, "a buffer of rpivid's (PU, coefficients, bitstream or collocated)", 0) : NULL;
    *addr = p ? bus : 0;
    return p;
}

void hevcdec_dma_free(void *ptr)
{
    if (ptr && cur) dma_put(cur, ptr);
}

/* phase 1's command list (malloc'd by rpivid_h265.c) copied where the
   block can read it */
dma_addr_t hevcdec_dma_map(const void *ptr, size_t size)
{
    hevcdec *d = cur;
    int k;
    if (!d) return 0;
    for (k = 0; k < RPIVID_DEC_ENV_COUNT && d->cmds[k].mapped; k++) {}
    if (k == RPIVID_DEC_ENV_COUNT) { hevcdec_klog(0, "No command list free (%d mapped)", k); return 0; }
    if (size > d->cmds[k].cap) {
        size_t cap = size < 65536 ? 65536 : size * 2;
        if (d->cmds[k].p) dma_put(d, d->cmds[k].p);
        d->cmds[k].cap = 0;
        if (!(d->cmds[k].p = dma_get(d, cap, &d->cmds[k].bus, "a phase 1 command list", 0))) return 0;
        d->cmds[k].cap = cap;
    }
    memcpy(d->cmds[k].p, ptr, size);
    d->cmds[k].mapped = 1;
    return d->cmds[k].bus;
}

void hevcdec_dma_unmap(dma_addr_t a)
{
    for (int k = 0; cur && k < RPIVID_DEC_ENV_COUNT; k++)
        if (cur->cmds[k].mapped && cur->cmds[k].bus == a) { cur->cmds[k].mapped = 0; return; }
}

/* ---- kshim's videobuf2 / mem2mem: the picture being decoded ---- */

dma_addr_t hevcdec_vb_addr(struct vb2_buffer *vb) { return ((struct vb2_v4l2_buffer *)vb)->addr; }
void *hevcdec_vb_vaddr(struct vb2_buffer *vb) { return ((struct vb2_v4l2_buffer *)vb)->vaddr; }
struct vb2_queue *hevcdec_get_vq(struct v4l2_m2m_ctx *m2m, int type) { (void)m2m; (void)type; return cur ? &cur->vq : NULL; }

struct vb2_buffer *hevcdec_find_buffer(struct vb2_queue *q, uint64_t timestamp)
{
    (void)q;
    for (int i = 0; cur && i < MAX_FRAMES; i++)
        if (cur->frames[i] && cur->frames[i]->used && cur->frames[i]->vb.timestamp == timestamp)
            return &cur->frames[i]->vb.vb2_buf;
    return NULL;
}

void hevcdec_buf_done(struct vb2_v4l2_buffer *vb, enum vb2_buffer_state state)
{
    if (!vb) return;
    vb->state = (int)state;
    if (state == VB2_BUF_STATE_ERROR && cur)               /* (a frame: why, kept with it) */
        for (int i = 0; i < MAX_FRAMES; i++)
            if (cur->frames[i] && &cur->frames[i]->vb == vb)
                snprintf(cur->frames[i]->err, sizeof cur->frames[i]->err, "%.150s",
                         cur->err[0] ? cur->err : "The block failed the picture");
}
void hevcdec_job_done(struct v4l2_m2m_ctx *m2m, enum vb2_buffer_state state) { (void)m2m; if (cur) cur->job_state = (int)state; }
struct vb2_v4l2_buffer *hevcdec_src_remove(struct v4l2_m2m_ctx *m2m) { (void)m2m; return cur ? cur->cur_src : NULL; }
struct vb2_v4l2_buffer *hevcdec_dst_remove(struct v4l2_m2m_ctx *m2m) { (void)m2m; return cur ? cur->cur_dst : NULL; }

/* ---- the decoder ---- */

void hevcdec_config_init(hevcdec_config *c)
{
    memset(c, 0, sizeof *c);
    c->bit_depth = 8;
    c->cached_frames = 1;
}

const char *hevcdec_open_error(void) { return open_err; }
const char *hevcdec_error(const hevcdec *d) { return d ? d->err : open_err; }
void hevcdec_get_stats(const hevcdec *d, hevcdec_stats *s)
{
    hevcdec *w = (hevcdec *)d;                   /* (the guards checked now) */
    for (int k = 0; k < MAX_GUARDED; k++) if (w->guarded[k].p) guard_check(w, k);
    *s = d->stats;
    s->cached_frames = d->cached_frames;
    s->overruns = d->overruns;
    s->overrun_max = d->overrun_max;
    s->overrun_what = d->overrun_what;
    s->app_page_moves = hevcdec_hw_app_page_moves(d->hw);
}

int hevcdec_open(hevcdec **out, const hevcdec_config *c)
{
    hevcdec *d;
    struct v4l2_pix_format_mplane *f;
    *out = NULL;
    open_err[0] = 0;
    if (cur) { snprintf(open_err, sizeof open_err, "The HEVC block is in use"); return HEVCDEC_ERROR; }
    if (c->width <= 0 || c->height <= 0 || c->width > 4096 || c->height > 4096) {
        snprintf(open_err, sizeof open_err, "%dx%d isn't a size the block decodes (up to 4096x4096)", c->width, c->height);
        return HEVCDEC_UNSUPPORTED;
    }
    if (c->bit_depth != 8) {
        snprintf(open_err, sizeof open_err, "%d-bit isn't done yet (8-bit only)", c->bit_depth);
        return HEVCDEC_UNSUPPORTED;
    }
    if (!(d = calloc(1, sizeof *d))) { snprintf(open_err, sizeof open_err, "Out of memory"); return HEVCDEC_ERROR; }
    memset(&d->flat, 16, sizeof d->flat);
    d->cfg = *c;
    if (hevcdec_hw_open(&d->hw, open_err, sizeof open_err)) { free(d); return HEVCDEC_ERROR; }
    cur = d;
    d->dev.hw = d->hw;
    d->dev.cache_align = 64;
    d->dev.enable1 = RPIVID_P2BUF_COUNT;     /* (phase 1 ahead of phase 2 by as many PU/coefficient buffer sets) */
    d->ctx.dev = &d->dev;
    d->ctx.fh.m2m_ctx = &d->m2m;
    /* the output: NV12 in 128-byte columns (V4L2_PIX_FMT_NV12_COL128): width
       up to whole columns, height to 16, each column holding the luma rows
       then the chroma rows (rpivid_video.c's shape, the smallest it takes) */
    f = &d->ctx.dst_fmt;
    f->pixelformat = V4L2_PIX_FMT_NV12_COL128;
    f->width = ALIGN((unsigned)c->width, 128);
    f->height = ALIGN((unsigned)c->height, 16);
    f->plane_fmt[0].bytesperline = f->height * 3 / 2;
    f->plane_fmt[0].sizeimage = f->plane_fmt[0].bytesperline * f->width;
    d->ctx.dst_fmt_set = 1;
    /* (the block's interrupts: both phases' bits latched, any pending cleared) */
    hevcdec_hw_ictrl_write(d->hw, ARG_IC_ICTRL_ACTIVE1_EN_SET | ARG_IC_ICTRL_ACTIVE2_EN_SET);
    hevcdec_hw_ictrl_write(d->hw, hevcdec_hw_ictrl(d->hw) & ~ARG_IC_ICTRL_SET_ZERO_MASK);
    if (rpivid_dec_ops_h265.start(&d->ctx)) {
        snprintf(open_err, sizeof open_err, "%s", d->err[0] ? d->err : "No memory for the block's buffers");
        hevcdec_close(d);
        return HEVCDEC_ERROR;
    }
    d->started = 1;
    d->cached_frames = c->cached_frames && hevcdec_hw_can_cache(d->hw);
    logf_(d, "hevcdec %s: frames %ux%u (NV12, 128-byte columns of %u lines), %u bytes each, %s", HEVCDEC_VERSION,
          f->width, f->height, f->plane_fmt[0].bytesperline, f->plane_fmt[0].sizeimage,
          d->cached_frames ? "cacheable" : c->cached_frames ? "not cacheable (no cache maintenance)" : "not cacheable");
    *out = d;
    return HEVCDEC_OK;
}

hevcdec_frame *hevcdec_frame_new(hevcdec *d)
{
    hevcdec_frame *fr;
    int i;
    for (i = 0; i < MAX_FRAMES && d->frames[i]; i++) {}
    if (i == MAX_FRAMES) { fail(d, "More than %d frames", MAX_FRAMES); return NULL; }
    if (!(fr = calloc(1, sizeof *fr))) { fail(d, "Out of memory"); return NULL; }
    fr->vb.vb2_buf.index = (unsigned)i;
    fr->vb.vb2_buf.num_planes = 1;
    fr->vb.planes[0].length = d->ctx.dst_fmt.plane_fmt[0].sizeimage;
    fr->cached = d->cached_frames;
    if (!(fr->vb.vaddr = dma_get(d, fr->vb.planes[0].length, &fr->vb.addr, "an output frame", fr->cached))) {
        free(fr);
        fail(d, "No contiguous memory for a %u byte frame", (unsigned)d->ctx.dst_fmt.plane_fmt[0].sizeimage);
        return NULL;
    }
    d->frames[i] = fr;
    return fr;
}

/* ---- the two phases: claims, completions (polled), waiting ---- */

static void kick(struct rpivid_dev *dev, int ph)
{
    for (;;) {
        struct rpivid_hw_irq_ent **q = ph == 1 ? &dev->q1 : &dev->q2, *e = *q;
        int *busy = ph == 1 ? &dev->busy1 : &dev->busy2;
        if (*busy || !e || (ph == 1 && dev->enable1 <= 0)) return;
        *q = e->next;
        *busy = 1;
        if (ph == 1) dev->enable1--;
        if (ph == 1) { dev->p1_cb = NULL; dev->t1 = hevcdec_hw_now_cs(); }
        else { dev->p2_cb = NULL; dev->t2 = hevcdec_hw_now_cs(); }
        e->cb(dev, e->v);                                /* (starts the phase, arming its callback) */
        if (!(ph == 1 ? dev->p1_cb : dev->p2_cb)) *busy = 0;   /* (didn't start: let go of) */
    }
}

void hevcdec_claim(struct rpivid_dev *dev, int phase, struct rpivid_hw_irq_ent *ient, rpivid_irq_callback cb, void *v)
{
    struct rpivid_hw_irq_ent **q = phase == 1 ? &dev->q1 : &dev->q2;
    ient->cb = cb;
    ient->v = v;
    ient->next = NULL;
    while (*q) q = &(*q)->next;
    *q = ient;
    kick(dev, phase);
}

void hevcdec_enable1(struct rpivid_dev *dev, int n)
{
    dev->enable1 += n;
    kick(dev, 1);
}

/* a phase seen finished: its callback (which may start it again: phase 1
   with bigger buffers), then the next claim */
static void finished(hevcdec *d, int ph)
{
    struct rpivid_dev *dev = &d->dev;
    rpivid_irq_callback cb = ph == 1 ? dev->p1_cb : dev->p2_cb;
    void *v = ph == 1 ? dev->p1_v : dev->p2_v;
    uint32_t now = hevcdec_hw_now_cs();
    if (ph == 1) { dev->p1_cb = NULL; d->stats.cs_phase1 += now - dev->t1; }
    else { dev->p2_cb = NULL; d->stats.cs_phase2 += now - dev->t2; }
    cb(dev, v);
    if (ph == 1 && dev->p1_cb) { d->stats.phase1_retries++; dev->t1 = hevcdec_hw_now_cs(); return; }
    if (ph == 2 && dev->p2_cb) { dev->t2 = hevcdec_hw_now_cs(); return; }
    if (ph == 1) dev->busy1 = 0; else dev->busy2 = 0;
    kick(dev, ph);
}

/* what's finished, done with; 0, or -1 if a phase has run too long (the
   decoder is then dead: the block may still be at work, and writing to
   memory, so nothing more is given to it and nothing it has is freed) */
static int poll_phases(hevcdec *d)
{
    struct rpivid_dev *dev = &d->dev;
    uint32_t ictrl = hevcdec_hw_ictrl(d->hw), now;
    if (ictrl & (ARG_IC_ICTRL_ACTIVE1_INT_SET | ARG_IC_ICTRL_ACTIVE2_INT_SET))
        hevcdec_hw_ictrl_write(d->hw, ictrl & ~ARG_IC_ICTRL_SET_ZERO_MASK);   /* (the latched bits cleared) */
    if ((ictrl & ARG_IC_ICTRL_ACTIVE2_INT_SET) && dev->p2_cb) finished(d, 2);
    if ((ictrl & ARG_IC_ICTRL_ACTIVE1_INT_SET) && dev->p1_cb) finished(d, 1);
    now = hevcdec_hw_now_cs();
    if ((dev->p1_cb && now - dev->t1 > WAIT_CS) || (dev->p2_cb && now - dev->t2 > WAIT_CS)) {
        int p1 = dev->p1_cb && now - dev->t1 > WAIT_CS;
        d->dead = 1;
        dev->p1_cb = dev->p2_cb = NULL;
        fail(d, "Phase %d didn't finish in %d cs (ictrl &%08X, status &%08X): restart the machine before "
             "decoding again", p1 ? 1 : 2, WAIT_CS, (unsigned)ictrl,
             (unsigned)hevcdec_hw_read(d->hw, p1 ? RPI_STATUS : RPI_STATUS2));
        return -1;
    }
    return 0;
}

static int idle(const hevcdec *d)
{
    return !d->dev.busy1 && !d->dev.busy2 && !d->dev.q1 && !d->dev.q2;
}

/* polls until done(d, arg); HEVCDEC_OK, or HEVCDEC_ERROR (d->err) */
static int wait_for(hevcdec *d, int (*done)(hevcdec *, void *), void *arg)
{
    uint32_t t0 = hevcdec_hw_now_cs();
    int r = HEVCDEC_OK;
    while (!done(d, arg)) {
        if (d->dead || poll_phases(d)) { r = HEVCDEC_ERROR; break; }
        if (!done(d, arg) && idle(d)) { r = fail(d, "The decode stopped: no phase running"); break; }
    }
    d->stats.cs_wait += hevcdec_hw_now_cs() - t0;
    return r;
}

static int frame_finished(hevcdec *d, void *f) { (void)d; return !((hevcdec_frame *)f)->submitted || ((hevcdec_frame *)f)->vb.state; }
static int room_for_one(hevcdec *d, void *a) { (void)a; return atomic_read(&d->ctx.p1out) < RPIVID_P1BUF_COUNT; }
static int all_done(hevcdec *d, void *a) { (void)a; return idle(d); }

int hevcdec_frame_wait(hevcdec *d, hevcdec_frame *f)
{
    if (wait_for(d, frame_finished, f) != HEVCDEC_OK) return HEVCDEC_ERROR;
    if (!f->submitted) return HEVCDEC_OK;
    if (f->vb.state != VB2_BUF_STATE_DONE) return fail(d, "%s", f->err[0] ? f->err : "The block failed the picture");
    return HEVCDEC_OK;
}

int hevcdec_finish(hevcdec *d)
{
    return wait_for(d, all_done, NULL);
}

int hevcdec_decode(hevcdec *d, const hevcdec_picture *pic, hevcdec_frame *f, uint64_t number)
{
    struct vb2_v4l2_buffer *src;
    size_t *cap_p;
    const struct v4l2_ctrl_hevc_sps *sps = pic->sps;
    const struct v4l2_ctrl_hevc_scaling_matrix *scaling = pic->scaling;
    if (d->dead) return fail(d, "The block didn't finish a picture: restart the machine before decoding again");
    d->err[0] = 0;
    if (sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8)
        return fail(d, "Chroma format %u, %u-bit: only 8-bit 4:2:0 for now", sps->chroma_format_idc,
                    sps->bit_depth_luma_minus8 + 8u), HEVCDEC_UNSUPPORTED;
    if (sps->pic_width_in_luma_samples > d->ctx.dst_fmt.width || sps->pic_height_in_luma_samples > d->ctx.dst_fmt.height)
        return fail(d, "%ux%u is bigger than the frames (%ux%u)", sps->pic_width_in_luma_samples,
                    sps->pic_height_in_luma_samples, d->ctx.dst_fmt.width, d->ctx.dst_fmt.height), HEVCDEC_UNSUPPORTED;
    if (!pic->nslices) return fail(d, "A picture with no slices");
    if ((sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED) && !pic->scaling)
        return fail(d, "Scaling lists enabled but none given");
    if (!(sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED)) {
        d->sps_flat = *sps;
        d->sps_flat.flags |= V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED;
        sps = &d->sps_flat;
        scaling = &d->flat;
    }
    /* the frame finished with (if it was being decoded), and room for one
       more picture (as Linux's driver: each of rpivid's bitstream copies,
       and so each of our slice buffers, free again) */
    if (hevcdec_frame_wait(d, f) != HEVCDEC_OK && d->dead) return HEVCDEC_ERROR;
    if (wait_for(d, room_for_one, NULL) != HEVCDEC_OK) return HEVCDEC_ERROR;
    d->err[0] = 0;
    src = &d->srcs[d->src_next];
    cap_p = &d->src_caps[d->src_next];
    d->src_next = (d->src_next + 1) % RPIVID_P1BUF_COUNT;
    f->used = 1;
    f->submitted = 0;
    f->err[0] = 0;
    f->vb.timestamp = number;
    f->vb.state = 0;
    for (unsigned i = 0; i < pic->nslices; i++) {
        const hevcdec_slice *sl = &pic->slices[i];
        struct rpivid_run run;
        if (sl->size > *cap_p) {
            size_t cap = sl->size < 65536 ? 65536 : sl->size * 2;
            if (src->vaddr) dma_put(d, src->vaddr);
            *cap_p = 0;
            if (!(src->vaddr = dma_get(d, cap, &src->addr, "a slice buffer", 0)))
                return fail(d, "No contiguous memory for a %u byte slice", (unsigned)sl->size);
            *cap_p = cap;
        }
        memcpy(src->vaddr, sl->data, sl->size);
        src->vb2_buf.num_planes = 1;
        src->planes[0].length = (unsigned)*cap_p;
        src->planes[0].bytesused = (unsigned)sl->size;
        src->flags = i + 1 < pic->nslices ? V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF : 0;
        src->state = 0;
        memset(&run, 0, sizeof run);
        run.src = src;
        run.dst = &f->vb;
        run.h265.slice_ents = 1;
        run.h265.sps = sps;
        run.h265.pps = pic->pps;
        run.h265.dec = pic->dec;
        run.h265.slice_params = sl->params;
        run.h265.scaling_matrix = scaling;
        d->cur_src = src;
        d->cur_dst = &f->vb;
        d->job_state = 0;
        rpivid_dec_ops_h265.setup(&d->ctx, &run);
        rpivid_dec_ops_h265.trigger(&d->ctx);
        if (d->job_state == VB2_BUF_STATE_ERROR || f->vb.state == VB2_BUF_STATE_ERROR) {
            char why[256];
            snprintf(why, sizeof why, "%s", d->err[0] ? d->err : "refused");
            if (src->flags) {                   /* (a slice before the last: the picture ended, so its */
                src->flags = 0;                 /* decode environment is let go of, not left for the next) */
                rpivid_dec_ops_h265.setup(&d->ctx, &run);
                rpivid_dec_ops_h265.trigger(&d->ctx);
            }
            return fail(d, "Slice %u: %s", i, why);
        }
    }
    f->submitted = 1;
    d->stats.pictures++;
    if (d->cfg.pipelined) return poll_phases(d) ? HEVCDEC_ERROR : HEVCDEC_OK;
    return hevcdec_frame_wait(d, f);
}

/* (NV12 in 128-byte columns: column k holds x 128k..128k+127; its luma
   rows, 128 bytes each, then from row height (the frames' 16-aligned
   height) its chroma rows, U and V interleaved.) A cacheable frame is
   cleaned and invalidated first: the block wrote it behind the cache,
   which may hold lines of what was there before (read, or fetched
   ahead). The copying is hevcdec_conv.c's (NEON). */
void hevcdec_frame_to_i420(hevcdec *d, const hevcdec_frame *f, uint8_t *const planes[3], const int strides[3], int x0,
                           int y0, int w, int h)
{
    const uint8_t *b = f->vb.vaddr;
    const size_t col = (size_t)d->ctx.dst_fmt.plane_fmt[0].bytesperline * 128, c_off = (size_t)d->ctx.dst_fmt.height * 128;
    if (f->submitted && !f->vb.state) hevcdec_frame_wait(d, (hevcdec_frame *)f);   /* (still being decoded) */
    if (f->cached) {
        uint32_t t0 = hevcdec_hw_now_cs();
        hevcdec_hw_cache_clean_inv(d->hw, b, f->vb.planes[0].length);
        d->stats.cs_cache += hevcdec_hw_now_cs() - t0;
    }
    hevcdec_col128_to_i420(b, col, c_off, planes, strides, x0, y0, w, h);
}

void hevcdec_close(hevcdec *d)
{
    if (!d) return;
    if (!d->dead && !idle(d)) hevcdec_finish(d);   /* (pictures still being decoded: finished first) */
    if (d->dead) {                           /* (the block may still write to its buffers: they stay) */
        logf_(d, "Closed after a phase didn't finish: the block's memory is left (restart the machine)");
        hevcdec_hw_close(d->hw, 1);
        if (cur == d) cur = NULL;
        free(d);
        return;
    }
    {                                        /* (2 cs for anything the block still has in flight) */
        uint32_t t0 = hevcdec_hw_now_cs();
        while (hevcdec_hw_now_cs() - t0 < 2) {}
        logf_(d, "Closing: interrupt control &%08X", (unsigned)hevcdec_hw_ictrl(d->hw));
    }
    if (d->started) rpivid_dec_ops_h265.stop(&d->ctx);
    for (int i = 0; i < MAX_FRAMES; i++)
        if (d->frames[i]) {
            dma_put(d, d->frames[i]->vb.vaddr);
            free(d->frames[i]);
        }
    for (int i = 0; i < RPIVID_P1BUF_COUNT; i++) if (d->srcs[i].vaddr) dma_put(d, d->srcs[i].vaddr);
    for (int k = 0; k < RPIVID_DEC_ENV_COUNT; k++) if (d->cmds[k].p) dma_put(d, d->cmds[k].p);
    hevcdec_hw_close(d->hw, 0);
    if (cur == d) cur = NULL;
    free(d);
}
