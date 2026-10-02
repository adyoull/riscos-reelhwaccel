/*
 * fake_hevc.c - a fake Pi 4 HEVC block for hevcdec's host tests: it takes
 * the place of hevcdec_hw.c (registers, interrupt control, memory, time).
 *
 * Phase 1 (started by the write to CFBASE) reads the command list from
 * memory and checks it: every write to a phase-1 register or table, the
 * bitstream the BFBASE/BFNUM pairs point at (the first slice's bytes are
 * hashed: that names the picture), and each slice's commands (its
 * references: DPB slot and POC). Then CFSTATUS = CFNUM and ACTIVE1 latches
 * (if it's enabled - rpivid's notes: it isn't latched otherwise).
 * Phase 2 (the write to NUMROWS) checks each reference's frame holds the
 * picture with that POC, then "decodes": it writes the picture the test
 * gives for the hash (FFmpeg's decode) into the output frame as the block
 * would - NV12 in 128-byte columns - records its POC, and ACTIVE2 latches.
 *
 * Switches: p1_exhaust (phase 1 says the PU buffer ran out, once),
 * p1_hang / p2_hang (the phase never finishes).
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hevcdec_hw.h"
#include "fake_hevc.h"

#define NREG    (0x10000 / 4)
#define NALLOC  256
#define NFRAMEPOC 256

struct hevcdec_hw { int unused; };

static struct hevcdec_hw the_hw;
static uint32_t regs[NREG], ictrl, clock_now;
static uint32_t pend[2 * 1024];
static int npend, opened;
static struct { void *p; size_t n; } allocs[NALLOC];
static struct { uint64_t addr; uint32_t poc; } framepoc[NFRAMEPOC];
static int nframepoc;
/* phase 1's findings, for phase 2 */
static uint32_t pic_hash;
static int have_hash;
static struct { unsigned dpb, poc; } refs[64];
static int nrefs;
static fake_hevc_picture_fn picture_fn;
/* The scaling factors (0x2000-0x2FDF): as on the Pi 4 (HEVCTest 0.1 and
   0.1.1), the block uses them for every picture and keeps them from one
   picture, and one decoder, to the next; at power on they're whatever
   they are. (SPS1's bit 20, scaling lists, is set anyway by the PCM
   fields' overflow when FFmpeg fills them for a stream without PCM.) So a
   picture whose phase 1 doesn't load them comes out wrong. */
static int factors_wrong;                /* this picture's */

fake_hevc_state fake_hevc;

#define CHECK(c, ...) do { if (!(c)) { fake_hevc.fails++; if (!fake_hevc.quiet) { printf("FAIL (fake HEVC block): " __VA_ARGS__); printf("\n"); } } } while (0)

void fake_hevc_reset(void)
{
    for (int i = 0; i < NALLOC; i++) { free(allocs[i].p); allocs[i].p = NULL; }
    memset(regs, 0, sizeof regs);
    ictrl = 0x44;                        /* (reset value: both enables set) */
    npend = 0; opened = 0; nframepoc = 0; have_hash = 0; nrefs = 0;
    memset(&fake_hevc, 0, sizeof fake_hevc);
}

void fake_hevc_set_pictures(fake_hevc_picture_fn fn) { picture_fn = fn; }

int fake_hevc_live(void)
{
    int n = 0;
    for (int i = 0; i < NALLOC; i++) n += allocs[i].p != NULL;
    return n;
}

static void *mem(uint64_t bus, size_t n, const char *what)
{
    for (int i = 0; i < NALLOC; i++)
        if (allocs[i].p && bus >= (uint64_t)(uintptr_t)allocs[i].p &&
            bus + n <= (uint64_t)(uintptr_t)allocs[i].p + allocs[i].n)
            return (void *)(uintptr_t)bus;
    CHECK(0, "%s: %u bytes at &%llx: not the block's memory", what, (unsigned)n, (unsigned long long)bus);
    return NULL;
}

uint32_t fake_hevc_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* ---- phase 1: the command list ---- */

