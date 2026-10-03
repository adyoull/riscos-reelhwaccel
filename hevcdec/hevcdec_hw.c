/*
 * hevcdec_hw.c - hevcdec's machine on RISC OS (see hevcdec_hw.h):
 *  - the block's registers ("hevc" at &FEB00000, 64 KB) and its interrupt
 *    control ("intc", &FEB10000) mapped with OS_Memory 13, read and written
 *    in SVC mode (hevcdec_svc.S);
 *  - its clock (firmware clock 11) on at its maximum, through BCMSupport's
 *    property mailbox;
 *  - memory: a Physical Memory Pool for each buffer (OS_DynamicArea 0, 21,
 *    22, OS_Memory 12 for contiguous pages, as vcdec's pools), mapped not
 *    cacheable (bufferable), or cacheable (output frames: cleaned and
 *    invalidated with the kernel's Cache_CleanInvalidateRange ARMop, from
 *    OS_MMUControl 2, called in SVC mode, as vcdec); on the Pi 4 the block's bus address is the
 *    physical address (the SCB's dma-ranges map bus 0-16 GB to physical
 *    0-16 GB: Raspberry Pi Linux's bcm2711-rpi-ds.dtsi);
 *  - the time: OS_ReadMonotonicTime.
 * The pages never include the program's page at &8000 (../common/contig.h:
 * ARMEABISupport finds the program by that page).
 * PRM: OS_Memory 0/12/13, OS_DynamicArea 0/1/21/22, OS_MMUControl 2.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "hevcdec_hw.h"
#include "../common/contig.h"

#define OS_Module             0x1E
#define OS_ReadMonotonicTime  0x42
#define OS_DynamicArea        0x66
#define OS_Memory             0x68
#define OS_MMUControl         0x6B
#define OS_SynchroniseCodeAreas 0x6E
#define ARMOP_CACHE_CLEAN_INVALIDATE_RANGE 21
#define BCMSupport_SendTempPropertyBuffer 0x591C5

#define HEVC_PHYS   0xFEB00000u
#define HEVC_SIZE   0x10000u
#define INTC_PHYS   0xFEB10000u
#define INTC_SIZE   0x1000u
#define HEVC_CLOCK  11

#define DA_NOT_DRAGGABLE   (1u << 7)
#define DA_SPECIFIC_PAGES  (1u << 8)
#define DA_PMP             (1u << 20)
#define PAGE_NOT_CACHEABLE (1u << 5)
#define PAGE_LOCK          (1u << 15)

#define MAX_POOLS  128
#define PHYS_TOP   0xFC000000u                 /* (pages below the peripherals: 32-bit addresses) */
#define MAX_WRITES 256

void hevcdec_svc_writes(uint32_t base, const uint32_t *pairs, int n);
uint32_t hevcdec_svc_read(uint32_t addr);
void hevcdec_svc_call(uint32_t fn, uint32_t r0, uint32_t r1);

typedef struct { void *base; uint32_t area, pages; } pool_t;

struct hevcdec_hw {
    uint32_t hevc, intc;              /* the mappings */
    uint32_t stub;                    /* RMA: the pools' dynamic area handler (MOV PC, R14) */
    uint32_t w[2 * MAX_WRITES];
    int nw;
    pool_t pools[MAX_POOLS];
    int stuck;                        /* a pool wouldn't go: the handler stays */
    uint32_t armop_cci;               /* Cache_CleanInvalidateRange (0: none) */
    unsigned app_page_moves;          /* claims that moved the page at &8000 (should be none) */
    char why[320];                    /* why the last hevcdec_hw_alloc gave NULL */
};

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

uint32_t hevcdec_hw_now_cs(void)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof r);
    swi(OS_ReadMonotonicTime, &r);
    return (uint32_t)r.r[0];
}

