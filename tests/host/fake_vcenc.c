/*
 * A fake MMAL ril.video_encode on the fake VCHIQ module of
 * mmaldecode_test.c (included without its main), behaving as Linux's
 * bcm2835-codec driver expects the firmware to and as the Pi 4 did for
 * VCEncTest 0.1:
 *   - ports: control, input (I420: width a multiple of 32, height of 16,
 *     the picture's size as the crop), output (H.264); parameters recorded
 *     as they're set (some can be refused); constant bit rate (2) refused
 *     at the input's enable with EINVAL, as on the Pi;
 *   - each input picture comes by bulk transmit, read from "RAM" (a cached
 *     pool's lines that weren't cleaned aren't there); the padding must
 *     repeat the edges; the Adler-32 of the visible picture is taken;
 *   - output: as the Pi, one CONFIG buffer (an SPS and PPS that carry the
 *     profile and level set) as soon as the output has buffers, and again
 *     on its own before each later keyframe with INLINE_HEADER; per
 *     picture an access unit holding the picture's number and Adler-32,
 *     KEYFRAME on keyframes (every INTRAPERIOD, or REQUEST_I_FRAME); units
 *     of up to 128 bytes come in the message (payload_in_message), longer
 *     ones by bulk transfer, in pieces of up to 60000 bytes with FRAME_END
 *     on the last; an empty EOS buffer (by bulk, 8 bytes) after the
 *     input's EOS;
 *   - while sending by bulk it answers nothing else until the receive is
 *     queued (as the Pi).
 * Included by vcenc_test.c, and by fake_vcenc_lib.c for FFmpeg's tests.
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
static int e_refuse_rc4 = 1;                 /* rate control 4 refused at the input's enable too (2 always is, as the Pi 4) */
static int e_refuse_rc_running;              /* rate control refused once the ports are on */
static int e_broken;                         /* an enable was refused: every enable is now (as the Pi 4), until made again */
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
    if (key && k > 0 && il && il->v[0]) {   /* INLINE_HEADER: as the Pi, again before a later keyframe, on their own */
        uint8_t h0[64];
        uint32_t hn = e_sps(h0);
        e_piece(h0, hn, 0x24, pts, -1);
    }
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
        e_broken = 0;                        /* (a new component: as the Pi, it enables again, and sends its CONFIG) */
        e_config_out = 0;
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
        else if (p[2] == P_RATECONTROL && (e_port_on[1] || e_port_on[2]) && e_refuse_rc_running) st = 3;
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
            const param_t *rc = param(P_RATECONTROL);
            if (e_broken || (port == 2 && rc && (rc->v[0] == 2 || (rc->v[0] == 4 && e_refuse_rc4)))) {
                e_broken = 1;
                r = 3;                       /* EINVAL, and the port stays off */
                e_reply(m, &r, 4, 0);
                break;
            }
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
            if (!e_config_out) {             /* as the Pi: SPS and PPS as soon as there's a buffer (CONFIG, FRAME_END, pts 0) */
                uint8_t h0[64];
                uint32_t hn = e_sps(h0);
                CHECK(!e_npieces && !e_frames, "pictures before the first output buffer");
                e_piece(h0, hn, 0x24, 0, -1);
                e_config_out = 1;
            }
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
    e_hold_in = e_quiet = 0; e_slow = e_slow_pending = 0; e_error_at = -1; e_pim = 1; e_rx_late = 0; e_refuse_id = 0; e_refuse_rc4 = 1; e_refuse_rc_running = 0; e_broken = 0; e_out_min = 65536; e_big_at = -1; pts_late = 0;
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

