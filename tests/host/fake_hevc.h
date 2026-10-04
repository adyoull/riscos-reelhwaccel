/* fake_hevc.h - the fake Pi 4 HEVC block (fake_hevc.c) for the host tests.
   Part of riscos-reelhwaccel. GPL version 2. */
#ifndef FAKE_HEVC_H
#define FAKE_HEVC_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fails;                        /* the fake's own CHECK failures */
    int opens, closes, allocs, writes;
    int left;                         /* closed leaving the memory (a phase never finished) */
    int phase1s, phase2s, slices, ref_errors, unknown_pictures;
    int scaling_writes, scaling_not_flat;  /* writes to the scaling factors, and those not all 16 */
    int factors_wrong;                /* pictures decoded with stale factors (no scaling lists, factors not flat) */
    /* switches */
    int p1_exhaust, p1_hang, p2_hang, no_block, no_memory;
    size_t memory_left;               /* (0: no limit) */
    int quiet;                        /* failures expected: counted, not printed */
    int overrun;                      /* bytes phase 2 writes past the end of each frame */
    int no_cache;                     /* no cache maintenance: frames must be uncached */
    int p1_fail;                      /* this phase 1 (counting from 1) fails: CFSTATUS short, no status */
    int cached_allocs, cache_ops, evictions;
    int p1_ticks, p2_ticks;           /* reads of the interrupt control register each phase takes (1) */
    int overlaps;                     /* reads seen with both phases running */
} fake_hevc_state;
extern fake_hevc_state fake_hevc;

/* the picture for its slices' bitstream hash (each slice's fake_hevc_hash,
   combined FNV-1a fashion from 2166136261): 0, its planes (I420,
   packed: strides w and (w+1)/2 samples; *bytes a sample: 1 for 8-bit, 2
   (uint16_t) for 10-bit) and size (FFmpeg's output: the SPS's output
   window, here always from 0,0), or -1 */
typedef int (*fake_hevc_picture_fn)(uint32_t hash, const void **y, const void **u, const void **v, int *w, int *h,
                                    int *bytes);

void fake_hevc_reset(void);
void fake_hevc_set_pictures(fake_hevc_picture_fn fn);
int fake_hevc_live(void);             /* allocations not freed */
uint32_t fake_hevc_hash(const uint8_t *p, size_t n);
#endif
