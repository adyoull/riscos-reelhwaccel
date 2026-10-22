/*
 * vcdec - H.264 decoded by the VideoCore, from RISC OS (see vcdec.h).
 *
 * MMAL over VCHIQ to the firmware's ril.video_decode, as MMALDecode 0.12 to
 * 0.17 found it works on a Pi 4:
 *
 *   1. buffers are numbered from 1 for the VideoCore (client_context; 0 is
 *      "no buffer" to it);
 *   2. (0.4.9) the output's buffers are handed over as soon as the decoder
 *      is created (before 0.4.9: only after its first format change, EFCH,
 *      or 200 cs after the first input, which VCDEC_OUT_AFTER_EFCH keeps);
 *      a cmd 0 "event" carries no data;
 *   3. every bulk receive is queued the moment its message arrives: the
 *      VideoCore answers nothing more until the data it's sending has a
 *      receive (it needn't have finished: the receives run on, counted by
 *      the RMA callback, unless VCDEC_SYNC_RECEIVE);
 *   4. 20 input buffers of 64 KB, PCI_RAMAlloc memory (physically
 *      contiguous, as VCHIQ's bulk transfers assume), one access unit a
 *      call: pts and dts on its first buffer with FRAME_START (and
 *      KEYFRAME), FRAME_END on its last, padded with zero bytes to whole
 *      words. The pts comes back with its picture, in display order;
 *   5. EOS on an empty buffer of its own, sent only at the real end;
 *   6. a seek is FLUSH of the input, then the output: the pictures already
 *      decoded come back during it and are dropped (0.4.11: with direct
 *      output, a new decoder instead: a picture could come after it).
 *      After an EOS a flush isn't enough (the next EOS loses the
 *      reorder-delay pictures), so the component is destroyed and created
 *      again;
 *   7. gpu_mem must be 128 or more for 1080p;
 *   8. pictures are copied out of PCI memory by LDM/STM (vcdec_copy.S);
 *   9. (0.4.5) direct output: the output buffers imported through the
 *      VideoCore's shared memory service and MMAL_PARAMETER_ZERO_COPY set
 *      on the output port, the decoder writes each picture straight into
 *      ours (each buffer's data field is its VideoCore handle) and says so
 *      (is_zero_copy): no bulk transfer, and no copy of it in gpu_mem
 *      (SMEMProbe 0.1). A picture that comes without it is received as
 *      before; without the service, an import or the parameter, the
 *      pictures come by bulk transfer as before;
 *  (0.4.9) Rule 2's old wait came from MMALDecode 0.5-0.9, whose lost
 *      first picture was later found to be buffer 0 (rule 1), not buffers
 *      given early. VCDecTest 0.3.8's Start on a Pi 4: with the buffers
 *      given at once every clip was right (a seek after the end and two
 *      1080p decoders too), the first picture 1-5 cs after the first
 *      access unit. Reel Edit's clips waited the whole 200 cs the old way
 *      (2 s at every clip start).
 *  10. (0.4.7) with direct output the empty EOS buffer can come back
 *      before the last picture (VCDecTest 0.3.6's Multi on a Pi 4: 3 and 4
 *      decoders at once, one picture short at the end): a picture after
 *      it is put before it, and the end waits up to EOS_WAIT_CS while
 *      fewer pictures have come than access units were sent since the
 *      last flush.
 *
 * The MMAL connection itself (messages, bulk transfers, the component and
 * its ports, memory the VideoCore reaches) is vcmmal.c's, shared with
 * vcenc (0.4.4; until then each had a copy).
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcdec.h"
#include "vcmmal.h"

#define IN_BUFS        20
#define IN_SIZE        (64 * 1024)
#define OUT_BUFS_DEFAULT 3                 /* (MMALDecode's; the decoder asks for 1) */
#define MAX_OUT        16
#define HOLD_SPARE     2                   /* buffers vcdec_receive_hold leaves to the decoder */
#define EFCH_WAIT_CS   200
#define EOS_WAIT_CS    20                  /* (rule 10) */
#define MAX_W          1920
#define MAX_H          1088
#define COPY_SVC 4                         /* (vcdec_copy_rows's mode: in SVC mode) */

/* the output buffers: with us and empty, with the decoder, holding a
   picture (or the EOS) that is still arriving or not yet taken, or a
   picture the caller holds (vcdec_receive_hold) */
enum { OB_FREE, OB_VC, OB_HELD, OB_USER };

struct vcdec_hold {                        /* (one per output buffer) */
    struct vcdec *d;
    int buf, live;
};

typedef struct {                           /* a picture (or the EOS) that has come back */
    int buf;
    uint32_t seq;                          /* its receive: done when the callback's count reaches it */
    uint32_t length, flags;
    uint64_t pts;
    uint32_t offsets[3], pitch[3];         /* where its planes are in the buffer */
    int width, height;                     /* visible */
} held_t;

struct vcdec {
    vcm m;                                 /* the MMAL connection (vcmmal.h): err, failed, comp, ... */
    vcdec_config cfg;
    int in_on, out_on;
    port_info_t in_info, out_info;
    uint8_t *in_buf[IN_BUFS];
    int in_busy[IN_BUFS];
    int nout;                              /* output buffers (cfg.out_buffers, or 3) */
    uint8_t *out_buf[MAX_OUT];
    uint32_t out_cap[MAX_OUT], out_need;
    int out_state[MAX_OUT];
    uint32_t out_area[MAX_OUT];            /* each pool buffer's dynamic area number */
    int out_pool[MAX_OUT];                 /* the buffer is in a pool (else PCI memory) */
    int use_pool;                          /* new buffers in pools (cleared on falling back to PCI memory) */
    int pool_only;                         /* VCDEC_OUT_PMP: no falling back */
    int pool_uncached;                     /* VCDEC_OUT_UNCACHED */
    int copy_way;                          /* vcdec_copy_rows's way: 0 LDM 4, 1 LDM 8, 2 NEON */
    held_t held[MAX_OUT + 1];
    held_t last_ok;                        /* the last picture copied out (its buffer, layout) */
    int nheld;
    int outputs_given, sent_any, eos_sent, eos_back;
    int eos_seen;                          /* the EOS buffer has come back (not yet taken) */
    int eos_vc;                            /* an EOS has reached the decoder (a flush must create it again) */
    int heard_output;                      /* a format change or a picture since the first input */
    uint32_t t_first_send;
    int reformat;                          /* an EFCH to another format, to act on at the top */
    format_changed_t fc;
    vcdec_stats stats;
    struct vcdec_hold holds[MAX_OUT];
    int nuser;                             /* buffers the caller holds */
    int closing;                           /* vcdec_close done but for the buffers still held */
    int parked;                            /* (0.4.3) at the end: the decoder destroyed, its memory given back */
    /* (0.4.5) direct output (rule 9) */
    uint32_t out_vch[MAX_OUT];             /* each buffer's VideoCore handle (imported), or 0 */
    int direct;                            /* wanted, and nothing has refused it yet */
    int direct_port;                       /* MMAL_PARAMETER_ZERO_COPY set on this component's output */
    /* (0.4.7) rule 10: since the last flush (or create) */
    unsigned seg_sent, seg_back;           /* access units sent, pictures back */
    uint32_t eos_seen_cs;                  /* when the EOS buffer came */
};

