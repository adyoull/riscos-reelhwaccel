/*
 * hevcdec_conv.c - the HEVC block's frames (128-byte columns) to planar
 * 4:2:0, for hevcdec_frame_to_i420 and hevcdec_frame_to_i420_16.
 *
 * 8-bit frames (NV12_COL128): column k holds x 128k..128k+127; its luma
 * rows, 128 bytes each, then its chroma rows, U and V interleaved. Luma
 * rows are copied 64 then 16 bytes at a time, chroma split into U and V
 * by NEON's VLD2.
 *
 * 10-bit frames (NV12_10_COL128): the same columns, each 128-byte row 32
 * little-endian words of three 10-bit samples (bits 0-9, 10-19, 20-29):
 * 96 luma samples, or 48 U,V pairs in the order U V U | V U V. A whole
 * row is unpacked straight into the planes (luma: 8 words into 24 samples
 * by VST3; chroma: VLD2 takes the words in pairs a, b, whose samples are
 * U = a0 a2 b1 and V = a1 b0 b2, and VST3 stores each); as 16-bit
 * samples, or as 8-bit (the top 8 bits). Part rows (a window's edges) go
 * through a row buffer.
 *
 * The order (0.1.8): column by column (the frame read in order; the
 * default), row by row (each row across every column), column by column
 * with rows 8 on preloaded (PLD), or (10-bit) every row through the row
 * buffer (0.1.7's way); HEVCTest's -K times them on the Pi. A tick
 * callback is called between columns (or every 64 rows) so hevcdec can
 * see a phase finished and start the next while the copy goes on.
 *
 * The block is only on the Pi 4 (Cortex-A72), which always has NEON; this
 * runs in USR mode, where the VFP/NEON context is the program's own.
 * Without NEON (another build), plain C.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <string.h>
#include "hevcdec_conv.h"
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#define PLD_AHEAD (8 * 128)                     /* (PLD: the row 8 rows on, in the same column) */

/* ---- 8-bit ---- */

static inline void copy_n(uint8_t *d, const uint8_t *s, int n)
{
#ifdef __ARM_NEON
    for (; n >= 64; n -= 64, s += 64, d += 64) {
        uint8x16_t a = vld1q_u8(s), b = vld1q_u8(s + 16), c = vld1q_u8(s + 32), e = vld1q_u8(s + 48);
        vst1q_u8(d, a); vst1q_u8(d + 16, b); vst1q_u8(d + 32, c); vst1q_u8(d + 48, e);
    }
    for (; n >= 16; n -= 16, s += 16, d += 16) vst1q_u8(d, vld1q_u8(s));
#endif
    if (n > 0) memcpy(d, s, (size_t)n);
}

static inline void split_n(uint8_t *u, uint8_t *v, const uint8_t *s, int n)
{
#ifdef __ARM_NEON
    for (; n >= 16; n -= 16, s += 32, u += 16, v += 16) {
        uint8x16x2_t t = vld2q_u8(s);
        vst1q_u8(u, t.val[0]);
        vst1q_u8(v, t.val[1]);
    }
#endif
    for (; n > 0; n--, s += 2) { *u++ = s[0]; *v++ = s[1]; }
}

/* ---- 10-bit: three samples a word ---- */

/* the 96 samples of one column's row (32 words), in order */
static inline void unpack30_row(uint16_t *d, const uint8_t *src)
{
    const uint32_t *s = (const uint32_t *)(const void *)src;
#ifdef __ARM_NEON
    const uint32x4_t m = vdupq_n_u32(0x3FF);
    for (int i = 0; i < 32; i += 8, s += 8, d += 24) {
        uint32x4_t w0 = vld1q_u32(s), w1 = vld1q_u32(s + 4);
        uint16x8x3_t t;
        t.val[0] = vcombine_u16(vmovn_u32(vandq_u32(w0, m)), vmovn_u32(vandq_u32(w1, m)));
        t.val[1] = vcombine_u16(vmovn_u32(vandq_u32(vshrq_n_u32(w0, 10), m)), vmovn_u32(vandq_u32(vshrq_n_u32(w1, 10), m)));
        t.val[2] = vcombine_u16(vmovn_u32(vandq_u32(vshrq_n_u32(w0, 20), m)), vmovn_u32(vandq_u32(vshrq_n_u32(w1, 20), m)));
        vst3q_u16(d, t);
    }
#else
    for (int i = 0; i < 32; i++, d += 3) {
        uint32_t w = s[i];
        d[0] = (uint16_t)(w & 0x3FF); d[1] = (uint16_t)((w >> 10) & 0x3FF); d[2] = (uint16_t)((w >> 20) & 0x3FF);
    }
#endif
}

