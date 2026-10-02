/*
 * rpivid.h - the structures rpivid_h265.c (Raspberry Pi's HEVC command
 * builder, kept as it is) works on, for hevcdec. Written for
 * riscos-reelhwaccel with the members that file uses; the names and the
 * sizes (6 decode environments, 3 phase-1 and 3 phase-2 buffer sets) are
 * those of Raspberry Pi Linux's rpivid driver (facts; no code copied).
 * GPL version 2 (see COPYING).
 */
#ifndef HEVCDEC_RPIVID_H
#define HEVCDEC_RPIVID_H

#include "kshim.h"

#define RPIVID_DEC_ENV_COUNT 6
#define RPIVID_P1BUF_COUNT   3
#define RPIVID_P2BUF_COUNT   3
#define RPIVID_AUX_ENT_COUNT VB2_MAX_FRAME

struct rpivid_dev;
typedef void (*rpivid_irq_callback)(struct rpivid_dev *dev, void *ctx);

struct rpivid_h265_run {
    u32 slice_ents;
    const struct v4l2_ctrl_hevc_sps *sps;
    const struct v4l2_ctrl_hevc_pps *pps;
    const struct v4l2_ctrl_hevc_decode_params *dec;
    const struct v4l2_ctrl_hevc_slice_params *slice_params;
    const struct v4l2_ctrl_hevc_scaling_matrix *scaling_matrix;
};

struct rpivid_run {
    struct vb2_v4l2_buffer *src;
    struct vb2_v4l2_buffer *dst;
    struct rpivid_h265_run h265;
};

struct rpivid_gptr {
    size_t size;
    u8 *ptr;
    dma_addr_t addr;
    unsigned long attrs;
};

struct rpivid_dec_state;
struct rpivid_dec_env;
struct rpivid_q_aux;

struct rpivid_ctx {
    struct v4l2_fh fh;
    struct rpivid_dev *dev;
    struct v4l2_pix_format_mplane dst_fmt;
    int dst_fmt_set;
    int fatal_err;                       /* decoding can't go on (memory) */

    struct rpivid_dec_state *state;
    struct rpivid_dec_env *dec0;
    spinlock_t dec_lock;
    struct rpivid_dec_env *dec_free;
    struct rpivid_dec_env *dec_pool;

    unsigned int p1idx;
    atomic_t p1out;
    struct rpivid_gptr bitbufs[RPIVID_P1BUF_COUNT];

    unsigned int p2idx;
    struct rpivid_gptr pu_bufs[RPIVID_P2BUF_COUNT];
    struct rpivid_gptr coeff_bufs[RPIVID_P2BUF_COUNT];

    spinlock_t aux_lock;
    struct rpivid_q_aux *aux_free;
    struct rpivid_q_aux *aux_ents[RPIVID_AUX_ENT_COUNT];

    unsigned int colmv_stride;
    unsigned int colmv_picsize;
};

struct rpivid_dec_ops {
    void (*setup)(struct rpivid_ctx *ctx, struct rpivid_run *run);
    int (*start)(struct rpivid_ctx *ctx);
    void (*stop)(struct rpivid_ctx *ctx);
    void (*trigger)(struct rpivid_ctx *ctx);
};

/* the block: its registers through hevcdec's hardware layer, and the two
   phases' completions (polled; see rpivid_hw.h) */
struct rpivid_dev {
    struct v4l2_device v4l2_dev;
    struct device *dev;
    struct v4l2_m2m_dev *m2m_dev;
    unsigned long max_clock_rate;
    int cache_align;
    void *hw;                            /* hevcdec's (hevcdec_hw.h) */
    /* the callbacks waiting for each phase's completion */
    rpivid_irq_callback p1_cb, p2_cb;
    void *p1_v, *p2_v;
};

extern const struct rpivid_dec_ops rpivid_dec_ops_h265;
extern const struct v4l2_ctrl_ops rpivid_hevc_sps_ctrl_ops;
extern const struct v4l2_ctrl_ops rpivid_hevc_pps_ctrl_ops;

#endif
