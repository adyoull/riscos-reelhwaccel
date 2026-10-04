/*
 * fake_hevc.c - a fake Pi 4 HEVC block for hevcdec's host tests: it takes
 * the place of hevcdec_hw.c (registers, interrupt control, memory, time).
 *
 * Phase 1 (started by the write to CFBASE) reads the command list from
 * memory and checks it: every write to a phase-1 register or table, the
 * bitstream the BFBASE/BFNUM pairs point at (every slice's bytes are
 * hashed: that names the picture), and each slice's commands (its
 * references: DPB slot and POC). Then CFSTATUS = CFNUM and ACTIVE1 latches
 * (if it's enabled - rpivid's notes: it isn't latched otherwise).
 * Phase 2 (the write to NUMROWS) checks each reference's frame holds the
 * picture with that POC, then "decodes": it writes the picture the test
 * gives for the hash (FFmpeg's decode) into the output frame as the block
 * would - NV12 in 128-byte columns, or for 10-bit (CONFIG2 bit 8) three
 * samples a 32-bit word, 96 a column's row - records its POC, and ACTIVE2
 * latches.
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
/* p: the program's view; ram: the block's (its bus address). Cacheable
   memory is modelled as the Pi's caches behave: the program sees p, the
   block ram; hevcdec_hw_cache_clean_inv writes back what the program
   changed (p differing from synced) and then reloads p from ram; and when
   phase 2 ends, anything the program changed and didn't clean is written
   back anyway (lines evicted), over what the block wrote. */
static struct { void *p, *ram, *synced; size_t n; int cached; } allocs[NALLOC];
static struct { uint64_t addr; uint32_t poc; } framepoc[NFRAMEPOC];
static int nframepoc;
/* phase 1's findings for the picture in hand (pic_hash ... factors_wrong),
   kept for its phase 2 in a record found by its PU buffer */
static uint32_t pic_hash;
static int have_hash;
static struct { unsigned dpb, poc; } refs[64];
static int nrefs;
#define NRECS 8
static struct {
    int used;                            /* phase 1 done, phase 2 not yet */
    uint64_t pu, coeff;
    uint32_t hash;
    int have_hash, nrefs, factors_wrong;
    struct { unsigned dpb, poc; } refs[64];
} recs[NRECS];
/* The phases run as on the Pi: started by their last register write, and
   finished some time later (here: a number of reads of the interrupt
   control register), each one at a time; what phase 1 reads (its command
   list, the bitstream) is read as it finishes, so memory changed while it
   runs shows. Registers as they were when each started. */
static uint32_t s1[NREG], s2[NREG];
static int p1_busy, p1_left, p2_busy, p2_left, p2_rec;
static fake_hevc_picture_fn picture_fn;
/* The scaling factors (0x2000-0x2FDF): as on the Pi 4 (HEVCTest 0.1 and
   0.1.1), the block uses them for every picture and keeps them from one
   picture, and one decoder, to the next; at power on they're whatever
   they are. (SPS1's bit 20, scaling lists, is set anyway by the PCM
   fields' overflow when FFmpeg fills them for a stream without PCM.) So a
   picture whose phase 1 doesn't load them comes out wrong. */
static int factors_wrong;                /* this picture's */
static void evict_dirty(void);

fake_hevc_state fake_hevc;

#define CHECK(c, ...) do { if (!(c)) { fake_hevc.fails++; if (!fake_hevc.quiet) { printf("FAIL (fake HEVC block): " __VA_ARGS__); printf("\n"); } } } while (0)

void fake_hevc_reset(void)
{
    for (int i = 0; i < NALLOC; i++) {
        if (allocs[i].p && allocs[i].cached) { free(allocs[i].ram); free(allocs[i].synced); }
        free(allocs[i].p); allocs[i].p = NULL;
    }
    memset(regs, 0, sizeof regs);
    ictrl = 0x44;                        /* (reset value: both enables set) */
    npend = 0; opened = 0; nframepoc = 0; have_hash = 0; nrefs = 0;
    memset(recs, 0, sizeof recs);
    p1_busy = p2_busy = 0;
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
        if (allocs[i].p && bus >= (uint64_t)(uintptr_t)allocs[i].ram &&
            bus + n <= (uint64_t)(uintptr_t)allocs[i].ram + allocs[i].n)
            return (void *)(uintptr_t)bus;
    CHECK(0, "%s: %u bytes at &%llx: not the block's memory", what, (unsigned)n, (unsigned long long)bus);
    return NULL;
}

