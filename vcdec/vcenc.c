/*
 * vcenc - H.264 encoded by the VideoCore, from RISC OS (see vcenc.h).
 *
 * MMAL over VCHIQ to the firmware's ril.video_encode, the same way vcdec
 * reaches its decoder (vcdec.c's rules 1 and 3 hold here too: buffers are
 * numbered from 1 for the VideoCore, and every bulk receive is queued the
 * moment its message arrives):
 *
 *   - the input port takes I420 in buffers of whole rows: width rounded up
 *     to 32, height to 16 (the edges copied out into the padding), crop
 *     the picture's size; one picture a buffer, by bulk transmit;
 *   - the output port gives H.264 (Annex B); a packet is the buffers up to
 *     FRAME_END, or one CONFIG buffer (SPS and PPS); data of up to 128
 *     bytes comes in the message itself (payload_in_message), the rest by
 *     bulk transfer;
 *   - the encoder's settings are port parameters (PORT_PARAMETER_SET):
 *     profile and level, bit rate and its control, the keyframe period,
 *     SPS/PPS before keyframes, timing in the SPS, as Linux's
 *     bcm2835-codec driver sets them;
 *   - buffers are in Physical Memory Pools as vcdec's (cacheable: cleaned
 *     before a transmit, cleaned and invalidated before a received packet
 *     is read), or PCI_RAMAlloc memory.
 *
 * Message layouts and parameter numbers (no code copied): Raspberry Pi
 * userland's MMAL headers (Broadcom, BSD-3-Clause) and Linux's vchiq-mmal
 * (GPL-2.0); the encoder's setup follows Linux's bcm2835-v4l2-codec.c.
 * The VCHIQ, message and memory code is vcmmal.c's, shared with vcdec
 * (0.1.5; until then this file had its own copy).
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcenc.h"
#include "vcmmal.h"

/* port parameters (MMAL's numbers: userland mmal_parameters*.h) */
#define P_PROFILE                 0x20002u     /* {profile, level} */
#define P_INTRAPERIOD             0x20003u
#define P_RATECONTROL             0x20004u     /* 1 variable, 2 constant */
#define P_MINIMISE_FRAGMENTATION  0x20006u
#define P_REQUEST_I_FRAME         0x2000Bu
#define P_BIT_RATE                0x2000Eu
#define P_MIN_QUANT               0x20010u
#define P_MAX_QUANT               0x20011u
#define P_INLINE_HEADER           0x2002Au
#define P_SPS_TIMING              0x20030u
#define PROFILE_BASELINE_C   32u               /* MMAL_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE */
#define PROFILE_MAIN         26u
#define PROFILE_HIGH         28u

#define MAX_IN         4
#define OUT_BUFS       3
#define OUT_MIN_SIZE   (512u * 1024u)          /* (a 1080p keyframe fits one buffer) */
#define MAX_W          1920
#define MAX_H          1088


/* an output buffer: with us and empty, with the encoder, or back with data */
enum { OB_FREE, OB_VC, OB_HELD };

typedef struct {                           /* an output buffer back with data (or the EOS), in order */
    int buf;
    uint32_t seq;                          /* its receive (0: the data came in the message) */
    uint32_t length, flags;
    uint64_t pts;
    uint8_t short_data[SHORT_DATA];
} held_t;

typedef struct {                           /* a buffer of either port */
    uint8_t *p;
    uint32_t cap, area;
    int pool;                              /* in a pool (else PCI memory: privileged, copied in SVC mode) */
} mem_t;

struct vcenc {
    vcm m;                                 /* the MMAL connection (vcmmal.h): err, failed, comp, ... */
    vcenc_config cfg;
    int in_on, out_on;
    port_info_t ctl_info, in_info, out_info;
    int use_pool;
    int nin;
    mem_t in[MAX_IN];
    int in_busy[MAX_IN];
    uint32_t in_size, stride_y, stride_c, rows_y, rows_c;
    mem_t out[OUT_BUFS];
    int out_state[OUT_BUFS];
    uint32_t out_size;
    held_t held[OUT_BUFS + 1];
    int nheld;
    uint8_t *pkt;                          /* the packet being put together, and the last one given out */
    size_t pkt_len, pkt_cap;
    uint64_t pkt_pts;
    unsigned pkt_flags;
    uint8_t *hdr;                          /* SPS and PPS */
    size_t hdr_len;
    uint8_t *row;                          /* (PCI memory: a row built here, then copied in) */
    int sent_any, eos_sent, eos_back, eos_seen, heard_output;
    uint32_t t_first_send;
    vcenc_stats stats;
};

static char open_err[200];

/* (0.1.4) every encoder not yet closed, for vcenc_close_all at exit */
#define MAX_LIVE 8
static vcenc *live[MAX_LIVE];
static int nlive, exit_set;
static void live_add(vcenc *e) { if (nlive < MAX_LIVE) live[nlive++] = e; }
static void live_remove(vcenc *e)
{
    for (int i = 0; i < nlive; i++)
        if (live[i] == e) { live[i] = live[--nlive]; return; }
}