/* one firmware property tag: 0, or -1 */
static int fw_tag(uint32_t tag, uint32_t *val, int in_words, int out_words)
{
    uint32_t buf[16];
    _kernel_swi_regs r;
    int words = in_words > out_words ? in_words : out_words;
    buf[0] = (uint32_t)((6 + words) * 4);
    buf[1] = 0;
    buf[2] = tag;
    buf[3] = (uint32_t)(words * 4);
    buf[4] = (uint32_t)(in_words * 4);
    for (int i = 0; i < words; i++) buf[5 + i] = i < in_words ? val[i] : 0;
    buf[5 + words] = 0;
    memset(&r, 0, sizeof r);
    r.r[0] = (int)(uintptr_t)buf; r.r[1] = (int)(uintptr_t)buf;
    if (swi(BCMSupport_SendTempPropertyBuffer, &r) || buf[1] != 0x80000000u) return -1;
    for (int i = 0; i < out_words; i++) val[i] = buf[5 + i];
    return 0;
}

static uint32_t map_io(uint32_t phys, uint32_t size)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof r);
    r.r[0] = 13; r.r[1] = (int)phys; r.r[2] = (int)size;
    return swi(OS_Memory, &r) ? 0 : (uint32_t)r.r[3];
}

int hevcdec_hw_open(hevcdec_hw **out, char *err, size_t errlen)
{
    hevcdec_hw *hw;
    _kernel_swi_regs r;
    uint32_t v[3], max[2];
    *out = NULL;
    if (!(hw = calloc(1, sizeof *hw))) { snprintf(err, errlen, "Out of memory"); return -1; }
    hw->hevc = map_io(HEVC_PHYS, HEVC_SIZE);
    hw->intc = map_io(INTC_PHYS, INTC_SIZE);
    if (!hw->hevc || !hw->intc) { snprintf(err, errlen, "The HEVC block can't be mapped (not a Pi 4?)"); free(hw); return -1; }
    /* the clock on, at its maximum (as Linux's rpivid sets its minimum rate) */
    max[0] = HEVC_CLOCK; max[1] = 0;
    if (fw_tag(0x00030004, max, 1, 2) || !max[1]) { snprintf(err, errlen, "The firmware gives no HEVC clock"); free(hw); return -1; }
    v[0] = HEVC_CLOCK; v[1] = 1;
    fw_tag(0x00038001, v, 2, 2);                       /* (on) */
    v[0] = HEVC_CLOCK; v[1] = max[1]; v[2] = 0;
    fw_tag(0x00038002, v, 3, 2);                       /* (at the maximum) */
    /* the pools' handler */
    memset(&r, 0, sizeof r);
    r.r[0] = 6; r.r[3] = 8;
    if (swi(OS_Module, &r)) { snprintf(err, errlen, "No RMA"); free(hw); return -1; }
    hw->stub = (uint32_t)r.r[2];
    {
        static const uint32_t code[2] = { 0xe1a0f00eu, 0xe1a0f00eu };   /* MOV PC, R14 */
        uint32_t pairs[4];
        /* (written in SVC mode: the RMA isn't user writable on every version) */
        pairs[0] = 0; pairs[1] = code[0]; pairs[2] = 4; pairs[3] = code[1];
        hevcdec_svc_writes(hw->stub, pairs, 2);
        memset(&r, 0, sizeof r);
        r.r[0] = 1; r.r[1] = (int)hw->stub; r.r[2] = (int)(hw->stub + 4);
        swi(OS_SynchroniseCodeAreas, &r);
    }
    memset(&r, 0, sizeof r);                           /* the cache maintenance for cached frames */
    r.r[0] = 2 | ARMOP_CACHE_CLEAN_INVALIDATE_RANGE << 8;
    if (!swi(OS_MMUControl, &r)) hw->armop_cci = (uint32_t)r.r[0];
    *out = hw;
    return 0;
}

int hevcdec_hw_can_cache(void *h) { return ((hevcdec_hw *)h)->armop_cci != 0; }

void hevcdec_hw_cache_clean_inv(void *h, const void *p, size_t n)
{
    hevcdec_hw *hw = h;
    uint32_t a = (uint32_t)(uintptr_t)p;
    if (hw->armop_cci && n) hevcdec_svc_call(hw->armop_cci, a & ~63u, (uint32_t)(a + n + 63) & ~63u);
}

