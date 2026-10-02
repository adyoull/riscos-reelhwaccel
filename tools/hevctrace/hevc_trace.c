/*
 * hevc_trace.c - FFmpeg's HEVC decoder writing, for each picture it
 * decodes, what a stateless hardware decoder (Raspberry Pi's rpivid, by
 * way of hevcdec/) is given for it: the V4L2 stateless HEVC controls (SPS,
 * PPS, decode parameters with the DPB, each slice's parameters and bytes,
 * the scaling matrix), and Adler-32s of the picture FFmpeg decoded, so
 * the hardware's picture can be checked against it.
 *
 * Put into libavcodec by tools/hevctrace/0001-hevc-trace.patch, with hooks
 * in hevcdec.c where a hwaccel's start_frame/decode_slice/end_frame would
 * be called; it does nothing unless HEVC_TRACE names the file to write.
 * Decode with one thread (-threads 1).
 *
 * The fill_* functions are Raspberry Pi's FFmpeg (rpi-ffmpeg,
 * libavcodec/v4l2_req_hevc_vx.c, release/5.1/main), cut down to the
 * HEVC_CTRLS_VERSION 4 (stable uAPI) case, with DPB timestamps that are
 * each picture's number in decoding order. That code is LGPL 2.1 or later,
 * as FFmpeg:
 *
 *   This file is part of FFmpeg.
 *
 *   FFmpeg is free software; you can redistribute it and/or modify it
 *   under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 2.1 of the License,
 *   or (at your option) any later version.
 *
 * The rest: part of riscos-reelhwaccel. GPL version 2 or later (see
 * COPYING); the whole is GPL 2 or later.
 *
 * The trace (all little endian; see tools/hevctrace/README.md):
 *   header: "HVTR", u32 version (1), u32 sizeof sps, pps, slice_params,
 *           decode_params, scaling_matrix
 *   each picture, in decoding order:
 *     "PIC1", u32 number (from 1), s32 poc, u32 width, height (coded, as
 *     the SPS), bit depth, u32 slices, u32 has_scaling, u32 the output
 *     window (the conformance and default display windows): left, top,
 *     width, height, in luma samples,
 *     sps, pps, decode_params, [scaling_matrix if has_scaling],
 *     each slice: slice_params, u32 bytes, the bytes (the NAL unit with its
 *                 header, emulation prevention kept, no start code), padded
 *                 to 4,
 *     u32 adler32 of Y, U, V over the output window (U and V: half its
 *     left, top and size, rounded up; one byte a sample at 8 bits, two
 *     (little endian) above)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/adler32.h"
#include "libavutil/common.h"
#include "hevcdec.h"
#include "hevc_trace.h"
#include "hevc_ctrls.h"

#define MAX_SLICES 600

static FILE *tf;
static int tried;
static uint32_t number;                    /* pictures so far */
static uint32_t slot_number[32];           /* each DPB slot's picture's number */
static struct v4l2_ctrl_hevc_decode_params dec;
static struct v4l2_ctrl_hevc_slice_params sp[MAX_SLICES];
static const uint8_t *sdata[MAX_SLICES];
static uint32_t slen[MAX_SLICES];
static unsigned nslices;
static int in_picture;

static void w32(uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, tf);
}

static int open_trace(void)
{
    const char *name;
    if (tried) return tf != NULL;
    tried = 1;
    if (!(name = getenv("HEVC_TRACE")) || !*name) return 0;
    if (!(tf = fopen(name, "wb"))) return 0;
    fwrite("HVTR", 1, 4, tf);
    w32(1);
    w32(sizeof(struct v4l2_ctrl_hevc_sps));
    w32(sizeof(struct v4l2_ctrl_hevc_pps));
    w32(sizeof(struct v4l2_ctrl_hevc_slice_params));
    w32(sizeof(struct v4l2_ctrl_hevc_decode_params));
    w32(sizeof(struct v4l2_ctrl_hevc_scaling_matrix));
    return 1;
}

/* ---- from rpi-ffmpeg's v4l2_req_hevc_vx.c (LGPL 2.1+) ---- */

