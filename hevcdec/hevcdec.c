/*
 * hevcdec.c - hevcdec's glue: the decoder, its frames and memory around
 * Raspberry Pi's rpivid_h265.c (kept as it is), and the polling that
 * stands in for the block's interrupts. See hevcdec.h.
 *
 * rpivid_h265.c is driven as the V4L2 stateless request API would: for
 * each slice, setup (the slice's commands added) then trigger; after the
 * last slice of a picture, trigger starts phase 1 at once (claims are
 * granted at once here). Phase 1's and phase 2's completions are seen in
 * the interrupt control register (polled), and the callbacks rpivid_h265.c
 * left run then: phase 1's starts phase 2, phase 2's marks the frame done.
 *
 * Part of riscos-reelhwaccel. GPL version 2 or later (see COPYING).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hevcdec.h"
#include "rpivid.h"
#include "rpivid_hw.h"

#define MAX_FRAMES  VB2_MAX_FRAME
#define WAIT_CS     100                    /* a phase not done in this: an error */

struct hevcdec_frame {
    struct vb2_v4l2_buffer vb;           /* (first: rpivid_h265.c sees this) */
    int used;
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
    char err[256];
    hevcdec_frame *frames[MAX_FRAMES];
    struct vb2_v4l2_buffer src;          /* the slice being sent (a contiguous copy) */
    size_t src_cap;
    struct vb2_v4l2_buffer *cur_src, *cur_dst;
    int job_state;                       /* trigger's verdict on the last slice sent */
    void *cmd;                           /* phase 1's command list, copied for the block */
    uint64_t cmd_bus;
    size_t cmd_cap;
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

void *hevcdec_dma_alloc(size_t size, dma_addr_t *addr)
{
    uint64_t bus = 0;
    void *p = cur ? hevcdec_hw_alloc(cur->hw, size, &bus) : NULL;
    *addr = p ? bus : 0;
    return p;
}

void hevcdec_dma_free(void *ptr)
{
    if (ptr && cur) hevcdec_hw_free(cur->hw, ptr);
}

/* phase 1's command list (malloc'd by rpivid_h265.c) copied where the
   block can read it */
dma_addr_t hevcdec_dma_map(const void *ptr, size_t size)
{
    hevcdec *d = cur;
    if (!d) return 0;
    if (size > d->cmd_cap) {
        size_t cap = size < 65536 ? 65536 : size * 2;
        if (d->cmd) hevcdec_hw_free(d->hw, d->cmd);
        d->cmd_cap = 0;
        if (!(d->cmd = hevcdec_hw_alloc(d->hw, cap, &d->cmd_bus))) return 0;
        d->cmd_cap = cap;
    }
    memcpy(d->cmd, ptr, size);
    return d->cmd_bus;
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

void hevcdec_buf_done(struct vb2_v4l2_buffer *vb, enum vb2_buffer_state state) { if (vb) vb->state = (int)state; }
void hevcdec_job_done(struct v4l2_m2m_ctx *m2m, enum vb2_buffer_state state) { (void)m2m; if (cur) cur->job_state = (int)state; }
struct vb2_v4l2_buffer *hevcdec_src_remove(struct v4l2_m2m_ctx *m2m) { (void)m2m; return cur ? cur->cur_src : NULL; }
struct vb2_v4l2_buffer *hevcdec_dst_remove(struct v4l2_m2m_ctx *m2m) { (void)m2m; return cur ? cur->cur_dst : NULL; }

/* ---- the decoder ---- */

void hevcdec_config_init(hevcdec_config *c)
{
    memset(c, 0, sizeof *c);
    c->bit_depth = 8;
}

const char *hevcdec_open_error(void) { return open_err; }
const char *hevcdec_error(const hevcdec *d) { return d ? d->err : open_err; }
void hevcdec_get_stats(const hevcdec *d, hevcdec_stats *s) { *s = d->stats; }

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
    d->cfg = *c;
    if (hevcdec_hw_open(&d->hw, open_err, sizeof open_err)) { free(d); return HEVCDEC_ERROR; }
    cur = d;
    d->dev.hw = d->hw;
    d->dev.cache_align = 64;
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
    logf_(d, "hevcdec %s: frames %ux%u (NV12, 128-byte columns of %u lines), %u bytes each", HEVCDEC_VERSION,
          f->width, f->height, f->plane_fmt[0].bytesperline, f->plane_fmt[0].sizeimage);
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
    if (!(fr->vb.vaddr = hevcdec_hw_alloc(d->hw, fr->vb.planes[0].length, &fr->vb.addr))) {
        free(fr);
        fail(d, "No contiguous memory for a %u byte frame", (unsigned)d->ctx.dst_fmt.plane_fmt[0].sizeimage);
        return NULL;
    }
    d->frames[i] = fr;
    return fr;
}

/* runs the block until the frame is done (or the job failed): the two
   phases' bits in the interrupt control register, phase 2's first */
static int wait_done(hevcdec *d, hevcdec_frame *f)
{
    uint32_t t0 = hevcdec_hw_now_cs(), phase_t0 = t0;
    while (!f->vb.state) {
        uint32_t ictrl = hevcdec_hw_ictrl(d->hw);
        int any = 0;
        if (ictrl & (ARG_IC_ICTRL_ACTIVE1_INT_SET | ARG_IC_ICTRL_ACTIVE2_INT_SET))
            hevcdec_hw_ictrl_write(d->hw, ictrl & ~ARG_IC_ICTRL_SET_ZERO_MASK);   /* (the latched bits cleared) */
        if ((ictrl & ARG_IC_ICTRL_ACTIVE2_INT_SET) && d->dev.p2_cb) {
            rpivid_irq_callback cb = d->dev.p2_cb;
            d->dev.p2_cb = NULL;
            d->stats.cs_phase2 += hevcdec_hw_now_cs() - phase_t0;
            phase_t0 = hevcdec_hw_now_cs();
            cb(&d->dev, d->dev.p2_v);
            any = 1;
        }
        if ((ictrl & ARG_IC_ICTRL_ACTIVE1_INT_SET) && d->dev.p1_cb) {
            rpivid_irq_callback cb = d->dev.p1_cb;
            int before = (int)d->ctx.p2idx;
            d->dev.p1_cb = NULL;
            d->stats.cs_phase1 += hevcdec_hw_now_cs() - phase_t0;
            phase_t0 = hevcdec_hw_now_cs();
            cb(&d->dev, d->dev.p1_v);
            if ((int)d->ctx.p2idx == before && d->dev.p1_cb) d->stats.phase1_retries++;   /* (buffers grown, phase 1 again) */
            any = 1;
        }
        if (!any && !d->dev.p1_cb && !d->dev.p2_cb && !f->vb.state)
            return fail(d, "The decode stopped: no phase running");
        if (!any && hevcdec_hw_now_cs() - phase_t0 > WAIT_CS) {
            /* the block may still be working (and writing to memory): nothing
               more is given to it, and nothing it was given is freed */
            int p1 = d->dev.p1_cb != NULL;
            d->dead = 1;
            d->dev.p1_cb = d->dev.p2_cb = NULL;
            return fail(d, "Phase %d didn't finish in %d cs (ictrl &%08X, status &%08X): restart the machine before "
                        "decoding again", p1 ? 1 : 2, WAIT_CS, (unsigned)ictrl,
                        (unsigned)hevcdec_hw_read(d->hw, p1 ? RPI_STATUS : RPI_STATUS2));
        }
    }
    return f->vb.state == VB2_BUF_STATE_DONE ? HEVCDEC_OK : fail(d, "%s", d->err[0] ? d->err : "The block failed the picture");
}

int hevcdec_decode(hevcdec *d, const hevcdec_picture *pic, hevcdec_frame *f, uint64_t number)
{
    const struct v4l2_ctrl_hevc_sps *sps = pic->sps;
    if (d->dead) return fail(d, "The block didn't finish a picture: restart the machine before decoding again");
    d->err[0] = 0;
    if (sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8)
        return fail(d, "Chroma format %u, %u-bit: only 8-bit 4:2:0 for now", sps->chroma_format_idc,
                    sps->bit_depth_luma_minus8 + 8u), HEVCDEC_UNSUPPORTED;
    if (sps->pic_width_in_luma_samples > d->ctx.dst_fmt.width || sps->pic_height_in_luma_samples > d->ctx.dst_fmt.height)
        return fail(d, "%ux%u is bigger than the frames (%ux%u)", sps->pic_width_in_luma_samples,
                    sps->pic_height_in_luma_samples, d->ctx.dst_fmt.width, d->ctx.dst_fmt.height), HEVCDEC_UNSUPPORTED;
    if (!pic->nslices) return fail(d, "A picture with no slices");
    f->used = 1;
    f->vb.timestamp = number;
    f->vb.state = 0;
    for (unsigned i = 0; i < pic->nslices; i++) {
        const hevcdec_slice *sl = &pic->slices[i];
        struct rpivid_run run;
        if (sl->size > d->src_cap) {
            size_t cap = sl->size < 65536 ? 65536 : sl->size * 2;
            if (d->src.vaddr) hevcdec_hw_free(d->hw, d->src.vaddr);
            d->src_cap = 0;
            if (!(d->src.vaddr = hevcdec_hw_alloc(d->hw, cap, &d->src.addr)))
                return fail(d, "No contiguous memory for a %u byte slice", (unsigned)sl->size);
            d->src_cap = cap;
        }
        memcpy(d->src.vaddr, sl->data, sl->size);
        d->src.vb2_buf.num_planes = 1;
        d->src.planes[0].length = (unsigned)d->src_cap;
        d->src.planes[0].bytesused = (unsigned)sl->size;
        d->src.flags = i + 1 < pic->nslices ? V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF : 0;
        memset(&run, 0, sizeof run);
        run.src = &d->src;
        run.dst = &f->vb;
        run.h265.slice_ents = 1;
        run.h265.sps = sps;
        run.h265.pps = pic->pps;
        run.h265.dec = pic->dec;
        run.h265.slice_params = sl->params;
        run.h265.scaling_matrix = pic->scaling;
        d->cur_src = &d->src;
        d->cur_dst = &f->vb;
        d->job_state = 0;
        rpivid_dec_ops_h265.setup(&d->ctx, &run);
        rpivid_dec_ops_h265.trigger(&d->ctx);
        if (d->job_state == VB2_BUF_STATE_ERROR || f->vb.state == VB2_BUF_STATE_ERROR) {
            char why[256];
            snprintf(why, sizeof why, "%s", d->err[0] ? d->err : "refused");
            if (d->src.flags) {                 /* (a slice before the last: the picture ended, so its */
                d->src.flags = 0;               /* decode environment is let go of, not left for the next) */
                rpivid_dec_ops_h265.setup(&d->ctx, &run);
                rpivid_dec_ops_h265.trigger(&d->ctx);
            }
            d->dev.p1_cb = d->dev.p2_cb = NULL;
            return fail(d, "Slice %u: %s", i, why);
        }
    }
    if (wait_done(d, f) != HEVCDEC_OK) {
        d->dev.p1_cb = d->dev.p2_cb = NULL;
        return HEVCDEC_ERROR;
    }
    d->stats.pictures++;
    return HEVCDEC_OK;
}

/* (NV12 in 128-byte columns: column k holds x 128k..128k+127; its luma
   rows, 128 bytes each, then from row height (the frames' 16-aligned
   height) its chroma rows, U and V interleaved) */
void hevcdec_frame_to_i420(hevcdec *d, const hevcdec_frame *f, uint8_t *const planes[3], const int strides[3], int x0,
                           int y0, int w, int h)
{
    const uint8_t *b = f->vb.vaddr;
    const size_t col = (size_t)d->ctx.dst_fmt.plane_fmt[0].bytesperline * 128, c_off = (size_t)d->ctx.dst_fmt.height * 128;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (int y = 0; y < h; y++) {                       /* luma: a run per column crossed */
        uint8_t *dst = planes[0] + (size_t)y * strides[0];
        for (int x = 0; x < w;) {
            int sx = x0 + x, n = 128 - (sx & 127);
            if (n > w - x) n = w - x;
            memcpy(dst + x, b + (size_t)(sx >> 7) * col + (size_t)(y0 + y) * 128 + (sx & 127), (size_t)n);
            x += n;
        }
    }
    for (int y = 0; y < ch; y++) {                      /* chroma */
        uint8_t *u = planes[1] + (size_t)y * strides[1], *v = planes[2] + (size_t)y * strides[2];
        for (int x = 0; x < cw; x++) {
            int sx = x0 + 2 * x;
            const uint8_t *s = b + (size_t)(sx >> 7) * col + c_off + (size_t)(y0 / 2 + y) * 128 + (sx & 126);
            u[x] = s[0];
            v[x] = s[1];
        }
    }
}

void hevcdec_close(hevcdec *d)
{
    if (!d) return;
    if (d->dead) {                           /* (the block may still write to its buffers: they stay) */
        logf_(d, "Closed after a phase didn't finish: the block's memory is left (restart the machine)");
        hevcdec_hw_close(d->hw, 1);
        if (cur == d) cur = NULL;
        free(d);
        return;
    }
    if (d->started) rpivid_dec_ops_h265.stop(&d->ctx);
    for (int i = 0; i < MAX_FRAMES; i++)
        if (d->frames[i]) {
            hevcdec_hw_free(d->hw, d->frames[i]->vb.vaddr);
            free(d->frames[i]);
        }
    if (d->src.vaddr) hevcdec_hw_free(d->hw, d->src.vaddr);
    if (d->cmd) hevcdec_hw_free(d->hw, d->cmd);
    hevcdec_hw_close(d->hw, 0);
    if (cur == d) cur = NULL;
    free(d);
}