void hevcdec_hw_write(void *h, unsigned int offset, uint32_t value)
{
    hevcdec_hw *hw = h;
    if (hw->nw == MAX_WRITES) hevcdec_hw_flush(hw);
    hw->w[2 * hw->nw] = offset;
    hw->w[2 * hw->nw + 1] = value;
    hw->nw++;
}

void hevcdec_hw_flush(void *h)
{
    hevcdec_hw *hw = h;
    if (hw->nw) hevcdec_svc_writes(hw->hevc, hw->w, hw->nw);
    hw->nw = 0;
}

uint32_t hevcdec_hw_read(void *h, unsigned int offset)
{
    hevcdec_hw *hw = h;
    hevcdec_hw_flush(hw);
    return hevcdec_svc_read(hw->hevc + offset);
}

uint32_t hevcdec_hw_ictrl(void *h) { return hevcdec_svc_read(((hevcdec_hw *)h)->intc); }

void hevcdec_hw_ictrl_write(void *h, uint32_t value)
{
    uint32_t pair[2] = { 0, value };
    hevcdec_svc_writes(((hevcdec_hw *)h)->intc, pair, 1);
}

/* ---- memory: a pool per buffer ---- */

static void pool_free(hevcdec_hw *hw, pool_t *p)
{
    _kernel_swi_regs r;
    uint32_t *l = malloc(p->pages * 12);
    if (l) {
        for (uint32_t j = 0; j < p->pages; j++) { l[3 * j] = j; l[3 * j + 1] = 0xFFFFFFFFu; l[3 * j + 2] = 0; }
        memset(&r, 0, sizeof r);
        r.r[0] = 22; r.r[1] = (int)p->area; r.r[2] = (int)(uintptr_t)l; r.r[3] = (int)p->pages;
        swi(OS_DynamicArea, &r);                       /* unmapped, */
        r.r[0] = 21; r.r[1] = (int)p->area; r.r[2] = (int)(uintptr_t)l; r.r[3] = (int)p->pages;
        swi(OS_DynamicArea, &r);                       /* released, */
        free(l);
    }
    memset(&r, 0, sizeof r);
    r.r[0] = 1; r.r[1] = (int)p->area;
    if (swi(OS_DynamicArea, &r)) hw->stuck = 1;        /* removed */
    p->base = NULL;
}