static char open_err[200];

/* (0.4.3) every decoder not yet freed, for vcdec_close_all at exit: on
   RISC OS nothing else gives a program's decoders back to the VideoCore */
#define MAX_LIVE 16
static vcdec *live[MAX_LIVE];
static int nlive, exit_set;
static void live_add(vcdec *d) { if (nlive < MAX_LIVE) live[nlive++] = d; }
static void live_remove(vcdec *d)
{
    for (int i = 0; i < nlive; i++)
        if (live[i] == d) { live[i] = live[--nlive]; return; }
}

/* ---- small things ---- */

/* a bad call: said, but nothing broken */
static int einval(vcdec *d, const char *what)
{
    snprintf(d->m.err, sizeof d->m.err, "%s", what);
    return VCDEC_EINVAL;
}

unsigned vcdec_gpu_mem(void) { return vcm_gpu_mem(); }

/* (0.4.5) direct output given up: every buffer's import freed, pictures by
   bulk transfer from here on (only before the output port has it set) */
static void direct_off(vcdec *d, const char *why)
{
    vcm_logf(&d->m, "No direct output (%s): the pictures come by bulk transfer", why);
    for (int i = 0; i < d->nout; i++) { vcm_smem_free(&d->m, d->out_vch[i]); d->out_vch[i] = 0; }
    d->direct = 0;
}

/* output buffer i made `size` bytes (any old one freed): 0, or -1 failed */
static void out_free(vcdec *d, int i)
{
    if (!d->out_buf[i]) return;
    vcm_smem_free(&d->m, d->out_vch[i]);   /* (the VideoCore's handle before the memory) */
    d->out_vch[i] = 0;
    if (d->out_pool[i]) vcm_pmp_free(&d->m, d->out_area[i], (d->out_cap[i] + 4095) >> 12);
    else vcm_pci_free(&d->m, d->out_buf[i]);
    d->out_buf[i] = NULL;
    d->out_cap[i] = 0;
    d->out_pool[i] = 0;
}

/* no pool to be had: PCI memory from now on (unless VCDEC_OUT_PMP) */
static int pool_fallback(vcdec *d)
{
    if (d->pool_only) return vcm_fail(&d->m, VCDEC_ERROR, "%s", d->m.why);
    vcm_logf(&d->m, "No picture pool (%s): PCI memory instead", d->m.why);
    d->use_pool = 0;
    return 0;
}

static int out_alloc(vcdec *d, int i, uint32_t size)
{
    out_free(d, i);
    if (d->use_pool) {
        if ((d->out_buf[i] = vcm_pmp_alloc(&d->m, size, &d->out_area[i], VCM_POOL_LOG | (d->pool_uncached ? VCM_POOL_UNCACHED : 0))) != NULL) d->out_pool[i] = 1;
        else if (pool_fallback(d)) return -1;
    }
    if (!d->out_buf[i] && !(d->out_buf[i] = vcm_pci_alloc(&d->m, size))) {
        vcm_fail(&d->m, VCDEC_ERROR, "No PCI memory (PCI_RAMAlloc) for %u byte pictures", (unsigned)size);
        return -1;
    }
    d->out_cap[i] = size;
    if (d->direct) {                       /* (0.4.5) imported, for the decoder to write into */
        if (d->out_pool[i]) vcm_cache_cci(&d->m, d->out_buf[i], size);   /* (no dirty lines to land on its pictures) */
        if (!(d->out_vch[i] = vcm_smem_import(&d->m, d->out_buf[i], size, d->m.pool_name))) {
            if (d->direct_port)            /* (the port expects handles now) */
                return vcm_fail(&d->m, VCDEC_ERROR, "No direct output for a new buffer (%s)", d->m.why);
            direct_off(d, d->m.why);
        }
    }
    return 0;
}

/* ---- messages ---- */

static int give_outputs(vcdec *d);
static void free_all(vcdec *d);

static int buffer_to_vc(vcdec *d, port_info_t *pi, int idx, uint8_t *data, uint32_t alloc, uint32_t len,
                        uint32_t flags, uint64_t pts, uint64_t dts)
{
    return vcm_buffer_to_vc(&d->m, pi, pi == &d->in_info ? "input" : "output", idx, data, alloc, len, flags, pts, dts);
}

/* One message that isn't the reply being waited for: buffers back, pictures
   (their receives queued at once: rule 3), events. Never waits for the
   decoder (a format change to another size is left for top_level). */