uint32_t fake_hevc_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n && !p[n - 1]) n--;      /* (trailing zero bytes: an Annex B stream's, before a 4-byte start code) */
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* ---- phase 1: the command list ---- */

static void phase1_done(void)
{
    const uint32_t *cmd;
    uint32_t n = s1[0x70 / 4];                         /* CFNUM */
    uint64_t base = (uint64_t)s1[0x6C / 4] << 6;       /* CFBASE */
    uint32_t bfbase = 0, bfnum = 0, slicecmds = 0, nmsg = 0, nfactors = 0;
    uint32_t msgs[512];
    fake_hevc.phase1s++;
    have_hash = 0;
    nrefs = 0;
    CHECK(n > 0 && n < 1000000, "phase 1 with %u commands", (unsigned)n);
    if (!(cmd = mem(base, (size_t)n * 8, "the command list"))) return;
    CHECK(s1[0x50 / 4] && s1[0x58 / 4] && s1[0x54 / 4] && s1[0x5C / 4], "phase 1 without PU/coeff buffers");
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
            if (b) {                                     /* (every slice's: the picture's, as read now) */
                if (!have_hash) pic_hash = 2166136261u;
                pic_hash = (pic_hash ^ fake_hevc_hash(b, bfnum)) * 16777619u;
                have_hash = 1;
            }
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
    if (fake_hevc.p1_fail && fake_hevc.phase1s == fake_hevc.p1_fail) {   /* a failure rpivid can't mend */
        regs[0x74 / 4] = n - 1;
        regs[0x38 / 4] = 0;
    } else if (fake_hevc.p1_exhaust) {                   /* the PU buffer ran out: CFSTATUS short */
        fake_hevc.p1_exhaust = 0;
        regs[0x74 / 4] = n - 1;
        regs[0x38 / 4] = 16;                             /* STATUS_PU_EXHAUSTED */
    } else {
        int k;
        regs[0x74 / 4] = n;                              /* CFSTATUS = CFNUM: done */
        regs[0x38 / 4] = 0;
        for (k = 0; k < NRECS && recs[k].used; k++) {}
        CHECK(k < NRECS, "more than %d pictures between phase 1 and phase 2", NRECS);
        if (k < NRECS) {                                 /* (kept for the picture's phase 2) */
            recs[k].used = 1;
            recs[k].pu = (uint64_t)s1[0x50 / 4] << 6;
            recs[k].coeff = (uint64_t)s1[0x58 / 4] << 6;
            recs[k].hash = pic_hash;
            recs[k].have_hash = have_hash;
            recs[k].nrefs = nrefs;
            recs[k].factors_wrong = factors_wrong;
            memcpy(recs[k].refs, refs, sizeof refs);
        }
    }
    if (ictrl & (1u << 2)) ictrl |= 1u << 0;            /* ACTIVE1 */
}

static void phase1_start(void)
{
    CHECK(!p1_busy, "phase 1 started while phase 1 is running");
    memcpy(s1, regs, sizeof regs);
    for (int k = 0; k < NRECS; k++)
        CHECK(!recs[k].used || (recs[k].pu != (uint64_t)s1[0x50 / 4] << 6 && recs[k].coeff != (uint64_t)s1[0x58 / 4] << 6),
              "phase 1 writing PU/coefficient buffers (&%llx) whose phase 2 hasn't run", (unsigned long long)recs[k].pu);
    p1_busy = 1;
    p1_left = fake_hevc.p1_ticks > 0 ? fake_hevc.p1_ticks : 1;
}

/* ---- phase 2: the picture ---- */

static uint32_t poc_of(uint64_t addr, int *found)
{
    for (int i = 0; i < nframepoc; i++)
        if (framepoc[i].addr == addr) { *found = 1; return framepoc[i].poc; }
    *found = 0;
    return 0;
}