static void phase1(void)
{
    const uint32_t *cmd;
    uint32_t n = regs[0x70 / 4];                         /* CFNUM */
    uint64_t base = (uint64_t)regs[0x6C / 4] << 6;       /* CFBASE */
    uint32_t bfbase = 0, bfnum = 0, slicecmds = 0, nmsg = 0, nfactors = 0;
    uint32_t msgs[512];
    fake_hevc.phase1s++;
    have_hash = 0;
    nrefs = 0;
    CHECK(n > 0 && n < 1000000, "phase 1 with %u commands", (unsigned)n);
    if (!(cmd = mem(base, (size_t)n * 8, "the command list"))) return;
    CHECK(regs[0x50 / 4] && regs[0x58 / 4] && regs[0x54 / 4] && regs[0x5C / 4], "phase 1 without PU/coeff buffers");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = cmd[2 * i], v = cmd[2 * i + 1];
        int ok = a < 0x80 || (a >= 0x1000 && a < 0x10A0) || (a >= 0x2000 && a < 0x2FE0) || (a >= 0x4000 && a < 0x4800);
        CHECK(ok, "command %u writes &%X", (unsigned)i, (unsigned)a);
        if (a >= 0x2000 && a < 0x2FE0) {                 /* the scaling factors */
            fake_hevc.scaling_writes++;
            if (v != 0x10101010) fake_hevc.scaling_not_flat++;
            nfactors++;
        }
        if (a == 64) bfbase = v;                         /* BFBASE */
        else if (a == 68) bfnum = v;                     /* BFNUM */
        else if (a == 72 && !(v & 0x80)) {               /* BFCONTROL, not the stop: the bitstream starts */
            const uint8_t *b = mem(((uint64_t)bfbase << 6) + (v & 63), bfnum, "a slice's bitstream");
            CHECK((v & 0x40) != 0, "bitstream without emulation prevention removal");
            if (b && !have_hash) { pic_hash = fake_hevc_hash(b, bfnum); have_hash = 1; }
            fake_hevc.slices++;
        } else if (a == 96) {                            /* SLICECMDS: then its messages */
            slicecmds = v & 0xFF;
            nmsg = 0;
        } else if (a >= 0x4000 && a < 0x4800) {
            uint32_t k = (a - 0x4000) / 4;
            if (k < 512) msgs[k] = v;
            if (++nmsg == slicecmds) {                   /* the slice's references */
                uint32_t m0 = msgs[0], type = m0 & 3, l0 = (m0 >> 2) & 15, l1 = (m0 >> 6) & 15, j = 1;
                if (type == 2 || type == 3)
                    for (uint32_t r = 0; r < l0 + l1 && j + 1 < slicecmds; r++) {
                        uint32_t m = msgs[j++], poc = msgs[j++];
                        if (nrefs < 64) { refs[nrefs].dpb = m & 15; refs[nrefs].poc = poc & 0xFFFF; nrefs++; }
                        if (m & (3 << 5)) j += 6;        /* (weights) */
                    }
            }
        }
    }
    CHECK(have_hash, "phase 1 without a bitstream");
    factors_wrong = nfactors != 4064 / 4;               /* all of them, for this picture */
    if (fake_hevc.p1_exhaust) {                          /* the PU buffer ran out: CFSTATUS short */
        fake_hevc.p1_exhaust = 0;
        regs[0x74 / 4] = n - 1;
        regs[0x38 / 4] = 16;                             /* STATUS_PU_EXHAUSTED */
    } else {
        regs[0x74 / 4] = n;                              /* CFSTATUS = CFNUM: done */
        regs[0x38 / 4] = 0;
    }
    if (fake_hevc.p1_hang) return;
    if (ictrl & (1u << 2)) ictrl |= 1u << 0;            /* ACTIVE1 */
}

/* ---- phase 2: the picture ---- */

static uint32_t poc_of(uint64_t addr, int *found)
{
    for (int i = 0; i < nframepoc; i++)
        if (framepoc[i].addr == addr) { *found = 1; return framepoc[i].poc; }
    *found = 0;
    return 0;
}