static void fill_pred_table(const HEVCContext *h, struct v4l2_hevc_pred_weight_table *table)
{
    int32_t luma_weight_denom, chroma_weight_denom;
    const SliceHeader *sh = &h->sh;

    if (sh->slice_type == HEVC_SLICE_I ||
        (sh->slice_type == HEVC_SLICE_P && !h->ps.pps->weighted_pred_flag) ||
        (sh->slice_type == HEVC_SLICE_B && !h->ps.pps->weighted_bipred_flag))
        return;

    table->luma_log2_weight_denom = sh->luma_log2_weight_denom;

    if (h->ps.sps->chroma_format_idc)
        table->delta_chroma_log2_weight_denom = sh->chroma_log2_weight_denom - sh->luma_log2_weight_denom;

    luma_weight_denom = (1 << sh->luma_log2_weight_denom);
    chroma_weight_denom = (1 << sh->chroma_log2_weight_denom);

    for (int i = 0; i < 15 && i < sh->nb_refs[L0]; i++) {
        table->delta_luma_weight_l0[i] = sh->luma_weight_l0[i] - luma_weight_denom;
        table->luma_offset_l0[i] = sh->luma_offset_l0[i];
        table->delta_chroma_weight_l0[i][0] = sh->chroma_weight_l0[i][0] - chroma_weight_denom;
        table->delta_chroma_weight_l0[i][1] = sh->chroma_weight_l0[i][1] - chroma_weight_denom;
        table->chroma_offset_l0[i][0] = sh->chroma_offset_l0[i][0];
        table->chroma_offset_l0[i][1] = sh->chroma_offset_l0[i][1];
    }

    if (sh->slice_type != HEVC_SLICE_B)
        return;

    for (int i = 0; i < 15 && i < sh->nb_refs[L1]; i++) {
        table->delta_luma_weight_l1[i] = sh->luma_weight_l1[i] - luma_weight_denom;
        table->luma_offset_l1[i] = sh->luma_offset_l1[i];
        table->delta_chroma_weight_l1[i][0] = sh->chroma_weight_l1[i][0] - chroma_weight_denom;
        table->delta_chroma_weight_l1[i][1] = sh->chroma_weight_l1[i][1] - chroma_weight_denom;
        table->chroma_offset_l1[i][0] = sh->chroma_offset_l1[i][0];
        table->chroma_offset_l1[i][1] = sh->chroma_offset_l1[i][1];
    }
}

static unsigned int get_ref_pic_index(const HEVCContext *h, const HEVCFrame *frame,
                                      const struct v4l2_hevc_dpb_entry *const entries,
                                      const unsigned int num_entries)
{
    uint64_t timestamp;
    if (!frame)
        return 0;
    timestamp = slot_number[frame - h->DPB];
    for (unsigned int i = 0; i < num_entries; i++)
        if (entries[i].timestamp == timestamp)
            return i;
    return 0;
}

/* the slice data's position in the raw NAL unit, from the bit count in
   the unescaped one (emulation prevention bytes skipped) */
static const uint8_t *ptr_from_index(const uint8_t *b, unsigned int idx)
{
    unsigned int z = 0;
    while (idx--) {
        if (*b++ == 0) {
            ++z;
            if (z >= 2 && *b == 3) {
                ++b;
                z = 0;
            }
        } else {
            z = 0;
        }
    }
    return b;
}

static unsigned int fill_dpb_entries(const HEVCContext *const h, struct v4l2_hevc_dpb_entry *const entries)
{
    unsigned int n = 0;
    const HEVCFrame *const pic = h->ref;

    for (unsigned i = 0; i < FF_ARRAY_ELEMS(h->DPB); i++) {
        const HEVCFrame *const frame = &h->DPB[i];
        if (frame != pic && (frame->flags & (HEVC_FRAME_FLAG_LONG_REF | HEVC_FRAME_FLAG_SHORT_REF))) {
            struct v4l2_hevc_dpb_entry *const entry = entries + n++;
            entry->timestamp = slot_number[i];
            entry->flags = (frame->flags & HEVC_FRAME_FLAG_LONG_REF) == 0 ? 0 : V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE;
            entry->field_pic = frame->frame->interlaced_frame;
            entry->pic_order_cnt_val = frame->poc;
        }
    }
    return n;
}