static void handle_msg(vcdec *d, uint32_t got)
{
    hdr_t *h = (hdr_t *)d->m.msg;
    if (h->type == T_BUFFER_TO_HOST) {
        buffer_msg_t *b = (buffer_msg_t *)(h + 1);
        uint32_t k = b->drvbuf.client_context - CTX_BASE;
        if (b->drvbuf.magic != MMAL_MAGIC || b->drvbuf.client_context < CTX_BASE) {
            vcm_logf(&d->m, "A returned buffer we don't know");
            return;
        }
        if (d->cfg.flags & VCDEC_LOG_MESSAGES)
            vcm_logf(&d->m, "rx%s: buffer back, status %u, port %u, buffer %u, length %u, flags &%X, pts %lld", d->m.waiting,
                  (unsigned)h->status, (unsigned)b->drvbuf.port_handle, (unsigned)k, (unsigned)b->length,
                  (unsigned)b->flags, (long long)b->pts);
        if (b->drvbuf.port_handle == d->in_info.port_handle && k < IN_BUFS) {
            d->in_busy[k] = 0;
            return;
        }
        if (b->drvbuf.port_handle != d->out_info.port_handle || k >= (uint32_t)d->nout) {
            vcm_logf(&d->m, "A buffer back from port %u", (unsigned)b->drvbuf.port_handle);
            return;
        }
        if (h->status || (!b->length && !(b->flags & FLAG_EOS))) {   /* empty: back with us */
            if (h->status) vcm_logf(&d->m, "Output buffer back with status %s", vcm_status(h->status));
            d->out_state[k] = OB_FREE;     /* (handed over again by vcdec_poll, unless it's stopping) */
            return;
        }
        {
            held_t *p = &d->held[d->nheld];
            uint32_t w = d->out_info.video.width, rows = d->out_info.video.height;
            if (d->nheld >= d->nout + 1) { vcm_fail(&d->m, VCDEC_ERROR, "More pictures back than buffers"); return; }
            memset(p, 0, sizeof *p);
            p->buf = (int)k;
            p->length = b->length;
            p->flags = b->flags;
            p->pts = b->pts;
            if (b->planes == 3 && b->pitch[0]) {
                for (int i = 0; i < 3; i++) { p->offsets[i] = b->offsets[i]; p->pitch[i] = b->pitch[i]; }
            } else {                       /* MMAL's I420: planes one after another */
                p->offsets[0] = 0; p->pitch[0] = w;
                p->offsets[1] = w * rows; p->pitch[1] = w / 2;
                p->offsets[2] = p->offsets[1] + (w / 2) * (rows / 2); p->pitch[2] = w / 2;
            }
            p->width = d->out_info.video.crop[2] ? d->out_info.video.crop[2] : (int)w;
            p->height = d->out_info.video.crop[3] ? d->out_info.video.crop[3] : (int)rows;
            if (b->length > d->out_cap[k] || (b->length && b->payload_in_message)) {
                vcm_fail(&d->m, VCDEC_ERROR, "A %u byte picture for a %u byte buffer", (unsigned)b->length, (unsigned)d->out_cap[k]);
                return;
            }
            if (b->is_zero_copy) {         /* (0.4.5) written straight into ours: nothing to receive */
                if (b->length) d->stats.direct++;
            } else {                       /* the picture's bytes, or an empty EOS's 8 (as Linux's */
                uint32_t t0 = vcm_now_cs();
                int bad = vcm_queue_receive(&d->m, d->out_buf[k], b->length ? b->length : 8, &p->seq);   /* driver) */
                d->stats.cs_queue += vcm_now_cs() - t0;
                if (bad) return;
            }
            d->out_state[k] = OB_HELD;
            d->nheld++;
            d->heard_output = 1;
            if (b->length) d->seg_back++;
            if (b->length && d->stats.first_cs < 0 && d->sent_any)
                d->stats.first_cs = (int)(vcm_now_cs() - d->t_first_send);
            if (d->eos_seen && b->length) {    /* (rule 10) after the empty EOS: before it */
                int j = 0;
                while (j < d->nheld - 1 && !((d->held[j].flags & FLAG_EOS) && !d->held[j].length)) j++;
                if (j < d->nheld - 1) {
                    held_t t = *p;
                    memmove(&d->held[j + 1], &d->held[j], sizeof d->held[0] * (size_t)(d->nheld - 1 - j));
                    d->held[j] = t;
                    if (!d->stats.after_eos++) vcm_logf(&d->m, "A picture after the EOS: taken before it");
                }
            }
            if ((b->flags & FLAG_EOS) && !d->eos_seen) d->eos_seen_cs = vcm_now_cs();
            if (b->flags & FLAG_EOS) d->eos_seen = 1;
            if (d->cfg.flags & VCDEC_SYNC_RECEIVE) vcm_wait_received(&d->m, p->seq);
        }
        return;
    }
    if (h->type == T_EVENT_TO_HOST) {
        event_msg_t *ev = (event_msg_t *)(h + 1);
        if (d->cfg.flags & VCDEC_LOG_MESSAGES)
            vcm_logf(&d->m, "rx%s: event &%08X, %u bytes, port type %u", d->m.waiting, (unsigned)ev->cmd, (unsigned)ev->length,
                  (unsigned)ev->port_type);
        if (!ev->cmd) return;              /* not an event: no data follows (rule 2) */
        if (vcm_event_data(&d->m, ev)) return;   /* (data that follows by bulk transfer) */
        if (ev->cmd == EV_FORMAT_CHANGED && ev->port_type == PORT_OUTPUT) {
            format_changed_t fc;
            memcpy(&fc, ev->data, sizeof fc);
            d->stats.format_changes++;
            d->heard_output = 1;
            if (d->stats.efch_cs < 0 && d->sent_any) {   /* (0.4.8) how long the first took */
                d->stats.efch_cs = (int)(vcm_now_cs() - d->t_first_send);
                d->stats.efch_aus = d->seg_sent;
                vcm_logf(&d->m, "The format change came %d cs after the first access unit (%u sent)%s", d->stats.efch_cs,
                         d->seg_sent, d->outputs_given ? ", the output buffers already handed over" : "");
            }
            vcm_logf(&d->m, "Format changed: %ux%u (crop %dx%d), buffers %u x %u bytes", (unsigned)fc.video.width,
                  (unsigned)fc.video.height, (int)fc.video.crop[2], (int)fc.video.crop[3],
                  (unsigned)fc.buffer_num_recommended, (unsigned)fc.buffer_size_recommended);
            if (fc.format.encoding == d->out_info.format.encoding && fc.video.width == d->out_info.video.width &&
                fc.video.height == d->out_info.video.height && fc.buffer_size_min <= d->out_need) {
                /* the format set already: only the visible size, and the buffers */
                d->out_info.video.crop[2] = fc.video.crop[2];
                d->out_info.video.crop[3] = fc.video.crop[3];
                d->outputs_given = 1;      /* (handed over by vcdec_poll) */
            } else {
                d->fc = fc;
                d->reformat = 1;
            }
        } else if (ev->cmd == EV_ERROR) {
            vcm_fail(&d->m, VCDEC_ERROR, "The decoder reports an error (port type %u)", (unsigned)ev->port_type);
        } else {
            vcm_logf(&d->m, "Event &%08X (%u bytes) on port type %u", (unsigned)ev->cmd, (unsigned)ev->length,
                  (unsigned)ev->port_type);
        }
        return;
    }
    vcm_logf(&d->m, "(a type %u message, %u bytes)", (unsigned)h->type, (unsigned)got);
}

/* (for vcm_transact: what comes while a reply is waited for) */
static void on_msg(void *owner, uint32_t got) { handle_msg(owner, got); }

/* ---- the output ---- */

