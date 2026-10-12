/*
 * vcenc (the encoder in libvcdec) against a fake VCHIQ module (the one in
 * mmaldecode_test.c, included without its main) and a fake MMAL
 * ril.video_encode, behaving as Linux's bcm2835-codec driver expects the
 * firmware to and as vcdec's Pi-proven rules say the VCHIQ side does:
 *   - ports: control, input (I420: width a multiple of 32, height of 16,
 *     the picture's size as the crop), output (H.264); parameters recorded
 *     as they're set (some can be refused);
 *   - each input picture comes by bulk transmit, read from "RAM" (a cached
 *     pool's lines that weren't cleaned aren't there); the fake checks the
 *     padding repeats the edges and takes the Adler-32 of the visible
 *     picture;
 *   - output: one CONFIG buffer first (an SPS and PPS that carry the
 *     profile and level set), then per picture an access unit (SPS and PPS
 *     again before keyframes with INLINE_HEADER) holding the picture's
 *     number and Adler-32, KEYFRAME on keyframes (every INTRAPERIOD, or
 *     REQUEST_I_FRAME); units of up to 128 bytes come in the message
 *     (payload_in_message), longer ones by bulk transfer, in pieces of up
 *     to 60000 bytes with FRAME_END on the last; an empty EOS buffer (by
 *     bulk, 8 bytes, as Linux) after the input's EOS;
 *   - while sending by bulk it answers nothing else until the receive is
 *     queued (as the Pi).
 * Checked: every packet right and in order with its pts, flags and
 * headers; encoders that hold inputs (VCENC_AGAIN), finish receives late,
 * want bigger output buffers, refuse settings, go quiet, report errors;
 * bad calls; everything closed and freed after each run.
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#define FAKE_ONLY
#include "mmaldecode_test.c"
#include "../../vcdec/vcenc.h"

void vcdec_host_enter_os(void) { pci_open(1); }
void vcdec_host_leave_os(void) { pci_open(0); }

/* ---- the fake encoder ---- */

#define EP_CTL 10
#define EP_IN  11
#define EP_OUT 12
#define FRAG   60000
#define MAXPIECES 64
#define P_PROFILE 0x20002u
#define P_INTRAPERIOD 0x20003u
#define P_RATECONTROL 0x20004u
#define P_MINFRAG 0x20006u
#define P_REQ_I 0x2000Bu
#define P_BITRATE 0x2000Eu
#define P_MINQ 0x20010u
#define P_MAXQ 0x20011u
#define P_INLINE 0x2002Au
#define P_SPS_TIMING 0x20030u

typedef struct { uint32_t id, port, n, v[4]; } param_t;
static param_t eparams[64];
static int neparams;

/* scenario */
static int e_hold_in;                        /* inputs back only once their unit is out */
static int e_slow;                           /* a picture encoded only this many dequeues after it came (its input held) */
static int e_slow_pending, e_slow_tick;
static uint32_t e_slow_len, e_slow_ctx;
static uint64_t e_slow_pts;
static uint8_t e_pic[1920 * 1088 * 3 / 2], e_slow_pic[1920 * 1088 * 3 / 2];
static int e_quiet, e_error_at = -1, e_pim = 1, e_rx_late;
static uint32_t e_refuse_id, e_out_min = 65536;
static int e_big_at = -1;                    /* this picture's unit: 100000 bytes (two pieces) */
static int pts_late;                         /* a unit's pts on its last piece (else its first); KEYFRAME on the first */
/* state */
static int e_created, e_destroyed, e_enabled, e_port_on[3], e_frames, e_eos_in, e_eos_out, e_force, e_config_out;
static uint32_t e_in_stride, e_in_rows, e_crop_w, e_crop_h, e_in_size_set, e_out_size_set, e_in_enc, e_out_enc;
static uint32_t e_pending_tx, e_pending_ctx, e_pending_flags;
static uint64_t e_pending_pts;
static uint32_t e_outs[8], e_nouts, e_ins_held[8], e_nins_held, e_ins_max;
static uint32_t e_frame_adler[64];
static int e_pad_bad, e_frame_key[64];
typedef struct { uint8_t *d; uint32_t n, flags; uint64_t pts; int frame; } piece_t;
static piece_t e_pieces[MAXPIECES];
static int e_npieces;
static uint32_t e_need_rx;
static uint8_t e_rx_data[1 << 16];
static uint32_t e_deferred[8][128], e_deferred_len[8];
static int e_ndeferred, e_bad_msgs, e_receives, e_in_msgs;

static const param_t *param(uint32_t id)
{
    const param_t *f = NULL;
    for (int i = 0; i < neparams; i++) if (eparams[i].id == id) f = &eparams[i];
    return f;
}

static void e_post_buffer(uint32_t port, uint32_t ctx, uint32_t length, uint32_t flags, uint64_t pts, const uint8_t *in_msg)
{
    uint32_t b[68];
    memset(b, 0, sizeof b);
    b[0] = MAGIC; b[1] = 0xE1C; b[2] = port; b[3] = ctx;
    b[13] = length; b[15] = flags;
    b[16] = (uint32_t)pts; b[17] = (uint32_t)(pts >> 32);
    b[18] = b[16]; b[19] = b[17];
    if (in_msg) { b[34] = 1; memcpy(&b[35], in_msg, length); }
    post(12, b, sizeof b, 0);
}

static void e_put(uint8_t *d, uint32_t *n, const uint8_t *s, uint32_t k) { memcpy(d + *n, s, k); *n += k; }