void *hevcdec_hw_alloc(void *h, size_t size, uint64_t *bus, int cached)
{
    hevcdec_hw *hw = h;
    _kernel_swi_regs r;
    uint32_t pages = (uint32_t)((size + 4095) >> 12), first, *l, phys[2];
    pool_t *p = NULL;
    const char *no;
    contig_app_page app = contig_read_app_page(swi);
    snprintf(hw->why, sizeof hw->why, "a pool, or its pages, couldn't be made");
    for (int i = 0; i < MAX_POOLS && !p; i++) if (!hw->pools[i].base) p = &hw->pools[i];
    if (!p) { snprintf(hw->why, sizeof hw->why, "all %d pools in use", MAX_POOLS); return NULL; }
    if (!pages || !(l = malloc(pages * 12))) { snprintf(hw->why, sizeof hw->why, "out of memory"); return NULL; }
    memset(&r, 0, sizeof r);                           /* the pool (no pages yet) */
    r.r[0] = 0; r.r[1] = -1; r.r[2] = 0; r.r[3] = -1;
    r.r[4] = (int)(DA_SPECIFIC_PAGES | DA_PMP | DA_NOT_DRAGGABLE);
    r.r[5] = (int)(pages << 12); r.r[6] = (int)hw->stub; r.r[7] = 0;
    r.r[8] = (int)(uintptr_t)"hevcdec buffer"; r.r[9] = (int)pages;
    if (swi(OS_DynamicArea, &r)) { free(l); return NULL; }
    p->area = (uint32_t)r.r[1];
    p->base = (void *)(uintptr_t)(uint32_t)r.r[3];
    p->pages = 0;
    /* contiguous pages below PHYS_TOP (claimed straight after: PRM), not the program's page at &8000 */
    if ((no = contig_recommend(swi, pages, PHYS_TOP - 1, &app, &first)) != NULL) {
        snprintf(hw->why, sizeof hw->why, "%s", no);
        free(l); pool_free(hw, p); return NULL;
    }
    for (uint32_t j = 0; j < pages; j++) { l[3 * j] = j; l[3 * j + 1] = first + j; l[3 * j + 2] = PAGE_LOCK; }
    memset(&r, 0, sizeof r);
    r.r[0] = 21; r.r[1] = (int)p->area; r.r[2] = (int)(uintptr_t)l; r.r[3] = (int)pages;
    if (swi(OS_DynamicArea, &r)) { free(l); pool_free(hw, p); return NULL; }
    p->pages = pages;
    if (cached && !hw->armop_cci) cached = 0;
    for (uint32_t j = 0; j < pages; j++) {
        l[3 * j] = j; l[3 * j + 1] = j; l[3 * j + 2] = PAGE_LOCK | (cached ? 0 : PAGE_NOT_CACHEABLE);
    }
    memset(&r, 0, sizeof r);
    r.r[0] = 22; r.r[1] = (int)p->area; r.r[2] = (int)(uintptr_t)l; r.r[3] = (int)pages;
    if (swi(OS_DynamicArea, &r)) { free(l); pool_free(hw, p); return NULL; }
    free(l);
    if (contig_app_page_moved(swi, &app)) hw->app_page_moves++;
    snprintf(hw->why, sizeof hw->why, "the pages weren't as asked for");
    for (int k = 0; k < 2; k++) {                       /* (contiguous, checked: first and last pages) */
        uint32_t blk[3] = { 0, (uint32_t)(uintptr_t)p->base + (k ? (pages - 1) << 12 : 0), 0 };
        memset(&r, 0, sizeof r);
        r.r[0] = 0x2200; r.r[1] = (int)(uintptr_t)blk; r.r[2] = 1;
        if (swi(OS_Memory, &r)) { pool_free(hw, p); return NULL; }
        phys[k] = blk[2];
    }
    if (phys[1] != phys[0] + ((pages - 1) << 12) || phys[1] > PHYS_TOP - 4096) { pool_free(hw, p); return NULL; }
    {                                                  /* readable in USR mode? (OS_Memory 24: flags in R1) */
        memset(&r, 0, sizeof r);
        r.r[0] = 24; r.r[1] = (int)(uintptr_t)p->base; r.r[2] = (int)((uintptr_t)p->base + (pages << 12));
        if (swi(OS_Memory, &r) || !(r.r[1] & 1)) { pool_free(hw, p); return NULL; }
    }
    memset(p->base, 0, (size_t)pages << 12);
    if (cached) hevcdec_hw_cache_clean_inv(hw, p->base, (size_t)pages << 12);   /* (the zeros out to memory) */
    *bus = phys[0];
    return p->base;
}

const char *hevcdec_hw_why(void *h) { return ((hevcdec_hw *)h)->why; }
unsigned hevcdec_hw_app_page_moves(void *h) { return ((hevcdec_hw *)h)->app_page_moves; }

void hevcdec_hw_free(void *h, void *ptr)
{
    hevcdec_hw *hw = h;
    for (int i = 0; ptr && i < MAX_POOLS; i++)
        if (hw->pools[i].base == ptr) { pool_free(hw, &hw->pools[i]); return; }
}

void hevcdec_hw_close(hevcdec_hw *hw, int leave)
{
    _kernel_swi_regs r;
    if (!hw) return;
    if (leave) { free(hw); return; }         /* (the pools and their handler stay) */
    for (int i = 0; i < MAX_POOLS; i++) if (hw->pools[i].base) pool_free(hw, &hw->pools[i]);
    if (!hw->stuck) {
        memset(&r, 0, sizeof r);
        r.r[0] = 7; r.r[2] = (int)hw->stub;
        swi(OS_Module, &r);
    }
    free(hw);
}
