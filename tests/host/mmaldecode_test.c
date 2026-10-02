/*
 * mmaldecode (tools/mmaldecode) against a fake VCHIQ module and a fake
 * MMAL video decoder. The fake "stream" is frame records (a start code and
 * a value); the fake decoder turns each into a picture (Y, U, V filled
 * from the value) laid out as MMAL's I420 (pitch 32-aligned, rows 16-
 * aligned), and the expected list is Adler-32 from 0 of the packed
 * visible picture, as FFmpeg's framecrc. The real callback stub runs (in
 * executable memory) when the fake completes a bulk receive.
 *   - the normal path: several 64 KB pieces by bulk, a short last piece in
 *     the message, EOS, an EFCH that needs no change, every picture right;
 *   - an EFCH to a bigger size: disable, reformat, re-enable, buffers again;
 *   - as the Pi, the fake answers nothing while data it's sending waits for
 *     a receive; and an output buffer numbered 0 is swallowed (a 44 byte
 *     cmd 0 "event", no data), its picture lost, and a disable of the
 *     output never answers (0.6-0.10 on the Pi);
 *   - a decoder that wants buffers before its format change: handed over
 *     after 200 cs anyway;
 *   - a wrong picture: stops there (and with -n goes on and counts);
 *   - a decoder that goes quiet: gives up; an error event: stops;
 *   - every run: component disabled and destroyed, ports disabled,
 *     use/release balanced, service closed, disconnected, stub freed.
 * MP4 input (0.13): a small MP4 written here (two closed GOPs of I P B B
 * P B, a first sample over 64 KB, SPS and PPS only in the avcC). The fake
 * decoder takes each access unit (FRAME_START to FRAME_END) with the pts
 * on its first buffer, wants SPS and PPS before every IDR, keeps a DPB of
 * two and gives pictures out in pts order with their pts (as FFmpeg's
 * mmaldec relies on; not yet seen on the Pi). A FLUSH or a disable of the
 * input drops what it holds and returns the buffers before the reply;
 * after it, nothing until an IDR. Checked: pts and display order, a
 * decoder that returns pts in decode order (caught) or none (matched in
 * order), flush or disable then seek, -t.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "kernel.h"

int probe_main(int argc, char **argv);
static void pci_open(int rw);
/* SVC mode: PCI_RAMAlloc memory is reachable only here (and by the "VideoCore") */
void probe_svc_copy(void *dst, const void *src, size_t n) { pci_open(1); memcpy(dst, src, n); pci_open(0); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define MAGIC 0x6C616D6Du
#define W 70
#define H 38
#define NFRAMES 9
static _kernel_oserror err = { 1, "fake error" }, empty = { 2, "none" };

/* scenario */
static int hijacked, vc_stuck, efch_late, swap_frames, hold_inputs;
static uint32_t held[32]; static int nheld;
static int big_efch, corrupt_at = -1, go_quiet, error_event, long_event, awaiting_reformat;
static int mp4_mode, pts_fifo, no_pts_back, efch_after_flush;
/* counters */
static int uses, releases, opens, closes, connects, disconnects, freed, created, destroyed, comp_enabled,
           port_on[4], shorts, bulks_tx, bulks_rx, efch_sent, disables_out, flushes, aus, au_pieces_max, idr_dropped;
static uint32_t stub, time_cs;

/* the fake decoder's state */
static uint8_t got_stream[1 << 20];
static uint32_t got_len, pending_tx, pending_tx_ctx;
static uint8_t txq[8][65536];
static uint32_t txq_len[8];
static int ntxq, pending_tx_now;
static uint32_t out_w, out_h, out_size_set;
static int out_bufs[8], nout, frames_made, eos_in, eos_out_sent, need_rx;
static uint32_t rx_len_expected;
static uint8_t cur_frame[1 << 16];
#define UNKNOWN 0x8000000000000000ull

static void buffer_back(uint32_t port, uint32_t ctx, uint32_t length, uint32_t flags, uint64_t pts);

/* MP4 mode: access units as they arrive, the DPB, the pictures ready */
typedef struct { uint64_t pts; uint8_t val; } pic_t;
static uint8_t au_data[1 << 18];
static uint32_t au_n, au_pieces;
static uint64_t au_pts, pending_pts, au_pts_seen[64];
static uint32_t pending_flags;
static int in_au, need_idr, aus_decoded, n_au_pts;
static pic_t dpb[8], ready[64];
static int ndpb, nready;
static uint64_t fifo_pts[64];               /* pts_fifo: the input's pts, in decode order */
static int nfifo;

static void to_ready(int all)
{
    while (ndpb > (all ? 0 : 2)) {          /* the smallest pts first: display order */
        int m = 0;
        for (int i = 1; i < ndpb; i++) if (dpb[i].pts < dpb[m].pts) m = i;
        ready[nready++] = dpb[m];
        dpb[m] = dpb[--ndpb];
    }
}

/* an access unit, whole: Annex B, SPS and PPS before an IDR */
static void decode_au(void)
{
    int sps = 0, pps = 0, idr = 0, slice = 0;
    uint8_t val = 0;
    aus++;
    if (au_pieces > (uint32_t)au_pieces_max) au_pieces_max = (int)au_pieces;
    CHECK(au_n >= 5 && !au_data[0] && !au_data[1] && !au_data[2] && au_data[3] == 1, "an access unit that isn't Annex B");
    for (uint32_t i = 0; i + 4 < au_n; i++) {
        if (au_data[i] || au_data[i + 1] || au_data[i + 2] != 1) continue;
        uint8_t t = au_data[i + 3] & 31;
        if (t == 7) sps = 1;
        if (t == 8) pps = 1;
        if ((t == 1 || t == 5) && !slice) {
            slice = 1;
            idr = t == 5;
            val = au_data[i + 4];
            CHECK(!idr || (sps && pps), "an IDR without SPS and PPS before it");
        }
        i += 2;
    }
    CHECK(slice, "an access unit with no slice");
    if (n_au_pts < 64) au_pts_seen[n_au_pts++] = au_pts;
    if (need_idr && !idr) { idr_dropped++; return; }   /* after a flush: nothing until an IDR */
    need_idr = 0;
    aus_decoded++;
    if (nfifo < 64) fifo_pts[nfifo++] = au_pts;
    dpb[ndpb].pts = au_pts;
    dpb[ndpb++].val = val;
    to_ready(0);
}

/* a flush (or a disable) of the input: the decoder starts again */
static void decoder_reset(void)
{
    in_au = 0; au_n = 0; ndpb = nready = 0; need_idr = 1; eos_in = eos_out_sent = 0; nfifo = 0;
    while (nheld) buffer_back(1, held[--nheld], 0, 0, UNKNOWN);
    if (efch_after_flush) efch_sent = 0;
}

/* a buffer's data (MP4 mode): to the access unit; FRAME_END: decoded */
static void mp4_piece(const uint8_t *d, uint32_t n)
{
    if (!in_au) return;
    CHECK(au_n + n <= sizeof au_data, "access unit too big");
    if (n) memcpy(au_data + au_n, d, n);
    au_n += n;
    au_pieces++;
    if (pending_flags & 4) { in_au = 0; decode_au(); }
}
static uint32_t outq[64][128], outq_len[64];
static int noutq;

static void post(uint32_t type, const void *payload, uint32_t len, uint32_t status)
{
    if (getenv("FAKEDBG")) fprintf(stderr, "post type %u len %u\n", type, len);
    uint32_t *m = outq[noutq];
    memset(m, 0, 512);
    m[0] = MAGIC; m[1] = type; m[4] = status;
    memcpy(m + 6, payload, len);
    outq_len[noutq++] = 24 + len;
}

static void reply(const uint32_t *req, const void *payload, uint32_t len) { post(req[1], payload, len, 0); }

typedef struct { uint32_t magic, comp, port, ctx; } drv_t;

static void buffer_back(uint32_t port, uint32_t ctx, uint32_t length, uint32_t flags, uint64_t pts)
{
    uint32_t b[68];                          /* 272 bytes */
    if (port == 1 && ctx == 0) {             /* as the Pi: a stray cmd 0 event before input buffer 0 */
        uint32_t e0[5] = { 1, 2, 0, 0, 0 };
        post(16, e0, sizeof e0, 0);
    }
    memset(b, 0, sizeof b);
    b[0] = MAGIC; b[1] = 0xC0DE; b[2] = port; b[3] = ctx;
    b[4 + 4 + 4] = 0;                        /* (buffer header) */
    b[8 + 5] = length;                       /* length at buffer header +20 */
    b[8 + 7] = flags;
    b[16] = (uint32_t)pts; b[17] = (uint32_t)(pts >> 32);   /* pts at buffer header +32 (byte 64) */
    post(12, b, sizeof b, 0);
}

/* frames: the value after each 00 00 00 01 */
static int frame_values(uint8_t *v)
{
    int n = 0;
    for (uint32_t i = 0; i + 4 < got_len; i++)
        if (!got_stream[i] && !got_stream[i + 1] && !got_stream[i + 2] && got_stream[i + 3] == 1) v[n++] = got_stream[i + 4];
    return n;
}

static void render(uint8_t val, uint8_t *dst)
{
    uint32_t pw = (out_w + 31) & ~31u, ph = (out_h + 15) & ~15u;
    memset(dst, 0x55, pw * ph * 3 / 2);      /* padding: must not be checksummed */
    for (uint32_t y = 0; y < H; y++) memset(dst + y * pw, val, W);
    for (uint32_t y = 0; y < (H + 1) / 2; y++) {
        memset(dst + pw * ph + y * (pw / 2), val + 1, (W + 1) / 2);
        memset(dst + pw * ph + (pw / 2) * (ph / 2) + y * (pw / 2), val + 2, (W + 1) / 2);
    }
}

/* hands out the next picture if there's an output buffer */
static void produce(void)
{
    uint8_t vals[64];
    int n;
    if (need_rx || go_quiet || awaiting_reformat || vc_stuck) return;
    if (mp4_mode) {                          /* pictures as access units are decoded */
        if (!aus_decoded) return;
        n = frames_made + nready;
    } else {                                 /* the whole stream first */
        if (!eos_in) return;
        n = frame_values(vals);
    }
    if (!efch_sent && (!efch_late || nout)) {   /* the first thing out: a format change (no buffers needed) */
        uint32_t ev[5 + 256 / 4 + 1];
        uint32_t *fc;
        memset(ev, 0, sizeof ev);
        ev[1] = 3; ev[2] = 0; ev[3] = 0x48434645u; ev[4] = 232;
        fc = &ev[5];
        if (big_efch) { out_w = 200; out_h = 100; }
        fc[0] = ((out_w + 31) & ~31u) * ((out_h + 15) & ~15u) * 3 / 2;   /* size min */
        fc[1] = 1; fc[2] = fc[0]; fc[3] = 3;
        fc[5 + 0] = 3; fc[5 + 1] = 0x30323449u;                          /* format: video, I420 */
        fc[13 + 0] = (out_w + 31) & ~31u; fc[13 + 1] = (out_h + 15) & ~15u; /* video width/height */
        fc[13 + 4] = W; fc[13 + 5] = H;                                   /* crop w/h */
        post(16, ev, sizeof ev, 0);
        efch_sent = 1;
        if (big_efch) { awaiting_reformat = 1; return; }   /* nothing more until the port is re-enabled */
    }
    if (!nout) return;
    if (long_event == 1) {                   /* an event whose data comes by bulk transfer */
        uint32_t ev[5 + 64 + 1];
        memset(ev, 0, sizeof ev);
        ev[1] = 2; ev[3] = 0x48435045u; ev[4] = 300;
        post(16, ev, sizeof ev, 0);
        need_rx = 3; rx_len_expected = 300;  /* its data follows by bulk */
        long_event = 2;
        return;
    }
    if (error_event && frames_made == 2) {
        uint32_t ev[5 + 64 + 1];
        memset(ev, 0, sizeof ev);
        ev[1] = 3; ev[3] = 0x4F525245u;
        post(16, ev, sizeof ev, 0);
        error_event = 0;
        return;
    }
    if (frames_made < n) {
        uint32_t pw = (out_w + 31) & ~31u, ph = (out_h + 15) & ~15u, size = pw * ph * 3 / 2;
        if (out_bufs[nout - 1] == 0 && !hijacked) {
            /* as the Pi (0.6-0.10): a buffer numbered 0 (no buffer, to the
               firmware) becomes a 44 byte cmd 0 "event" with a picture's
               length and no data, the picture is lost with it, and the
               buffer is held: a disable of the output never answers */
            uint32_t e0[5] = { 1, 3, 0, 0, size };
            post(16, e0, sizeof e0, 0);
            nout--;
            hijacked = 1;
            if (mp4_mode) memmove(ready, ready + 1, sizeof ready[0] * (size_t)--nready);
            frames_made++;
            return;
        }
        if (mp4_mode) {
            uint64_t pts = no_pts_back ? UNKNOWN : ready[0].pts;
            if (pts_fifo) {                  /* the input's pts in the order they came, not the pictures' */
                pts = fifo_pts[0];
                memmove(fifo_pts, fifo_pts + 1, sizeof fifo_pts[0] * (size_t)--nfifo);
            }
            render(ready[0].val, cur_frame);
            memmove(ready, ready + 1, sizeof ready[0] * (size_t)--nready);
            if (frames_made == corrupt_at) cur_frame[5] ^= 1;
            buffer_back(2, (uint32_t)out_bufs[--nout], size, 0, pts);
        } else {
            render(vals[swap_frames && frames_made == 1 ? 2 : swap_frames && frames_made == 2 ? 1 : frames_made], cur_frame);
            if (frames_made == corrupt_at) cur_frame[5] ^= 1;
            buffer_back(2, (uint32_t)out_bufs[--nout], size, 0, UNKNOWN);
        }
        need_rx = 1; rx_len_expected = size;
        frames_made++;
    } else if (eos_in && !eos_out_sent) {
        buffer_back(2, (uint32_t)out_bufs[--nout], 0, 1, UNKNOWN);
        need_rx = 2; rx_len_expected = 8;
        eos_out_sent = 1;
    }
}

/* As the Pi: while the VideoCore is sending data by bulk transfer (an
   event's or a picture's) and the host hasn't queued the receive, it
   answers nothing else. Messages wait here until the receive (0.8 sent a
   port disable at such a time, waited for the reply, and never got one). */
static uint32_t deferred[8][128], deferred_len[8];
static int ndeferred, deferred_ever;

static void take_tx(void);

static void firmware(const uint32_t *m, uint32_t len)
{
    const uint32_t *p = m + 6;
    CHECK(m[0] == MAGIC && len <= 512, "bad message");
    if (vc_stuck) return;                    /* (until the machine is restarted) */
    if (m[1] == 10 && p[1] == 2 && p[2] == 2 && hijacked) { vc_stuck = 1; return; }
    if (need_rx) {
        CHECK(ndeferred < 8, "too many messages while sending");
        if (ndeferred < 8) { memcpy(deferred[ndeferred], m, len); deferred_len[ndeferred++] = len; deferred_ever++; }
        return;
    }
    switch (m[1]) {
    case 4: { uint32_t r[5] = { 0, 0xC0DE, 1, 1, 1 }; created++; reply(m, r, sizeof r); break; }
    case 8: {                                /* PORT_INFO_GET */
        uint32_t r[6 + 16 + 8 + 13 + 32];
        memset(r, 0, sizeof r);
        r[1] = 0xC0DE; r[2] = p[1]; r[5] = p[1] - 1;          /* input handle 1, output 2 */
        r[6 + 6] = 1; r[6 + 7] = 2048; r[6 + 9] = 20; r[6 + 10] = 81920;
        reply(m, r, sizeof r);
        break;
    }
    case 9: {                                /* PORT_INFO_SET */
        uint32_t r[6 + 16 + 8 + 13 + 32];
        const uint32_t *port = p + 3, *fmt = p + 3 + 16, *vid = p + 3 + 16 + 8;
        memset(r, 0, sizeof r);
        r[1] = 0xC0DE; r[2] = p[0]; r[5] = p[1] - 1;
        memcpy(&r[6], port, 64); memcpy(&r[6 + 16], fmt, 32); memcpy(&r[6 + 16 + 8], vid, 44);
        if (p[1] == 2) {
            CHECK(fmt[1] == 0x34363248u && vid[0] == W && vid[1] == H && port[11] == 20 && port[12] == 65536,
                  "input format %08X %ux%u, %u x %u", fmt[1], vid[0], vid[1], port[11], port[12]);
        } else if (p[1] == 3) {
            CHECK(fmt[1] == 0x30323449u && vid[0] % 32 == 0 && vid[1] % 16 == 0, "output format %08X %ux%u", fmt[1], vid[0], vid[1]);
            r[6 + 10] = vid[0] * vid[1] * 3 / 2;               /* size recommended */
            out_size_set = port[12];
        }
        reply(m, r, sizeof r);
        break;
    }
    case 6: comp_enabled = 1; { uint32_t r = 0; reply(m, &r, 4); } break;
    case 7: comp_enabled = 0; { uint32_t r = 0; reply(m, &r, 4); } break;
    case 5: destroyed++; { uint32_t r = 0; reply(m, &r, 4); } break;
    case 10: {                               /* PORT_ACTION */
        uint32_t r = 0, port = p[1];
        CHECK(port == 1 || port == 2, "port action on %u", port);
        CHECK(p[2] >= 1 && p[2] <= 3, "port action %u", p[2]);
        if (p[2] == 1) { port_on[port] = 1; if (port == 2) awaiting_reformat = 0; }
        else {                               /* 2 disable, 3 flush: the port's buffers back before the reply */
            if (p[2] == 2) port_on[port] = 0;
            else flushes++;
            if (port == 2) {
                if (p[2] == 2) disables_out++;
                while (nout) buffer_back(2, (uint32_t)out_bufs[--nout], 0, 0, UNKNOWN);
            } else {
                decoder_reset();
            }
        }
        reply(m, &r, 4);
        produce();
        break;
    }
    case 11: {                               /* BUFFER_FROM_HOST */
        const drv_t *d = (const drv_t *)p;
        uint32_t length = p[8 + 5], flags = p[8 + 7], pim = p[8 + 14 + 10 + 2];
        CHECK(d->magic == MAGIC && d->comp == 0xC0DE, "drvbuf");
        if (d->port == 1) {
            CHECK(port_on[1], "input buffer with the port off");
            if (pim) {
                CHECK(pim == length && length <= 128, "short data %u/%u", pim, length);
                memcpy(got_stream + got_len, (const uint8_t *)&p[8 + 14 + 10 + 3], pim);
                got_len += pim;
                shorts++;
                buffer_back(1, d->ctx, 0, 0, UNKNOWN);
            } else if (length) {
                pending_tx = length; pending_tx_ctx = d->ctx;
                pending_tx_now = 1;
            } else {
                buffer_back(1, d->ctx, 0, 0, UNKNOWN);
            }
            pending_flags = flags;
            pending_pts = (uint64_t)p[16] | (uint64_t)p[17] << 32;
            if (mp4_mode) {
                CHECK(flags & (2 | 4) || in_au || (flags == 1 && !length), "a buffer outside an access unit (flags &%X)", flags);
                if (flags & 2) {             /* FRAME_START: the pts is on this one */
                    CHECK(!in_au, "FRAME_START inside an access unit");
                    in_au = 1; au_n = 0; au_pieces = 0; au_pts = pending_pts;
                    CHECK(pending_pts != UNKNOWN, "an access unit with no pts");
                } else {
                    CHECK(pending_pts == UNKNOWN, "a pts on a buffer that doesn't start an access unit");
                }
                if (!length) mp4_piece(NULL, 0);
            }
            if (flags & 1) {
                eos_in = 1;
                if (mp4_mode && !pending_tx) to_ready(1);   /* EOS: the DPB emptied */
            }
            if (pending_tx_now && ntxq) { pending_tx_now = 0; take_tx(); return; }
            pending_tx_now = 0;
        } else {
            CHECK(d->port == 2 && port_on[2], "output buffer for port %u (on %d)", d->port, port_on[2]);
            CHECK(p[8 + 4] >= out_size_set, "output alloc_size %u < %u", p[8 + 4], out_size_set);
            out_bufs[nout++] = (int)d->ctx;
        }
        produce();
        break;
    }
    default:
        CHECK(0, "message type %u", m[1]);
    }
}

typedef void stub_fn(uint32_t param, uint32_t reason, uint32_t h);

/* the data for the input buffer whose message has been taken */
static void take_tx(void)
{
    uint32_t n = txq_len[0];
    CHECK(pending_tx && n == pending_tx, "transmit %u (expected %u)", n, pending_tx);
    memcpy(got_stream + got_len, txq[0], n);
    if (mp4_mode) mp4_piece(txq[0], n);
    got_len += n;
    bulks_tx++;
    memmove(txq[0], txq[1], sizeof txq[0] * (size_t)(ntxq - 1));
    memmove(txq_len, txq_len + 1, sizeof txq_len[0] * (size_t)(ntxq - 1));
    ntxq--;
    if (hold_inputs) {                       /* as the Pi with 1080p: input kept until there's enough */
        held[nheld++] = pending_tx_ctx;
        if (nheld >= hold_inputs || eos_in) { while (nheld) buffer_back(1, held[--nheld], 0, 0, UNKNOWN); }
    } else {
        buffer_back(1, pending_tx_ctx, 0, 0, UNKNOWN);
    }
    pending_tx = 0;
    if (mp4_mode && eos_in) to_ready(1);
    produce();
}

/* PCI_RAMAlloc: the only memory VCHIQ's bulk transfers handle (it assumes
   physically contiguous); every bulk range must lie inside one block */
static uint32_t pci_lo[32], pci_hi[32];
static int npci_blocks, pci_live, no_pci_mem;
static void pci_open(int rw)
{
    for (int i = 0; i < npci_blocks; i++)
        if (pci_lo[i]) mprotect((void *)(uintptr_t)pci_lo[i], (pci_hi[i] - pci_lo[i] + 4095) & ~4095u, rw ? PROT_READ | PROT_WRITE : PROT_NONE);
}
static int in_pci(uint32_t a, uint32_t n)
{
    for (int i = 0; i < npci_blocks; i++)
        if (pci_lo[i] && a >= pci_lo[i] && a + n <= pci_hi[i]) return 1;
    return 0;
}

_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r)
{
    uint32_t *R = (uint32_t *)r->r;
    switch (n) {
    case 0x42: R[0] = time_cs++; return NULL;
    case 0x591C5: {                          /* BCMSupport_SendTempPropertyBuffer: VC memory */
        uint32_t *b = (uint32_t *)(uintptr_t)R[0];
        CHECK(b[2] == 0x00010006, "property tag %08X", b[2]);
        b[1] = 0x80000000u; b[5] = 0x3B400000; b[6] = 76u << 20;
        return NULL;
    }
    case 0x1E:
        if (R[0] == 6) {
            void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            stub = (uint32_t)(uintptr_t)p;
            R[2] = stub;
            return NULL;
        }
        if (R[0] == 7) { freed++; CHECK(R[2] == stub, "freed another block"); return NULL; }
        break;
    case 0x6E: CHECK(R[0] == 1 && R[1] == stub, "sync"); __builtin___clear_cache((char *)(uintptr_t)stub, (char *)(uintptr_t)stub + 64); return NULL;
    case 0x59200: R[0] = 0x1A57; return NULL;
    case 0x59201: connects++; return NULL;
    case 0x59202: disconnects++; return NULL;
    case 0x5920A: {
        uint32_t *s = (uint32_t *)(uintptr_t)R[1];
        opens++;
        CHECK(s[2] == 0x6D6D616Cu && s[6] == stub && s[7] == stub + 64, "open params");
        uses++;
        R[0] = 0x5E7;
        return NULL;
    }
    case 0x59209: closes++; return NULL;
    case 0x5920D: uses++; return NULL;
    case 0x5920E: releases++; return NULL;
    case 0x59205: CHECK(uses > releases, "queue while not in use"); firmware((const uint32_t *)(uintptr_t)R[1], R[2]); return NULL;
    case 0x39: {
        const char *nm = (const char *)(uintptr_t)R[1];
        R[0] = !strcmp(nm, "PCI_RAMAlloc") ? 0x50100 : !strcmp(nm, "PCI_RAMFree") ? 0x50101 : 0;
        return R[0] ? NULL : &err;
    }
    case 0x50100: {
        void *p;
        CHECK(R[1] == 4096, "PCI alignment %u", R[1]);
        if (no_pci_mem) return &err;
        /* privileged on RISC OS: no access from "USR mode" here (a stray
           access is a SIGSEGV); probe_svc_copy and the fake VideoCore open it */
        p = mmap(NULL, (R[0] + 4095) & ~4095u, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        pci_lo[npci_blocks] = (uint32_t)(uintptr_t)p; pci_hi[npci_blocks++] = (uint32_t)(uintptr_t)p + R[0];
        pci_live++;
        R[0] = (uint32_t)(uintptr_t)p; R[1] = 0x01000000;
        return NULL;
    }
    case 0x50101:
        for (int i = 0; i < npci_blocks; i++)
            if (pci_lo[i] == R[0]) {
                munmap((void *)(uintptr_t)R[0], (pci_hi[i] - pci_lo[i] + 4095) & ~4095u);
                pci_lo[i] = 0; pci_live--; return NULL;
            }
        CHECK(0, "PCI_RAMFree of an unknown block");
        return &err;
    case 0x59203:                            /* BulkQueueTransmit */
        CHECK(in_pci(R[1], R[2]), "transmit from memory that isn't PCI_RAMAlloc'd");
        CHECK(uses > releases, "transmit while not in use");
        CHECK(R[3] == 4 && (R[1] & 3) == 0, "transmit flags %u", R[3]);
        /* its message may be waiting (the fake is sending): then so does
           the data, until the message is taken */
        CHECK(ntxq < 8 && R[2] <= sizeof txq[0], "transmit queue");
        pci_open(1);
        memcpy(txq[ntxq], (const void *)(uintptr_t)R[1], R[2]);
        pci_open(0);
        txq_len[ntxq++] = R[2];
        if (pending_tx) take_tx();
        else CHECK(ndeferred, "a transmit with no buffer message");
        return NULL;
    case 0x5920F: {                          /* BulkQueueReceive */
        if (!need_rx) {                      /* nothing being sent: it waits (for good, here) */
            CHECK(0, "a receive with nothing being sent (%u bytes)", R[2]);
            return NULL;
        }
        CHECK(need_rx && R[2] == ((rx_len_expected + 3) & ~3u) && R[3] == 6, "receive %u (expected %u), flags %u",
              R[2], rx_len_expected, R[3]);
        CHECK((R[1] & 63) == 0, "receive buffer not aligned");
        CHECK(in_pci(R[1], R[2]), "receive into memory that isn't PCI_RAMAlloc'd");
        pci_open(1);
        if (need_rx == 1) memcpy((void *)(uintptr_t)R[1], cur_frame, R[2]);
        pci_open(0);
        need_rx = 0;
        bulks_rx++;
        ((stub_fn *)(uintptr_t)stub)(stub + 64, 4, R[4]);      /* the real callback */
        while (!need_rx && ndeferred) {                         /* what waited, in order */
            uint32_t m[128], l = deferred_len[0];
            memcpy(m, deferred[0], l);
            memmove(deferred[0], deferred[1], sizeof deferred[0] * (size_t)(ndeferred - 1));
            memmove(deferred_len, deferred_len + 1, sizeof deferred_len[0] * (size_t)(ndeferred - 1));
            ndeferred--;
            firmware(m, l);
        }
        produce();
        return NULL;
    }
    case 0x59204:
        CHECK(R[2] == 512 && R[3] == 0, "dequeue regs");
        if (!noutq) return &empty;
        memcpy((void *)(uintptr_t)R[1], outq[0], outq_len[0]);
        R[2] = outq_len[0];
        memmove(outq[0], outq[1], sizeof outq[0] * (size_t)(noutq - 1));
        memmove(outq_len, outq_len + 1, sizeof outq_len[0] * (size_t)(noutq - 1));
        noutq--;
        return NULL;
    }
    CHECK(0, "unexpected SWI &%X", n);
    return &err;
}

/* ---- the test stream and its expected list ---- */

static uint32_t adler0(uint32_t a, const uint8_t *p, size_t n)
{
    uint32_t s1 = a & 0xFFFF, s2 = a >> 16;
    while (n--) { s1 = (s1 + *p++) % 65521; s2 = (s2 + s1) % 65521; }
    return s2 << 16 | s1;
}

static void make_files(void)
{
    FILE *f = fopen("/tmp/mmaldecode_test.h264", "wb"), *c = fopen("/tmp/mmaldecode_test.crc", "w");
    FILE *sg = fopen("/tmp/mmaldecode_test.sig", "w");
    uint8_t filler[40000];
#define FILLER 36415
    memset(filler, 0xAA, sizeof filler);
    fprintf(c, "#tb 0: 1/25\n#dimensions 0: %dx%d\n", W, H);
    for (int k = 0; k < NFRAMES; k++) {
        uint8_t v = (uint8_t)(10 + 7 * k), sc[5] = { 0, 0, 0, 1, v };
        uint8_t y[W], u[(W + 1) / 2], vv[(W + 1) / 2];
        uint32_t a = 0;
        fwrite(sc, 1, 5, f);
        fwrite(filler, 1, FILLER, f);        /* 5 whole 64 KB pieces, then 100 bytes (in the message) */
        memset(y, v, W); memset(u, v + 1, sizeof u); memset(vv, v + 2, sizeof vv);
        for (int r = 0; r < H; r++) a = adler0(a, y, W);
        for (int r = 0; r < (H + 1) / 2; r++) a = adler0(a, u, sizeof u);
        for (int r = 0; r < (H + 1) / 2; r++) a = adler0(a, vv, sizeof vv);
        fprintf(c, "0, %10d, %10d, 1, %8d, 0x%08x\n", k, k, W * H * 3 / 2, a);
        for (int q = 0; q < 96; q++) fprintf(sg, "%d.00%c", q < 64 ? v : q < 80 ? v + 1 : v + 2, q == 95 ? '\n' : ' ');
    }
    fclose(f);
    fclose(c);
    fclose(sg);
}

static void reset_fake(void)
{
    uses = releases = opens = closes = connects = disconnects = freed = created = destroyed = comp_enabled = 0;
    memset(port_on, 0, sizeof port_on);
    shorts = bulks_tx = bulks_rx = efch_sent = disables_out = flushes = aus = au_pieces_max = idr_dropped = 0;
    got_len = pending_tx = 0; nheld = 0; awaiting_reformat = 0; hijacked = vc_stuck = 0; out_w = W; out_h = H; nout = frames_made = eos_in = eos_out_sent = need_rx = 0;
    noutq = 0; ndeferred = deferred_ever = 0; ntxq = pending_tx_now = 0;
    in_au = 0; au_n = 0; need_idr = 0; aus_decoded = 0; n_au_pts = 0; ndpb = nready = 0; nfifo = 0;
}

/* ---- the MP4: two closed GOPs, decode order I P B B P B, pts in frames ---- */

#define NMP4 12
static const int mp4_pts[NMP4] = { 0, 3, 1, 2, 5, 4, 6, 9, 7, 8, 11, 10 };
static uint8_t mp4buf[1 << 18];
static uint32_t mp4n, boxstack[16];
static int nbox;

static void put8(uint32_t v) { mp4buf[mp4n++] = (uint8_t)v; }
static void put16(uint32_t v) { put8(v >> 8); put8(v); }
static void put32(uint32_t v) { put16(v >> 16); put16(v); }
static void putn(const void *d, uint32_t n) { memcpy(mp4buf + mp4n, d, n); mp4n += n; }
static void open_box(const char *t) { boxstack[nbox++] = mp4n; put32(0); putn(t, 4); }
static void full_box(const char *t, uint32_t vf) { open_box(t); put32(vf); }
static void close_box(void)
{
    uint32_t at = boxstack[--nbox], n = mp4n - at;
    mp4buf[at] = (uint8_t)(n >> 24); mp4buf[at + 1] = (uint8_t)(n >> 16); mp4buf[at + 2] = (uint8_t)(n >> 8); mp4buf[at + 3] = (uint8_t)n;
}

static uint8_t mp4_val(int pts) { return (uint8_t)(10 + 7 * pts); }

static void make_mp4(void)
{
    static const uint8_t sps[] = { 0x67, 0x4D, 0x00, 0x1E, 0xAB }, pps[] = { 0x68, 0xCE, 0x3C, 0x80 };
    static const uint8_t sei[] = { 0x06, 0x05, 0x01, 0x00, 0x80 };
    uint32_t size[NMP4], mdat_data;
    FILE *f = fopen("/tmp/mmaldecode_test.mp4", "wb"), *c = fopen("/tmp/mmaldecode_test4.crc", "w");
    FILE *sg = fopen("/tmp/mmaldecode_test4.sig", "w");
    mp4n = 0; nbox = 0;
    open_box("ftyp"); putn("isom", 4); put32(512); putn("isomavc1", 8); close_box();
    open_box("mdat");
    mdat_data = mp4n;
    for (int k = 0; k < NMP4; k++) {         /* each sample: (SEI,) one slice; the first over 64 KB */
        uint32_t at = mp4n, fill = k == 0 ? 70000 : 40 + 13 * (uint32_t)k;
        int key = k % 6 == 0;
        if (k == 0) { put32(sizeof sei); putn(sei, sizeof sei); }
        put32(2 + fill);
        put8(key ? 0x65 : 0x41);
        put8(mp4_val(mp4_pts[k]));
        memset(mp4buf + mp4n, 0xAA, fill); mp4n += fill;
        size[k] = mp4n - at;
    }
    close_box();
    open_box("moov");
    open_box("trak");
    open_box("edts"); full_box("elst", 0); put32(1); put32(12 * 512); put32(1024); put32(0x10000); close_box(); close_box();
    open_box("mdia");
    full_box("mdhd", 0); put32(0); put32(0); put32(12800); put32(NMP4 * 512); put32(0); close_box();
    open_box("minf");
    open_box("stbl");
    full_box("stsd", 0); put32(1);
    open_box("avc1");
    for (int i = 0; i < 6; i++) put8(0);
    put16(1); for (int i = 0; i < 16; i++) put8(0);
    put16(W); put16(H); put32(0x480000); put32(0x480000); put32(0); put16(1);
    for (int i = 0; i < 32; i++) put8(0);
    put16(24); put16(0xFFFF);
    open_box("avcC"); put8(1); put8(0x4D); put8(0); put8(0x1E); put8(0xFF); put8(0xE1);
    put16(sizeof sps); putn(sps, sizeof sps); put8(1); put16(sizeof pps); putn(pps, sizeof pps); close_box();
    close_box();
    close_box();
    full_box("stts", 0); put32(1); put32(NMP4); put32(512); close_box();
    full_box("ctts", 0); put32(NMP4);
    for (int k = 0; k < NMP4; k++) { put32(1); put32((uint32_t)(mp4_pts[k] + 2 - k) * 512); }
    close_box();
    full_box("stss", 0); put32(2); put32(1); put32(7); close_box();
    full_box("stsc", 0); put32(1); put32(1); put32(NMP4); put32(1); close_box();
    full_box("stsz", 0); put32(0); put32(NMP4); for (int k = 0; k < NMP4; k++) put32(size[k]); close_box();
    full_box("stco", 0); put32(1); put32(mdat_data); close_box();
    close_box(); close_box(); close_box(); close_box(); close_box();
    fwrite(mp4buf, 1, mp4n, f);
    fclose(f);
    fprintf(c, "#tb 0: 1/25\n#dimensions 0: %dx%d\n", W, H);
    for (int k = 0; k < NMP4; k++) {         /* FFmpeg's list: display order, by pts */
        uint8_t v = mp4_val(k), y[W], u[(W + 1) / 2], vv[(W + 1) / 2];
        uint32_t a = 0;
        memset(y, v, W); memset(u, v + 1, sizeof u); memset(vv, v + 2, sizeof vv);
        for (int r = 0; r < H; r++) a = adler0(a, y, W);
        for (int r = 0; r < (H + 1) / 2; r++) a = adler0(a, u, sizeof u);
        for (int r = 0; r < (H + 1) / 2; r++) a = adler0(a, vv, sizeof vv);
        fprintf(c, "0, %10d, %10d, 1, %8d, 0x%08x\n", k, k, W * H * 3 / 2, a);
        for (int q = 0; q < 96; q++) fprintf(sg, "%d.00%c", q < 64 ? v : q < 80 ? v + 1 : v + 2, q == 95 ? '\n' : ' ');
    }
    fclose(c);
    fclose(sg);
}

static void reset_fake(void);

/* mmaldecode on the MP4, with these options */
static char *run_mp4(int *ret, const char *o1, const char *o2, const char *o3)
{
    static char buf[16384];
    char *argv[12];
    int n = 0;
    FILE *f;
    size_t got;
    argv[n++] = "mmaldecode"; argv[n++] = "-o"; argv[n++] = "/tmp/mmaldecode_test.out";
    if (o1) argv[n++] = (char *)o1;
    if (o2) argv[n++] = (char *)o2;
    if (o3) argv[n++] = (char *)o3;
    argv[n++] = "/tmp/mmaldecode_test.mp4"; argv[n++] = "/tmp/mmaldecode_test4.crc"; argv[n++] = "/tmp/mmaldecode_test4.sig";
    argv[n] = NULL;
    reset_fake();
    mp4_mode = 1;
    remove("/tmp/mmaldecode_test.out");
    *ret = probe_main(n, argv);
    mp4_mode = 0;
    f = fopen("/tmp/mmaldecode_test.out", "r");
    got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got] = 0;
    if (f) fclose(f);
    return buf;
}

