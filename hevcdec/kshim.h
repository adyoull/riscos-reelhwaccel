/*
 * kshim.h - just enough of the Linux kernel's and V4L2's interfaces for
 * Raspberry Pi's rpivid_h265.c (the HEVC block's command builder, kept as
 * it is in hevcdec/rpivid_h265.c) to build as part of a RISC OS library:
 * types and macros, allocation, logging, locks (none: one thread),
 * DMA memory (hevcdec's contiguous pools), and the videobuf2/mem2mem
 * calls, which hevcdec.c answers for the one picture being decoded.
 *
 * Written for riscos-reelhwaccel from the interfaces' documented meaning;
 * no kernel code. GPL version 2 (see COPYING).
 */
#ifndef HEVCDEC_KSHIM_H
#define HEVCDEC_KSHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "hevc_ctrls.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint64_t dma_addr_t;            /* a bus address (on the Pi 4's SCB: the physical address) */

#define BIT(n)            (1u << (n))
#define ALIGN(x, a)       ((((x) + (a) - 1) / (a)) * (a))
#define ALIGN_DOWN(x, a)  (((x) / (a)) * (a))
#define ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))
#define __packed          __attribute__((packed))
#define fallthrough       __attribute__((fallthrough))
#define __iomem

static inline unsigned long roundup_pow_of_two(unsigned long n)
{
    unsigned long p = 1;
    while (p < n) p <<= 1;
    return p;
}

static inline unsigned int ilog2(unsigned long x)
{
    unsigned int n = 0;
    while (x >>= 1) n++;
    return n;
}

/* ---- allocation ---- */
#define GFP_KERNEL 0
#define kmalloc(n, f)          malloc(n)
#define kzalloc(n, f)          calloc(1, (n))
#define krealloc(p, n, f)      realloc((p), (n))
#define kmalloc_array(n, s, f) malloc((size_t)(n) * (s))
#define kfree(p)               free(p)

