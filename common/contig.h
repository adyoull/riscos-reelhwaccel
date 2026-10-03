/*
 * contig.h - physically contiguous pages for a Physical Memory Pool that
 * never include the page mapped at &8000 (vcdec's and hevcdec's pools).
 *
 * Why: OS_Memory 12 may recommend pages that an application slot is using,
 * the running program's own included. Claiming them (OS_DynamicArea 21)
 * makes the kernel copy the program's page elsewhere (Service_PagesUnsafe /
 * PagesSafe), so the program's page at &8000 gets a new physical address.
 * ARMEABISupport (1.08) identifies a program by that address and has no
 * service handler, so it loses the program: its stacks are never freed, and
 * a later program given the old page gets no abort handlers (the "code 6"
 * EMT trap). See riscos-unixlib's handoff of 2026-10-03.
 *
 * So: read the physical page at &8000 (OS_Memory 0), and ask OS_Memory 12
 * for pages wholly below it, else wholly above it (R4-R7, RISC OS 5.29+).
 * An older RISC OS ignores R4-R7, so the recommendation is also checked by
 * page number; if it still holds the program's page, there are no pages.
 *
 * PRM: OS_Memory 0 (bits 9, 11, 13), OS_Memory 12.
 *
 * Copyright (C) 2026 Andrew Youll. GNU GPL version 2 (see COPYING).
 */
#ifndef CONTIG_H
#define CONTIG_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kernel.h"

#ifndef OS_Memory
#define OS_Memory 0x68
#endif

typedef _kernel_oserror *contig_swi_fn(int swi, _kernel_swi_regs *r);

typedef struct {
    int have_pn, have_pa;
    uint32_t pn;                /* physical page number of the page at &8000 */
    uint32_t pa;                /* and its physical address (if under 4 GB) */
} contig_app_page;

/* The page mapped at &8000 now (OS_Memory 0: logical address given; page
   number, then physical address, filled in). */
static inline contig_app_page contig_read_app_page(contig_swi_fn *sw)
{
    contig_app_page a = { 0, 0, 0, 0 };
    _kernel_swi_regs r;
    uint32_t blk[3] = { 0, 0x8000, 0 };
    memset(&r, 0, sizeof r);
    r.r[0] = 1 << 9 | 1 << 11; r.r[1] = (int)(uintptr_t)blk; r.r[2] = 1;
    if (!sw(OS_Memory, &r)) { a.have_pn = 1; a.pn = blk[0]; }
    blk[0] = 0; blk[1] = 0x8000; blk[2] = 0;
    memset(&r, 0, sizeof r);
    r.r[0] = 1 << 9 | 1 << 13; r.r[1] = (int)(uintptr_t)blk; r.r[2] = 1;
    if (!sw(OS_Memory, &r)) { a.have_pa = 1; a.pa = blk[2]; }
    return a;
}

/* 1 if the page at &8000 is not the one in *before (as far as can be told) */
static inline int contig_app_page_moved(contig_swi_fn *sw, const contig_app_page *before)
{
    contig_app_page now = contig_read_app_page(sw);
    return (before->have_pn && now.have_pn && now.pn != before->pn) ||
           (before->have_pa && now.have_pa && now.pa != before->pa);
}

/* Page number of the first of `pages` physically contiguous pages, all at or
   below `top` (inclusive) and none of them the page at &8000, in *first;
   claim them next (OS_DynamicArea 21: the recommendation holds only until
   something else takes pages). NULL, or why not. */
static inline const char *contig_recommend(contig_swi_fn *sw, uint32_t pages, uint32_t top, const contig_app_page *app,
                                    uint32_t *first)
{
    static char why[320];
    uint32_t lo[2], hi[2];
    int n = 0;
    const char *e = NULL;
    if (app->have_pa && app->pa <= top) {
        if (app->pa >= pages << 12) { lo[n] = 0; hi[n] = app->pa - 1; n++; }                  /* below it, */
        if (top - app->pa >= pages << 12) { lo[n] = app->pa + 4096; hi[n] = top; n++; }        /* else above it */
    } else {
        lo[n] = 0; hi[n] = top; n++;                                                           /* (not in the range) */
    }
    for (int i = 0; i < n; i++) {
        _kernel_swi_regs r;
        _kernel_oserror *err;
        memset(&r, 0, sizeof r);
        r.r[0] = 12 | 1 << 8 | 1 << 9; r.r[1] = (int)(pages << 12); r.r[2] = 12;
        r.r[4] = (int)lo[i]; r.r[5] = 0; r.r[6] = (int)hi[i]; r.r[7] = 0;
        if ((err = sw(OS_Memory, &r)) != NULL) {
            snprintf(why, sizeof why, "OS_Memory 12 (%u pages, &%08X-&%08X): %s", (unsigned)pages, (unsigned)lo[i],
                     (unsigned)hi[i], err->errmess);
            e = why;
            continue;
        }
        if (app->have_pn && app->pn - (uint32_t)r.r[3] < pages) {   /* (a RISC OS that ignores R4-R7) */
            snprintf(why, sizeof why, "the only %u contiguous pages include the program's own page at &8000",
                     (unsigned)pages);
            e = why;
            continue;
        }
        *first = (uint32_t)r.r[3];
        return NULL;
    }
    if (!n) { snprintf(why, sizeof why, "no room for %u pages either side of the program's page at &8000", (unsigned)pages); e = why; }
    return e;
}

#endif