static int with_sig;
static char *run(int *ret, int keep_going)
{
    static char buf[16384];
    char *argv[] = { "mmaldecode", "-o", "/tmp/mmaldecode_test.out", "-n",
                     "/tmp/mmaldecode_test.h264", "/tmp/mmaldecode_test.crc", NULL };
    char *argv2[] = { "mmaldecode", "-o", "/tmp/mmaldecode_test.out",
                      "/tmp/mmaldecode_test.h264", "/tmp/mmaldecode_test.crc", NULL };
    FILE *f;
    size_t got;
    reset_fake();
    remove("/tmp/mmaldecode_test.out");
    if (with_sig) {
        char *argv3[] = { "mmaldecode", "-o", "/tmp/mmaldecode_test.out", "-n", "/tmp/mmaldecode_test.h264",
                          "/tmp/mmaldecode_test.crc", "/tmp/mmaldecode_test.sig", NULL };
        *ret = probe_main(7, argv3);
    } else {
        *ret = keep_going ? probe_main(6, argv) : probe_main(5, argv2);
    }
    f = fopen("/tmp/mmaldecode_test.out", "r");
    got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got] = 0;
    if (f) fclose(f);
    return buf;
}

static void cleaned(const char *what)
{
    CHECK(uses == releases, "%s: use/release %d/%d", what, uses, releases);
    CHECK(opens == closes && connects == disconnects && freed == 1, "%s: open/close %d/%d, connect %d/%d, freed %d",
          what, opens, closes, connects, disconnects, freed);
    CHECK(pci_live == 0, "%s: %d PCI blocks not freed", what, pci_live);
    npci_blocks = 0;
    CHECK(!comp_enabled && !port_on[1] && !port_on[2] && created == destroyed, "%s: left enabled (%d %d %d), %d/%d",
          what, comp_enabled, port_on[1], port_on[2], created, destroyed);
}