static uint32_t e_sps(uint8_t *d)
{
    const param_t *pr = param(P_PROFILE), *t = param(P_SPS_TIMING);
    uint8_t s[] = { 0, 0, 0, 1, 0x67, (uint8_t)(pr ? pr->v[0] : 0), 0, (uint8_t)(pr ? pr->v[1] : 0), (uint8_t)(t && t->v[0]),
                    0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80 };
    uint32_t n = 0;
    e_put(d, &n, s, sizeof s);
    return n;
}

static void e_piece(const uint8_t *d, uint32_t n, uint32_t flags, uint64_t pts, int frame)
{
    CHECK(e_npieces < MAXPIECES, "too many pieces waiting");
    if (e_npieces >= MAXPIECES) return;
    e_pieces[e_npieces].d = malloc(n ? n : 1);
    memcpy(e_pieces[e_npieces].d, d, n);
    e_pieces[e_npieces].n = n;
    e_pieces[e_npieces].flags = flags;
    e_pieces[e_npieces].pts = pts;
    e_pieces[e_npieces++].frame = frame;
}

/* a picture "encoded": its access unit, in pieces */
static void e_encode(const uint8_t *pic, uint32_t len, uint64_t pts, uint32_t ctx)
{
    static uint8_t au[120000];
    uint32_t n = 0, a = 0, sz, cs = e_in_stride / 2, cr = e_in_rows / 2;
    const param_t *gp = param(P_INTRAPERIOD), *il = param(P_INLINE);
    int k = e_frames++, key = (gp && gp->v[0] ? k % (int)gp->v[0] == 0 : k == 0) || e_force;
    const uint8_t *pl[3] = { pic, pic + e_in_stride * e_in_rows, pic + e_in_stride * e_in_rows + cs * cr };
    uint32_t st[3] = { e_in_stride, cs, cs }, rows[3] = { e_in_rows, cr, cr }, w[3] = { e_crop_w, e_crop_w / 2, e_crop_w / 2 },
             h[3] = { e_crop_h, e_crop_h / 2, e_crop_h / 2 };
    e_force = 0;
    CHECK(len == e_in_stride * e_in_rows * 3 / 2, "an input picture of %u bytes", len);
    for (int p = 0; p < 3; p++)              /* the visible picture; the padding the edges repeated */
        for (uint32_t y = 0; y < rows[p]; y++) {
            const uint8_t *r = pl[p] + y * st[p], *src = pl[p] + (y < h[p] ? y : h[p] - 1) * st[p];
            if (y < h[p]) a = adler0(a, r, w[p]);
            for (uint32_t x = 0; x < st[p]; x++)
                if (r[x] != src[x < w[p] ? x : w[p] - 1]) { e_pad_bad++; break; }
        }
    if (k < 64) { e_frame_adler[k] = a; e_frame_key[k] = key; }
    if (!e_config_out) {                     /* SPS and PPS first, on their own */
        uint8_t h0[64];
        uint32_t hn = e_sps(h0);
        e_piece(h0, hn, 0x20, ~0ull << 63, -1);
        e_config_out = 1;
    }
    if (key && il && il->v[0]) n = e_sps(au);
    {
        uint8_t sc[4] = { 0, 0, 0, 1 }, nal = key ? 0x65 : 0x41;
        e_put(au, &n, sc, 4); e_put(au, &n, &nal, 1);
        e_put(au, &n, (const uint8_t *)&k, 4); e_put(au, &n, (const uint8_t *)&a, 4);
    }
    sz = k == e_big_at ? 100000 : 20 + 37u * (uint32_t)k;   /* (the first few in the message) */
    if (sz > n) { memset(au + n, 0xA5 ^ k, sz - n); n = sz; }
    for (uint32_t o = 0; o < n; o += FRAG) {
        uint32_t m = n - o > FRAG ? FRAG : n - o;
        e_piece(au + o, m, (o + m == n ? 4 : 0) | (key && !o ? 8 : 0) | (o ? 0 : 2),
                (pts_late ? o + m == n : !o) ? pts : ~0ull << 63, k);
    }
    if (e_hold_in) e_ins_held[e_nins_held++] = ctx;
    else e_post_buffer(EP_IN, ctx, 0, 0, ~0ull << 63, NULL);
}

static void e_firmware(const uint32_t *m, uint32_t len);

/* the next piece out, if there's an output buffer */
static void e_produce(void)
{
    while (!e_need_rx && !e_quiet && e_nouts && (e_npieces || (e_eos_in && !e_eos_out && !e_pending_tx))) {
        uint32_t ctx = e_outs[0];
        memmove(e_outs, e_outs + 1, sizeof e_outs[0] * --e_nouts);
        if (e_error_at >= 0 && e_npieces && e_pieces[0].frame == e_error_at) {
            uint32_t ev[5 + 64 + 1];
            memset(ev, 0, sizeof ev);
            ev[1] = 3; ev[3] = 0x4F525245u;
            post(16, ev, sizeof ev, 0);
            e_error_at = -1;
            e_quiet = 1;
            return;
        }
        if (!e_npieces) {                    /* the end: an empty EOS, its 8 bytes by bulk */
            e_post_buffer(EP_OUT, ctx, 0, 1, ~0ull << 63, NULL);
            e_need_rx = 8; memset(e_rx_data, 0, 8);
            e_eos_out = 1;
        } else {
            piece_t p = e_pieces[0];
            memmove(e_pieces, e_pieces + 1, sizeof e_pieces[0] * (size_t)--e_npieces);
            if (e_pim && p.n <= 128) {
                e_post_buffer(EP_OUT, ctx, p.n, p.flags, p.pts, p.d);
            } else {
                e_post_buffer(EP_OUT, ctx, p.n, p.flags, p.pts, NULL);
                memcpy(e_rx_data, p.d, p.n);
                e_need_rx = p.n;
            }
            free(p.d);
            if (e_hold_in && (p.flags & 4) && e_nins_held) {   /* its input back now */
                e_post_buffer(EP_IN, e_ins_held[0], 0, 0, ~0ull << 63, NULL);
                memmove(e_ins_held, e_ins_held + 1, sizeof e_ins_held[0] * --e_nins_held);
            }
        }
    }
}