static void phase2(void)
{
    uint64_t y = (uint64_t)regs[0x8018 / 4] << 6, c = (uint64_t)regs[0x8020 / 4] << 6;
    uint32_t col = regs[0x801C / 4] << 6, size = regs[0x802C / 4], poc = regs[0x8040 / 4] & 0xFFFF;
    int w = (int)(size & 0xFFFF), h = (int)(size >> 16), pw = 0, ph = 0;
    const uint8_t *py = NULL, *pu = NULL, *pv = NULL;
    uint8_t *out;
    fake_hevc.phase2s++;
    CHECK(regs[0x8000 / 4] == regs[0x50 / 4] && regs[0x8008 / 4] == regs[0x58 / 4],
          "phase 2 reads other PU/coeff buffers than phase 1 wrote");
    CHECK(regs[0x8024 / 4] == regs[0x801C / 4] && c > y && c - y < col, "output planes: Y &%llx, C &%llx, column %u",
          (unsigned long long)y, (unsigned long long)c, (unsigned)col);
    for (int i = 0; i < nrefs; i++) {                   /* each reference holds the right picture */
        int found;
        uint64_t ra = (uint64_t)regs[(0x9000 + 16 * refs[i].dpb) / 4] << 6;
        uint32_t rp = poc_of(ra, &found);
        if (!found || rp != refs[i].poc) {
            fake_hevc.ref_errors++;
            CHECK(0, "reference DPB %u: frame &%llx holds poc %u%s, not %u", refs[i].dpb, (unsigned long long)ra,
                  (unsigned)rp, found ? "" : " (nothing)", (unsigned)refs[i].poc);
        }
    }
    if (!(out = mem(y, (size_t)col * (size_t)((w + 127) / 128), "the output frame"))) return;
    if (have_hash && picture_fn && picture_fn(pic_hash, &py, &pu, &pv, &pw, &ph) == 0) {
        int cw = (pw + 1) / 2, ch = (ph + 1) / 2;
        CHECK(pw <= w && ph <= h, "a %dx%d picture in a %dx%d frame", pw, ph, w, h);
        memset(out, 0x80, (size_t)col * (size_t)((w + 127) / 128));      /* (outside the window: something) */
        if (factors_wrong) fake_hevc.factors_wrong++;
        for (int x0 = 0; x0 < pw; x0 += 128) {
            uint8_t *cy = out + (size_t)(x0 / 128) * col, *cc = (uint8_t *)(uintptr_t)c + (size_t)(x0 / 128) * col;
            int n = pw - x0 < 128 ? pw - x0 : 128, nc = cw - x0 / 2 < 64 ? cw - x0 / 2 : 64;
            for (int r = 0; r < ph; r++) memcpy(cy + (size_t)r * 128, py + (size_t)r * pw + x0, (size_t)n);
            if (factors_wrong) cy[0] ^= 1;                /* (dequantised wrongly: some samples off) */
            for (int r = 0; r < ch; r++)
                for (int x = 0; x < nc; x++) {
                    cc[(size_t)r * 128 + 2 * x] = pu[(size_t)r * cw + x0 / 2 + x];
                    cc[(size_t)r * 128 + 2 * x + 1] = pv[(size_t)r * cw + x0 / 2 + x];
                }
        }
    } else {
        fake_hevc.unknown_pictures++;
        memset(out, 0x10, (size_t)col * (size_t)((w + 127) / 128));
    }
    if (fake_hevc.overrun) {                             /* (a block writing past the frame's end) */
        uint8_t *past = mem(y, (size_t)col * (size_t)((w + 127) / 128) + (size_t)fake_hevc.overrun, "past the frame");
        if (past) memset(past + (size_t)col * (size_t)((w + 127) / 128), 0x55, (size_t)fake_hevc.overrun);
    }
    {
        int found;
        poc_of(y, &found);
        if (found) {
            for (int i = 0; i < nframepoc; i++) if (framepoc[i].addr == y) framepoc[i].poc = poc;
        } else if (nframepoc < NFRAMEPOC) {
            framepoc[nframepoc].addr = y; framepoc[nframepoc].poc = poc; nframepoc++;
        }
    }
    if (fake_hevc.p2_hang) return;
    if (ictrl & (1u << 6)) ictrl |= 1u << 4;            /* ACTIVE2 */
}