static void fill_slice_params(const HEVCContext *const h, const struct v4l2_ctrl_hevc_decode_params *const dec,
                              struct v4l2_ctrl_hevc_slice_params *slice_params, uint32_t bit_size,
                              uint32_t bit_offset)
{
    const SliceHeader *const sh = &h->sh;
    const struct v4l2_hevc_dpb_entry *const dpb = dec->dpb;
    const unsigned int dpb_n = dec->num_active_dpb_entries;
    const RefPicList *rpl;

    *slice_params = (struct v4l2_ctrl_hevc_slice_params) {
        .bit_size = bit_size,
        .data_byte_offset = bit_offset / 8 + 1,
        .slice_segment_addr = sh->slice_segment_addr,
        .nal_unit_type = h->nal_unit_type,
        .nuh_temporal_id_plus1 = h->temporal_id + 1,
        .slice_type = sh->slice_type,
        .colour_plane_id = sh->colour_plane_id,
        .slice_pic_order_cnt = h->ref->poc,
        .num_ref_idx_l0_active_minus1 = sh->nb_refs[L0] ? sh->nb_refs[L0] - 1 : 0,
        .num_ref_idx_l1_active_minus1 = sh->nb_refs[L1] ? sh->nb_refs[L1] - 1 : 0,
        .collocated_ref_idx = sh->slice_temporal_mvp_enabled_flag ? sh->collocated_ref_idx : 0,
        .five_minus_max_num_merge_cand = sh->slice_type == HEVC_SLICE_I ? 0 : 5 - sh->max_num_merge_cand,
        .slice_qp_delta = sh->slice_qp_delta,
        .slice_cb_qp_offset = sh->slice_cb_qp_offset,
        .slice_cr_qp_offset = sh->slice_cr_qp_offset,
        .slice_act_y_qp_offset = 0,
        .slice_act_cb_qp_offset = 0,
        .slice_act_cr_qp_offset = 0,
        .slice_beta_offset_div2 = sh->beta_offset / 2,
        .slice_tc_offset_div2 = sh->tc_offset / 2,
        .pic_struct = h->sei.picture_timing.picture_struct,
    };

    if (sh->slice_sample_adaptive_offset_flag[0])
        slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA;
    if (sh->slice_sample_adaptive_offset_flag[1])
        slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA;
    if (sh->slice_temporal_mvp_enabled_flag)
        slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED;
    if (sh->mvd_l1_zero_flag)
        slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO;
    if (sh->cabac_init_flag)
        slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT;
    if (sh->collocated_list == L0)
        slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0;
    if (sh->disable_deblocking_filter_flag)
        slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED;
    if (sh->slice_loop_filter_across_slices_enabled_flag)
        slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED;
    if (sh->dependent_slice_segment_flag)
        slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT;

    if (sh->slice_type != HEVC_SLICE_I) {
        rpl = &h->ref->refPicList[0];
        for (int i = 0; i < rpl->nb_refs; i++)
            slice_params->ref_idx_l0[i] = get_ref_pic_index(h, rpl->ref[i], dpb, dpb_n);
    }
    if (sh->slice_type == HEVC_SLICE_B) {
        rpl = &h->ref->refPicList[1];
        for (int i = 0; i < rpl->nb_refs; i++)
            slice_params->ref_idx_l1[i] = get_ref_pic_index(h, rpl->ref[i], dpb, dpb_n);
    }

    fill_pred_table(h, &slice_params->pred_weight_table);
    slice_params->num_entry_point_offsets = sh->num_entry_point_offsets;
}

static void fill_decode_params(const HEVCContext *const h, struct v4l2_ctrl_hevc_decode_params *const dec)
{
    *dec = (struct v4l2_ctrl_hevc_decode_params) {
        .pic_order_cnt_val = h->poc,
        .num_poc_st_curr_before = h->rps[ST_CURR_BEF].nb_refs,
        .num_poc_st_curr_after = h->rps[ST_CURR_AFT].nb_refs,
        .num_poc_lt_curr = h->rps[LT_CURR].nb_refs,
    };

    dec->num_active_dpb_entries = fill_dpb_entries(h, dec->dpb);

    for (int i = 0; i != h->rps[ST_CURR_BEF].nb_refs; ++i)
        dec->poc_st_curr_before[i] = h->rps[ST_CURR_BEF].ref[i]->poc;
    for (int i = 0; i != h->rps[ST_CURR_AFT].nb_refs; ++i)
        dec->poc_st_curr_after[i] = h->rps[ST_CURR_AFT].ref[i]->poc;
    for (int i = 0; i != h->rps[LT_CURR].nb_refs; ++i)
        dec->poc_lt_curr[i] = h->rps[LT_CURR].ref[i]->poc;

    if (IS_IRAP(h))
        dec->flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC;
    if (IS_IDR(h))
        dec->flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC;
    if (h->sh.no_output_of_prior_pics_flag)
        dec->flags |= V4L2_HEVC_DECODE_PARAM_FLAG_NO_OUTPUT_OF_PRIOR;
}

