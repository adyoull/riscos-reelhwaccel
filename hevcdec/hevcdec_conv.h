/* hevcdec_conv.h - the HEVC block's frames (128-byte columns, 8-bit or
   10-bit) to planar 4:2:0 (hevcdec_conv.c).
   Part of riscos-reelhwaccel. GPL version 2 (see COPYING). */
#ifndef HEVCDEC_CONV_H
#define HEVCDEC_CONV_H
#include <stddef.h>
#include <stdint.h>

/* the order of the copy (HEVCTest -K times them) */
#define HEVCDEC_CONV_COLUMNS 0          /* column by column (the default) */
#define HEVCDEC_CONV_ROWS    1          /* row by row across the columns */
#define HEVCDEC_CONV_PLD     2          /* column by column, preloading 8 rows ahead */
#define HEVCDEC_CONV_TEMP    3          /* 10-bit: every row through a row buffer (as 0.1.7) */
#define HEVCDEC_CONV_WAYS    4

typedef struct {
    int way;                            /* HEVCDEC_CONV_... */
    void (*tick)(void *arg);            /* called between columns (or every 64 rows): NULL for none */
    void *arg;
} hevcdec_conv_opts;

/* b: the frame; col: bytes per column; c_off: where a column's chroma rows
   start in it; ten: 10-bit (three samples a word, 96 a column's row).
   w x h from (x0, y0) (x0, y0 even) into planes (U and V (w+1)/2 x
   (h+1)/2), samples of bits 8 or 16 (16: 10-bit frames only; strides in
   bytes); a 10-bit frame to 8 bits gives each sample's top 8 bits. */
void hevcdec_conv(const uint8_t *b, size_t col, size_t c_off, int ten, void *const planes[3], const int strides[3],
                  int bits, int x0, int y0, int w, int h, const hevcdec_conv_opts *o);
/* Halved: ow x oh output samples (U and V (ow+1)/2 x (oh+1)/2), each the
   rounded mean of a 2x2 block of the frame from (x0, y0) (x0 a multiple of
   4, y0 even), 8-bit (10-bit frames: rounded to 8 bits, at most 255). A
   block on the frame's last chroma row (oh odd) uses that row twice. */
void hevcdec_conv_half(const uint8_t *b, size_t col, size_t c_off, int ten, uint8_t *const planes[3], const int strides[3],
                       int x0, int y0, int ow, int oh, const hevcdec_conv_opts *o);
#endif
