/*
 * rpivid_video.h - the two sizing rules rpivid_h265.c takes from Raspberry
 * Pi Linux's rpivid_video.c (the rules as facts; written here):
 *   rpivid_round_up_size: sizes go up in steps of 3 x 2^n and 4 x 2^n
 *     (at least 3 x 256);
 *   rpivid_bit_buf_size: room for a picture's bitstream - 3/4 of a byte a
 *     pixel up to 983040 pixels (H.265 Annex A's minimum compression of 2
 *     at level 3.1, for 4:2:0), 983040 x 3/4 up to twice that, 3/8 a pixel
 *     above; plus an eighth per bit of depth above 8.
 * And is_sps_set: an SPS has been given once it has a picture width.
 * Part of riscos-reelhwaccel. GPL version 2 or later (see COPYING).
 */
#ifndef HEVCDEC_RPIVID_VIDEO_H
#define HEVCDEC_RPIVID_VIDEO_H

#include "kshim.h"

static inline size_t rpivid_round_up_size(const size_t x)
{
    const unsigned int n = x < 256 ? 8 : ilog2(x);
    return x >= ((size_t)3 << n) ? (size_t)4 << n : (size_t)3 << n;
}

static inline int is_sps_set(const struct v4l2_ctrl_hevc_sps *const sps)
{
    return sps && sps->pic_width_in_luma_samples != 0;
}

static inline size_t rpivid_bit_buf_size(unsigned int w, unsigned int h, unsigned int bits_minus8)
{
    const size_t wxh = (size_t)w * h;
    size_t bits_alloc = wxh < 983040 ? wxh * 3 / 4 : wxh < 983040 * 2 ? 983040 * 3 / 4 : wxh * 3 / 8;
    bits_alloc += (bits_alloc * bits_minus8) / 8;
    return rpivid_round_up_size(bits_alloc);
}

#endif
