/* hevcdec_conv.h - NV12 in 128-byte columns to planar 4:2:0 (hevcdec_conv.c).
   Part of riscos-reelhwaccel. GPL version 2 (see COPYING). */
#ifndef HEVCDEC_CONV_H
#define HEVCDEC_CONV_H
#include <stddef.h>
#include <stdint.h>

/* b: the frame; col: bytes per column; c_off: where a column's chroma rows
   start in it. w x h from (x0, y0) (x0, y0 even) into planes (U and V
   (w+1)/2 x (h+1)/2). */
void hevcdec_col128_to_i420(const uint8_t *b, size_t col, size_t c_off, uint8_t *const planes[3],
                            const int strides[3], int x0, int y0, int w, int h);
#endif