static void fill_sps(struct v4l2_ctrl_hevc_sps *ctrl, const HEVCSPS *sps)
{
    *ctrl = (struct v4l2_ctrl_hevc_sps) {
        .chroma_format_idc = sps->chroma_format_idc,
        .pic_width_in_luma_samples = sps->width,
        .pic_height_in_luma_samples = sps->height,
        .bit_depth_luma_minus8 = sps->bit_depth - 8,
        .bit_depth_chroma_minus8 = sps->bit_depth - 8,
        .log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_poc_lsb - 4,
        .sps_max_dec_pic_buffering_minus1 = sps->temporal_layer[sps->max_sub_layers - 1].max_dec_pic_buffering - 1,
        .sps_max_num_reorder_pics = sps->temporal_layer[sps->max_sub_layers - 1].num_reorder_pics,
        .sps_max_latency_increase_plus1 = sps->temporal_layer[sps->max_sub_layers - 1].max_latency_increase + 1,
        .log2_min_luma_coding_block_size_minus3 = sps->log2_min_cb_size - 3,
        .log2_diff_max_min_luma_coding_block_size = sps->log2_diff_max_min_coding_block_size,
        .log2_min_luma_transform_block_size_minus2 = sps->log2_min_tb_size - 2,
        .log2_diff_max_min_luma_transform_block_size = sps->log2_max_trafo_size - sps->log2_min_tb_size,
        .max_transform_hierarchy_depth_inter = sps->max_transform_hierarchy_depth_inter,
        .max_transform_hierarchy_depth_intra = sps->max_transform_hierarchy_depth_intra,
        .pcm_sample_bit_depth_luma_minus1 = sps->pcm.bit_depth - 1,
        .pcm_sample_bit_depth_chroma_minus1 = sps->pcm.bit_depth_chroma - 1,
        .log2_min_pcm_luma_coding_block_size_minus3 = sps->pcm.log2_min_pcm_cb_size - 3,
        .log2_diff_max_min_pcm_luma_coding_block_size = sps->pcm.log2_max_pcm_cb_size - sps->pcm.log2_min_pcm_cb_size,
        .num_short_term_ref_pic_sets = sps->nb_st_rps,
        .num_long_term_ref_pics_sps = sps->num_long_term_ref_pics_sps,
        .sps_max_sub_layers_minus1 = sps->max_sub_layers - 1,
    };

    if (sps->separate_colour_plane_flag)
        ctrl->flags |= V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE;
    if (sps->scaling_list_enable_flag)
        ctrl->flags |= V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED;
    if (sps->amp_enabled_flag)
        ctrl->flags |= V4L2_HEVC_SPS_FLAG_AMP_ENABLED;
    if (sps->sao_enabled)
        ctrl->flags |= V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET;
    if (sps->pcm_enabled_flag)
        ctrl->flags |= V4L2_HEVC_SPS_FLAG_PCM_ENABLED;
    if (sps->pcm.loop_filter_disable_flag)
        ctrl->flags |= V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED;
    if (sps->long_term_ref_pics_present_flag)
        ctrl->flags |= V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT;
    if (sps->sps_temporal_mvp_enabled_flag)
        ctrl->flags |= V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED;
    if (sps->sps_strong_intra_smoothing_enable_flag)
        ctrl->flags |= V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED;
}

