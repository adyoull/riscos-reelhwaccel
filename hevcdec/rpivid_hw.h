/*
 * rpivid_hw.h - the HEVC block's registers and the interrupt "claims" as
 * rpivid_h265.c uses them, for hevcdec. Register offsets and bits are
 * those of Raspberry Pi Linux's rpivid driver (rpivid_hw.h; facts, no code
 * copied). Here registers go through hevcdec's hardware layer (writes
 * gathered until the one that starts a phase), and there are no
 * interrupts: a claim runs at once, and a phase's completion callback is
 * kept until hevcdec.c sees the phase's bit in the interrupt control
 * register (polled). Part of riscos-reelhwaccel. GPL version 2.
 */
#ifndef HEVCDEC_RPIVID_HW_H
#define HEVCDEC_RPIVID_HW_H

#include "rpivid.h"
#include "hevcdec_hw.h"

struct rpivid_hw_irq_ent {
    struct rpivid_hw_irq_ent *next;
    rpivid_irq_callback cb;
    void *v;
};

/* Phase 1 */
#define RPI_SPS0          0
#define RPI_SPS1          4
#define RPI_PPS           8
#define RPI_SLICE         12
#define RPI_TILESTART     16
#define RPI_TILEEND       20
#define RPI_SLICESTART    24
#define RPI_MODE          28
#define RPI_LEFT0         32
#define RPI_LEFT1         36
#define RPI_LEFT2         40
#define RPI_LEFT3         44
#define RPI_QP            48
#define RPI_CONTROL       52
#define RPI_STATUS        56
#define RPI_VERSION       60
#define RPI_BFBASE        64
#define RPI_BFNUM         68
#define RPI_BFCONTROL     72
#define RPI_BFSTATUS      76
#define RPI_PUWBASE       80
#define RPI_PUWSTRIDE     84
#define RPI_COEFFWBASE    88
#define RPI_COEFFWSTRIDE  92
#define RPI_SLICECMDS     96
#define RPI_BEGINTILEEND  100
#define RPI_TRANSFER      104
#define RPI_CFBASE        108
#define RPI_CFNUM         112
#define RPI_CFSTATUS      116
/* Phase 2 */
#define RPI_PURBASE       0x8000
#define RPI_PURSTRIDE     0x8004
#define RPI_COEFFRBASE    0x8008
#define RPI_COEFFRSTRIDE  0x800C
#define RPI_NUMROWS       0x8010
#define RPI_CONFIG2       0x8014
#define RPI_OUTYBASE      0x8018
#define RPI_OUTYSTRIDE    0x801C
#define RPI_OUTCBASE      0x8020
#define RPI_OUTCSTRIDE    0x8024
#define RPI_STATUS2       0x8028
#define RPI_FRAMESIZE     0x802C
#define RPI_MVBASE        0x8030
#define RPI_MVSTRIDE      0x8034
#define RPI_COLBASE       0x8038
#define RPI_COLSTRIDE     0x803C
#define RPI_CURRPOC       0x8040

/* the interrupt control register (at the "intc" block) */
#define ARG_IC_ICTRL                      0
#define ARG_IC_ICTRL_ACTIVE1_INT_SET      BIT(0)
#define ARG_IC_ICTRL_ACTIVE1_EN_SET       BIT(2)
#define ARG_IC_ICTRL_ACTIVE2_INT_SET      BIT(4)
#define ARG_IC_ICTRL_ACTIVE2_EN_SET       BIT(6)
#define ARG_IC_ICTRL_SET_ZERO_MASK        ((0xffu << 12) | BIT(11))

static inline void apb_write(const struct rpivid_dev *const dev, const unsigned int offset, const u32 val)
{
    hevcdec_hw_write(dev->hw, offset, val);
}

/* the write that starts a phase: everything gathered goes out */
static inline void apb_write_final(const struct rpivid_dev *const dev, const unsigned int offset, const u32 val)
{
    hevcdec_hw_write(dev->hw, offset, val);
    hevcdec_hw_flush(dev->hw);
}

static inline u32 apb_read(const struct rpivid_dev *const dev, const unsigned int offset)
{
    return hevcdec_hw_read(dev->hw, offset);
}

static inline void apb_write_vc_addr(const struct rpivid_dev *const dev, const unsigned int offset, const dma_addr_t a)
{
    apb_write(dev, offset, (u32)(a >> 6));
}

static inline void apb_write_vc_addr_final(const struct rpivid_dev *const dev, const unsigned int offset,
                                           const dma_addr_t a)
{
    apb_write_final(dev, offset, (u32)(a >> 6));
}

static inline void apb_write_vc_len(const struct rpivid_dev *const dev, const unsigned int offset, const unsigned int x)
{
    apb_write(dev, offset, (x + 63) >> 6);
}

/* claims: one decode at a time, so a claim is granted at once */
static inline void rpivid_hw_irq_active1_enable_claim(struct rpivid_dev *dev, int n) { (void)dev; (void)n; }
static inline void rpivid_hw_irq_active1_claim(struct rpivid_dev *dev, struct rpivid_hw_irq_ent *ient,
                                               rpivid_irq_callback ready_cb, void *ctx)
{
    (void)ient;
    ready_cb(dev, ctx);
}
static inline void rpivid_hw_irq_active1_irq(struct rpivid_dev *dev, struct rpivid_hw_irq_ent *ient,
                                             rpivid_irq_callback irq_cb, void *ctx)
{
    (void)ient;
    dev->p1_cb = irq_cb;
    dev->p1_v = ctx;
}
static inline void rpivid_hw_irq_active1_thread(struct rpivid_dev *dev, struct rpivid_hw_irq_ent *ient,
                                                rpivid_irq_callback thread_cb, void *ctx)
{
    (void)ient;
    thread_cb(dev, ctx);
}
static inline void rpivid_hw_irq_active2_claim(struct rpivid_dev *dev, struct rpivid_hw_irq_ent *ient,
                                               rpivid_irq_callback ready_cb, void *ctx)
{
    (void)ient;
    ready_cb(dev, ctx);
}
static inline void rpivid_hw_irq_active2_irq(struct rpivid_dev *dev, struct rpivid_hw_irq_ent *ient,
                                             rpivid_irq_callback irq_cb, void *ctx)
{
    (void)ient;
    dev->p2_cb = irq_cb;
    dev->p2_v = ctx;
}

#endif