/* ---- logging: hevcdec.c's ---- */
struct v4l2_device { int unused; };
void hevcdec_klog(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define v4l2_err(d, ...)  hevcdec_klog(0, __VA_ARGS__)
#define v4l2_warn(d, ...) hevcdec_klog(1, __VA_ARGS__)
#define v4l2_info(d, ...) hevcdec_klog(2, __VA_ARGS__)

/* ---- locks and atomics: one thread, no interrupts ---- */
typedef int spinlock_t;
#define spin_lock_init(l)              ((void)(l))
#define spin_lock_irqsave(l, f)        ((void)(l), (void)(f))
#define spin_unlock_irqrestore(l, f)   ((void)(l), (void)(f))
typedef struct { int counter; } atomic_t;
static inline int atomic_add_return(int i, atomic_t *v) { return v->counter += i; }
#define atomic_set(v, i)  ((v)->counter = (i))
#define atomic_read(v)    ((v)->counter)
typedef struct { int unused; } wait_queue_head_t;
#define init_waitqueue_head(q)  ((void)(q))
#define wake_up(q)              ((void)(q))
#define wait_event(q, cond)     ((void)(q), (void)(cond))     /* (claims are granted at once: done already) */

/* ---- DMA memory: hevcdec's contiguous pools ---- */
struct device { int unused; };
#define DMA_ATTR_FORCE_CONTIGUOUS  1ul
#define DMA_ATTR_NO_KERNEL_MAPPING 2ul
enum dma_data_direction { DMA_TO_DEVICE = 1 };
void *hevcdec_dma_alloc(size_t size, dma_addr_t *addr);
void hevcdec_dma_free(void *ptr);
dma_addr_t hevcdec_dma_map(const void *ptr, size_t size);    /* a copy the block can read, */
void hevcdec_dma_unmap(dma_addr_t a);                         /* kept until it's unmapped */
#define dma_alloc_attrs(d, size, addr, gfp, attrs) hevcdec_dma_alloc((size), (addr))
#define dma_free_attrs(d, size, ptr, addr, attrs)  hevcdec_dma_free(ptr)
#define dma_map_single(d, ptr, size, dir)          hevcdec_dma_map((ptr), (size))
#define dma_mapping_error(d, a)                    ((a) == 0)
#define dma_unmap_single(d, a, size, dir)          hevcdec_dma_unmap(a)

/* ---- videobuf2 / mem2mem / media requests: hevcdec.c's picture ---- */
struct media_request { int unused; };
struct media_request_object { int unused; };
#define media_request_pin(r)    ((void)(r))
#define media_request_unpin(r)  ((void)(r))

#define VB2_MAX_FRAME 32
enum vb2_buffer_state { VB2_BUF_STATE_DONE = 1, VB2_BUF_STATE_ERROR = 2 };
#define V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF 0x200u
#define V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 9

struct vb2_buffer {
    unsigned int index;
    unsigned int num_planes;
    struct { struct media_request *req; } req_obj;
};
struct v4l2_plane_info { unsigned int length, bytesused; };
struct vb2_v4l2_buffer {
    struct vb2_buffer vb2_buf;
    unsigned int flags;
    struct v4l2_plane_info planes[1];
    /* hevcdec's */
    dma_addr_t addr;                     /* the buffer's bus address */
    void *vaddr;
    uint64_t timestamp;
    int state;                           /* VB2_BUF_STATE_, 0 while decoding */
};
struct vb2_queue { int unused; };
struct v4l2_m2m_ctx { int unused; };
struct v4l2_m2m_dev { int unused; };
struct v4l2_fh { struct v4l2_m2m_ctx *m2m_ctx; };

dma_addr_t hevcdec_vb_addr(struct vb2_buffer *vb);
void *hevcdec_vb_vaddr(struct vb2_buffer *vb);
struct vb2_queue *hevcdec_get_vq(struct v4l2_m2m_ctx *m2m, int type);
struct vb2_buffer *hevcdec_find_buffer(struct vb2_queue *q, uint64_t timestamp);
void hevcdec_buf_done(struct vb2_v4l2_buffer *vb, enum vb2_buffer_state state);
void hevcdec_job_done(struct v4l2_m2m_ctx *m2m, enum vb2_buffer_state state);
struct vb2_v4l2_buffer *hevcdec_src_remove(struct v4l2_m2m_ctx *m2m);
struct vb2_v4l2_buffer *hevcdec_dst_remove(struct v4l2_m2m_ctx *m2m);
#define vb2_dma_contig_plane_dma_addr(vb, plane)     hevcdec_vb_addr(vb)
#define vb2_plane_vaddr(vb, plane)                   hevcdec_vb_vaddr(vb)
#define v4l2_m2m_get_vq(m2m, type)                   hevcdec_get_vq((m2m), (type))
#define vb2_find_buffer(q, ts)                       hevcdec_find_buffer((q), (ts))
#define v4l2_m2m_buf_done(vb, state)                 hevcdec_buf_done((vb), (state))
#define v4l2_m2m_buf_done_and_job_finish(d, m2m, st) hevcdec_job_done((m2m), (st))
#define v4l2_m2m_job_finish(d, m2m)                  ((void)0)
#define v4l2_m2m_src_buf_remove(m2m)                 hevcdec_src_remove(m2m)
#define v4l2_m2m_dst_buf_remove(m2m)                 hevcdec_dst_remove(m2m)

/* ---- formats and controls ---- */
#define V4L2_PIX_FMT_NV12_COL128    0x3231434eu     /* ('N','C','1','2') */
#define V4L2_PIX_FMT_NV12_10_COL128 0x3033434eu     /* ('N','C','3','0') */
struct v4l2_plane_pix_format { unsigned int sizeimage, bytesperline; };
struct v4l2_pix_format_mplane {
    unsigned int width, height, pixelformat;
    struct v4l2_plane_pix_format plane_fmt[1];
};
struct v4l2_ctrl {
    void *priv;
    union {
        const struct v4l2_ctrl_hevc_sps *p_hevc_sps;
        const struct v4l2_ctrl_hevc_pps *p_hevc_pps;
    } p_new;
};
struct v4l2_ctrl_ops { int (*try_ctrl)(struct v4l2_ctrl *ctrl); };

#define EINVAL 22
#define ENOMEM 12

#endif