/* ---- small things ---- */

static int einval(vcenc *e, const char *what)
{
    snprintf(e->m.err, sizeof e->m.err, "%s", what);
    return VCENC_EINVAL;
}

/* ---- memory: Physical Memory Pools, or PCI memory (vcmmal.c) ---- */

static void mem_free(vcenc *e, mem_t *m)
{
    if (!m->p) return;
    if (m->pool) vcm_pmp_free(&e->m, m->area, (m->cap + 4095) >> 12);
    else vcm_pci_free(&e->m, m->p);
    memset(m, 0, sizeof *m);
}

static int mem_alloc(vcenc *e, mem_t *m, uint32_t size, const char *what)
{
    mem_free(e, m);
    if (e->use_pool) {
        if ((m->p = vcm_pmp_alloc(&e->m, size, &m->area, VCM_POOL_RW)) != NULL) m->pool = 1;
        else {
            vcm_logf(&e->m, "No buffer pool (%s): PCI memory instead", e->m.why);
            e->use_pool = 0;
        }
    }
    if (!m->p && !(m->p = vcm_pci_alloc(&e->m, size)))
        return vcm_fail(&e->m, VCENC_ERROR, "No memory the VideoCore can reach for %s (%u bytes)", what, (unsigned)size);
    m->cap = size;
    return 0;
}

/* a cacheable pool's range cleaned (written out) and invalidated */
static void cache_cci(vcenc *e, const mem_t *m, uint32_t n)
{
    if (m->pool) vcm_cache_cci(&e->m, m->p, n);
}

/* ---- messages ---- */

static int buffer_to_vc(vcenc *e, port_info_t *pi, int idx, const mem_t *m, uint32_t len, uint32_t flags,
                        uint64_t pts)
{
    return vcm_buffer_to_vc(&e->m, pi, pi == &e->in_info ? "input" : "output", idx, m->p, m->cap, len, flags, pts, pts);
}

/* a message that isn't the reply being waited for */
static void handle_msg(vcenc *e, uint32_t got)
{
    hdr_t *h = (hdr_t *)e->m.msg;
    if (h->type == T_BUFFER_TO_HOST) {
        buffer_msg_t *b = (buffer_msg_t *)(h + 1);
        uint32_t k = b->drvbuf.client_context - CTX_BASE;
        if (b->drvbuf.magic != MMAL_MAGIC || b->drvbuf.client_context < CTX_BASE) {
            vcm_logf(&e->m, "A returned buffer we don't know");
            return;
        }
        if (e->cfg.flags & VCENC_LOG_MESSAGES)
            vcm_logf(&e->m, "rx%s: buffer back, status %u, port %u, buffer %u, length %u, flags &%X, pts %lld%s", e->m.waiting,
                  (unsigned)h->status, (unsigned)b->drvbuf.port_handle, (unsigned)k, (unsigned)b->length,
                  (unsigned)b->flags, (long long)b->pts, b->payload_in_message ? " (in the message)" : "");
        if (b->drvbuf.port_handle == e->in_info.port_handle && k < (uint32_t)e->nin) {
            e->in_busy[k] = 0;
            return;
        }
        if (b->drvbuf.port_handle != e->out_info.port_handle || k >= OUT_BUFS || e->out_state[k] != OB_VC) {
            vcm_logf(&e->m, "A buffer back from port %u (buffer %u) that isn't with the encoder", (unsigned)b->drvbuf.port_handle,
                  (unsigned)k);
            return;
        }
        if (h->status || (!b->length && !(b->flags & FLAG_EOS))) {   /* empty: back with us */
            if (h->status) vcm_logf(&e->m, "Output buffer back with status %s", vcm_status(h->status));
            e->out_state[k] = OB_FREE;
            return;
        }
        {
            held_t *p = &e->held[e->nheld];
            if (e->nheld >= OUT_BUFS + 1) { vcm_fail(&e->m, VCENC_ERROR, "More packets back than buffers"); return; }
            memset(p, 0, sizeof *p);
            p->buf = (int)k;
            p->length = b->length;
            p->flags = b->flags;
            p->pts = b->pts;
            if (b->length > e->out[k].cap) {
                vcm_fail(&e->m, VCENC_ERROR, "A %u byte packet for a %u byte buffer", (unsigned)b->length, (unsigned)e->out[k].cap);
                return;
            }
            if (b->length && b->payload_in_message) {   /* (short: in the message itself) */
                if (b->length > SHORT_DATA) { vcm_fail(&e->m, VCENC_ERROR, "A %u byte packet in a message", (unsigned)b->length); return; }
                memcpy(p->short_data, b->short_data, b->length);
                p->seq = 0;
            } else if (vcm_queue_receive(&e->m, e->out[k].p, b->length ? b->length : 8, &p->seq)) {
                return;                    /* (the bytes, or an empty EOS's 8, as Linux's driver: in order) */
            }
            e->out_state[k] = OB_HELD;
            e->nheld++;
            e->heard_output = 1;
            if (b->flags & FLAG_EOS) e->eos_seen = 1;
        }
        return;
    }
    if (h->type == T_EVENT_TO_HOST) {
        event_msg_t *ev = (event_msg_t *)(h + 1);
        if (e->cfg.flags & VCENC_LOG_MESSAGES)
            vcm_logf(&e->m, "rx%s: event &%08X, %u bytes, port type %u", e->m.waiting, (unsigned)ev->cmd, (unsigned)ev->length,
                  (unsigned)ev->port_type);
        if (!ev->cmd) return;
        if (vcm_event_data(&e->m, ev)) return;   /* (data that follows by bulk transfer) */
        if (ev->cmd == EV_ERROR) vcm_fail(&e->m, VCENC_ERROR, "The encoder reports an error (port type %u)", (unsigned)ev->port_type);
        else vcm_logf(&e->m, "Event &%08X (%u bytes) on port type %u", (unsigned)ev->cmd, (unsigned)ev->length, (unsigned)ev->port_type);
        return;
    }
    vcm_logf(&e->m, "(a type %u message, %u bytes)", (unsigned)h->type, (unsigned)got);
}