static void e_reply(const uint32_t *m, const void *payload, uint32_t len, uint32_t status) { post(m[1], payload, len, status); }

static void e_firmware(const uint32_t *m, uint32_t len)
{
    const uint32_t *p = m + 6;
    if (getenv("FAKEDBG")) fprintf(stderr, "enc msg %u\n", m[1]);
    CHECK(m[0] == MAGIC && len <= 512, "bad message");
    if (e_need_rx) {                         /* (sending: answers nothing until the receive) */
        CHECK(e_ndeferred < 8, "too many messages while sending");
        if (e_ndeferred < 8) { memcpy(e_deferred[e_ndeferred], m, len); e_deferred_len[e_ndeferred++] = len; }
        return;
    }
    switch (m[1]) {
    case 4: {
        uint32_t r[5] = { 0, 0xE1C, 3, 1, 1 };
        CHECK(!strcmp((const char *)&p[1], "ril.video_encode"), "create %s", (const char *)&p[1]);
        e_created++;
        e_reply(m, r, sizeof r, 0);
        break;
    }
    case 8: {                                /* PORT_INFO_GET */
        uint32_t r[6 + 16 + 8 + 11 + 32];
        memset(r, 0, sizeof r);
        CHECK(p[0] == 0xE1C && p[1] >= 1 && p[1] <= 3, "port info %u", p[1]);
        r[1] = 0xE1C; r[2] = p[1]; r[4] = 1; r[5] = EP_CTL + p[1] - 1;
        r[6 + 6] = 1; r[6 + 9] = 3;
        r[6 + 7] = p[1] == 3 ? e_out_min : 0; r[6 + 10] = r[6 + 7];
        e_reply(m, r, sizeof r, 0);
        break;
    }
    case 9: {                                /* PORT_INFO_SET */
        uint32_t r[6 + 16 + 8 + 11 + 32];
        const uint32_t *port = p + 3, *fmt = p + 3 + 16, *vid = p + 3 + 16 + 8;
        memset(r, 0, sizeof r);
        r[1] = 0xE1C; r[2] = p[1]; r[4] = 1; r[5] = EP_CTL + p[1] - 1;
        memcpy(&r[6], port, 64); memcpy(&r[6 + 16], fmt, 32); memcpy(&r[6 + 16 + 8], vid, 44);
        CHECK(!e_port_on[1] && !e_port_on[2], "a format set with a port on");
        if (p[1] == 2) {
            e_in_enc = fmt[1]; e_in_stride = vid[0]; e_in_rows = vid[1]; e_crop_w = vid[4]; e_crop_h = vid[5];
            e_in_size_set = port[12];
            CHECK(vid[0] % 32 == 0 && vid[1] % 16 == 0 && !vid[2] && !vid[3] && vid[4] <= vid[0] && vid[5] <= vid[1] &&
                  vid[0] - vid[4] < 32 && vid[1] - vid[5] < 16 && (int32_t)vid[6] > 0 && (int32_t)vid[7] > 0,
                  "input video %ux%u crop %u,%u %ux%u rate %d/%d", vid[0], vid[1], vid[2], vid[3], vid[4], vid[5], vid[6], vid[7]);
            CHECK(port[12] >= vid[0] * vid[1] * 3 / 2 && port[11] >= 1, "input buffers %u x %u", port[11], port[12]);
            r[6 + 7] = vid[0] * vid[1] * 3 / 2;   /* size min, now the format's known */
        } else if (p[1] == 3) {
            e_out_enc = fmt[1];
            e_out_size_set = port[12];
            CHECK(e_in_stride && vid[4] == e_crop_w && vid[5] == e_crop_h, "output video crop %ux%u", vid[4], vid[5]);
            r[6 + 7] = e_out_min;
        } else CHECK(0, "a format set on port type %u", p[1]);
        e_reply(m, r, sizeof r, 0);
        break;
    }
    case 14: {                               /* PORT_PARAMETER_SET */
        uint32_t st = 0;
        CHECK(p[0] == 0xE1C && p[1] >= EP_CTL && p[1] <= EP_OUT && p[3] >= 12 && p[3] <= 24 && (p[3] & 3) == 0,
              "parameter &%X on %u, size %u", p[2], p[1], p[3]);
        if (p[2] == e_refuse_id) st = 4;     /* ENOSYS */
        else if (p[2] == P_REQ_I) e_force = 1;
        else if (neparams < 64) {
            param_t *q = &eparams[neparams++];
            q->id = p[2]; q->port = p[1]; q->n = (p[3] - 8) / 4;
            memcpy(q->v, p + 4, 4 * q->n);
        }
        if (p[2] != P_MINFRAG) CHECK(p[1] == EP_OUT, "parameter &%X on port %u", p[2], p[1]);
        { uint32_t r[1] = { st }; e_reply(m, r, sizeof r, 0); }
        break;
    }
    case 6: e_enabled = 1; { uint32_t r = 0; e_reply(m, &r, 4, 0); } break;
    case 7: e_enabled = 0; { uint32_t r = 0; e_reply(m, &r, 4, 0); } break;
    case 5:
        CHECK(!e_port_on[1] && !e_port_on[2] && !e_enabled && !e_nouts && !e_nins_held,
              "destroyed with ports on or buffers held (%u out, %u in)", e_nouts, e_nins_held);
        e_destroyed++;
        { uint32_t r = 0; e_reply(m, &r, 4, 0); }
        break;
    case 10: {                               /* PORT_ACTION */
        uint32_t r = 0, port = p[1] - EP_CTL + 1;
        CHECK(port == 2 || port == 3, "port action on %u", p[1]);
        if (p[2] == 1) {
            e_port_on[port - 1] = 1;
        } else if (p[2] == 2) {              /* its buffers back before the reply */
            e_port_on[port - 1] = 0;
            if (port == 3) { while (e_nouts) e_post_buffer(EP_OUT, e_outs[--e_nouts], 0, 0, ~0ull << 63, NULL); }
            else { while (e_nins_held) e_post_buffer(EP_IN, e_ins_held[--e_nins_held], 0, 0, ~0ull << 63, NULL); }
        } else CHECK(0, "port action %u", p[2]);
        e_reply(m, &r, 4, 0);
        break;
    }
    case 11: {                               /* BUFFER_FROM_HOST */
        const drv_t *d = (const drv_t *)p;
        uint32_t length = p[13], flags = p[15], data = p[11], alloc = p[12];
        CHECK(d->magic == MAGIC && d->comp == 0xE1C && d->ctx >= 1, "drvbuf (context %u)", d->ctx);
        CHECK(!p[34], "payload in the message from the host");
        if (d->port == EP_IN) {
            CHECK(e_port_on[1], "input buffer with the port off");
            CHECK(!e_eos_in, "an input buffer after the EOS");
            CHECK(alloc >= e_in_size_set && length <= alloc && data, "input buffer %u of %u", length, alloc);
            e_in_msgs++;
            if (length) {
                CHECK(!e_pending_tx, "two input buffers before a transmit");
                CHECK(flags & 4, "an input picture without FRAME_END (&%X)", flags);
                e_pending_tx = length; e_pending_ctx = d->ctx; e_pending_flags = flags;
                e_pending_pts = (uint64_t)p[16] | (uint64_t)p[17] << 32;
            } else {
                CHECK(flags & 1, "an empty input buffer that isn't the EOS");
                e_eos_in = 1;
                e_post_buffer(EP_IN, d->ctx, 0, 0, ~0ull << 63, NULL);
            }
        } else {
            CHECK(d->port == EP_OUT && e_port_on[2], "output buffer for port %u", d->port);
            CHECK(alloc >= e_out_size_set && alloc >= e_out_min && !length, "output buffer %u bytes (of %u), %u in it",
                  alloc, e_out_size_set, length);
            CHECK(e_nouts < 8, "more output buffers than made");
            for (uint32_t i = 0; i < e_nouts; i++) CHECK(e_outs[i] != d->ctx, "output buffer %u given twice", d->ctx);
            if (e_nouts < 8) e_outs[e_nouts++] = d->ctx;
        }
        e_produce();
        break;
    }
    default:
        CHECK(0, "message type %u", m[1]);
        e_bad_msgs++;
    }
}

