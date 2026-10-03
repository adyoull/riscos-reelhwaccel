/*
 * hevcdec_hw.h - what hevcdec needs from the machine: the HEVC block's
 * registers, its clock, physically contiguous memory, the time. On RISC
 * OS: hevcdec_hw.c; in the host tests: a fake block.
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#ifndef HEVCDEC_HW_H
#define HEVCDEC_HW_H

#include <stddef.h>
#include <stdint.h>

typedef struct hevcdec_hw hevcdec_hw;

/* the block mapped and its clock on, at its maximum: 0, or -1 (err says why) */
int hevcdec_hw_open(hevcdec_hw **hw, char *err, size_t errlen);
/* leave: the memory is left as it is (the block may still be writing to it) */
void hevcdec_hw_close(hevcdec_hw *hw, int leave);

/* block registers: writes are gathered and go out (in order) at a flush or
   a read; the interrupt control register separately */
void hevcdec_hw_write(void *hw, unsigned int offset, uint32_t value);
void hevcdec_hw_flush(void *hw);
uint32_t hevcdec_hw_read(void *hw, unsigned int offset);
uint32_t hevcdec_hw_ictrl(void *hw);
void hevcdec_hw_ictrl_write(void *hw, uint32_t value);

/* physically contiguous memory the block can use, readable and writable
   in USR mode; *bus its address on the block's bus. NULL if there's none.
   Not cached (writes buffered: they reach memory before the next register
   write), or with cached set (only if hevcdec_hw_can_cache) cacheable:
   then what the program writes must be cleaned out to memory, and what
   the block writes invalidated, with hevcdec_hw_cache_clean_inv. */
void *hevcdec_hw_alloc(void *hw, size_t size, uint64_t *bus, int cached);
void hevcdec_hw_free(void *hw, void *p);
/* why the last hevcdec_hw_alloc gave NULL */
const char *hevcdec_hw_why(void *hw);
/* allocations whose claim moved the program's page at &8000 to another
   physical page (ARMEABISupport finds programs by it): should be 0 */
unsigned hevcdec_hw_app_page_moves(void *hw);
int hevcdec_hw_can_cache(void *hw);
/* cached memory from p for n bytes: written back and dropped from the
   caches (whole 64-byte lines) */
void hevcdec_hw_cache_clean_inv(void *hw, const void *p, size_t n);

uint32_t hevcdec_hw_now_cs(void);

#endif