/* (for vcm_transact: what comes while a reply is waited for) */
static void on_msg(void *owner, uint32_t got) { handle_msg(owner, got); }

/* a port parameter set: 0; -1 if it failed (needed: then the encoder
   fails; otherwise only said in the log) */
static int param_set(vcenc *e, port_info_t *pi, uint32_t id, const uint32_t *v, int nv, int needed, const char *what)
{
    uint32_t req[4 + 4];
    int n;
    req[0] = e->m.comp; req[1] = pi->port_handle; req[2] = id; req[3] = 8 + 4 * (uint32_t)nv;
    memcpy(&req[4], v, 4 * (size_t)nv);
    n = vcm_transact(&e->m, T_PORT_PARAMETER_SET, req, (uint32_t)(16 + 4 * nv), what);
    if (n < 0) return -1;
    if (n < 4 || vcm_reply(&e->m)[0]) {
        if (needed) return vcm_fail(&e->m, VCENC_ERROR, "%s: %s", what, n >= 4 ? vcm_status(vcm_reply(&e->m)[0]) : "a short reply");
        vcm_logf(&e->m, "%s: %s (carrying on without it)", what, n >= 4 ? vcm_status(vcm_reply(&e->m)[0]) : "a short reply");
        return -1;
    }
    return 0;
}

static int bool_set(vcenc *e, port_info_t *pi, uint32_t id, uint32_t v, const char *what)
{
    return param_set(e, pi, id, &v, 1, 0, what);
}

/* ---- the output ---- */

static int give_outputs(vcenc *e)
{
    for (int i = 0; i < OUT_BUFS; i++)
        if (e->out_state[i] == OB_FREE) {
            if (buffer_to_vc(e, &e->out_info, i, &e->out[i], 0, 0, TIME_UNKNOWN)) return -1;
            e->out_state[i] = OB_VC;
        }
    return 0;
}

/* the head buffer done with: back to the encoder (unless the stream has ended) */
static void release_head(vcenc *e)
{
    held_t *p = &e->held[0];
    int k = p->buf, eos = p->flags & FLAG_EOS;
    memmove(e->held, e->held + 1, sizeof e->held[0] * (size_t)(e->nheld - 1));
    e->nheld--;
    e->out_state[k] = OB_FREE;
    if (eos) e->eos_back = 1;
    else if (e->out_on && !e->eos_seen) give_outputs(e);
}

/* ---- the component ---- */

/* H.264 levels: MMAL's numbers for 10, 11, ... 51 (times 10) */
static uint32_t mmal_level(int l)
{
    static const struct { int l; uint32_t m; } t[] = {
        { 10, 0x11 }, { 11, 0x13 }, { 12, 0x14 }, { 13, 0x15 }, { 20, 0x16 }, { 21, 0x17 }, { 22, 0x18 }, { 30, 0x19 },
        { 31, 0x1A }, { 32, 0x1B }, { 40, 0x1C }, { 41, 0x1D }, { 42, 0x1E }, { 50, 0x1F }, { 51, 0x20 } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].l == l) return t[i].m;
    return 0;
}

/* the level the size, rate and bit rate need (macroblocks a picture and a second, Annex A) */
static int level_for(const vcenc_config *c)
{
    long mbs = (long)((c->width + 15) / 16) * ((c->height + 15) / 16);
    long long mbps = (long long)mbs * c->fps_num / (c->fps_den ? c->fps_den : 1);
    if (mbs <= 3600 && mbps <= 108000 && c->bitrate <= 14000000u) return 31;
    if (mbs <= 5120 && mbps <= 216000 && c->bitrate <= 20000000u) return 32;
    if (mbs <= 8192 && mbps <= 245760) return c->bitrate <= 20000000u ? 40 : 41;
    return 42;
}