static int enc_swi(int n, _kernel_swi_regs *r, _kernel_oserror **er)
{
    uint32_t *R = (uint32_t *)r->r;
    (void)er;
    switch (n) {
    case 0x59205:
        CHECK(uses > releases, "queue while not in use");
        e_firmware((const uint32_t *)(uintptr_t)R[1], R[2]);
        return 1;
    case 0x59203: {                          /* BulkQueueTransmit: an input picture */
        uint8_t *pic = e_slow ? e_slow_pic : e_pic;
        CHECK(in_pci(R[1], R[2]), "transmit from memory that isn't contiguous (a pool or PCI_RAMAlloc'd)");
        CHECK(uses > releases && R[3] == 4 && (R[1] & 3) == 0, "transmit flags %u", R[3]);
        CHECK(e_pending_tx && R[2] == ((e_pending_tx + 3) & ~3u), "transmit %u (expected %u)", R[2], e_pending_tx);
        if (!e_pending_tx || R[2] > sizeof e_pic || !in_pci(R[1], R[2])) return 1;
        dma_read(pic, R[1], e_pending_tx);
        e_pending_tx = 0;
        if (e_slow) {                        /* (later: the picture's read now, as the Pi may) */
            CHECK(!e_slow_pending, "a second picture while encoding");
            e_slow_pending = 1; e_slow_tick = 0; e_slow_len = R[2]; e_slow_ctx = e_pending_ctx; e_slow_pts = e_pending_pts;
            return 1;
        }
        e_encode(pic, R[2], e_pending_pts, e_pending_ctx);
        e_produce();
        return 1;
    }
    case 0x59204:
        if (e_slow_pending && !e_need_rx && ++e_slow_tick >= e_slow) {
            e_slow_pending = 0;
            e_encode(e_slow_pic, e_slow_len, e_slow_pts, e_slow_ctx);
            e_produce();
        }
        return 0;
    case 0x5920F: {                          /* BulkQueueReceive */
        CHECK(e_need_rx && R[2] == ((e_need_rx + 3) & ~3u) && R[3] == 6, "receive %u (expected %u), flags %u", R[2],
              e_need_rx, R[3]);
        CHECK((R[1] & 3) == 0 && in_pci(R[1], R[2]), "receive into memory that isn't contiguous");
        if (!e_need_rx) return 1;
        e_receives++;
        if (e_rx_late && nlate < 4) {
            lateq[nlate].addr = R[1]; lateq[nlate].len = e_need_rx; lateq[nlate].kind = 1;
            memcpy(lateq[nlate].data, e_rx_data, e_need_rx);
            nlate++;
        } else {
            dma_write(R[1], e_rx_data, e_need_rx);
            ((stub_fn *)(uintptr_t)stub)(stub + 64, 4, R[4]);
        }
        e_need_rx = 0;
        while (!e_need_rx && e_ndeferred) {
            uint32_t mm[128], l = e_deferred_len[0];
            memcpy(mm, e_deferred[0], l);
            memmove(e_deferred[0], e_deferred[1], sizeof e_deferred[0] * (size_t)(e_ndeferred - 1));
            memmove(e_deferred_len, e_deferred_len + 1, sizeof e_deferred_len[0] * (size_t)(e_ndeferred - 1));
            e_ndeferred--;
            e_firmware(mm, l);
        }
        e_produce();
        return 1;
    }
    }
    return 0;
}

