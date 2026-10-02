/*
 * hevcdec_conv.c - NV12 in 128-byte columns (as the HEVC block writes it)
 * to planar 4:2:0, for hevcdec_frame_to_i420.
 *
 * Column by column, so the frame is read in order (each column's rows are
 * consecutive 128-byte lines); luma rows copied 64 then 16 bytes at a
 * time, chroma split into U and V by NEON's VLD2 (32 bytes into 16 + 16).
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