/* rc_before: the rate control set before the ports are enabled; with
   rc_after (constant asked for), 2 and then 4 are tried once they're
   running (the Pi 4 refused rate control 2 and 4 at the input's enable,
   and then every enable on that encoder: VCEncTest 0.1.1) */
static int create_component(vcenc *e, uint32_t rc_before, int rc_after)
{
    const vcenc_config *c = &e->cfg;
    uint32_t v[2];
    if (vcm_create(&e->m, "ril.video_encode")) return -1;
    if (vcm_port_get(&e->m, PORT_CONTROL, &e->ctl_info, "Control port") || vcm_port_get(&e->m, PORT_INPUT, &e->in_info, "Input port") ||
        vcm_port_get(&e->m, PORT_OUTPUT, &e->out_info, "Output port"))
        return -1;
    vcm_logf(&e->m, "Output port: %u buffers of %u bytes recommended (at least %u of %u)",
          (unsigned)e->out_info.port.buffer_num_recommended, (unsigned)e->out_info.port.buffer_size_recommended,
          (unsigned)e->out_info.port.buffer_num_min, (unsigned)e->out_info.port.buffer_size_min);
    bool_set(e, &e->ctl_info, P_MINIMISE_FRAGMENTATION, 1, "Minimise fragmentation");   /* (as Linux) */
    /* the input: I420, whole rows (32) and macroblock rows (16) */
    e->in_info.format.type = ES_VIDEO;
    e->in_info.format.encoding = ENC_I420;
    e->in_info.video.width = e->stride_y;
    e->in_info.video.height = e->rows_y;
    e->in_info.video.crop[0] = e->in_info.video.crop[1] = 0;
    e->in_info.video.crop[2] = c->width;
    e->in_info.video.crop[3] = c->height;
    e->in_info.video.frame_rate[0] = c->fps_num;
    e->in_info.video.frame_rate[1] = c->fps_den;
    e->in_info.port.buffer_num = (uint32_t)e->nin;
    e->in_info.port.buffer_size = e->in_size;
    if (vcm_port_set(&e->m, PORT_INPUT, &e->in_info, "Input format")) return -1;
    if (e->in_info.port.buffer_size_min > e->in_size)
        return vcm_fail(&e->m, VCENC_ERROR, "The encoder wants %u byte input buffers (%u made)",
                    (unsigned)e->in_info.port.buffer_size_min, (unsigned)e->in_size);
    /* the output: H.264 at the bit rate */
    e->out_info.format.type = ES_VIDEO;
    e->out_info.format.encoding = ENC_H264;
    e->out_info.format.bitrate = c->bitrate;
    e->out_info.video.width = e->stride_y;
    e->out_info.video.height = e->rows_y;
    e->out_info.video.crop[0] = e->out_info.video.crop[1] = 0;
    e->out_info.video.crop[2] = c->width;
    e->out_info.video.crop[3] = c->height;
    e->out_info.video.frame_rate[0] = c->fps_num;
    e->out_info.video.frame_rate[1] = c->fps_den;
    e->out_info.port.buffer_num = OUT_BUFS;
    e->out_info.port.buffer_size = e->out_size;
    if (vcm_port_set(&e->m, PORT_OUTPUT, &e->out_info, "Output format")) return -1;
    if (e->out_info.port.buffer_size_min > e->out_size) {   /* (larger buffers, then the port told again) */
        e->out_size = (e->out_info.port.buffer_size_min + 4095) & ~4095u;
        vcm_logf(&e->m, "The encoder wants %u byte output buffers", (unsigned)e->out_info.port.buffer_size_min);
        for (int i = 0; i < OUT_BUFS; i++) if (mem_alloc(e, &e->out[i], e->out_size, "the output")) return -1;
        e->out_info.port.buffer_size = e->out_size;
        if (vcm_port_set(&e->m, PORT_OUTPUT, &e->out_info, "Output format")) return -1;
    }
    /* the encoder's settings (as Linux's driver sets them, once the formats are) */
    v[0] = c->profile == VCENC_PROFILE_BASELINE ? PROFILE_BASELINE_C : c->profile == VCENC_PROFILE_MAIN ? PROFILE_MAIN
                                                                                                        : PROFILE_HIGH;
    v[1] = mmal_level(c->level ? c->level : level_for(c));
    if (param_set(e, &e->out_info, P_PROFILE, v, 2, 1, "Profile and level")) return -1;
    v[0] = c->bitrate;
    if (param_set(e, &e->out_info, P_BIT_RATE, v, 1, 1, "Bit rate")) return -1;
    v[0] = rc_before;
    e->stats.rate_control = param_set(e, &e->out_info, P_RATECONTROL, v, 1, 0, "Rate control") ? 0 : (int)v[0];
    if (c->gop > 0) { v[0] = (uint32_t)c->gop; param_set(e, &e->out_info, P_INTRAPERIOD, v, 1, 0, "Keyframe period"); }
    if (c->qp_min > 0) { v[0] = (uint32_t)c->qp_min; param_set(e, &e->out_info, P_MIN_QUANT, v, 1, 0, "Smallest quantiser"); }
    if (c->qp_max > 0) { v[0] = (uint32_t)c->qp_max; param_set(e, &e->out_info, P_MAX_QUANT, v, 1, 0, "Largest quantiser"); }
    bool_set(e, &e->out_info, P_INLINE_HEADER, (c->flags & VCENC_INLINE_HEADERS) != 0, "SPS and PPS before keyframes");
    bool_set(e, &e->out_info, P_SPS_TIMING, 1, "Timing in the SPS");
    if (e->m.failed) return -1;
    if (vcm_simple(&e->m, T_COMPONENT_ENABLE, "Enable")) return -1;
    e->m.enabled = 1;
    if (vcm_port_action(&e->m, &e->in_info, ACTION_ENABLE, "Input enable")) return -1;
    e->in_on = 1;
    if (vcm_port_action(&e->m, &e->out_info, ACTION_ENABLE, "Output enable")) return -1;
    e->out_on = 1;
    if (rc_after) {                        /* constant, set on the running encoder */
        static const uint32_t rcs[2] = { 2, 4 };
        for (int k = 0; k < 2; k++) {
            v[0] = rcs[k];
            if (!param_set(e, &e->out_info, P_RATECONTROL, v, 1, 0, "Rate control (running)")) {
                e->stats.rate_control = (int)rcs[k];
                e->stats.rate_control_running = 1;
                break;
            }
            if (e->m.failed) return -1;
        }
    }
    return give_outputs(e);
}