static int give_outputs(vcdec *d)
{
    for (int i = 0; i < d->nout; i++)
        if (d->out_state[i] == OB_FREE) {
            if (d->out_cap[i] < d->out_need && out_alloc(d, i, d->out_need))   /* (a picture was in it */
                return d->m.failed;                                              /* when the size grew) */
            uint32_t t0 = vcm_now_cs();
            /* (direct output: the buffer's VideoCore handle, as Linux's driver) */
            if (buffer_to_vc(d, &d->out_info, i, d->direct_port ? (uint8_t *)(uintptr_t)d->out_vch[i] : d->out_buf[i],
                             d->out_cap[i], 0, 0, TIME_UNKNOWN, TIME_UNKNOWN))
                return -1;
            d->stats.cs_give += vcm_now_cs() - t0;
            d->out_state[i] = OB_VC;
        }
    return 0;
}

/* the output port's format set, and its buffers big enough (those holding a
   picture are grown when it's been taken) */
static int configure_output(vcdec *d)
{
    uint32_t need;
    if (vcm_port_set(&d->m, PORT_OUTPUT, &d->out_info, "Output format")) return -1;
    need = d->out_info.port.buffer_size;
    if (d->out_info.port.buffer_size_recommended > need) need = d->out_info.port.buffer_size_recommended;
    if (d->out_info.port.buffer_size_min > need) need = d->out_info.port.buffer_size_min;
    d->out_need = need;
    for (int i = 0; i < d->nout; i++)
        if (d->out_state[i] == OB_FREE && d->out_cap[i] < need && out_alloc(d, i, need))
            return d->m.failed;
    if (d->out_info.port.buffer_size != need) {
        d->out_info.port.buffer_size = need;
        d->out_info.port.buffer_num = d->nout;
        if (vcm_port_set(&d->m, PORT_OUTPUT, &d->out_info, "Output buffers")) return -1;
    }
    return 0;
}

/* an EFCH to another format: the output disabled (its buffers come back),
   the new format set, enabled, the buffers handed over again */
static int do_reformat(vcdec *d)
{
    format_changed_t *fc = &d->fc;
    d->reformat = 0;
    if (vcm_port_action(&d->m, &d->out_info, ACTION_DISABLE, "Output disable")) return -1;
    d->out_on = 0;
    for (int i = 0; i < d->nout; i++) if (d->out_state[i] == OB_VC) d->out_state[i] = OB_FREE;
    d->out_info.format = fc->format;
    d->out_info.video = fc->video;
    d->out_info.port.buffer_num = d->nout;
    d->out_info.port.buffer_size = fc->buffer_size_recommended > fc->buffer_size_min ?
                                   fc->buffer_size_recommended : fc->buffer_size_min;
    if (configure_output(d) || vcm_port_action(&d->m, &d->out_info, ACTION_ENABLE, "Output enable")) return -1;
    d->out_on = 1;
    d->outputs_given = 1;
    return give_outputs(d);
}

/* the picture at the head is done with: its buffer back to the decoder */
static void release_head(vcdec *d)
{
    held_t *p = &d->held[0];
    int k = p->buf;
    int eos = p->flags & FLAG_EOS;
    memmove(d->held, d->held + 1, sizeof d->held[0] * (size_t)(d->nheld - 1));
    d->nheld--;
    d->out_state[k] = OB_FREE;
    if (eos) d->eos_back = 1;
    else if (d->outputs_given && d->out_on && !d->eos_seen) give_outputs(d);
}

/* ---- the component ---- */

static int create_component(vcdec *d)
{
    if (vcm_create(&d->m, "ril.video_decode")) return -1;
    if (vcm_port_get(&d->m, PORT_INPUT, &d->in_info, "Input port") || vcm_port_get(&d->m, PORT_OUTPUT, &d->out_info, "Output port"))
        return -1;
    d->in_info.format.type = ES_VIDEO;
    d->in_info.format.encoding = ENC_H264;
    d->in_info.video.width = (uint32_t)d->cfg.width;
    d->in_info.video.height = (uint32_t)d->cfg.height;
    d->in_info.port.buffer_num = IN_BUFS;
    d->in_info.port.buffer_size = IN_SIZE;
    if (vcm_port_set(&d->m, PORT_INPUT, &d->in_info, "Input format")) return -1;
    d->out_info.format.type = ES_VIDEO;
    d->out_info.format.encoding = ENC_I420;
    d->out_info.video.width = ((uint32_t)d->cfg.width + 31) & ~31u;
    d->out_info.video.height = ((uint32_t)d->cfg.height + 15) & ~15u;
    d->out_info.video.crop[0] = d->out_info.video.crop[1] = 0;
    d->out_info.video.crop[2] = d->cfg.width;
    d->out_info.video.crop[3] = d->cfg.height;
    d->out_info.port.buffer_num = d->nout;
    d->out_info.port.buffer_size = d->out_info.video.width * d->out_info.video.height * 3 / 2;
    d->direct_port = 0;
    if (configure_output(d)) return -1;
    if (d->direct) {                       /* (0.4.5) rule 9 */
        int r = vcm_param_set(&d->m, &d->out_info, P_ZERO_COPY, 1, "Zero copy on the output");
        if (r < 0) return -1;
        if (r) direct_off(d, d->m.why);
        else d->direct_port = 1;
    }
    if (vcm_simple(&d->m, T_COMPONENT_ENABLE, "Enable")) return -1;
    d->m.enabled = 1;
    if (vcm_port_action(&d->m, &d->in_info, ACTION_ENABLE, "Input enable")) return -1;
    d->in_on = 1;
    if (vcm_port_action(&d->m, &d->out_info, ACTION_ENABLE, "Output enable")) return -1;
    d->out_on = 1;
    /* (the output's buffers: after the decoder's first format change) */
    d->outputs_given = d->sent_any = d->eos_sent = d->eos_back = d->eos_seen = d->heard_output = d->reformat = 0;
    d->seg_sent = d->seg_back = 0;
    d->eos_vc = 0;
    if (!(d->cfg.flags & VCDEC_OUT_AFTER_EFCH)) d->outputs_given = 1;   /* (rule 2: at once) */
    return 0;
}

/* ports off, the component off and gone (each step only if it was done).
   closing: every step is tried even if one before it failed (a component
   left on the VideoCore keeps its memory) */
static void destroy_component(vcdec *d, int closing)
{
    vcm_destroy(&d->m, &d->in_info, &d->in_on, &d->out_info, &d->out_on, closing);
}

/* ---- the API ---- */