static void phase2_start(void)
{
    int k;
    CHECK(!p2_busy, "phase 2 started while phase 2 is running");
    memcpy(s2, regs, sizeof regs);
    for (k = 0; k < NRECS; k++)
        if (recs[k].used && recs[k].pu == (uint64_t)s2[0x8000 / 4] << 6) break;
    CHECK(k < NRECS && recs[k].coeff == (uint64_t)s2[0x8008 / 4] << 6,
          "phase 2 reads PU/coefficient buffers (&%llx) no finished phase 1 wrote",
          (unsigned long long)((uint64_t)s2[0x8000 / 4] << 6));
    p2_rec = k < NRECS ? k : -1;
    if (p2_rec >= 0)                                     /* each reference holds the right picture, now */
        for (int i = 0; i < recs[k].nrefs; i++) {
            int found;
            uint64_t ra = (uint64_t)s2[(0x9000 + 16 * recs[k].refs[i].dpb) / 4] << 6;
            uint32_t rp = poc_of(ra, &found);
            if (!found || rp != recs[k].refs[i].poc) {
                fake_hevc.ref_errors++;
                CHECK(0, "reference DPB %u: frame &%llx holds poc %u%s, not %u", recs[k].refs[i].dpb,
                      (unsigned long long)ra, (unsigned)rp, found ? "" : " (nothing)", (unsigned)recs[k].refs[i].poc);
            }
        }
    p2_busy = 1;
    p2_left = fake_hevc.p2_ticks > 0 ? fake_hevc.p2_ticks : 1;
}

static void phase2_done(void)
{
    uint64_t y = (uint64_t)s2[0x8018 / 4] << 6, c = (uint64_t)s2[0x8020 / 4] << 6;
    uint32_t col = s2[0x801C / 4] << 6, size = s2[0x802C / 4], poc = s2[0x8040 / 4] & 0xFFFF;
    int w = (int)(size & 0xFFFF), h = (int)(size >> 16), pw = 0, ph = 0, bytes = 0;
    int ten = (s2[0x8014 / 4] >> 8) & 1;               /* CONFIG2: 10-bit luma */
    int per_col = ten ? 96 : 128;                      /* samples a column's row */
    size_t fsize = (size_t)col * (size_t)((w + per_col - 1) / per_col);
    const void *py = NULL, *pu = NULL, *pv = NULL;
    uint8_t *out;
    fake_hevc.phase2s++;
    have_hash = 0; factors_wrong = 0;
    if (p2_rec >= 0) {                                   /* (its phase 1's findings) */
        pic_hash = recs[p2_rec].hash;
        have_hash = recs[p2_rec].have_hash;
        factors_wrong = recs[p2_rec].factors_wrong;
        recs[p2_rec].used = 0;
    }
    CHECK(s2[0x8024 / 4] == s2[0x801C / 4] && c > y && c - y < col, "output planes: Y &%llx, C &%llx, column %u",
          (unsigned long long)y, (unsigned long long)c, (unsigned)col);
    CHECK(((s2[0x8014 / 4] >> 9) & 1) == ten && (s2[0x8014 / 4] & 0xFF) == (ten ? 0xAA : 0x88),
          "CONFIG2 &%08X: luma and chroma depths differ", (unsigned)s2[0x8014 / 4]);
    if (!(out = mem(y, fsize, "the output frame"))) return;
    if (have_hash && picture_fn && picture_fn(pic_hash, &py, &pu, &pv, &pw, &ph, &bytes) == 0 && bytes == (ten ? 2 : 1)) {
        int cw = (pw + 1) / 2, ch = (ph + 1) / 2;
        CHECK(pw <= w && ph <= h, "a %dx%d picture in a %dx%d frame", pw, ph, w, h);
        memset(out, 0x80, fsize);                        /* (outside the window: something) */
        if (factors_wrong) fake_hevc.factors_wrong++;
        if (ten) {                                       /* three samples a word, 96 a column's row */
            const uint16_t *sy = py, *su = pu, *sv = pv;
            for (int x0 = 0; x0 < pw; x0 += 96) {
                uint32_t *cy = (uint32_t *)(void *)(out + (size_t)(x0 / 96) * col);
                uint32_t *cc = (uint32_t *)(uintptr_t)(c + (size_t)(x0 / 96) * col);
                for (int r = 0; r < ph; r++)
                    for (int k = 0; k < 32; k++) {
                        uint32_t wd = 0;
                        for (int j = 0; j < 3; j++) {
                            int x = x0 + 3 * k + j;
                            wd |= (uint32_t)(x < pw ? sy[(size_t)r * pw + x] & 0x3FF : 0) << (10 * j);
                        }
                        cy[(size_t)r * 32 + k] = wd;
                    }
                if (factors_wrong) cy[0] ^= 1;
                for (int r = 0; r < ch; r++)
                    for (int k = 0; k < 32; k++) {
                        uint32_t wd = 0;
                        for (int j = 0; j < 3; j++) {
                            int i = 3 * k + j, x = x0 / 2 + i / 2;       /* (U V U V ...) */
                            const uint16_t *pl = i & 1 ? sv : su;
                            wd |= (uint32_t)(x < cw ? pl[(size_t)r * cw + x] & 0x3FF : 0) << (10 * j);
                        }
                        cc[(size_t)r * 32 + k] = wd;
                    }
            }
        } else
        for (int x0 = 0; x0 < pw; x0 += 128) {
            uint8_t *cy = out + (size_t)(x0 / 128) * col, *cc = (uint8_t *)(uintptr_t)c + (size_t)(x0 / 128) * col;
            int n = pw - x0 < 128 ? pw - x0 : 128, nc = cw - x0 / 2 < 64 ? cw - x0 / 2 : 64;
            for (int r = 0; r < ph; r++) memcpy(cy + (size_t)r * 128, (const uint8_t *)py + (size_t)r * pw + x0, (size_t)n);
            if (factors_wrong) cy[0] ^= 1;                /* (dequantised wrongly: some samples off) */
            for (int r = 0; r < ch; r++)
                for (int x = 0; x < nc; x++) {
                    cc[(size_t)r * 128 + 2 * x] = ((const uint8_t *)pu)[(size_t)r * cw + x0 / 2 + x];
                    cc[(size_t)r * 128 + 2 * x + 1] = ((const uint8_t *)pv)[(size_t)r * cw + x0 / 2 + x];
                }
        }
    } else {
        fake_hevc.unknown_pictures++;
        CHECK(!bytes || bytes == (ten ? 2 : 1), "a %d-bit frame for a picture of %d bytes a sample", ten ? 10 : 8, bytes);
        memset(out, 0x10, fsize);
    }
    {   /* the motion vectors: this picture's written at MVBASE for later ones, a collocated picture's read
           at COLBASE; MVSTRIDE (COLSTRIDE) x 16-row bands of the height to 64 (rpivid's colmv_picsize) */
        uint64_t mv = (uint64_t)s2[0x8030 / 4] << 6, colb = (uint64_t)s2[0x8038 / 4] << 6;
        size_t bands = (size_t)(((h + 63) & ~63) >> 4);
        size_t mvn = ((size_t)s2[0x8034 / 4] << 6) * bands, coln = ((size_t)s2[0x803C / 4] << 6) * bands;
        uint8_t *m;
        if (colb) mem(colb, coln, "the collocated picture's motion vectors");
        if (mv && (m = mem(mv, mvn, "the motion vectors written"))) memset(m, 0x3C, mvn);
    }
    if (fake_hevc.overrun) {                             /* (a block writing past the frame's end) */
        uint8_t *past = mem(y, fsize + (size_t)fake_hevc.overrun, "past the frame");
        if (past) memset(past + fsize, 0x55, (size_t)fake_hevc.overrun);
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
    evict_dirty();
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
        if (o == 0x6C) phase1_start();                   /* CFBASE: phase 1 starts */
        if (o == 0x8010) phase2_start();                 /* NUMROWS: phase 2 starts */
    }
    npend = 0;
}