/* ---- hevcdec_hw.h ---- */

int hevcdec_hw_open(hevcdec_hw **hw, char *err, size_t errlen)
{
    if (fake_hevc.no_block) { snprintf(err, errlen, "The HEVC block can't be mapped (not a Pi 4?)"); return -1; }
    CHECK(!opened, "opened twice");
    opened = 1;
    fake_hevc.opens++;
    *hw = &the_hw;
    return 0;
}

void hevcdec_hw_close(hevcdec_hw *hw, int leave)
{
    fake_hevc.left = leave;
    CHECK(hw == &the_hw && opened, "closed without being opened");
    CHECK(npend == 0, "closed with %d register writes not sent", npend);
    opened = 0;
    fake_hevc.closes++;
}

void hevcdec_hw_write(void *hw, unsigned int offset, uint32_t value)
{
    CHECK(hw == &the_hw && opened, "a write while closed");
    CHECK(offset < 0x10000 && !(offset & 3), "a write to &%X", offset);
    if (npend < 1024) { pend[2 * npend] = offset; pend[2 * npend + 1] = value; npend++; }
}

void hevcdec_hw_flush(void *hw)
{
    (void)hw;
    for (int i = 0; i < npend; i++) {
        uint32_t o = pend[2 * i], v = pend[2 * i + 1];
        regs[o / 4] = v;
        fake_hevc.writes++;
        if (o == 0x6C) phase1();                         /* CFBASE: phase 1 starts */
        if (o == 0x8010) phase2();                       /* NUMROWS: phase 2 starts */
    }
    npend = 0;
}

uint32_t hevcdec_hw_read(void *hw, unsigned int offset)
{
    hevcdec_hw_flush(hw);
    return regs[offset / 4];
}

uint32_t hevcdec_hw_ictrl(void *hw) { (void)hw; return ictrl; }

void hevcdec_hw_ictrl_write(void *hw, uint32_t v)
{
    (void)hw;
    CHECK((v & ((0xFFu << 12) | (1u << 11))) == 0, "ICTRL written with reserved bits &%08X", (unsigned)v);
    ictrl &= ~(v & ((1u << 0) | (1u << 4) | (1u << 25) | (1u << 29)));   /* latched bits: 1 clears */
    ictrl = (ictrl & ~((1u << 2) | (1u << 6))) | (v & ((1u << 2) | (1u << 6)));   /* enables */
}

void *hevcdec_hw_alloc(void *hw, size_t size, uint64_t *bus)
{
    (void)hw;
    if (fake_hevc.no_memory || (fake_hevc.memory_left && size > fake_hevc.memory_left)) return NULL;
    for (int i = 0; i < NALLOC; i++)
        if (!allocs[i].p) {
            size_t n = (size + 4095) & ~(size_t)4095;
            if (!(allocs[i].p = aligned_alloc(4096, n))) return NULL;
            memset(allocs[i].p, 0, n);
            allocs[i].n = n;
            if (fake_hevc.memory_left) fake_hevc.memory_left -= size;
            *bus = (uint64_t)(uintptr_t)allocs[i].p;
            fake_hevc.allocs++;
            return allocs[i].p;
        }
    return NULL;
}

void hevcdec_hw_free(void *hw, void *p)
{
    (void)hw;
    for (int i = 0; i < NALLOC; i++)
        if (allocs[i].p == p) {
            /* (a frame's POC forgotten with it) */
            for (int k = 0; k < nframepoc; k++)
                if (framepoc[k].addr >= (uint64_t)(uintptr_t)p && framepoc[k].addr < (uint64_t)(uintptr_t)p + allocs[i].n)
                    framepoc[k] = framepoc[--nframepoc], k--;
            free(p);
            allocs[i].p = NULL;
            return;
        }
    CHECK(0, "freeing memory that isn't the block's");
}

uint32_t hevcdec_hw_now_cs(void) { return clock_now += 1; }