void vcdec_config_init(vcdec_config *c)
{
    memset(c, 0, sizeof *c);
}

const char *vcdec_open_error(void) { return open_err; }
const char *vcdec_error(const vcdec *d) { return d ? d->m.err : open_err; }

void vcdec_get_stats(const vcdec *d, vcdec_stats *s)
{
    *s = d->stats;
    s->app_page_moves = d->m.app_page_moves;
    s->held_now = (unsigned)d->nuser;
    s->pool_buffers = s->pci_buffers = 0;
    s->direct_on = d->direct_port;
    for (int i = 0; i < d->nout; i++)
        if (d->out_buf[i]) {
            if (d->out_pool[i]) s->pool_buffers++;
            else s->pci_buffers++;
        }
}

int vcdec_open(vcdec **out, const vcdec_config *c)
{
    vcdec *d;
    unsigned mb;
    _kernel_oserror *e;
    int r = VCDEC_ERROR;
    *out = NULL;
    open_err[0] = 0;
    if (c->width <= 0 || c->height <= 0) {
        snprintf(open_err, sizeof open_err, "The stream's size is needed");
        return VCDEC_EINVAL;
    }
    if (c->width > MAX_W || c->height > MAX_H) {
        snprintf(open_err, sizeof open_err, "%dx%d is larger than the VideoCore decodes (%dx%d)", c->width, c->height,
                 MAX_W, MAX_H);
        return VCDEC_UNSUPPORTED;
    }
    mb = vcdec_gpu_mem();
    if (mb && mb < 128 && c->width * c->height > 1280 * 720 && !(c->flags & VCDEC_NO_GPU_MEM_CHECK)) {
        snprintf(open_err, sizeof open_err, "The VideoCore has %u MB (gpu_mem): %dx%d needs gpu_mem=128 or more "
                 "in CONFIG/TXT", mb, c->width, c->height);
        return VCDEC_UNSUPPORTED;
    }
    if (!exit_set) { atexit(vcdec_close_all); exit_set = 1; }
    if (!(d = calloc(1, sizeof *d))) {
        snprintf(open_err, sizeof open_err, "Out of memory");
        return VCDEC_ERROR;
    }
    live_add(d);
    d->cfg = *c;
    d->stats.efch_cs = d->stats.first_cs = -1;
    d->m.name = "decoder";
    d->m.pool_name = "vcdec picture";
    d->m.log = c->log;
    d->m.log_handle = c->log_handle;
    d->m.log_messages = (c->flags & VCDEC_LOG_MESSAGES) != 0;
    d->m.on_msg = on_msg;
    d->m.owner = d;
    d->m.waiting = "";
    d->nout = c->out_buffers > 0 ? (c->out_buffers < MAX_OUT ? c->out_buffers : MAX_OUT) : OUT_BUFS_DEFAULT;
    d->copy_way = c->flags & VCDEC_COPY_NEON ? 2 : c->flags & VCDEC_COPY_LDM8 ? 1 : c->flags & VCDEC_COPY_LDM4 ? 0 : 1;
    d->use_pool = !(c->flags & VCDEC_OUT_PCI);
    d->pool_only = d->use_pool && (c->flags & VCDEC_OUT_PMP);
    d->pool_uncached = (c->flags & VCDEC_OUT_UNCACHED) != 0;
    if (d->copy_way == 2 && !d->use_pool)
        vcm_logf(&d->m, "NEON is only used in USR mode: pictures from PCI memory are copied by LDM 8 instead");
    vcm_logf(&d->m, "vcdec %s: %dx%d; the VideoCore has %u MB (gpu_mem); pictures copied out by %s", VCDEC_VERSION, c->width,
          c->height, mb, d->copy_way == 2 ? "NEON" : d->copy_way ? "LDM 8" : "LDM 4");
    for (int i = 0; i < IN_BUFS; i++)
        if (!(d->in_buf[i] = vcm_pci_alloc(&d->m, IN_SIZE))) {
            vcm_fail(&d->m, VCDEC_ERROR, "No physically contiguous memory (PCI_RAMAlloc; is the PCI module loaded?)");
            goto bad;
        }
    if (vcm_claim_stub(&d->m)) goto bad;
    if (d->use_pool && !d->pool_uncached && (e = vcm_find_cci(&d->m)) != NULL) {
        vcm_why(&d->m, "OS_MMUControl 2 (Cache_CleanInvalidateRange; RISC OS 5.23 or later): %s", e->errmess);
        if (pool_fallback(d)) goto bad;
    }
    if (vcm_connect(&d->m)) goto bad;
    if (!(c->flags & VCDEC_NO_DIRECT)) {   /* (0.4.5) rule 9 */
        d->direct = 1;
        if (vcm_smem_open(&d->m)) direct_off(d, d->m.why);
    }
    if (create_component(d)) goto bad;
    if (d->direct_port) vcm_logf(&d->m, "Direct output: the decoder writes its pictures straight into vcdec's buffers");
    *out = d;
    return VCDEC_OK;
bad:
    r = d->m.failed ? d->m.failed : VCDEC_ERROR;
    snprintf(open_err, sizeof open_err, "%s", d->m.err);
    vcdec_close(d);
    return r;
}

void vcdec_close(vcdec *d)
{
    if (!d) return;
    destroy_component(d, 1);
    if (d->m.created && d->direct_port) {  /* (0.4.10) stuck: the buffers it holds are left to it */
        int kept = 0;
        for (int i = 0; i < d->nout; i++)
            if (d->out_state[i] == OB_VC && d->out_buf[i]) {
                if (d->out_pool[i]) d->m.pool_stuck = 1;   /* (so the pools' handler stays too) */
                d->out_buf[i] = NULL;       /* (never freed: the decoder may still write a picture into it) */
                d->out_vch[i] = 0;
                d->out_state[i] = OB_FREE;
                kept++;
            }
        if (kept) vcm_logf(&d->m, "%d picture buffers left to the decoder that stopped answering (it writes into them "
                           "itself): their memory stays taken", kept);
    }
    for (int i = 0; i < d->nout; i++) { vcm_smem_free(&d->m, d->out_vch[i]); d->out_vch[i] = 0; }   /* (held ones too: */
    vcm_smem_close(&d->m);                 /* the decoder is gone) */
    vcm_service_close(&d->m, "the callback is left in the RMA");
    vcm_disconnect(&d->m);
    for (int i = 0; i < IN_BUFS; i++) vcm_pci_free(&d->m, d->in_buf[i]);
    for (int i = 0; i < d->nout; i++)      /* (the pools before the RMA block: it holds their handler) */
        if (d->out_state[i] != OB_USER) out_free(d, i);
    if (d->nuser) {                        /* pictures still held: the rest at the last vcdec_release */
        vcm_logf(&d->m, "Closed with %d pictures still held: their buffers go when they're released", d->nuser);
        d->closing = 1;
        d->m.log = NULL;                 /* (its handle may be gone by then: an FFmpeg context) */
        return;
    }
    free_all(d);
}