static void enc_reset(void)
{
    reset_fake();
    if (!pmp_live && !pci_live) npci_blocks = 0;   /* (the fake's blocks above 32 would be above 1 GB) */
    for (int i = 0; i < e_npieces; i++) free(e_pieces[i].d);
    neparams = 0;
    e_hold_in = e_quiet = 0; e_slow = e_slow_pending = 0; e_error_at = -1; e_pim = 1; e_rx_late = 0; e_refuse_id = 0; e_out_min = 65536; e_big_at = -1; pts_late = 0;
    e_created = e_destroyed = e_enabled = e_frames = e_eos_in = e_eos_out = e_force = e_config_out = 0;
    memset(e_port_on, 0, sizeof e_port_on);
    e_in_stride = e_in_rows = e_crop_w = e_crop_h = e_in_size_set = e_out_size_set = e_in_enc = e_out_enc = 0;
    e_pending_tx = 0; e_nouts = e_nins_held = e_ins_max = 0; e_pad_bad = 0; e_npieces = 0; e_need_rx = 0;
    e_ndeferred = e_bad_msgs = e_receives = e_in_msgs = 0;
    swi_hook = enc_swi;
}

/* everything closed and freed */
static void closed_ok(const char *what)
{
    CHECK(closes == opens && disconnects == connects && freed == (opens ? 1 : freed), "%s: service %d/%d, connection %d/%d, stub %d",
          what, closes, opens, disconnects, connects, freed);
    CHECK(uses == releases, "%s: use %d, release %d", what, uses, releases);
    CHECK(pmp_live == 0 && pci_live == 0, "%s: %d pools and %d PCI blocks left", what, pmp_live, pci_live);
    CHECK(e_destroyed == e_created, "%s: %d components made, %d destroyed", what, e_created, e_destroyed);
    CHECK(!e_port_on[1] && !e_port_on[2] && !e_enabled, "%s: left on", what);
}

/* ---- the pictures ---- */

#define MAXW 200
static uint8_t src_mem[3][MAXW * 2 * 160 + 64];
static const uint8_t *src[3];
static int src_stride[3];

/* picture k (w x h): a pattern that differs at every edge, each row strided past its width */
static void make_picture(int k, int w, int h)
{
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (int p = 0; p < 3; p++) {
        int pw = p ? cw : w, ph = p ? ch : h;
        uint8_t *b = src_mem[p] + 3;          /* (not word aligned) */
        src_stride[p] = pw + 13;
        memset(src_mem[p], 0xC3, sizeof src_mem[p]);
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) b[y * src_stride[p] + x] = (uint8_t)(x * 3 + y * 7 + k * 11 + p * 50 + (x * y & 15));
        src[p] = b;
    }
}

static uint32_t picture_adler(int w, int h)
{
    uint32_t a = 0;
    for (int p = 0; p < 3; p++) {
        int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
        for (int y = 0; y < ph; y++) a = adler0(a, src[p] + y * src_stride[p], (size_t)pw);
    }
    return a;
}

static void logcb(void *h, const char *t) { (void)h; if (getenv("ENCLOG")) printf("  log: %s\n", t); }

typedef struct {
    int frames, w, h, flags, in_buffers, force_at, gop, profile, level, qp_min, qp_max, expect_open;
    unsigned bitrate;
    const char *open_err_has;
    int stop_at_error;                        /* vcenc_send/receive fails part way (and says this) */
    const char *fail_has;
} run_t;

