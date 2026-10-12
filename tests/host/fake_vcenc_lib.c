/*
 * The fake VCHIQ module and MMAL encoder (fake_vcenc.c) as a library, with
 * a small interface for tests outside this file: FFmpeg's h264_vchiq
 * encoder (tests/host/ffmpeg), linked with vcenc (VCDEC_HOST) and this,
 * under qemu.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include "fake_vcenc.c"
#include "fake_vcenc.h"

void fake_vcenc_reset(void) { enc_reset(); }
int fake_vcenc_fails(void) { return fails; }

int fake_vcenc_cleaned(const char *what)
{
    int before = fails;
    closed_ok(what);
    return fails == before;
}

int *fake_vcenc_var(const char *name)
{
    static const struct { const char *n; int *p; } v[] = {
        { "slow", &e_slow }, { "hold_in", &e_hold_in }, { "quiet", &e_quiet }, { "error_at", &e_error_at },
        { "rx_late", &e_rx_late }, { "frames", &e_frames }, { "created", &e_created }, { "destroyed", &e_destroyed },
        { "pad_bad", &e_pad_bad }, { "big_at", &e_big_at }, { "refuse_rc4", &e_refuse_rc4 },
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
        if (!strcmp(v[i].n, name)) return v[i].p;
    fprintf(stderr, "fake_vcenc_var: no %s\n", name);
    abort();
}

long long fake_vcenc_param(uint32_t id, int i)
{
    const param_t *p = param(id);
    return p && i < (int)p->n ? (long long)p->v[i] : -1;
}

uint32_t fake_vcenc_adler(int k) { return k >= 0 && k < 64 ? e_frame_adler[k] : 0; }

/* a whole program (FFmpeg's own ffmpeg) against the fake: set up before
   main; with FAKE_VCENC_REPORT set, at the end: the fake's failures and
   whether everything was closed and freed */
__attribute__((constructor)) static void fake_vcenc_start(void) { enc_reset(); }
__attribute__((destructor)) static void fake_vcenc_end(void)
{
    if (!getenv("FAKE_VCENC_REPORT")) return;
    if (!fake_vcenc_cleaned("at exit")) return;
    fprintf(stderr, "fake_vcenc: %d failures, %d pictures, padding wrong %d\n", fails, e_frames, e_pad_bad);
}