static void destroy_component(vcenc *e)
{
    vcm_destroy(&e->m, &e->in_info, &e->in_on, &e->out_info, &e->out_on, 1);   /* (every step tried: a component */
}                                                                               /* left on the VideoCore keeps its memory) */

/* ---- the API ---- */

void vcenc_config_init(vcenc_config *c)
{
    memset(c, 0, sizeof *c);
}

const char *vcenc_open_error(void) { return open_err; }
const char *vcenc_error(const vcenc *e) { return e ? e->m.err : open_err; }

void vcenc_get_stats(const vcenc *e, vcenc_stats *s)
{
    *s = e->stats;
    s->app_page_moves = e->m.app_page_moves;
    s->in_pools = s->out_pools = 0;
    for (int i = 0; i < e->nin; i++) s->in_pools += e->in[i].pool;
    for (int i = 0; i < OUT_BUFS; i++) s->out_pools += e->out[i].pool;
    s->in_size = e->in_size;
    s->out_size = e->out_size;
    s->out_buffers = OUT_BUFS;
}

int vcenc_open(vcenc **out, const vcenc_config *c)
{
    vcenc *e;
    unsigned mb;
    _kernel_oserror *er;
    int r;
    *out = NULL;
    open_err[0] = 0;
    if (c->width < 2 || c->height < 2 || (c->width & 1) || (c->height & 1)) {
        snprintf(open_err, sizeof open_err, "%dx%d: the size must be even, and at least 2x2", c->width, c->height);
        return VCENC_EINVAL;
    }
    if (c->width > MAX_W || c->height > MAX_H) {
        snprintf(open_err, sizeof open_err, "%dx%d is larger than the VideoCore encodes (%dx%d)", c->width, c->height,
                 MAX_W, MAX_H);
        return VCENC_UNSUPPORTED;
    }
    if (c->fps_num < 0 || c->fps_den < 0 || c->in_buffers < 0 || c->in_buffers > MAX_IN || c->gop < 0 ||
        c->qp_min < 0 || c->qp_max > 51 || c->qp_min > 51 || (c->qp_max && c->qp_min > c->qp_max) ||
        c->profile < 0 || c->profile > VCENC_PROFILE_BASELINE || (c->level && !mmal_level(c->level))) {
        snprintf(open_err, sizeof open_err, "A setting out of range (frame rate, buffers, keyframe period, "
                 "quantisers, profile or level)");
        return VCENC_EINVAL;
    }
    mb = vcm_gpu_mem();
    if (mb && mb < 128 && c->width * c->height > 1280 * 720 && !(c->flags & VCENC_NO_GPU_MEM_CHECK)) {
        snprintf(open_err, sizeof open_err, "The VideoCore has %u MB (gpu_mem): %dx%d needs gpu_mem=128 or more "
                 "in CONFIG/TXT", mb, c->width, c->height);
        return VCENC_UNSUPPORTED;
    }
    if (!exit_set) { atexit(vcenc_close_all); exit_set = 1; }
    if (!(e = calloc(1, sizeof *e))) {
        snprintf(open_err, sizeof open_err, "Out of memory");
        return VCENC_ERROR;
    }
    live_add(e);
    e->cfg = *c;
    e->m.name = "encoder";
    e->m.pool_name = "vcenc buffer";
    e->m.log = c->log;
    e->m.log_handle = c->log_handle;
    e->m.log_messages = (c->flags & VCENC_LOG_MESSAGES) != 0;
    e->m.on_msg = on_msg;
    e->m.owner = e;
    e->m.waiting = "";
    if (!e->cfg.fps_num || !e->cfg.fps_den) { e->cfg.fps_num = 25; e->cfg.fps_den = 1; }
    if (!e->cfg.bitrate) e->cfg.bitrate = 10000000u;
    e->nin = c->in_buffers ? c->in_buffers : 2;
    e->stride_y = ((uint32_t)c->width + 31) & ~31u;
    e->rows_y = ((uint32_t)c->height + 15) & ~15u;
    e->stride_c = e->stride_y / 2;
    e->rows_c = e->rows_y / 2;
    e->in_size = e->stride_y * e->rows_y * 3 / 2;
    e->out_size = OUT_MIN_SIZE;
    e->use_pool = !(c->flags & VCENC_PCI);
    vcm_logf(&e->m, "vcenc %s: %dx%d at %d/%d a second, %u bit/s; the VideoCore has %u MB (gpu_mem)", VCENC_VERSION, c->width,
          c->height, e->cfg.fps_num, e->cfg.fps_den, e->cfg.bitrate, mb);
    if (vcm_claim_stub(&e->m)) goto bad;
    if (e->use_pool && (er = vcm_find_cci(&e->m)) != NULL) {
        vcm_logf(&e->m, "No buffer pools (OS_MMUControl 2, RISC OS 5.23 or later: %s): PCI memory instead", er->errmess);
        e->use_pool = 0;
    }
    for (int i = 0; i < e->nin; i++) if (mem_alloc(e, &e->in[i], e->in_size, "an input picture")) goto bad;
    for (int i = 0; i < OUT_BUFS; i++) if (mem_alloc(e, &e->out[i], e->out_size, "the output")) goto bad;
    if (!(e->row = malloc(e->stride_y))) { vcm_fail(&e->m, VCENC_ERROR, "Out of memory"); goto bad; }
    if (vcm_connect(&e->m)) goto bad;
    /* constant bit rate: variable at the enables, then constant (2, else 4)
       on the running encoder. (The Pi 4's firmware refused 2 and 4 at the
       input's enable, after which it refused every enable on that
       encoder, and refused them while running too, VCEncTest 0.1.2: so
       vcenc 0.1.3 no longer makes three encoders to find that out.) */
    if (create_component(e, 1, (c->flags & VCENC_CBR) != 0)) goto bad;
    *out = e;
    return VCENC_OK;
bad:
    r = e->m.failed ? e->m.failed : VCENC_ERROR;
    snprintf(open_err, sizeof open_err, "%s", e->m.err);
    vcenc_close(e);
    return r;
}