static int run(const char *name, run_t t)
{
    vcenc *e = NULL;
    vcenc_config c;
    vcenc_packet pk;
    uint32_t want[64];
    int sent = 0, got = 0, configs = 0, againsend = 0, r, keys = 0, eof = 0, failed = 0;
    int f0 = fails;
    vcenc_config_init(&c);
    c.width = t.w ? t.w : 70; c.height = t.h ? t.h : 38;
    c.fps_num = 25; c.fps_den = 1; c.bitrate = t.bitrate; c.flags = (unsigned)t.flags | VCENC_LOG_MESSAGES;
    c.in_buffers = t.in_buffers; c.gop = t.gop; c.profile = t.profile; c.level = t.level; c.qp_min = t.qp_min; c.qp_max = t.qp_max;
    c.log = logcb;
    r = vcenc_open(&e, &c);
    if (r != t.expect_open) {
        CHECK(0, "%s: open gave %d (%s), expected %d", name, r, vcenc_open_error(), t.expect_open);
        if (e) vcenc_close(e);
        return 0;
    }
    if (r != VCENC_OK) {
        CHECK(!e && (!t.open_err_has || strstr(vcenc_open_error(), t.open_err_has)), "%s: open error \"%s\"", name,
              vcenc_open_error());
        closed_ok(name);
        printf("%s: open refused (%s)\n", name, vcenc_open_error());
        return fails == f0;
    }
    {
        const uint8_t *hd; size_t hn;
        CHECK(vcenc_headers(e, &hd, &hn) == VCENC_AGAIN, "%s: headers before any came", name);
    }
    while (!eof && !failed) {
        if (sent < t.frames) {
            make_picture(sent, c.width, c.height);
            want[sent] = picture_adler(c.width, c.height);
            r = vcenc_send(e, src, src_stride, 1000 + 40 * sent, sent == t.force_at ? VCENC_FORCE_KEY : 0);
            if (r == VCENC_OK) sent++;
            else if (r == VCENC_AGAIN) againsend++;
            else { failed = r; break; }
        } else if (sent == t.frames) {
            r = vcenc_send_eos(e);
            if (r == VCENC_OK) sent++;
            else if (r != VCENC_AGAIN) { failed = r; break; }
        }
        while ((r = vcenc_receive(e, &pk)) == VCENC_OK) {
            if (pk.flags & VCENC_PKT_CONFIG) {
                const param_t *pr = param(P_PROFILE);
                configs++;
                CHECK(got == 0 && configs == 1, "%s: a CONFIG packet after %d pictures", name, got);
                CHECK(pk.size == 17 && pk.data[4] == 0x67 && pr && pk.data[5] == pr->v[0] && pk.data[7] == pr->v[1] &&
                      pk.data[8] == 1 && pk.data[13] == 0x68, "%s: the CONFIG packet (%u bytes)", name, (unsigned)pk.size);
                continue;
            }
            {
                const uint8_t *d = pk.data;
                size_t o = 0;
                int k, key, inl = 0;
                uint32_t a, n0;
                CHECK(configs == 1, "%s: a picture before the CONFIG packet", name);
                if (pk.size > 17 && d[4] == 0x67) { inl = 1; o = 17; }
                CHECK(pk.size >= o + 13 && !d[o] && !d[o + 1] && !d[o + 2] && d[o + 3] == 1, "%s: packet %d isn't Annex B", name, got);
                key = d[o + 4] == 0x65;
                memcpy(&k, d + o + 5, 4); memcpy(&a, d + o + 9, 4);
                n0 = got == e_big_at ? 100000 : 20 + 37u * (uint32_t)got;
                CHECK(k == got, "%s: packet %d holds picture %d", name, got, k);
                CHECK(got < 64 && a == want[got], "%s: picture %d's data (Adler %08X, sent %08X)", name, got, a,
                      got < 64 ? want[got] : 0);
                CHECK(pk.size == (n0 > o + 13 ? n0 : o + 13), "%s: packet %d is %u bytes", name, got, (unsigned)pk.size);
                CHECK(pk.pts == 1000 + 40 * got && pk.dts == pk.pts, "%s: packet %d pts %lld", name, got, (long long)pk.pts);
                CHECK(!!(pk.flags & VCENC_PKT_KEY) == key, "%s: packet %d's key flag", name, got);
                CHECK(inl == (key && (t.flags & VCENC_INLINE_HEADERS)), "%s: packet %d: headers %d", name, got, inl);
                if (key) keys++;
                if (t.gop) CHECK(key == (got % t.gop == 0 || got == t.force_at), "%s: packet %d key %d", name, got, key);
                else CHECK(key == (got == 0 || got == t.force_at), "%s: packet %d key %d", name, got, key);
                for (size_t i = o + 13; i < pk.size; i++)
                    if (d[i] != (uint8_t)(0xA5 ^ got)) { CHECK(0, "%s: packet %d byte %u", name, got, (unsigned)i); break; }
                got++;
            }
        }
        if (r == VCENC_EOF) eof = 1;
        else if (r != VCENC_AGAIN) failed = r;
    }
    if (failed) {
        CHECK(t.stop_at_error && failed == VCENC_ERROR && (!t.fail_has || strstr(vcenc_error(e), t.fail_has)),
              "%s: failed (%d) after %d pictures: %s", name, failed, got, vcenc_error(e));
    } else {
        vcenc_stats s;
        const uint8_t *hd; size_t hn;
        CHECK(!t.stop_at_error, "%s: didn't fail", name);
        CHECK(got == t.frames && configs == 1 && e_frames == t.frames, "%s: %d of %d packets (%d encoded)", name, got, t.frames,
              e_frames);
        CHECK(vcenc_headers(e, &hd, &hn) == VCENC_OK && hn == 17 && hd[4] == 0x67, "%s: vcenc_headers", name);
        CHECK(vcenc_receive(e, &pk) == VCENC_EOF, "%s: EOF again", name);
        CHECK(vcenc_send(e, src, src_stride, 0, 0) == VCENC_EINVAL, "%s: a picture after the EOS", name);
        vcenc_get_stats(e, &s);
        CHECK(s.pictures == (unsigned)t.frames && s.packets == (unsigned)t.frames + 1 && s.config_packets == 1 &&
              s.keyframes == (unsigned)keys, "%s: stats %u %u %u %u", name, s.pictures, s.packets, s.config_packets, s.keyframes);
        CHECK(!!(t.flags & VCENC_PCI) == !s.in_pools && !!(t.flags & VCENC_PCI) == !s.out_pools, "%s: pools %d %d", name,
              s.in_pools, s.out_pools);
        CHECK(!e_pad_bad, "%s: %d rows' padding not the edge repeated", name, e_pad_bad);
        CHECK(!(t.flags & VCENC_PCI) || !pmp_invalidates, "%s: cache maintenance on PCI memory", name);
        printf("%s: %d pictures, %d keyframes, %d receives, %d times input full\n", name, got, keys, e_receives, againsend);
    }
    vcenc_close(e);
    closed_ok(name);
    return fails == f0;
}

