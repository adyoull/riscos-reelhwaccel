/*
 * hevcdec_conv_test.c - hevcdec_conv and hevcdec_conv_half against a plain
 * reference, on frames shaped as hevcdec makes them (8-bit and 10-bit
 * columns), each ending against a page that can't be read, with guard
 * bytes around and between the output rows: random windows (1:1, 8-bit
 * and 16-bit, every way; halved), a 10-bit frame of 1023s halved (255,
 * not 0), and halving to the frame's last chroma row with an odd height
 * (no read below it). Run as NEON under qemu, and as plain C.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "hevcdec_conv.h"

static int W, H, TEN, NCOL, PER;
static size_t COL, COFF, SZ;
static uint8_t *B;
static int ticks;
static void tick(void *a) { (void)a; ticks++; }

static unsigned rnd(void) { static unsigned s = 12345; s = s * 1103515245u + 12345u; return s >> 8; }

static uint8_t *alloc_frame(void) /* frame ending exactly at a PROT_NONE page */
{
    long pg = sysconf(_SC_PAGESIZE);
    size_t n = (SZ + pg - 1) / pg * pg;
    uint8_t *m = mmap(NULL, n + 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(m, pg, PROT_NONE);
    mprotect(m + pg + n, pg, PROT_NONE);
    return m + pg + n - SZ;
}

static void geom(int w, int h, int ten)
{
    TEN = ten; H = (h + 15) & ~15; PER = ten ? 96 : 128;
    W = ten ? ((w + 2) / 3 + 31) / 32 * 32 * 3 : (w + 127) / 128 * 128;
    NCOL = W / PER;
    COL = (size_t)(H * 3 / 2) * 128; COFF = (size_t)H * 128;
    SZ = COL * NCOL;
    B = alloc_frame();
    for (size_t i = 0; i < SZ; i += 4) {
        uint32_t v = rnd() & 0x3FFFFFFF;
        memcpy(B + i, &v, 4);
    }
}
static unsigned samp10(const uint8_t *row, int s) { uint32_t w; memcpy(&w, row + 4 * (s / 3), 4); return (w >> (10 * (s % 3))) & 0x3FF; }
static unsigned Y(int x, int y)
{
    const uint8_t *r = B + (size_t)(x / PER) * COL + (size_t)y * 128;
    return TEN ? samp10(r, x % PER) : r[x % PER];
}
static unsigned C(int p, int y, int c)
{
    int pp = PER / 2;
    const uint8_t *r = B + (size_t)(p / pp) * COL + COFF + (size_t)y * 128;
    return TEN ? samp10(r, 2 * (p % pp) + c) : r[2 * (p % pp) + c];
}

#define G 64
static int fails;
static int check_plane(const char *nm, uint8_t *buf, int stride, int ww, int hh, int bps, unsigned (*ref)(int, int, int, void *), int c, void *ctx, const char *desc)
{
    int bad = 0;
    for (int i = 0; i < G; i++) if (buf[i] != 0xA5) { bad++; }
    for (int y = 0; y < hh; y++) {
        uint8_t *r = buf + G + (size_t)y * stride;
        for (int x = ww * bps; x < stride; x++) if (r[x] != 0xA5) { if (bad < 3) printf("  %s overwrite row %d byte %d (w=%d)\n", nm, y, x, ww); bad++; }
        for (int x = 0; x < ww; x++) {
            unsigned e = ref(x, y, c, ctx), g = bps == 2 ? (unsigned)(r[2 * x] | r[2 * x + 1] << 8) : r[x];
            if (e != g) { if (bad < 3) printf("  %s mismatch (%d,%d): got %u want %u\n", nm, x, y, g, e); bad++; }
        }
    }
    for (int i = 0; i < G; i++) if (buf[G + (size_t)hh * stride + i] != 0xA5) bad++;
    if (bad) { printf("FAIL %s %s: %d bad\n", desc, nm, bad); fails++; }
    return bad;
}

typedef struct { int x0, y0, bits, half; } ctx_t;
static unsigned refY(int x, int y, int c, void *v)
{
    ctx_t *k = v; (void)c;
    if (k->half) {
        int sx = k->x0 + 2 * x, sy = k->y0 + 2 * y;
        unsigned s = Y(sx, sy) + Y(sx + 1, sy) + Y(sx, sy + 1) + Y(sx + 1, sy + 1);
        unsigned r = TEN ? (s + 8) >> 4 : (s + 2) >> 2;
        return r > 255 ? 255 : r;
    }
    unsigned s = Y(k->x0 + x, k->y0 + y);
    return TEN && k->bits == 8 ? s >> 2 : s;
}
static unsigned refC(int x, int y, int c, void *v)
{
    ctx_t *k = v;
    if (k->half) {
        int sx = k->x0 / 2 + 2 * x, sy = k->y0 / 2 + 2 * y;
        int sy1 = sy + 1 < H / 2 ? sy + 1 : sy;     /* (clamped: the frame's last chroma row) */
        unsigned s = C(sx, sy, c) + C(sx + 1, sy, c) + C(sx, sy1, c) + C(sx + 1, sy1, c);
        unsigned r = TEN ? (s + 8) >> 4 : (s + 2) >> 2;
        return r > 255 ? 255 : r;
    }
    unsigned s = C(k->x0 / 2 + x, k->y0 / 2 + y, c);
    return TEN && k->bits == 8 ? s >> 2 : s;
}

static void run(int x0, int y0, int w, int h, int bits, int way, int half, int odd_align)
{
    int bps = bits / 8, cw = (w + 1) / 2, chh = (h + 1) / 2;
    int st[3] = { w * bps + 7 + odd_align, cw * bps + 3 + odd_align, cw * bps + 5 + odd_align };
    if (bps == 2) { st[0] &= ~1; st[1] &= ~1; st[2] &= ~1; }
    uint8_t *buf[3]; int hh[3] = { h, chh, chh };
    void *pl[3];
    for (int i = 0; i < 3; i++) {
        size_t n = 2 * G + (size_t)hh[i] * st[i];
        buf[i] = malloc(n + 2); memset(buf[i], 0xA5, n + 2);
        pl[i] = buf[i] + G;
    }
    hevcdec_conv_opts o = { way, tick, NULL };
    ctx_t k = { x0, y0, bits, half };
    char desc[160];
    snprintf(desc, sizeof desc, "%s ten=%d bits=%d way=%d W=%d H=%d x0=%d y0=%d w=%d h=%d", half ? "half" : "conv", TEN, bits, way, W, H, x0, y0, w, h);
    ticks = 0;
    if (half) hevcdec_conv_half(B, COL, COFF, TEN, (uint8_t *const *)pl, st, x0, y0, w, h, &o);
    else hevcdec_conv(B, COL, COFF, TEN, pl, st, bits, x0, y0, w, h, &o);
    check_plane("Y", buf[0], st[0], w, h, half ? 1 : bps, refY, 0, &k, desc);
    check_plane("U", buf[1], st[1], cw, chh, half ? 1 : bps, refC, 0, &k, desc);
    check_plane("V", buf[2], st[2], cw, chh, half ? 1 : bps, refC, 1, &k, desc);
    for (int i = 0; i < 3; i++) free(buf[i]);
}

int main(void)
{
    /* a 10-bit frame of 1023s halved: every sample 255 (it was 0) */
    geom(192, 32, 1);
    for (size_t i = 0; i < SZ; i += 4) { uint32_t v = 0x3FFFFFFF; memcpy(B + i, &v, 4); }
    run(0, 0, 40, 8, 8, 0, 1, 0);
    run(4, 2, 44, 15, 8, 0, 1, 1);
    /* halved to the frame's last chroma row, odd height: its own row twice */
    geom(128, 32, 0);
    run(0, 2, 64, 15, 8, 0, 1, 0);
    geom(300, 48, 1);
    run(96, 6, 50, 21, 8, 0, 1, 1);
    for (int it = 0; it < 3000; it++) {
        int ten = rnd() & 1;
        geom(1 + rnd() % 400, 2 + rnd() % 70, ten);
        int half = (rnd() % 3) == 0;
        int bits = ten && (rnd() & 1) ? 16 : 8;
        int way = rnd() % (ten ? 4 : 3);
        int x0, y0, w, h;
        if (half) {
            bits = 8;
            x0 = (rnd() % (W / 4)) * 4; y0 = (rnd() % (H / 2)) * 2;
            int mw = (W - x0) / 2, mh = (H - y0) / 2;
            if (mw < 1 || mh < 1) continue;
            w = 1 + rnd() % mw; h = 1 + rnd() % mh;
        } else {
            x0 = (rnd() % (W / 2)) * 2; y0 = (rnd() % (H / 2)) * 2;
            w = 1 + rnd() % (W - x0); h = 1 + rnd() % (H - y0);
        }
        run(x0, y0, w, h, bits, way, half, rnd() & 1);
        /* (frames are left mapped) */
    }
    printf("hevcdec_conv_test: %s (%d failures)\n", fails ? "FAILED" : "all right", fails);
    return fails != 0;
}
