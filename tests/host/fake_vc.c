/*
 * The fake VCHIQ module and MMAL decoder (mmaldecode_test.c, which behaves
 * as the Pi did) as a library, with a small interface for tests that live
 * outside this file: FFmpeg's h264_vchiq decoder (tests/host/ffmpeg),
 * linked with vcdec (VCDEC_HOST) and this, under qemu.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#define FAKE_ONLY
#include "mmaldecode_test.c"
#include "fake_vc.h"

void vcdec_host_enter_os(void) { pci_open(1); }
void vcdec_host_leave_os(void) { pci_open(0); }

void fake_vc_reset(void)
{
    reset_fake();
    mp4_mode = 1;
}

void fake_vc_make_mp4(void) { make_mp4(); }

int fake_vc_fails(void) { return fails; }

/* every component destroyed, everything closed and freed: 1 if so */
int fake_vc_cleaned(const char *what)
{
    int before = fails;
    cleaned(what);
    return fails == before;
}

int *fake_vc_var(const char *name)
{
    static const struct { const char *n; int *p; } v[] = {
        { "go_quiet", &go_quiet }, { "error_event", &error_event }, { "big_efch", &big_efch },
        { "rx_late", &rx_late }, { "hold_inputs", &hold_inputs }, { "corrupt_at", &corrupt_at },
        { "big_efch_at", &big_efch_at }, { "efch_late", &efch_late }, { "no_pci_mem", &no_pci_mem },
        { "aus", &aus }, { "flushes", &flushes }, { "recreated", &recreated }, { "eos_lost", &eos_lost },
        { "created", &created }, { "destroyed", &destroyed }, { "opens", &opens }, { "idr_dropped", &idr_dropped },
        { "bulks_tx", &bulks_tx }, { "disc_seen", &disc_seen }, { "in_slow", &in_slow }, { "slow_max", &slow_max },
        { "no_pmp", &no_pmp }, { "pmp_invalidates", &pmp_invalidates }, { "pmp_scattered", &pmp_scattered },
        { "pmp_made", &pmp_made }, { "pmp_live", &pmp_live },
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
        if (!strcmp(v[i].n, name)) return v[i].p;
    fprintf(stderr, "fake_vc_var: no %s\n", name);
    abort();
}

/* the fake MP4's pictures: picture k (display order) is Y = 10 + 7k,
   U = Y + 1, V = Y + 2, 70x38; its pts in frames */
int fake_vc_width(void) { return W; }
int fake_vc_height(void) { return H; }
int fake_vc_value(int k) { return mp4_val(k); }
int fake_vc_in_pool(const void *p)
{
    uint32_t a = (uint32_t)(uintptr_t)p;
    for (int i = 0; i < npci_blocks; i++)
        if (pmp_block[i] && a >= pci_lo[i] && a < pci_hi[i]) return 1;
    return 0;
}

/* a whole program (FFmpeg's own ffmpeg) against the fake: set up before
   main; with FAKE_VC_REPORT set, at the end: the fake's failures and
   whether everything was closed and freed */
__attribute__((constructor)) static void fake_vc_start(void) { fake_vc_reset(); }
__attribute__((destructor)) static void fake_vc_end(void)
{
    if (!getenv("FAKE_VC_REPORT")) return;
    if (!fake_vc_cleaned("at exit")) return;
    fprintf(stderr, "fake_vc: %d failures, %d access units, %d flushes, %d created again, %d lost\n", fails, aus,
            flushes, recreated, eos_lost);
}