static void params_check(const char *name, uint32_t profile, uint32_t level, uint32_t bitrate, uint32_t rc, int gop, int qmin,
                         int qmax, int inl)
{
    const param_t *p;
    CHECK((p = param(P_PROFILE)) && p->n == 2 && p->v[0] == profile && p->v[1] == level, "%s: profile %u level &%X", name,
          p ? p->v[0] : 0, p ? p->v[1] : 0);
    CHECK((p = param(P_BITRATE)) && p->v[0] == bitrate, "%s: bit rate %u", name, p ? p->v[0] : 0);
    CHECK((p = param(P_RATECONTROL)) && p->v[0] == rc, "%s: rate control %u", name, p ? p->v[0] : 0);
    CHECK(gop ? (p = param(P_INTRAPERIOD)) && p->v[0] == (uint32_t)gop : !param(P_INTRAPERIOD), "%s: intra period", name);
    CHECK(qmin ? (p = param(P_MINQ)) && p->v[0] == (uint32_t)qmin : !param(P_MINQ), "%s: min quant", name);
    CHECK(qmax ? (p = param(P_MAXQ)) && p->v[0] == (uint32_t)qmax : !param(P_MAXQ), "%s: max quant", name);
    CHECK((p = param(P_INLINE)) && p->v[0] == (uint32_t)inl, "%s: inline header", name);
    CHECK((p = param(P_SPS_TIMING)) && p->v[0] == 1, "%s: SPS timing", name);
    CHECK((p = param(P_MINFRAG)) && p->port == EP_CTL && p->v[0] == 1, "%s: minimise fragmentation", name);
    CHECK(e_in_enc == 0x30323449u && e_out_enc == 0x34363248u, "%s: encodings %08X %08X", name, e_in_enc, e_out_enc);
}

