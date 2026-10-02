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

/* physically contiguous memory the block can use, not cached (writes
   buffered: they reach memory before the next register write), readable
   and writable in USR mode; *bus its address on the block's bus. NULL if
   there's none. */
void *hevcdec_hw_alloc(void *hw, size_t size, uint64_t *bus);
void hevcdec_hw_free(void *hw, void *p);

uint32_t hevcdec_hw_now_cs(void);

#endif