static void fill_scaling_matrix(const ScalingList *const sl, struct v4l2_ctrl_hevc_scaling_matrix *const sm)
{
    for (unsigned i = 0; i < 6; i++) {
        for (unsigned j = 0; j < 16; j++)
            sm->scaling_list_4x4[i][j] = sl->sl[0][i][j];
        for (unsigned j = 0; j < 64; j++) {
            sm->scaling_list_8x8[i][j] = sl->sl[1][i][j];
            sm->scaling_list_16x16[i][j] = sl->sl[2][i][j];
            if (i < 2)
                sm->scaling_list_32x32[i][j] = sl->sl[3][i * 3][j];
        }
        sm->scaling_list_dc_coef_16x16[i] = sl->sl_dc[0][i];
        if (i < 2)
            sm->scaling_list_dc_coef_32x32[i] = sl->sl_dc[1][i * 3];
    }
}

static void fill_pps(struct v4l2_ctrl_hevc_pps *const ctrl, const HEVCPPS *const pps)
{
    uint64_t flags = 0;

    if (pps->dependent_slice_segments_enabled_flag)
        flags |= V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED;
    if (pps->output_flag_present_flag)
        flags |= V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT;
    if (pps->sign_data_hiding_flag)
        flags |= V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED;
    if (pps->cabac_init_present_flag)
        flags |= V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT;
    if (pps->constrained_intra_pred_flag)
        flags |= V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED;
    if (pps->transform_skip_enabled_flag)
        flags |= V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED;
    if (pps->cu_qp_delta_enabled_flag)
        flags |= V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED;
    if (pps->pic_slice_level_chroma_qp_offsets_present_flag)
        flags |= V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT;
    if (pps->weighted_pred_flag)
        flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED;
    if (pps->weighted_bipred_flag)
        flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED;
    if (pps->transquant_bypass_enable_flag)
        flags |= V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED;
    if (pps->tiles_enabled_flag)
        flags |= V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
    if (pps->entropy_coding_sync_enabled_flag)
        flags |= V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED;
    if (pps->loop_filter_across_tiles_enabled_flag)
        flags |= V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED;
    if (pps->seq_loop_filter_across_slices_enabled_flag)
        flags |= V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED;
    if (pps->deblocking_filter_override_enabled_flag)
        flags |= V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED;
    if (pps->disable_dbf)
        flags |= V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER;
    if (pps->lists_modification_present_flag)
        flags |= V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT;
    if (pps->slice_header_extension_present_flag)
        flags |= V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT;

    *ctrl = (struct v4l2_ctrl_hevc_pps) {
        .num_extra_slice_header_bits = pps->num_extra_slice_header_bits,
        .init_qp_minus26 = pps->pic_init_qp_minus26,
        .diff_cu_qp_delta_depth = pps->diff_cu_qp_delta_depth,
        .pps_cb_qp_offset = pps->cb_qp_offset,
        .pps_cr_qp_offset = pps->cr_qp_offset,
        .pps_beta_offset_div2 = pps->beta_offset / 2,
        .pps_tc_offset_div2 = pps->tc_offset / 2,
        .log2_parallel_merge_level_minus2 = pps->log2_parallel_merge_level - 2,
        .flags = flags
    };

    if (pps->tiles_enabled_flag) {
        ctrl->num_tile_columns_minus1 = pps->num_tile_columns - 1;
        ctrl->num_tile_rows_minus1 = pps->num_tile_rows - 1;
        for (int i = 0; i < pps->num_tile_columns; i++)
            ctrl->column_width_minus1[i] = pps->column_width[i] - 1;
        for (int i = 0; i < pps->num_tile_rows; i++)
            ctrl->row_height_minus1[i] = pps->row_height[i] - 1;
    }
}

/* ---- the hooks ---- */

/* a slice, parsed up to its data (where a hwaccel's decode_slice is
   called): buffer/size the raw NAL unit */