void vcenc_close(vcenc *e)
{
    if (!e) return;
    live_remove(e);
    if (e->m.opened) destroy_component(e);
    /* (the receives queued before: finished before their memory goes) */
    {
        uint32_t last = 0;
        for (int i = 0; i < e->nheld; i++) if (e->held[i].seq) last = e->held[i].seq;
        if (last) vcm_wait_received(&e->m, last);
    }
    if (vcm_service_close(&e->m, "the callback and buffers are left")) {
        free(e);                           /* (the memory the VideoCore may still reach is kept) */
        return;
    }
    vcm_disconnect(&e->m);
    for (int i = 0; i < MAX_IN; i++) mem_free(e, &e->in[i]);
    for (int i = 0; i < OUT_BUFS; i++) mem_free(e, &e->out[i]);
    vcm_free_stub(&e->m);
    free(e->row);
    free(e->pkt);
    free(e->hdr);
    free(e);
}

/* (0.1.4) every encoder still open closed: run at exit (atexit, from the
   first vcenc_open), or by a program's own handler for the signals that
   end it (UnixLib doesn't run atexit's then) */
void vcenc_close_all(void)
{
    while (nlive) {
        vcenc *e = live[nlive - 1];
        e->m.log = NULL;                 /* (its handle may be gone) */
        vcenc_close(e);
    }
}

