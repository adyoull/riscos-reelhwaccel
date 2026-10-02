/* hevc_trace.h - tools/hevctrace's hooks in FFmpeg's HEVC decoder (see
   hevc_trace.c). Part of riscos-reelhwaccel. GPL version 2. */
#ifndef AVCODEC_HEVC_TRACE_H
#define AVCODEC_HEVC_TRACE_H
#include <stdint.h>
struct HEVCContext;
void ff_hevc_trace_slice(struct HEVCContext *h, const uint8_t *buffer, uint32_t size);
void ff_hevc_trace_end(struct HEVCContext *h);
#endif