int main(int argc, char **argv)
{
    int ret;
    char *o;
    if (argc > 1) return probe_main(argc, argv);   /* (run.sh's -x check of real MP4s) */
    make_files();

    o = run(&ret, 0);
    printf("%s", o);
    CHECK(ret == 0 && strstr(o, "9 pictures decoded") && strstr(o, "9 of 9 checked against FFmpeg: 0 wrong") &&
          strstr(o, "Result: OK"), "normal path (%d)", ret);
    CHECK(bulks_tx == 6 && shorts == 0, "pieces: %d by bulk, %d in the message", bulks_tx, shorts);
    CHECK(bulks_rx == 10 && disables_out == 1, "receives %d (9 pictures + EOS), disables %d (only the end: the format was the one set)", bulks_rx, disables_out);
    CHECK(got_len == 9 * (FILLER + 5), "stream arrived whole: %u", got_len);
    CHECK(!hijacked && !vc_stuck, "an output buffer numbered 0 (swallowed, %d)", vc_stuck);
    cleaned("normal");

    CHECK(strstr(o, "The VideoCore has 76 MB of memory"), "VideoCore memory not shown");

    hold_inputs = 4;                          /* keeps 4 input buffers before giving any back (the Pi, 1080p) */
    o = run(&ret, 0);
    CHECK(ret == 0 && strstr(o, "Result: OK"), "a decoder that keeps 4 input buffers (%d):\n%s", ret, o);
    cleaned("held inputs");
    hold_inputs = 0;

    efch_late = 1;                            /* a decoder that waits for buffers before its format change */
    o = run(&ret, 0);
    CHECK(ret == 0 && strstr(o, "handed over anyway") && strstr(o, "Result: OK"), "late format change (%d):\n%s", ret, o);
    cleaned("late EFCH");
    efch_late = 0;

    big_efch = 1;
    o = run(&ret, 0);
    CHECK(ret == 0 && disables_out == 2 && strstr(o, "Format changed: I420 224x112") && strstr(o, "Result: OK"),
          "bigger format (%d, disables %d):\n%s", ret, disables_out, o);
    cleaned("bigger");
    big_efch = 0;

    corrupt_at = 3;
    o = run(&ret, 0);
    CHECK(ret == 1 && strstr(o, "Picture 3: checksum") && strstr(o, "pictures differ"), "wrong picture (%d)", ret);
    cleaned("wrong");
    o = run(&ret, 1);
    CHECK(ret == 1 && strstr(o, "9 of 9 checked against FFmpeg: 1 wrong"), "-n (%d):\n%s", ret, o);
    cleaned("-n");
    corrupt_at = -1;

    go_quiet = 1;
    o = run(&ret, 0);
    CHECK(ret == 1 && strstr(o, "Nothing from the decoder") && strstr(o, "didn't finish"), "quiet (%d)", ret);
    cleaned("quiet");
    go_quiet = 0;

    long_event = 1;
    o = run(&ret, 0);
    CHECK(ret == 0 && long_event == 2 && bulks_rx == 11 && strstr(o, "Event EPCH (&48435045, 300 bytes) on port type 2"),
          "long event (%d, %d receives):\n%s", ret, bulks_rx, o);
    cleaned("long event");
    long_event = 0;

    error_event = 1;
    o = run(&ret, 0);
    CHECK(ret == 1 && strstr(o, "reports an error"), "error event (%d)", ret);
    cleaned("error");

    /* signatures: a picture 1 value off is "close"; one out of order is named */
    with_sig = 1;
    corrupt_at = 3;
    o = run(&ret, 1);
    CHECK(ret == 0 && strstr(o, "close to FFmpeg's (block means within 1): 1; not close: 0") &&
          strstr(o, "some within rounding"), "sig, 1 value off (%d):\n%s", ret, o);
    cleaned("sig close");
    corrupt_at = -1;
    swap_frames = 1;
    o = run(&ret, 1);
    CHECK(ret == 1 && strstr(o, "Picture 1: not close to FFmpeg's picture 1") && strstr(o, "closest is FFmpeg's 2"),
          "sig, swapped (%d):\n%s", ret, o);
    cleaned("sig swapped");
    swap_frames = 0;
    with_sig = 0;

    no_pci_mem = 1;
    o = run(&ret, 0);
    CHECK(ret == 1 && strstr(o, "No physically contiguous memory") && !opens && !pci_live, "no PCI memory (%d)", ret);
    npci_blocks = 0;
    no_pci_mem = 0;

    /* ---- MP4 input (0.13) ---- */
    make_mp4();
    o = run_mp4(&ret, NULL, NULL, NULL);
    printf("%s", o);
    CHECK(ret == 0 && strstr(o, "MP4: 12 samples, 70x38") && strstr(o, "12 pictures decoded") &&
          strstr(o, "12 of 12 checked against FFmpeg: 0 wrong") &&
          strstr(o, "pts: every picture came back with its own pts, in display order") && strstr(o, "Result: OK"),
          "MP4 (%d):\n%s", ret, o);
    CHECK(aus == 12 && au_pieces_max == 2, "access units %d (12), the first in %d buffers (2)", aus, au_pieces_max);
    {
        int ok = n_au_pts == 12;
        for (int k = 0; k < 12 && ok; k++) ok = au_pts_seen[k] == (uint64_t)mp4_pts[k] * 40000;
        CHECK(ok, "the access units' pts aren't the samples' (in microseconds, decode order)");
    }
    cleaned("MP4");

    pts_fifo = 1;                             /* a decoder that hands the pts back in decode order: caught */
    o = run_mp4(&ret, "-n", NULL, NULL);
    CHECK(ret == 1 && strstr(o, "not close to FFmpeg's picture") && strstr(o, "out of display order"),
          "pts in decode order (%d):\n%s", ret, o);
    cleaned("pts FIFO");
    pts_fifo = 0;

    no_pts_back = 1;                          /* no pts back: matched in order, and said so */
    o = run_mp4(&ret, NULL, NULL, NULL);
    CHECK(ret == 0 && strstr(o, "12 pictures came back without a pts") && !strstr(o, "pts: every picture") &&
          strstr(o, "came back without their pts"),
          "no pts back (%d):\n%s", ret, o);
    cleaned("no pts");
    no_pts_back = 0;

    o = run_mp4(&ret, "-s", "3", NULL);       /* flush after 3 pictures, on from the keyframe at pts 6 */
    printf("%s", o);
    CHECK(ret == 0 && flushes == 2 && strstr(o, "both ports flushed") && strstr(o, "sample 6, pts 240000") &&
          strstr(o, "Before the flush: 3 pictures") && strstr(o, "from the keyframe on, 6\n") && strstr(o, "all of them") && strstr(o, "Result: OK"),
          "flush and seek (%d, %d flushes):\n%s", ret, flushes, o);
    cleaned("seek");

    o = run_mp4(&ret, "-s", "3", "-F");       /* the same with disable and enable */
    CHECK(ret == 0 && flushes == 0 && disables_out == 2 && strstr(o, "disabled and enabled again") &&
          strstr(o, "all of them") && strstr(o, "Result: OK"), "disable and seek (%d, %d disables):\n%s", ret, disables_out, o);
    cleaned("seek -F");

    efch_after_flush = 1;                     /* a decoder that announces its format again after a flush */
    o = run_mp4(&ret, "-s", "3", NULL);
    CHECK(ret == 0 && strstr(o, "all of them") && strstr(o, "Result: OK"), "EFCH after flush (%d):\n%s", ret, o);
    cleaned("seek EFCH");
    efch_after_flush = 0;

    o = run_mp4(&ret, "-s", "50", NULL);      /* a seek that never comes: not OK */
    CHECK(ret == 1 && strstr(o, "never happened") && strstr(o, "the seek wasn't tried"), "seek never tried (%d):\n%s", ret, o);
    cleaned("no seek");

    o = run_mp4(&ret, "-s", "9", NULL);       /* the keyframe already passed: refused, not counted twice */
    CHECK(ret == 1 && strstr(o, "has been passed") && !flushes, "keyframe passed (%d):\n%s", ret, o);
    cleaned("passed");

    corrupt_at = 3;                           /* -t: timed, not checked */
    o = run_mp4(&ret, "-t", NULL, NULL);
    CHECK(ret == 0 && strstr(o, "One copy of a") && strstr(o, "Receiving:") && strstr(o, "OK - timed") &&
          !strstr(o, "checked against FFmpeg"), "-t (%d):\n%s", ret, o);
    cleaned("-t");
    corrupt_at = -1;

    {                                         /* -s with a raw stream: refused */
        char *argv[] = { "mmaldecode", "-o", "/tmp/mmaldecode_test.out", "-s", "3", "/tmp/mmaldecode_test.h264",
                         "/tmp/mmaldecode_test.crc", NULL };
        FILE *f;
        static char b2[4096];
        size_t got;
        reset_fake();
        remove("/tmp/mmaldecode_test.out");
        ret = probe_main(7, argv);
        f = fopen("/tmp/mmaldecode_test.out", "r");
        got = f ? fread(b2, 1, sizeof b2 - 1, f) : 0;
        b2[got] = 0;
        if (f) fclose(f);
        CHECK(ret == 1 && strstr(b2, "-s needs an MP4") && !opens, "-s with a raw stream (%d)", ret);
        npci_blocks = 0;
    }

    printf(fails ? "mmaldecode_test: %d failures\n" : "mmaldecode_test: all passed\n", fails);
    return fails != 0;
}