int vcenc_poll(vcenc *e)
{
    uint32_t got, now;
    if (e->m.failed) return e->m.failed;
    while ((got = vcm_dequeue(&e->m)) != 0) {
        handle_msg(e, got);
        if (e->m.failed) return e->m.failed;
    }
    if (e->out_on && !e->eos_seen && give_outputs(e)) return e->m.failed;
    now = vcm_now_cs();
    if (e->sent_any && !e->heard_output && now - e->t_first_send > STALL_CS)
        return vcm_fail(&e->m, VCENC_ERROR, "Nothing from the encoder in %d cs (the VideoCore has %u MB: 1080p needs "
                    "gpu_mem=128)", STALL_CS, vcm_gpu_mem());
    if (e->eos_sent && !e->eos_seen && now - e->m.last_heard > STALL_CS)
        return vcm_fail(&e->m, VCENC_ERROR, "Nothing from the encoder for %d cs after the EOS", STALL_CS);
    return VCENC_OK;
}

/* the picture into input buffer m: rows of stride_y (32), rows_y (16), the
   right and bottom edges repeated into the padding */
static void copy_in(vcenc *e, const mem_t *m, const uint8_t *const planes[3], const int strides[3])
{
    int w[3], h[3];
    uint32_t stride[3] = { e->stride_y, e->stride_c, e->stride_c }, rows[3] = { e->rows_y, e->rows_c, e->rows_c };
    uint8_t *dst = m->p;
    w[0] = e->cfg.width; h[0] = e->cfg.height;
    w[1] = w[2] = (e->cfg.width + 1) / 2;
    h[1] = h[2] = (e->cfg.height + 1) / 2;
    for (int k = 0; k < 3; k++) {
        for (uint32_t y = 0; y < rows[k]; y++) {
            const uint8_t *src = planes[k] + (size_t)((int)y < h[k] ? (int)y : h[k] - 1) * (size_t)strides[k];
            uint8_t *row = m->pool ? dst + (size_t)y * stride[k] : e->row;
            memcpy(row, src, (size_t)w[k]);
            memset(row + w[k], src[w[k] - 1], stride[k] - (uint32_t)w[k]);
            if (!m->pool) vcdec_svc_copy(dst + (size_t)y * stride[k], e->row, stride[k]);
        }
        dst += (size_t)stride[k] * rows[k];
    }
}

int vcenc_send(vcenc *e, const uint8_t *const planes[3], const int strides[3], int64_t pts, unsigned flags)
{
    int r, i;
    uint32_t t0;
    if (e->m.failed) return e->m.failed;
    if (e->eos_sent) return einval(e, "A picture after the EOS");
    if (!planes || !planes[0] || !planes[1] || !planes[2] || !strides || strides[0] < e->cfg.width ||
        strides[1] < (e->cfg.width + 1) / 2 || strides[2] < (e->cfg.width + 1) / 2)
        return einval(e, "Planes missing, or strides shorter than the picture's rows");
    if ((r = vcenc_poll(e)) != VCENC_OK) return r;
    for (i = 0; i < e->nin && e->in_busy[i]; i++) {}
    if (i == e->nin) return VCENC_AGAIN;
    if (flags & VCENC_FORCE_KEY) {
        uint32_t one = 1;
        param_set(e, &e->out_info, P_REQUEST_I_FRAME, &one, 1, 0, "A keyframe asked for");
        if (e->m.failed) return e->m.failed;
    }
    t0 = vcm_now_cs();
    copy_in(e, &e->in[i], planes, strides);
    cache_cci(e, &e->in[i], e->in_size);   /* (a cacheable pool: written out to memory first) */
    e->stats.cs_copy += vcm_now_cs() - t0;
    if (!e->sent_any) { e->sent_any = 1; e->t_first_send = vcm_now_cs(); }
    if (buffer_to_vc(e, &e->in_info, i, &e->in[i], e->in_size, FLAG_FRAME_END | (flags & VCENC_FORCE_KEY ? FLAG_KEYFRAME : 0),
                     (uint64_t)pts))
        return e->m.failed;
    e->in_busy[i] = 1;
    e->m.last_heard = vcm_now_cs();
    e->stats.pictures++;
    return VCENC_OK;
}

int vcenc_send_eos(vcenc *e)
{
    int r;
    if (e->m.failed) return e->m.failed;
    if (e->eos_sent) return VCENC_OK;
    if ((r = vcenc_poll(e)) != VCENC_OK) return r;
    if (!e->sent_any) {                    /* nothing sent: the end at once */
        e->eos_sent = e->eos_back = 1;
        return VCENC_OK;
    }
    for (int i = 0; i < e->nin; i++)
        if (!e->in_busy[i]) {
            if (buffer_to_vc(e, &e->in_info, i, &e->in[i], 0, FLAG_EOS, TIME_UNKNOWN)) return e->m.failed;
            e->in_busy[i] = 1;
            e->eos_sent = 1;
            e->m.last_heard = vcm_now_cs();
            return VCENC_OK;
        }
    return VCENC_AGAIN;
}