#ifdef __ARM_NEON
/* bits 0-9, 10-19 or 20-29 of eight words (four in x, four in y) */
static inline uint16x8_t f0(uint32x4_t x, uint32x4_t y, uint32x4_t m)
{
    return vcombine_u16(vmovn_u32(vandq_u32(x, m)), vmovn_u32(vandq_u32(y, m)));
}
static inline uint16x8_t f10(uint32x4_t x, uint32x4_t y, uint32x4_t m)
{
    return vcombine_u16(vmovn_u32(vandq_u32(vshrq_n_u32(x, 10), m)), vmovn_u32(vandq_u32(vshrq_n_u32(y, 10), m)));
}
static inline uint16x8_t f20(uint32x4_t x, uint32x4_t y, uint32x4_t m)
{
    return vcombine_u16(vmovn_u32(vandq_u32(vshrq_n_u32(x, 20), m)), vmovn_u32(vandq_u32(vshrq_n_u32(y, 20), m)));
}

/* a column's whole luma row straight to 8-bit samples */
static inline void unpack30_row_8(uint8_t *d, const uint8_t *src)
{
    const uint32_t *s = (const uint32_t *)(const void *)src;
    const uint32x4_t m = vdupq_n_u32(0x3FF);
    for (int i = 0; i < 32; i += 8, s += 8, d += 24) {
        uint32x4_t w0 = vld1q_u32(s), w1 = vld1q_u32(s + 4);
        uint8x8x3_t t;
        t.val[0] = vshrn_n_u16(f0(w0, w1, m), 2);
        t.val[1] = vshrn_n_u16(f10(w0, w1, m), 2);
        t.val[2] = vshrn_n_u16(f20(w0, w1, m), 2);
        vst3_u8(d, t);
    }
}

/* a column's whole chroma row (16 word pairs a, b: U = a.0 a.2 b.1, V =
   a.1 b.0 b.2) straight into U and V: 48 samples each, 16-bit or 8-bit */
static inline void unpack30_uv(uint8_t *u, uint8_t *v, const uint8_t *src, int bits)
{
    const uint32_t *s = (const uint32_t *)(const void *)src;
    const uint32x4_t m = vdupq_n_u32(0x3FF);
    for (int i = 0; i < 32; i += 16, s += 16) {
        uint32x4x2_t p = vld2q_u32(s), q = vld2q_u32(s + 8);   /* (val[0]: the a words, val[1]: the b words) */
        uint16x8x3_t tu, tv;
        tu.val[0] = f0(p.val[0], q.val[0], m);
        tu.val[1] = f20(p.val[0], q.val[0], m);
        tu.val[2] = f10(p.val[1], q.val[1], m);
        tv.val[0] = f10(p.val[0], q.val[0], m);
        tv.val[1] = f0(p.val[1], q.val[1], m);
        tv.val[2] = f20(p.val[1], q.val[1], m);
        if (bits == 16) {
            vst3q_u16((uint16_t *)(void *)u, tu);
            vst3q_u16((uint16_t *)(void *)v, tv);
            u += 48; v += 48;
        } else {
            uint8x8x3_t bu, bv;
            bu.val[0] = vshrn_n_u16(tu.val[0], 2); bu.val[1] = vshrn_n_u16(tu.val[1], 2); bu.val[2] = vshrn_n_u16(tu.val[2], 2);
            bv.val[0] = vshrn_n_u16(tv.val[0], 2); bv.val[1] = vshrn_n_u16(tv.val[1], 2); bv.val[2] = vshrn_n_u16(tv.val[2], 2);
            vst3_u8(u, bu);
            vst3_u8(v, bv);
            u += 24; v += 24;
        }
    }
}
#endif

static inline void out16(uint16_t *d, const uint16_t *s, int n) { memcpy(d, s, (size_t)n * 2); }

static inline void out8(uint8_t *d, const uint16_t *s, int n)
{
#ifdef __ARM_NEON
    for (; n >= 8; n -= 8, s += 8, d += 8) vst1_u8(d, vshrn_n_u16(vld1q_u16(s), 2));
#endif
    for (; n > 0; n--) *d++ = (uint8_t)(*s++ >> 2);
}

static inline void split16(uint16_t *u, uint16_t *v, const uint16_t *s, int n)
{
#ifdef __ARM_NEON
    for (; n >= 8; n -= 8, s += 16, u += 8, v += 8) {
        uint16x8x2_t t = vld2q_u16(s);
        vst1q_u16(u, t.val[0]);
        vst1q_u16(v, t.val[1]);
    }
#endif
    for (; n > 0; n--, s += 2) { *u++ = s[0]; *v++ = s[1]; }
}

static inline void split8(uint8_t *u, uint8_t *v, const uint16_t *s, int n)
{
#ifdef __ARM_NEON
    for (; n >= 8; n -= 8, s += 16, u += 8, v += 8) {
        uint16x8x2_t t = vld2q_u16(s);
        vst1_u8(u, vshrn_n_u16(t.val[0], 2));
        vst1_u8(v, vshrn_n_u16(t.val[1], 2));
    }
#endif
    for (; n > 0; n--, s += 2) { *u++ = (uint8_t)(s[0] >> 2); *v++ = (uint8_t)(s[1] >> 2); }
}

