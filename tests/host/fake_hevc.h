/* fake_hevc.h - the fake Pi 4 HEVC block (fake_hevc.c) for the host tests.
   Part of riscos-reelhwaccel. GPL version 2 or later. */
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
} fake_hevc_state;
extern fake_hevc_state fake_hevc;

/* the picture for a first slice's bitstream hash: 0, its planes (8-bit
   I420, packed: strides w and (w+1)/2) and size (FFmpeg's output: the
   SPS's output window, here always from 0,0), or -1 */
typedef int (*fake_hevc_picture_fn)(uint32_t hash, const uint8_t **y, const uint8_t **u, const uint8_t **v, int *w,
                                    int *h);

void fake_hevc_reset(void);
void fake_hevc_set_pictures(fake_hevc_picture_fn fn);
int fake_hevc_live(void);             /* allocations not freed */
uint32_t fake_hevc_hash(const uint8_t *p, size_t n);
#endif