/* the last of a closed decoder: the RMA block (the pools' handler, once
   they've gone) and the structure */
static void free_all(vcdec *d)
{
    vcm_free_stub(&d->m);
    live_remove(d);
    free(d);
}

/* (0.4.3) every decoder closed and its memory freed, pictures still held
   included (their holds go with it): run at exit (atexit, from the first
   vcdec_open), or by a program's own handler for the signals that end it
   (UnixLib doesn't run atexit's then) */
void vcdec_close_all(void)
{
    while (nlive) {
        vcdec *d = live[nlive - 1];
        d->m.log = NULL;                 /* (its handle may be gone) */
        if (!d->closing) vcdec_close(d);
        if (nlive && live[nlive - 1] == d) {   /* (closed, but with pictures held) */
            for (int i = 0; i < d->nout; i++) out_free(d, i);
            d->nuser = 0;
            free_all(d);
        }
    }
}

/* takes in whatever the decoder has sent; acts on a format change; hands
   the output buffers over if the decoder hasn't said its format in time;
   notices a decoder that has stopped */
int vcdec_poll(vcdec *d)
{
    uint32_t got, now;
    if (d->m.failed) return d->m.failed;
    for (;;) {
        /* (a format change can also have come while waiting for a reply) */
        if (d->reformat && do_reformat(d)) return d->m.failed ? d->m.failed : VCDEC_ERROR;
        if (!(got = vcm_dequeue(&d->m))) break;
        now = vcm_now_cs();
        handle_msg(d, got);
        d->stats.cs_messages += vcm_now_cs() - now;
        if (d->m.failed) return d->m.failed;
    }
    now = vcm_now_cs();
    if (d->sent_any && !d->outputs_given && now - d->t_first_send > EFCH_WAIT_CS) {
        vcm_logf(&d->m, "No format change from the decoder in %d cs (%u access units sent): the output buffers handed over "
                 "anyway", EFCH_WAIT_CS, d->seg_sent);
        d->stats.efch_waits++;
        d->outputs_given = 1;
    }
    /* empty buffers (back unused, or after a format change) to the decoder */
    if (d->outputs_given && d->out_on && !d->eos_seen && give_outputs(d)) return d->m.failed;
    /* A decoder that has stopped: nothing at all since the first input (as
       at gpu_mem=64 with 1080p), or nothing after the EOS with a buffer to
       fill. (Not part way: a decoder can keep input while it waits for
       more, and a paused player sends none.) */
    if (d->sent_any && !d->heard_output && now - d->t_first_send > STALL_CS) {
        unsigned mb = vcdec_gpu_mem();
        if (mb && mb < 128)
            return vcm_fail(&d->m, VCDEC_ERROR, "Nothing from the decoder in %d cs (the VideoCore has %u MB: 1080p needs "
                        "gpu_mem=128)", STALL_CS, mb);
        /* (0.4.3) one 1080p decoder fills gpu_mem=128: another open one waits for its memory */
        return vcm_fail(&d->m, VCDEC_ERROR, "Nothing from the decoder in %d cs (the VideoCore has no memory left for it: "
                    "another decoder open, or one left by a program that stopped without closing)", STALL_CS);
    }
    if (d->eos_vc && !d->eos_seen && now - d->m.last_heard > STALL_CS) {
        int outs = 0;
        for (int i = 0; i < d->nout; i++) outs += d->out_state[i] == OB_VC;
        if (outs) return vcm_fail(&d->m, VCDEC_ERROR, "Nothing from the decoder for %d cs after the EOS", STALL_CS);
    }
    return VCDEC_OK;
}

/* the profile in an SPS (NAL type 7): Baseline 66, Main 77, High 100 */
int vcdec_check_stream(const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i + 4 < len; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1) continue;
        if ((p[i + 3] & 31) == 7) {
            uint8_t profile = p[i + 4];
            if (profile != 66 && profile != 77 && profile != 100) return VCDEC_UNSUPPORTED;
        }
        i += 2;
    }
    return VCDEC_OK;
}

int vcdec_send(vcdec *d, const void *data, size_t len, int64_t pts, int64_t dts, unsigned flags)
{
    static const uint32_t zero = 0;
    const uint8_t *src = data;
    uint32_t padded, pieces, n, off = 0;
    int r, free_n = 0;
    if (d->m.failed) return d->m.failed;
    if (!len) return einval(d, "An empty access unit");
    if (d->eos_sent) return einval(d, "Sent after the EOS (flush first)");
    padded = ((uint32_t)len + 3) & ~3u;
    pieces = (padded + IN_SIZE - 1) / IN_SIZE;
    if (len > (size_t)IN_BUFS * IN_SIZE) return einval(d, "An access unit larger than the decoder's input");
    if (vcdec_check_stream(data, len) == VCDEC_UNSUPPORTED)
        return vcm_fail(&d->m, VCDEC_UNSUPPORTED, "The stream's profile isn't one the VideoCore decodes (Baseline, Main, High)");
    if ((r = vcdec_poll(d)) != VCDEC_OK) return r;
    for (int i = 0; i < IN_BUFS; i++) free_n += !d->in_busy[i];
    if ((uint32_t)free_n < pieces) return VCDEC_AGAIN;
    if (!d->sent_any) { d->sent_any = 1; d->t_first_send = vcm_now_cs(); }
    for (int i = 0; i < IN_BUFS && off < padded; i++) {
        uint32_t fl = 0;
        uint64_t p = TIME_UNKNOWN, q = TIME_UNKNOWN;
        if (d->in_busy[i]) continue;
        n = padded - off < IN_SIZE ? padded - off : IN_SIZE;
        if (off + n > len) {               /* the last piece: its last word zeros first, then the data */
            vcdec_svc_copy(d->in_buf[i] + ((len - off) & ~3u), &zero, 4);
            vcdec_svc_copy(d->in_buf[i], src + off, len - off);
        } else {
            vcdec_svc_copy(d->in_buf[i], src + off, n);
        }
        if (!off) {
            fl = FLAG_FRAME_START | (flags & VCDEC_KEYFRAME ? FLAG_KEYFRAME : 0) |
                 (flags & VCDEC_DISCONTINUITY ? FLAG_DISCONTINUITY : 0);
            p = (uint64_t)pts;
            q = (uint64_t)dts;
        }
        if (off + n == padded) fl |= FLAG_FRAME_END;
        if (buffer_to_vc(d, &d->in_info, i, d->in_buf[i], IN_SIZE, n, fl, p, q)) return d->m.failed;
        d->in_busy[i] = 1;
        d->m.last_heard = vcm_now_cs();          /* (the decoder has work: the stall clock starts) */
        off += n;
    }
    d->stats.sent++;
    d->seg_sent++;
    return VCDEC_OK;
}