uint32_t hevcdec_hw_read(void *hw, unsigned int offset)
{
    hevcdec_hw_flush(hw);
    return regs[offset / 4];
}

uint32_t hevcdec_hw_ictrl(void *hw)              /* (time passes: the phases running get on) */
{
    (void)hw;
    if (p1_busy && p2_busy) fake_hevc.overlaps++;
    if (p2_busy && !fake_hevc.p2_hang && --p2_left <= 0) { p2_busy = 0; phase2_done(); }
    if (p1_busy && !fake_hevc.p1_hang && --p1_left <= 0) { p1_busy = 0; phase1_done(); }
    return ictrl;
}

void hevcdec_hw_ictrl_write(void *hw, uint32_t v)
{
    (void)hw;
    CHECK((v & ((0xFFu << 12) | (1u << 11))) == 0, "ICTRL written with reserved bits &%08X", (unsigned)v);
    ictrl &= ~(v & ((1u << 0) | (1u << 4) | (1u << 25) | (1u << 29)));   /* latched bits: 1 clears */
    ictrl = (ictrl & ~((1u << 2) | (1u << 6))) | (v & ((1u << 2) | (1u << 6)));   /* enables */
}

void *hevcdec_hw_alloc(void *hw, size_t size, uint64_t *bus, int cached)
{
    (void)hw;
    if (fake_hevc.no_memory || (fake_hevc.memory_left && size > fake_hevc.memory_left)) return NULL;
    CHECK(!cached || !fake_hevc.no_cache, "cacheable memory asked for without cache maintenance");
    for (int i = 0; i < NALLOC; i++)
        if (!allocs[i].p) {
            size_t n = (size + 4095) & ~(size_t)4095;
            if (!(allocs[i].p = aligned_alloc(4096, n))) return NULL;
            memset(allocs[i].p, 0, n);                   /* (zeroed, and for cacheable memory cleaned: as hevcdec_hw.c) */
            allocs[i].n = n;
            allocs[i].cached = cached;
            allocs[i].ram = allocs[i].p;
            if (cached) {
                allocs[i].ram = aligned_alloc(4096, n);
                allocs[i].synced = malloc(n);
                memset(allocs[i].ram, 0, n);
                memset(allocs[i].synced, 0, n);
                fake_hevc.cached_allocs++;
            }
            if (fake_hevc.memory_left) fake_hevc.memory_left -= size;
            *bus = (uint64_t)(uintptr_t)allocs[i].ram;
            fake_hevc.allocs++;
            return allocs[i].p;
        }
    return NULL;
}