/* ---- one row of one column's part of the window ---- */

typedef struct {
    const uint8_t *b;
    size_t col, c_off;
    int ten, bits, way;                         /* bits: of the output samples, 8 or 16 */
    void *const *planes;
    const int *strides;
    int x0, y0;
} job_t;

/* luma: samples [x, x+n) of the window (in one column), row y */
static inline void luma_row(const job_t *j, int x, int n, int y)
{
    int sx = j->x0 + x, per = j->ten ? 96 : 128, off = sx % per;
    const uint8_t *s = j->b + (size_t)(sx / per) * j->col + (size_t)(j->y0 + y) * 128;
    uint8_t *d = (uint8_t *)j->planes[0] + (size_t)y * (size_t)j->strides[0] + (size_t)x * (j->bits == 16 ? 2 : 1);
    if (j->way == HEVCDEC_CONV_PLD) __builtin_prefetch(s + PLD_AHEAD);
    if (!j->ten) { copy_n(d, s + off, n); return; }
    if (n == 96 && j->way != HEVCDEC_CONV_TEMP) {
        if (j->bits == 16 && !((uintptr_t)d & 1)) { unpack30_row((uint16_t *)(void *)d, s); return; }
#ifdef __ARM_NEON
        if (j->bits == 8) { unpack30_row_8(d, s); return; }
#endif
    }
    {
        uint16_t t[96] __attribute__((aligned(16)));
        unpack30_row(t, s);
        if (j->bits == 16) out16((uint16_t *)(void *)d, t + off, n); else out8(d, t + off, n);
    }
}

/* chroma: U,V pairs [x, x+n) of the window's (in one column), chroma row y */
static inline void chroma_row(const job_t *j, int x, int n, int y)
{
    int sx = j->x0 / 2 + x, per = j->ten ? 48 : 64, off = sx % per, bps = j->bits == 16 ? 2 : 1;
    const uint8_t *s = j->b + (size_t)(sx / per) * j->col + j->c_off + (size_t)(j->y0 / 2 + y) * 128;
    uint8_t *u = (uint8_t *)j->planes[1] + (size_t)y * (size_t)j->strides[1] + (size_t)x * bps;
    uint8_t *v = (uint8_t *)j->planes[2] + (size_t)y * (size_t)j->strides[2] + (size_t)x * bps;
    if (j->way == HEVCDEC_CONV_PLD) __builtin_prefetch(s + PLD_AHEAD);
    if (!j->ten) { split_n(u, v, s + 2 * off, n); return; }
#ifdef __ARM_NEON
    if (n == 48 && j->way != HEVCDEC_CONV_TEMP && (bps == 1 || !(((uintptr_t)u | (uintptr_t)v) & 1))) {
        unpack30_uv(u, v, s, j->bits);
        return;
    }
#endif
    {
        uint16_t t[96] __attribute__((aligned(16)));
        unpack30_row(t, s);
        if (bps == 2) split16((uint16_t *)(void *)u, (uint16_t *)(void *)v, t + 2 * off, n);
        else split8(u, v, t + 2 * off, n);
    }
}

void hevcdec_conv(const uint8_t *b, size_t col, size_t c_off, int ten, void *const planes[3], const int strides[3],
                  int bits, int x0, int y0, int w, int h, const hevcdec_conv_opts *o)
{
    const int cw = (w + 1) / 2, ch = (h + 1) / 2, per = ten ? 96 : 128;
    job_t j = { b, col, c_off, ten, bits, o ? o->way : HEVCDEC_CONV_COLUMNS, planes, strides, x0, y0 };
    void (*tick)(void *) = o ? o->tick : NULL;
    void *arg = o ? o->arg : NULL;
    if (j.way == HEVCDEC_CONV_ROWS) {           /* row by row, each across every column */
        for (int y = 0; y < h; y++) {
            for (int x = 0, n; x < w; x += n) {
                n = per - (x0 + x) % per;
                if (n > w - x) n = w - x;
                luma_row(&j, x, n, y);
            }
            if (tick && (y & 63) == 63) tick(arg);
        }
        for (int y = 0; y < ch; y++) {
            for (int x = 0, n; x < cw; x += n) {
                n = per / 2 - (x0 / 2 + x) % (per / 2);
                if (n > cw - x) n = cw - x;
                chroma_row(&j, x, n, y);
            }
            if (tick && (y & 63) == 63) tick(arg);
        }
        return;
    }
    for (int x = 0, n; x < w; x += n) {         /* column by column: each column's rows in turn */
        n = per - (x0 + x) % per;
        if (n > w - x) n = w - x;
        for (int y = 0; y < h; y++) luma_row(&j, x, n, y);
        if (tick) tick(arg);
    }
    for (int x = 0, n; x < cw; x += n) {
        n = per / 2 - (x0 / 2 + x) % (per / 2);
        if (n > cw - x) n = cw - x;
        for (int y = 0; y < ch; y++) chroma_row(&j, x, n, y);
        if (tick) tick(arg);
    }
}
