/*
 * smemprobe (tools/smemprobe) against the fake VideoCore
 * (mmaldecode_test.c, included without its main) with its SMEM service
 * and zero copy, modelled on Linux's vc-sm-cma and mmal-vchiq (the Pi's
 * behaviour is what the probe is for):
 *   - all four steps: the service opened, a pool imported and freed, a
 *     clip decoded with every picture written straight into imported
 *     memory and checked, the speed test (zero copy, then vcdec);
 *   - a picture within rounding; zero copy refused (pictures by bulk
 *     transfer instead, said); no SMEM service; imports refused;
 *   - every run: imports freed, the service closed, pools, PCI memory
 *     and both stubs freed, components destroyed.
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#define FAKE_ONLY
#include <unistd.h>
#include "mmaldecode_test.c"

int probe_main(int argc, char **argv);   /* tools/smemprobe */
void vcdec_host_enter_os(void) { pci_open(1); }   /* (vcdec_copy.S under VCDEC_HOST: the fake's PCI memory opened) */
void vcdec_host_leave_os(void) { pci_open(0); }

#define NPIC 12
static uint8_t pic_val(int k) { return (uint8_t)(10 + 7 * k); }

/* an Annex B clip as the fake decodes it: a delimiter before each picture,
   SPS and PPS before each IDR (every 6th), the slice's first byte its value;
   the first over 64 KB (sent in pieces) */
static void make_clip(void)
{
    static const uint8_t aud[] = { 0, 0, 0, 1, 0x09, 0xF0 }, sps[] = { 0, 0, 0, 1, 0x67, 0x4D, 0x00, 0x1E, 0xAB },
                         pps[] = { 0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80 };
    FILE *f = fopen("/tmp/smemprobe_test.h264", "wb"), *c = fopen("/tmp/smemprobe_test.crc", "w"),
         *sg = fopen("/tmp/smemprobe_test.sig", "w");
    static uint8_t fill[70000];
    memset(fill, 0xAA, sizeof fill);
    fprintf(c, "#tb 0: 1/25\n#dimensions 0: %dx%d\n", W, H);
    for (int k = 0; k < NPIC; k++) {
        int key = k % 6 == 0;
        uint8_t v = pic_val(k), sl[6] = { 0, 0, 0, 1, (uint8_t)(key ? 0x65 : 0x41), v };
        uint8_t y[W], u[(W + 1) / 2], vv[(W + 1) / 2];
        uint32_t a = 0;
        fwrite(aud, 1, sizeof aud, f);
        if (key) { fwrite(sps, 1, sizeof sps, f); fwrite(pps, 1, sizeof pps, f); }
        fwrite(sl, 1, sizeof sl, f);
        fwrite(fill, 1, k == 0 ? 70000 : 40 + 13 * (size_t)k, f);
        memset(y, v, W); memset(u, v + 1, sizeof u); memset(vv, v + 2, sizeof vv);
        for (int r = 0; r < H; r++) a = adler0(a, y, W);
        for (int r = 0; r < (H + 1) / 2; r++) a = adler0(a, u, sizeof u);
        for (int r = 0; r < (H + 1) / 2; r++) a = adler0(a, vv, sizeof vv);
        fprintf(c, "0, %10d, %10d, 1, %8d, 0x%08x\n", k, k, W * H * 3 / 2, a);
        for (int q = 0; q < 96; q++) fprintf(sg, "%d.00%c", q < 64 ? v : q < 80 ? v + 1 : v + 2, q == 95 ? '\n' : ' ');
    }
    fclose(f); fclose(c); fclose(sg);
}

static char *run(int *ret, int with_speed)
{
    static char buf[16384];
    char *argv[] = { "smemprobe", "-o", "/tmp/smemprobe_test.out", "/tmp/smemprobe_test.h264", "/tmp/smemprobe_test.crc",
                     "/tmp/smemprobe_test.sig", "-t", "/tmp/smemprobe_test.h264", "/tmp/smemprobe_test.crc", NULL };
    FILE *f;
    size_t got;
    remove("/tmp/smemprobe_test.out");
    *ret = probe_main(with_speed ? 9 : 6, argv);
    f = fopen("/tmp/smemprobe_test.out", "r");
    got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got] = 0;
    if (f) fclose(f);
    return buf;
}

static void setup(void)
{
    reset_fake();
    mp4_mode = 1;
    multi = 1;                               /* (the probe's connection and vcdec's, each its own) */
}

static void all_freed(const char *what)
{
    CHECK(!smem_live() && !smem_is_open && smem_imports == smem_frees, "%s: SMEM: %d imports live, open %d, %d/%d",
          what, smem_live(), smem_is_open, smem_imports, smem_frees);
    multi_cleaned(what);
}

int main(void)
{
    int ret;
    char *o;
    setvbuf(stdout, NULL, _IONBF, 0);
    make_clip();

    setup();
    o = run(&ret, 1);
    printf("%s", o);
    CHECK(ret == 0 && strstr(o, "MMAL_PARAMETER_ZERO_COPY on the output port: accepted") &&
          strstr(o, "a VideoCore handle, &") &&
          strstr(o, "12 pictures: 12 came back zero copy, 0 by bulk transfer; 12 exact, 0 within rounding, 0 wrong") &&
          strstr(o, "  Zero copy: 12 pictures in") && strstr(o, "  vcdec: 12 pictures in") &&
          strstr(o, "Result: OK - zero-copy decoding works from RISC OS"), "everything (%d):\n%s", ret, o);
    all_freed("everything");

    setup();
    corrupt_at = 3;                          /* (one byte one level out: within rounding) */
    o = run(&ret, 0);
    CHECK(ret == 0 && strstr(o, "12 pictures: 12 came back zero copy, 0 by bulk transfer; 11 exact, 1 within rounding, 0 wrong"),
          "within rounding (%d):\n%s", ret, o);
    corrupt_at = -1;
    all_freed("within rounding");

    setup();
    zc_refuse = 1;
    o = run(&ret, 0);
    printf("%s", o);
    CHECK(ret == 1 && strstr(o, "MMAL_PARAMETER_ZERO_COPY on the output port: refused") &&
          strstr(o, "12 pictures: 0 came back zero copy, 12 by bulk transfer; 12 exact") &&
          strstr(o, "Result: SMEM import works, but zero-copy decoding didn't"), "zero copy refused (%d):\n%s", ret, o);
    all_freed("zero copy refused");

    setup();
    smem_present = 0;
    o = run(&ret, 0);
    CHECK(ret == 1 && strstr(o, "VCHIQ_ServiceOpen \"SMEM\": fake error") &&
          strstr(o, "Result: the SMEM service can't be opened from RISC OS"), "no SMEM (%d):\n%s", ret, o);
    all_freed("no SMEM");

    setup();
    smem_refuse_import = 1;
    o = run(&ret, 0);
    CHECK(ret == 1 && strstr(o, "refused") && strstr(o, "Result: SMEM opens, but importing memory failed"),
          "imports refused (%d):\n%s", ret, o);
    all_freed("imports refused");

    printf("smemprobe_test: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