int hevcdec_hw_can_cache(void *hw) { (void)hw; return !fake_hevc.no_cache; }

void hevcdec_hw_cache_clean_inv(void *hw, const void *p, size_t n)
{
    (void)hw;
    fake_hevc.cache_ops++;
    for (int i = 0; i < NALLOC; i++) {
        uint8_t *b = allocs[i].p, *r = allocs[i].ram, *sy = allocs[i].synced;
        size_t a, e;
        if (!b || (const uint8_t *)p + n <= b || (const uint8_t *)p >= b + allocs[i].n) continue;
        CHECK(allocs[i].cached, "cache maintenance on memory that isn't cacheable");
        if (!allocs[i].cached) return;
        a = (const uint8_t *)p < b ? 0 : (size_t)((const uint8_t *)p - b);
        e = (const uint8_t *)p + n > b + allocs[i].n ? allocs[i].n : (size_t)((const uint8_t *)p + n - b);
        a &= ~(size_t)63; e = (e + 63) & ~(size_t)63;
        if (e > allocs[i].n) e = allocs[i].n;
        for (size_t k = a; k < e; k++) if (b[k] != sy[k]) r[k] = b[k];   /* (cleaned) */
        memcpy(b + a, r + a, e - a);                                     /* (invalidated) */
        memcpy(sy + a, r + a, e - a);
        return;
    }
    CHECK(0, "cache maintenance on memory that isn't the block's");
}

static void evict_dirty(void)                            /* (lines the program wrote and didn't clean) */
{
    for (int i = 0; i < NALLOC; i++)
        if (allocs[i].p && allocs[i].cached) {
            uint8_t *b = allocs[i].p, *r = allocs[i].ram, *sy = allocs[i].synced;
            for (size_t k = 0; k < allocs[i].n; k++)
                if (b[k] != sy[k]) { r[k] = b[k]; sy[k] = b[k]; fake_hevc.evictions++; }
        }
}

const char *hevcdec_hw_why(void *hw) { (void)hw; return "the fake's memory is used up"; }
unsigned hevcdec_hw_app_page_moves(void *hw) { (void)hw; return 0; }

void hevcdec_hw_free(void *hw, void *p)
{
    (void)hw;
    for (int i = 0; i < NALLOC; i++)
        if (allocs[i].p == p) {
            /* (a frame's POC forgotten with it) */
            for (int k = 0; k < nframepoc; k++)
                if (framepoc[k].addr >= (uint64_t)(uintptr_t)allocs[i].ram &&
                    framepoc[k].addr < (uint64_t)(uintptr_t)allocs[i].ram + allocs[i].n)
                    framepoc[k] = framepoc[--nframepoc], k--;
            if (allocs[i].cached) { free(allocs[i].ram); free(allocs[i].synced); }
            free(p);
            allocs[i].p = NULL;
            return;
        }
    CHECK(0, "freeing memory that isn't the block's");
}

uint32_t hevcdec_hw_now_cs(void) { return clock_now += 1; }