/* n more bytes on the packet being put together */
static int pkt_add(vcenc *e, const held_t *p)
{
    size_t need = e->pkt_len + p->length + 4;
    const mem_t *m = &e->out[p->buf];
    if (need > e->pkt_cap) {
        size_t cap = need * 2 > 65536 ? need * 2 : 65536;
        uint8_t *n = realloc(e->pkt, cap);
        if (!n) return vcm_fail(&e->m, VCENC_ERROR, "Out of memory for a %u byte packet", (unsigned)need);
        e->pkt = n;
        e->pkt_cap = cap;
    }
    if (!p->seq) memcpy(e->pkt + e->pkt_len, p->short_data, p->length);
    else if (m->pool) {
        cache_cci(e, m, p->length);        /* (written behind the cache: lines of before may be in it) */
        memcpy(e->pkt + e->pkt_len, m->p, p->length);
    } else {
        vcdec_svc_copy(e->pkt + e->pkt_len, m->p, (p->length + 3) & ~3u);
    }
    if (!e->pkt_len) { e->pkt_pts = TIME_UNKNOWN; e->pkt_flags = 0; }
    if (e->pkt_pts == TIME_UNKNOWN) e->pkt_pts = p->pts;   /* (the first piece's, or the first piece's that has one) */
    e->pkt_len += p->length;
    if (p->flags & FLAG_KEYFRAME) e->pkt_flags |= VCENC_PKT_KEY;
    if (p->flags & FLAG_CONFIG) e->pkt_flags |= VCENC_PKT_CONFIG;
    return 0;
}

int vcenc_receive(vcenc *e, vcenc_packet *pkt)
{
    int r;
    if ((r = vcenc_poll(e)) != VCENC_OK) return r;
    if (e->pkt_len && e->pkt_flags & 0x80000000u) e->pkt_len = 0;   /* (the last packet given out: gone now) */
    for (;;) {
        held_t *p;
        int end;
        if (!e->nheld) return e->eos_back && !e->pkt_len ? VCENC_EOF : VCENC_AGAIN;
        p = &e->held[0];
        if ((r = vcm_received(&e->m, p->seq)) < 0) return vcm_fail(&e->m, VCENC_ERROR, "A bulk receive was aborted");
        if (!r) return VCENC_AGAIN;
        if (p->length && pkt_add(e, p)) return e->m.failed;
        end = (p->flags & (FLAG_FRAME_END | FLAG_CONFIG | FLAG_EOS)) != 0;
        release_head(e);
        if (!end || !e->pkt_len) continue;
        if (e->pkt_flags & VCENC_PKT_CONFIG) {   /* SPS and PPS: kept for vcenc_headers */
            uint8_t *h = realloc(e->hdr, e->pkt_len);
            int again = e->hdr_len != 0;
            if (!h) return vcm_fail(&e->m, VCENC_ERROR, "Out of memory");
            memcpy(h, e->pkt, e->pkt_len);
            e->hdr = h;
            e->hdr_len = e->pkt_len;
            if (again) {                   /* (INLINE_HEADER: before each keyframe, on their own) */
                e->pkt_len = 0;
                continue;
            }
            e->stats.config_packets++;
        } else if ((e->cfg.flags & VCENC_INLINE_HEADERS) && (e->pkt_flags & VCENC_PKT_KEY) && e->hdr_len &&
                   !(e->pkt_len > 4 && !e->pkt[0] && !e->pkt[1] && ((e->pkt[2] == 1 && (e->pkt[3] & 31) == 7) ||
                                                                    (!e->pkt[2] && e->pkt[3] == 1 && (e->pkt[4] & 31) == 7)))) {
            if (e->pkt_len + e->hdr_len > e->pkt_cap) {   /* the SPS and PPS in front of the keyframe */
                uint8_t *n = realloc(e->pkt, e->pkt_len + e->hdr_len);
                if (!n) return vcm_fail(&e->m, VCENC_ERROR, "Out of memory");
                e->pkt = n;
                e->pkt_cap = e->pkt_len + e->hdr_len;
            }
            memmove(e->pkt + e->hdr_len, e->pkt, e->pkt_len);
            memcpy(e->pkt, e->hdr, e->hdr_len);
            e->pkt_len += e->hdr_len;
            e->stats.headers_repeated++;
        }
        pkt->data = e->pkt;
        pkt->size = e->pkt_len;
        pkt->pts = pkt->dts = e->pkt_pts == TIME_UNKNOWN ? VCENC_NOPTS : (int64_t)e->pkt_pts;
        pkt->flags = e->pkt_flags;
        e->stats.packets++;
        e->stats.bytes += e->pkt_len;
        if (e->pkt_flags & VCENC_PKT_KEY) e->stats.keyframes++;
        e->pkt_flags |= 0x80000000u;           /* (given out: cleared at the next call) */
        return VCENC_OK;
    }
}

int vcenc_headers(vcenc *e, const uint8_t **data, size_t *size)
{
    if (!e->hdr_len) return VCENC_AGAIN;
    *data = e->hdr;
    *size = e->hdr_len;
    return VCENC_OK;
}