int vcdec_send_eos(vcdec *d)
{
    int r;
    if (d->m.failed) return d->m.failed;
    if (d->eos_sent) return VCDEC_OK;
    if ((r = vcdec_poll(d)) != VCDEC_OK) return r;
    if (!d->sent_any) {                    /* nothing sent: the end at once, the decoder not told */
        d->eos_sent = d->eos_back = 1;
        return VCDEC_OK;
    }
    for (int i = 0; i < IN_BUFS; i++)
        if (!d->in_busy[i]) {
            if (!d->sent_any) { d->sent_any = 1; d->t_first_send = vcm_now_cs(); }
            if (buffer_to_vc(d, &d->in_info, i, d->in_buf[i], IN_SIZE, 0, FLAG_EOS, TIME_UNKNOWN, TIME_UNKNOWN))
                return d->m.failed;
            d->in_busy[i] = 1;
            d->eos_sent = d->eos_vc = 1;
            d->m.last_heard = vcm_now_cs();
            return VCDEC_OK;
        }
    return VCDEC_AGAIN;
}

/* (0.4.3) the stream's end has been taken: the decoder is destroyed, so
   the VideoCore has its memory back (one 1080p decoder fills gpu_mem=128,
   and a decoder left open at its end kept the next waiting: VCDecTest
   0.3.3's Multi); a flush creates it again, as after any EOS */
static int park(vcdec *d)
{
    destroy_component(d, 0);
    if (d->m.failed) return d->m.failed;
    for (int i = 0; i < IN_BUFS; i++) d->in_busy[i] = 0;
    for (int i = 0; i < d->nout; i++) if (d->out_state[i] != OB_USER) d->out_state[i] = OB_FREE;
    d->parked = 1;
    d->stats.freed_at_end++;
    vcm_logf(&d->m, "At the end: the decoder destroyed, its VideoCore memory given back (a flush creates it again)");
    return 0;
}

/* the head picture, if its receive is done: 1; 0 if none yet; VCDEC_EOF;
   or the failure */
static int head_ready(vcdec *d)
{
    int r;
    for (;;) {
        if (d->m.failed) return d->m.failed;
        if (!d->nheld) {
            if (!d->eos_back) return 0;
            if (d->m.created && d->eos_vc && park(d)) return d->m.failed;   /* (an EOS alone: nothing taken yet) */
            return VCDEC_EOF;
        }
        if ((r = vcm_received(&d->m, d->held[0].seq)) < 0) return vcm_fail(&d->m, VCDEC_ERROR, "A bulk receive was aborted");
        if (!r) return 0;
        if (!(d->held[0].flags & FLAG_EOS) || d->held[0].length) return 1;
        /* (rule 10) an empty EOS: the end, unless pictures may still come */
        if (d->direct_port && d->seg_back < d->seg_sent && vcm_now_cs() - d->eos_seen_cs < EOS_WAIT_CS) return 0;
        release_head(d);
    }
}

static void fill_info(const held_t *p, vcdec_picture *pic)
{
    pic->width = p->width;
    pic->height = p->height;
    pic->pts = (int64_t)p->pts;
    pic->flags = (p->flags & FLAG_KEYFRAME ? VCDEC_PIC_KEYFRAME : 0) |
                 (p->flags & FLAG_DISCONTINUITY ? VCDEC_PIC_DISCONTINUITY : 0);
}

int vcdec_peek(vcdec *d, vcdec_picture *pic)
{
    int r;
    if ((r = vcdec_poll(d)) != VCDEC_OK) return r;
    if ((r = head_ready(d)) != 1) return r ? r : VCDEC_AGAIN;
    fill_info(&d->held[0], pic);
    return VCDEC_OK;
}

/* A picture out of its buffer into the caller's planes. A cacheable pool
   is cleaned and invalidated over the picture first: the VideoCore wrote
   it to memory behind the cache, which may hold lines of the last one
   (read from here, or fetched ahead). */
static void picture_cache(vcdec *d, const held_t *p)
{
    const uint8_t *b = d->out_buf[p->buf];
    uint32_t t0 = vcm_now_cs();
    if (d->out_pool[p->buf]) vcm_cache_cci(&d->m, b, p->length);
    d->stats.cs_cache += vcm_now_cs() - t0;
}

static void copy_picture(vcdec *d, const held_t *p, uint8_t *const planes[3], const int strides[3], int way)
{
    const uint8_t *b = d->out_buf[p->buf];
    int cw = (p->width + 1) / 2, ch = (p->height + 1) / 2;
    int pool = d->out_pool[p->buf];
    int svc = !pool || d->m.pmp_svc;
    uint32_t t1;
    /* (NEON only in USR mode: in SVC mode an IRQ could leave the VFP context
       lazily switched off, and a trap there isn't VFPSupport's to mend) */
    int mode = (svc && way == 2 ? 1 : way) | (svc ? COPY_SVC : 0);
    picture_cache(d, p);
    t1 = vcm_now_cs();
    vcdec_copy_rows(planes[0], strides[0], b + p->offsets[0], (int)p->pitch[0], p->width, p->height, mode);
    vcdec_copy_rows(planes[1], strides[1], b + p->offsets[1], (int)p->pitch[1], cw, ch, mode);
    vcdec_copy_rows(planes[2], strides[2], b + p->offsets[2], (int)p->pitch[2], cw, ch, mode);
    d->stats.cs_copy += vcm_now_cs() - t1;
}

int vcdec_copy_benchmark(vcdec *d, unsigned way, int reps, uint8_t *const planes[3], const int strides[3],
                         unsigned *cs)
{
    uint32_t t0;
    if (!d->last_ok.length || !d->out_buf[d->last_ok.buf]) return einval(d, "No picture received yet to time");
    t0 = vcm_now_cs();
    for (int i = 0; i < reps; i++)
        copy_picture(d, &d->last_ok, planes, strides, way & VCDEC_COPY_NEON ? 2 : way & VCDEC_COPY_LDM4 ? 0 : 1);
    *cs = vcm_now_cs() - t0;
    return VCDEC_OK;
}