void ff_hevc_trace_slice(HEVCContext *h, const uint8_t *buffer, uint32_t size)
{
    int bcount;
    uint32_t boff;
    if (!open_trace()) return;
    if (h->sh.first_slice_in_pic_flag) {
        for (unsigned i = 0; i < FF_ARRAY_ELEMS(h->DPB); i++)   /* (slots let go of forget their pictures: a */
            if (!h->DPB[i].flags)                               /* missing reference made up there isn't */
                slot_number[i] = 0;                             /* taken for an old picture) */
        number++;
        slot_number[h->ref - h->DPB] = number;
        fill_decode_params(h, &dec);
        nslices = 0;
        in_picture = 1;
    }
    if (!in_picture) return;
    if (nslices >= MAX_SLICES) {
        fprintf(stderr, "hevc_trace: more than %d slices in a picture\n", MAX_SLICES);
        in_picture = 0;
        return;
    }
    bcount = get_bits_count(&h->HEVClc->gb);
    boff = (uint32_t)(ptr_from_index(buffer, bcount / 8 + 1) - (buffer + bcount / 8 + 1)) * 8 + bcount;
    fill_slice_params(h, &dec, &sp[nslices], size * 8, boff);
    sdata[nslices] = buffer;               /* (the packet stays until the picture ends) */
    slen[nslices] = size;
    nslices++;
}

static uint32_t plane_adler(const AVFrame *f, int plane, int x0, int y0, int w, int h, int bytes)
{
    unsigned long a = 1;
    for (int y = 0; y < h; y++)
        a = av_adler32_update(a, f->data[plane] + (size_t)(y0 + y) * f->linesize[plane] + (size_t)x0 * bytes,
                              (size_t)w * bytes);
    return (uint32_t)a;
}

/* the picture decoded (the end of its packet) */
void ff_hevc_trace_end(HEVCContext *h)
{
    struct v4l2_ctrl_hevc_sps sps;
    struct v4l2_ctrl_hevc_pps pps;
    struct v4l2_ctrl_hevc_scaling_matrix sm;
    const ScalingList *sl;
    const HEVCSPS *s;
    int bytes, ow, oh, ol, ot;
    if (!tf || !in_picture || !h->ref || !nslices) return;
    in_picture = 0;
    s = h->ps.sps;
    sl = h->ps.pps->scaling_list_data_present_flag ? &h->ps.pps->scaling_list :
         s->scaling_list_enable_flag ? &s->scaling_list : NULL;
    memset(&sps, 0, sizeof sps); memset(&pps, 0, sizeof pps); memset(&sm, 0, sizeof sm);
    fill_sps(&sps, s);
    fill_pps(&pps, h->ps.pps);
    if (sl) fill_scaling_matrix(sl, &sm);
    fwrite("PIC1", 1, 4, tf);
    w32(number);
    w32((uint32_t)h->ref->poc);
    w32((uint32_t)s->width);
    w32((uint32_t)s->height);
    w32((uint32_t)s->bit_depth);
    w32(nslices);
    w32(sl != NULL);
    ol = s->output_window.left_offset;
    ot = s->output_window.top_offset;
    ow = s->width - ol - s->output_window.right_offset;
    oh = s->height - ot - s->output_window.bottom_offset;
    w32((uint32_t)ol);
    w32((uint32_t)ot);
    w32((uint32_t)ow);
    w32((uint32_t)oh);
    fwrite(&sps, sizeof sps, 1, tf);
    fwrite(&pps, sizeof pps, 1, tf);
    fwrite(&dec, sizeof dec, 1, tf);
    if (sl) fwrite(&sm, sizeof sm, 1, tf);
    for (unsigned i = 0; i < nslices; i++) {
        static const uint8_t zero[3];
        fwrite(&sp[i], sizeof sp[i], 1, tf);
        w32(slen[i]);
        fwrite(sdata[i], 1, slen[i], tf);
        fwrite(zero, 1, (4 - (slen[i] & 3)) & 3, tf);
    }
    bytes = s->bit_depth > 8 ? 2 : 1;
    w32(plane_adler(h->ref->frame, 0, ol, ot, ow, oh, bytes));
    w32(plane_adler(h->ref->frame, 1, ol >> s->hshift[1], ot >> s->vshift[1], (ow + (1 << s->hshift[1]) - 1) >> s->hshift[1],
                    (oh + (1 << s->vshift[1]) - 1) >> s->vshift[1], bytes));
    w32(plane_adler(h->ref->frame, 2, ol >> s->hshift[1], ot >> s->vshift[1], (ow + (1 << s->hshift[1]) - 1) >> s->hshift[1],
                    (oh + (1 << s->vshift[1]) - 1) >> s->vshift[1], bytes));
    fflush(tf);
}
