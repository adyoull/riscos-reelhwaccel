/*
 * hevcdec_conv.c - NV12 in 128-byte columns (as the HEVC block writes it)
 * to planar 4:2:0, for hevcdec_frame_to_i420.
 *
 * Column by column, so the frame is read in order (each column's rows are
 * consecutive 128-byte lines); luma rows copied 64 then 16 bytes at a
 * time, chroma split into U and V by NEON's VLD2 (32 bytes into 16 + 16).
 *
 * 10-bit frames (NV12_10_COL128, 0.1.7): the same columns, each 128-byte
 * row 32 little-endian words of three 10-bit samples (bits 0-9, 10-19,
 * 20-29: 96 luma samples, or 48 U,V pairs in order U V U | V U V ...).
 * A column's row is unpacked (NEON: 8 words into 24 samples by VST3) and
 * the part wanted copied out: as 16-bit samples, or as 8-bit (the top 8
 * bits of each).
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

void hevcdec_col128_to_i420(const uint8_t *b, size_t col, size_t c_off, uint8_t *const planes[3],
                            const int strides[3], int x0, int y0, int w, int h)
{
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (int x = 0; x < w;) {                           /* luma: each column crossed, its rows in turn */
        int sx = x0 + x, off = sx & 127, n = 128 - off;
        const uint8_t *s = b + (size_t)(sx >> 7) * col + (size_t)y0 * 128 + (size_t)off;
        uint8_t *d = planes[0] + x;
        if (n > w - x) n = w - x;
        for (int y = 0; y < h; y++, s += 128, d += strides[0]) copy_n(d, s, n);
        x += n;
    }
    for (int x = 0; x < cw;) {                          /* chroma: U and V pairs */
        int sx = x0 + 2 * x, off = sx & 127, n = (128 - off) / 2;
        const uint8_t *s = b + (size_t)(sx >> 7) * col + c_off + (size_t)(y0 / 2) * 128 + (size_t)off;
        uint8_t *u = planes[1] + x, *v = planes[2] + x;
        if (n > cw - x) n = cw - x;
        for (int y = 0; y < ch; y++, s += 128, u += strides[1], v += strides[2]) split_n(u, v, s, n);
        x += n;
    }
}

/* ---- 10-bit: three samples a word ---- */

/* the 96 samples of one column's row (32 words) */
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

/* bits: 16 (planes of uint16_t, strides in bytes) or 8 */
static void col30_convert(const uint8_t *b, size_t col, size_t c_off, void *const planes[3], const int strides[3], int x0,
                          int y0, int w, int h, int bits)
{
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    uint16_t t[96] __attribute__((aligned(16)));
    for (int x = 0; x < w;) {                           /* luma: 96 samples a column */
        int sx = x0 + x, off = sx % 96, n = 96 - off;
        const uint8_t *s = b + (size_t)(sx / 96) * col + (size_t)y0 * 128;
        uint8_t *d = (uint8_t *)planes[0] + (size_t)x * (bits == 16 ? 2 : 1);
        if (n > w - x) n = w - x;
        for (int y = 0; y < h; y++, s += 128, d += strides[0]) {
            if (bits == 16 && n == 96 && !((uintptr_t)d & 1)) { unpack30_row((uint16_t *)(void *)d, s); continue; }
            unpack30_row(t, s);
            if (bits == 16) out16((uint16_t *)(void *)d, t + off, n); else out8(d, t + off, n);
        }
        x += n;
    }
    for (int x = 0; x < cw;) {                          /* chroma: 48 U,V pairs a column */
        int sx = x0 / 2 + x, off = sx % 48, n = 48 - off;
        const uint8_t *s = b + (size_t)(sx / 48) * col + c_off + (size_t)(y0 / 2) * 128;
        uint8_t *u = (uint8_t *)planes[1] + (size_t)x * (bits == 16 ? 2 : 1);
        uint8_t *v = (uint8_t *)planes[2] + (size_t)x * (bits == 16 ? 2 : 1);
        if (n > cw - x) n = cw - x;
        for (int y = 0; y < ch; y++, s += 128, u += strides[1], v += strides[2]) {
            unpack30_row(t, s);
            if (bits == 16) split16((uint16_t *)(void *)u, (uint16_t *)(void *)v, t + 2 * off, n);
            else split8(u, v, t + 2 * off, n);
        }
        x += n;
    }
}

void hevcdec_col30_to_planar16(const uint8_t *b, size_t col, size_t c_off, uint16_t *const planes[3], const int strides[3],
                               int x0, int y0, int w, int h)
{
    void *p[3] = { planes[0], planes[1], planes[2] };
    col30_convert(b, col, c_off, p, strides, x0, y0, w, h, 16);
}

void hevcdec_col30_to_i420(const uint8_t *b, size_t col, size_t c_off, uint8_t *const planes[3], const int strides[3],
                           int x0, int y0, int w, int h)
{
    void *p[3] = { planes[0], planes[1], planes[2] };
    col30_convert(b, col, c_off, p, strides, x0, y0, w, h, 8);
}