/* the planes don't fit the buffer's bytes */
static int layout_bad(const held_t *p)
{
    int cw = (p->width + 1) / 2, ch = (p->height + 1) / 2;
    return (uint32_t)p->width > p->pitch[0] || (uint32_t)cw > p->pitch[1] || (uint32_t)cw > p->pitch[2] ||
           p->offsets[0] + p->pitch[0] * (uint32_t)p->height > p->length ||
           p->offsets[1] + p->pitch[1] * (uint32_t)ch > p->length || p->offsets[2] + p->pitch[2] * (uint32_t)ch > p->length;
}

int vcdec_receive_hold(vcdec *d, vcdec_picture *pic, uint8_t *planes[3], int strides[3], vcdec_hold **hold)
{
    const held_t *p;
    struct vcdec_hold *h;
    int r, k;
    *hold = NULL;
    if ((r = vcdec_poll(d)) != VCDEC_OK) return r;
    if ((r = head_ready(d)) != 1) return r ? r : VCDEC_AGAIN;
    p = &d->held[0];
    k = p->buf;
    if (!d->out_pool[k] || d->m.pmp_svc || d->nuser >= d->nout - HOLD_SPARE)
        return VCDEC_UNSUPPORTED;          /* (left at the head, for vcdec_receive) */
    if (layout_bad(p)) {
        release_head(d);
        return vcm_fail(&d->m, VCDEC_ERROR, "A %u byte picture too small for %dx%d", (unsigned)p->length, p->width, p->height);
    }
    picture_cache(d, p);
    if (pic) fill_info(p, pic);
    for (int i = 0; i < 3; i++) {
        planes[i] = d->out_buf[k] + p->offsets[i];
        strides[i] = (int)p->pitch[i];
    }
    h = &d->holds[k];
    h->d = d; h->buf = k; h->live = 1;
    if (p->flags & FLAG_EOS) d->eos_back = 1;
    memmove(d->held, d->held + 1, sizeof d->held[0] * (size_t)(d->nheld - 1));
    d->nheld--;
    d->out_state[k] = OB_USER;
    d->nuser++;
    d->stats.pictures++;
    d->stats.holds++;
    *hold = h;
    return VCDEC_OK;
}

void vcdec_release(vcdec_hold *h)
{
    vcdec *d;
    if (!h || !h->live) return;
    d = h->d;
    h->live = 0;
    d->nuser--;
    d->out_state[h->buf] = OB_FREE;
    if (d->closing) {
        out_free(d, h->buf);
        if (!d->nuser) free_all(d);
    }
    /* (otherwise the next vcdec_poll hands it to the decoder: nothing is
       sent from here, which may be an FFmpeg frame's free callback) */
}

int vcdec_receive(vcdec *d, vcdec_picture *pic, uint8_t *const planes[3], const int strides[3])
{
    const held_t *p;
    int r;
    if ((r = vcdec_poll(d)) != VCDEC_OK) return r;
    if ((r = head_ready(d)) != 1) return r ? r : VCDEC_AGAIN;
    p = &d->held[0];
    if (pic) fill_info(p, pic);
    if (planes) {
        if (layout_bad(p)) {
            release_head(d);
            return vcm_fail(&d->m, VCDEC_ERROR, "A %u byte picture too small for %dx%d", (unsigned)p->length, p->width, p->height);
        }
        copy_picture(d, p, planes, strides, d->copy_way);
        d->last_ok = *p;                   /* (for vcdec_copy_benchmark) */
        d->stats.pictures++;
    } else {
        d->stats.discarded++;
    }
    release_head(d);
    return VCDEC_OK;
}

int vcdec_flush(vcdec *d)
{
    int r, recreate;
    if ((r = vcdec_poll(d)) != VCDEC_OK) return r;
    d->stats.flushes++;
    if (d->parked) {                       /* (0.4.3) destroyed at the end: created again */
        d->parked = 0;
        d->nheld = 0;
        d->eos_back = d->eos_seen = 0;
        d->stats.recreated++;
        vcm_logf(&d->m, "Flush after the end: the decoder created again");
        if (create_component(d)) return d->m.failed;
        return VCDEC_OK;
    }
    if (d->eos_sent && !d->eos_vc) {       /* an EOS with nothing before it: nothing to undo */
        d->eos_sent = d->eos_back = 0;
        return VCDEC_OK;
    }
    /* (0.4.11) with direct output a picture decoded before the flush can
       still come after it (VCDecTest 0.3.10's Reverse on a Pi 4: pts
       733333 after a seek to 3333333): a new decoder instead, as after an
       EOS (1-3 cs on the Pi) */
    recreate = d->eos_sent || (d->direct_port && d->sent_any);
    if (recreate) {
        /* after an EOS a flush leaves the decoder losing its last pictures
           at the next EOS (0.14-0.17 on the Pi): a new one instead */
        destroy_component(d, 0);
        if (d->m.failed) return d->m.failed;
        for (int i = 0; i < IN_BUFS; i++) d->in_busy[i] = 0;
    } else {
        /* the pictures already decoded come back during these (rule 6) */
        if (vcm_port_action(&d->m, &d->in_info, ACTION_FLUSH, "Input flush") ||
            vcm_port_action(&d->m, &d->out_info, ACTION_FLUSH, "Output flush"))
            return d->m.failed;
    }
    /* every picture held dropped, once its receive is done */
    if (d->nheld && vcm_wait_received(&d->m, d->held[d->nheld - 1].seq)) return d->m.failed;
    d->stats.discarded += (unsigned)d->nheld;
    d->nheld = 0;
    for (int i = 0; i < d->nout; i++)     /* (the flush or the disables sent back the decoder's) */
        if (d->out_state[i] == OB_HELD || (recreate && d->out_state[i] != OB_USER)) d->out_state[i] = OB_FREE;
    d->eos_back = d->eos_seen = 0;
    d->seg_sent = d->seg_back = 0;
    if (recreate) {
        d->stats.recreated++;
        vcm_logf(&d->m, "Flush %s: the decoder created again", d->eos_sent ? "after the EOS" : "with direct output");
        if (create_component(d)) return d->m.failed;
        return VCDEC_OK;
    }
    /* (vcdec_poll acts on a format change that came during the flush) */
    return vcdec_poll(d);
}
