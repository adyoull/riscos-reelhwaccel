/*
 * hevc_fake_env.c - the fake HEVC block (fake_hevc.c) in an ffmpeg built
 * for the host tests: which pictures it "decodes" comes from the
 * environment, and it says how it went when the program ends.
 *
 *   HEVC_FAKE_TRACE  a trace (tools/hevctrace) of the clip
 *   HEVC_FAKE_YUV    FFmpeg's decode of it (8-bit I420, display order)
 *   HEVC_FAKE_SIZE   WxH of that
 *   HEVC_FAKE_P1 / HEVC_FAKE_P2   the phases' lengths (reads of the
 *                    interrupt control register), as fake_hevc's ticks
 *   HEVC_FAKE_NOBLOCK  set: no HEVC block (not a Pi 4)
 *   HEVC_FAKE_FAIL   n: the nth phase 1 fails (so its picture does)
 *   HEVC_FAKE_QUIET  set: the fake's complaints counted, not printed
 * At exit, to stderr: "fake_hevc: <complaints> complaints, <n> phase 1s,
 * <n> phase 2s, <n> references wrong, <n> pictures unknown, <n> stale
 * factors, <n> evictions, <n> buffers not freed, <n> overlaps".
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_hevc.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t adler(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return b << 16 | a;
}

#define MAXP 1024
static struct { uint32_t hash; const uint8_t *y, *u, *v; } map[MAXP];
static int nmap, map_w, map_h, map_bytes;

static int picture(uint32_t hash, const void **y, const void **u, const void **v, int *w, int *h, int *bytes)
{
    for (int i = 0; i < nmap; i++)
        if (map[i].hash == hash) {
            *y = map[i].y; *u = map[i].u; *v = map[i].v; *w = map_w; *h = map_h; *bytes = map_bytes;
            return 0;
        }
    return -1;
}

static uint8_t *slurp(const char *name, long *n)
{
    FILE *f = fopen(name, "rb");
    uint8_t *d;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    d = malloc((size_t)*n + 1);
    if (!d || fread(d, 1, (size_t)*n, f) != (size_t)*n) { fclose(f); free(d); return NULL; }
    fclose(f);
    return d;
}

static void report(void)
{
    fprintf(stderr, "fake_hevc: %d complaints, %d phase 1s, %d phase 2s, %d references wrong, %d pictures unknown, "
            "%d stale factors, %d evictions, %d buffers not freed, %d overlaps\n", fake_hevc.fails, fake_hevc.phase1s,
            fake_hevc.phase2s, fake_hevc.ref_errors, fake_hevc.unknown_pictures, fake_hevc.factors_wrong,
            fake_hevc.evictions, fake_hevc_live(), fake_hevc.overlaps);
}

__attribute__((constructor)) static void load(void)
{
    const char *tn = getenv("HEVC_FAKE_TRACE"), *yn = getenv("HEVC_FAKE_YUV"), *sz = getenv("HEVC_FAKE_SIZE");
    uint8_t *d, *raw;
    long n, rn;
    size_t o = 28, fs, yb, cb, s_sps, s_pps, s_sp, s_dec, s_sm;
    int w = 0, h = 0;
    fake_hevc_reset();
    if (getenv("HEVC_FAKE_P1")) fake_hevc.p1_ticks = atoi(getenv("HEVC_FAKE_P1"));
    if (getenv("HEVC_FAKE_P2")) fake_hevc.p2_ticks = atoi(getenv("HEVC_FAKE_P2"));
    if (getenv("HEVC_FAKE_NOBLOCK")) fake_hevc.no_block = 1;   /* (not a Pi 4) */
    if (getenv("HEVC_FAKE_FAIL")) fake_hevc.p1_fail = atoi(getenv("HEVC_FAKE_FAIL"));
    if (getenv("HEVC_FAKE_QUIET")) fake_hevc.quiet = 1;
    atexit(report);
    if (!tn || !yn || !sz || sscanf(sz, "%dx%d", &w, &h) != 2) return;
    if (!(d = slurp(tn, &n)) || !(raw = slurp(yn, &rn))) { fprintf(stderr, "fake_hevc: can't read %s or %s\n", tn, yn); return; }
    map_w = w; map_h = h;
    s_sps = rd32(d + 8); s_pps = rd32(d + 12); s_sp = rd32(d + 16); s_dec = rd32(d + 20); s_sm = rd32(d + 24);
    map_bytes = o + 48 <= (size_t)n && rd32(d + o + 20) > 8 ? 2 : 1;   /* (the first picture's depth) */
    yb = (size_t)w * h * map_bytes; cb = (size_t)((w + 1) / 2) * ((h + 1) / 2) * map_bytes;
    fs = yb + 2 * cb;
    while (o + 48 <= (size_t)n) {
        uint32_t nsl = rd32(d + o + 24), has_sm = rd32(d + o + 28), hash = 0, crc[3];
        o += 48 + s_sps + s_pps + s_dec + (has_sm ? s_sm : 0);
        for (uint32_t i = 0; i < nsl; i++) {
            uint32_t bits = rd32(d + o), off = rd32(d + o + 4), len;
            o += s_sp;
            len = rd32(d + o); o += 4;
            if (i == 0) hash = fake_hevc_hash(d + o + off, (bits + 7) / 8 - off);
            o += (len + 3) & ~3u;
        }
        for (int k = 0; k < 3; k++) { crc[k] = rd32(d + o); o += 4; }
        for (size_t k = 0; k * fs + fs <= (size_t)rn && nmap < MAXP; k++) {
            const uint8_t *y = raw + k * fs, *u = y + yb, *v = u + cb;
            if (adler(y, yb) == crc[0] && adler(u, cb) == crc[1] && adler(v, cb) == crc[2]) {
                map[nmap].hash = hash; map[nmap].y = y; map[nmap].u = u; map[nmap].v = v; nmap++;
                break;
            }
        }
    }
    fake_hevc_set_pictures(picture);
}