int main(void)
{
    run_t t;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* a whole encode: pools, the first units in the message, the rest by bulk, one in two pieces */
    enc_reset(); e_big_at = 6;
    memset(&t, 0, sizeof t); t.frames = 12; t.gop = 5; t.bitrate = 2000000;
    run("pools", t);
    params_check("pools", 28, 0x1A, 2000000, 1, 5, 0, 0, 0);
    CHECK(e_in_stride == 96 && e_in_rows == 48 && e_crop_w == 70 && e_crop_h == 38, "input format %ux%u %ux%u", e_in_stride,
          e_in_rows, e_crop_w, e_crop_h);
    CHECK(pmp_invalidates > 0, "pools: no cache maintenance");

    /* PCI memory; receives finishing late; a keyframe asked for; headers before keyframes; main, level 4.1, CBR */
    enc_reset(); e_rx_late = 1; e_big_at = 3; pts_late = 1;
    memset(&t, 0, sizeof t); t.frames = 10; t.gop = 4; t.flags = VCENC_PCI | VCENC_INLINE_HEADERS | VCENC_CBR; t.force_at = 6;
    t.profile = VCENC_PROFILE_MAIN; t.level = 41; t.qp_min = 20; t.qp_max = 40;
    run("pci", t);
    params_check("pci", 26, 0x1D, 10000000, 2, 4, 20, 40, 1);

    /* a slow encoder that holds each input until its unit is out (input full), one input buffer; baseline */
    enc_reset(); e_hold_in = 1; e_pim = 0; e_rx_late = 1; e_slow = 5;
    memset(&t, 0, sizeof t); t.frames = 8; t.in_buffers = 1; t.profile = VCENC_PROFILE_BASELINE; t.w = 64; t.h = 32;
    run("held", t);
    params_check("held", 32, 0x1A, 10000000, 1, 0, 0, 0, 0);
    CHECK(e_in_stride == 64 && e_in_rows == 32, "held: input %ux%u", e_in_stride, e_in_rows);

    /* an encoder that wants bigger output buffers than 512 KB */
    enc_reset(); e_out_min = 700000;
    memset(&t, 0, sizeof t); t.frames = 3; t.w = 2; t.h = 2;
    run("bigger output", t);
    CHECK(e_out_size_set >= 700000, "bigger output: %u", e_out_size_set);

    /* a setting refused: carried on without it; a needed one: open fails */
    enc_reset(); e_refuse_id = P_RATECONTROL;
    memset(&t, 0, sizeof t); t.frames = 2;
    run("refused rate control", t);
    enc_reset(); e_refuse_id = P_PROFILE;
    memset(&t, 0, sizeof t); t.expect_open = VCENC_ERROR; t.open_err_has = "Profile and level: ENOSYS";
    run("refused profile", t);

    /* an encoder that goes quiet; one that reports an error part way */
    enc_reset(); e_quiet = 1;
    memset(&t, 0, sizeof t); t.frames = 4; t.stop_at_error = 1; t.fail_has = "Nothing from the encoder";
    run("quiet", t);
    enc_reset(); e_error_at = 2;
    memset(&t, 0, sizeof t); t.frames = 5; t.stop_at_error = 1; t.fail_has = "reports an error";
    run("error event", t);

    /* refusals before anything's made */
    enc_reset();
    memset(&t, 0, sizeof t); t.w = 71; t.expect_open = VCENC_EINVAL;
    run("odd width", t);
    memset(&t, 0, sizeof t); t.w = 1922; t.h = 1080; t.expect_open = VCENC_UNSUPPORTED;
    run("too wide", t);
    memset(&t, 0, sizeof t); t.w = 1920; t.h = 1080; t.expect_open = VCENC_UNSUPPORTED; t.open_err_has = "gpu_mem=128";
    run("gpu_mem", t);                       /* (the fake has 76 MB) */
    memset(&t, 0, sizeof t); t.level = 33; t.expect_open = VCENC_EINVAL;
    run("bad level", t);
    memset(&t, 0, sizeof t); t.in_buffers = 5; t.expect_open = VCENC_EINVAL;
    run("five buffers", t);

    /* 1080p (gpu_mem check off): sizes and level */
    enc_reset();
    memset(&t, 0, sizeof t); t.frames = 0; t.w = 1920; t.h = 1080; t.flags = VCENC_NO_GPU_MEM_CHECK;
    {
        vcenc *e; vcenc_config c;
        vcenc_config_init(&c);
        c.width = 1920; c.height = 1080; c.flags = VCENC_NO_GPU_MEM_CHECK; c.fps_num = 30; c.fps_den = 1;
        CHECK(vcenc_open(&e, &c) == VCENC_OK, "1080p open: %s", vcenc_open_error());
        CHECK(e_in_stride == 1920 && e_in_rows == 1088 && e_crop_h == 1080, "1080p input %ux%u", e_in_stride, e_in_rows);
        CHECK(param(P_PROFILE) && param(P_PROFILE)->v[1] == 0x1C, "1080p30 at 10 Mbit/s: level 4.0 (&%X)",
              param(P_PROFILE) ? param(P_PROFILE)->v[1] : 0);
        {
            const uint8_t *pl[3] = { NULL, NULL, NULL }; int st[3] = { 1920, 960, 960 };
            CHECK(vcenc_send(e, pl, st, 0, 0) == VCENC_EINVAL, "no planes");
        }
        CHECK(vcenc_send_eos(e) == VCENC_OK, "an EOS with nothing sent");
        { vcenc_packet pk; CHECK(vcenc_receive(e, &pk) == VCENC_EOF, "nothing sent: EOF"); }
        vcenc_close(e);
        closed_ok("1080p");
        printf("1080p: opened and closed\n");
    }
    /* short strides refused, the encoder carries on */
    enc_reset();
    {
        vcenc *e; vcenc_config c; int st[3];
        vcenc_config_init(&c);
        c.width = 70; c.height = 38;
        CHECK(vcenc_open(&e, &c) == VCENC_OK, "open: %s", vcenc_open_error());
        make_picture(0, 70, 38);
        st[0] = 69; st[1] = st[2] = 35;
        CHECK(vcenc_send(e, src, st, 0, 0) == VCENC_EINVAL && strstr(vcenc_error(e), "strides"), "short strides");
        CHECK(vcenc_send(e, src, src_stride, 0, 0) == VCENC_OK, "a picture after the refusal");
        vcenc_close(e);                      /* (closed with a picture in) */
        closed_ok("short strides");
        printf("short strides: refused; closed with a picture in\n");
    }

    /* VCEncTest (tools/vcenctest) on the fake: its checks, report and stream file */
    {
        static const char *runs[][20] = {
            { "vcenctest", "-s", "64x32", "-n", "20", "-g", "5", "-i", "-k", "7", "-w", "/tmp/vcenctest.h264", "-o",
              "/tmp/vcenctest.out", NULL },
            { "vcenctest", "-s", "128x72", "-n", "9", "-m", "pci", "-p", "baseline", "-c", "-t", "-B", "1", "-o",
              "/tmp/vcenctest.out", NULL },
        };
        remove("/tmp/vcenctest.out");
        for (int i = 0; i < 2; i++) {
            int argc = 0, rr;
            enc_reset();
            while (runs[i][argc]) argc++;
            rr = probe_main(argc, (char **)runs[i]);
            CHECK(rr == 0, "VCEncTest run %d: %d", i, rr);
            closed_ok("VCEncTest");
        }
        {
            FILE *f = fopen("/tmp/vcenctest.h264", "rb");
            static uint8_t b[65536];
            size_t n = f ? fread(b, 1, sizeof b, f) : 0, want = 17;
            int keys = 0;
            if (f) fclose(f);
            for (int k = 0; k < 20; k++) {    /* (the fake's units: 13 bytes or 20+37k, headers before keys) */
                int key = k % 5 == 0 || k == 7;
                size_t u = (key ? 17 : 0) + 13, s0 = 20 + 37u * (size_t)k;
                want += s0 > u ? s0 : u;
                keys += key;
            }
            CHECK(n == want && b[4] == 0x67 && b[17 + 4] == 0x67, "VCEncTest's stream: %u bytes (%u expected)", (unsigned)n,
                  (unsigned)want);
            printf("VCEncTest: the stream file is %u bytes, %d keyframes\n", (unsigned)n, keys);
        }
        {
            FILE *f = fopen("/tmp/vcenctest.out", "r");
            char line[256];
            int oks = 0;
            while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "Result: OK", 10)) oks++;
            if (f) fclose(f);
            CHECK(oks == 2, "VCEncTest's report: %d OK results", oks);
        }
    }

    swi_hook = NULL;
    printf("vcenc_test: %s (%d failures)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
