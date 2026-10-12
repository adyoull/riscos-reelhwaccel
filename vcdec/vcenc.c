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
 * The VCHIQ, message and memory code is vcdec.c's (Pi-proven), repeated
 * here so vcdec is left as it is.
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "vcenc.h"
#include "../common/contig.h"

#define OS_Module                 0x1E
#define OS_SWINumberFromString    0x39
#define OS_ReadMonotonicTime      0x42
#define OS_DynamicArea            0x66
#define OS_Memory                 0x68
#define OS_MMUControl             0x6B
#define OS_SynchroniseCodeAreas   0x6E
#define DA_NOT_DRAGGABLE          (1u << 7)
#define DA_SPECIFIC_PAGES         (1u << 8)
#define DA_PMP                    (1u << 20)
#define PAGE_LOCK                 (1u << 15)
#define VC_RAM_TOP                0x40000000u
#define ARMOP_CACHE_CLEAN_INVALIDATE_RANGE 21
#define BCMSupport_SendTempPropertyBuffer 0x591C5
#define TAG_GET_VC_MEMORY         0x00010006
#define VCHIQ_Initialise          0x59200
#define VCHIQ_Connect             0x59201
#define VCHIQ_Disconnect          0x59202
#define VCHIQ_BulkQueueTransmit   0x59203
#define VCHIQ_MsgDequeue          0x59204
#define VCHIQ_MsgQueue            0x59205
#define VCHIQ_ServiceClose        0x59209
#define VCHIQ_ServiceOpen         0x5920A
#define VCHIQ_ServiceUse          0x5920D
#define VCHIQ_ServiceRelease      0x5920E
#define VCHIQ_BulkQueueReceive    0x5920F
#define VCHI_FLAGS_CALLBACK_WHEN_DONE 6
#define VCHI_FLAGS_BLOCK_UNTIL_QUEUED 4

#define FOURCC_BE(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define FOURCC_LE(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define MMAL_SERVICE   FOURCC_BE('m', 'm', 'a', 'l')
#define MMAL_MAGIC     FOURCC_LE('m', 'm', 'a', 'l')
#define ENC_H264       FOURCC_LE('H', '2', '6', '4')
#define ENC_I420       FOURCC_LE('I', '4', '2', '0')
#define EV_ERROR       FOURCC_LE('E', 'R', 'R', 'O')

enum { T_COMPONENT_CREATE = 4, T_COMPONENT_DESTROY, T_COMPONENT_ENABLE, T_COMPONENT_DISABLE, T_PORT_INFO_GET,
       T_PORT_INFO_SET, T_PORT_ACTION, T_BUFFER_FROM_HOST, T_BUFFER_TO_HOST, T_PORT_PARAMETER_SET = 14,
       T_EVENT_TO_HOST = 16 };
enum { PORT_CONTROL = 1, PORT_INPUT = 2, PORT_OUTPUT = 3 };
enum { ACTION_ENABLE = 1, ACTION_DISABLE = 2 };
enum { ES_VIDEO = 3 };
#define FLAG_EOS           1u
#define FLAG_FRAME_END     4u
#define FLAG_KEYFRAME      8u
#define FLAG_CONFIG     0x20u
#define TIME_UNKNOWN   0x8000000000000000ull
#define SHORT_DATA     128

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
#define CTX_BASE       1
#define REPLY_CS       300
#define STALL_CS       500
#define MAX_W          1920
#define MAX_H          1088

typedef struct { uint32_t magic, type, control_service, context, status, padding; } hdr_t;
typedef struct {
    uint32_t priv, name, type;
    uint16_t index, index_all;
    uint32_t is_enabled, format, buffer_num_min, buffer_size_min, buffer_alignment_min,
             buffer_num_recommended, buffer_size_recommended, buffer_num, buffer_size,
             component, userdata, capabilities;
} port_t;
typedef struct { uint32_t type, encoding, encoding_variant, es, bitrate, flags, extradata_size, extradata; } fmt_t;
typedef struct { uint32_t width, height; int32_t crop[4], frame_rate[2], par[2]; uint32_t color_space; } video_t;
typedef struct {
    uint32_t status, component_handle, port_type, port_index;
    int32_t found;
    uint32_t port_handle;
    port_t port;
    fmt_t format;
    video_t video;
    uint8_t extradata[128];
} port_info_t;
typedef struct {
    uint32_t component_handle, port_type, port_index;
    port_t port;
    fmt_t format;
    video_t video;
    uint8_t extradata[128];
} port_set_t;
typedef struct { uint32_t magic, component_handle, port_handle, client_context; } drvbuf_t;
typedef struct {
    drvbuf_t drvbuf, drvbuf_ref;
    uint32_t next, priv, cmd, data, alloc_size, length, offset, flags;
    uint64_t pts, dts;
    uint32_t type, user_data;
    uint32_t planes, offsets[4], pitch[4], vflags;
    int32_t is_zero_copy, has_reference;
    uint32_t payload_in_message;
    uint8_t short_data[SHORT_DATA];
} buffer_msg_t;
typedef struct {
    uint32_t client_component, port_type, port_num, cmd, length;
    uint8_t data[256];
    uint32_t delayed_buffer;
} event_msg_t;

typedef char chk_hdr[sizeof(hdr_t) == 24 ? 1 : -1];
typedef char chk_port[sizeof(port_t) == 64 ? 1 : -1];
typedef char chk_buf[offsetof(buffer_msg_t, pts) == 64 && sizeof(buffer_msg_t) == 272 ? 1 : -1];
typedef char chk_set[sizeof(port_set_t) == 280 ? 1 : -1];

#ifdef VCDEC_HOST
_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r);
#define vc_swi probe_swi
#else
static _kernel_oserror *vc_swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }
#endif
void vcdec_svc_copy(void *dst, const void *src, size_t n);      /* vcdec_copy.S */
void vcdec_svc_call(uint32_t fn, uint32_t r0, uint32_t r1);

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
    vcenc_config cfg;
    char err[200];
    int failed;
    uint32_t instance, handle, stub, comp, context;
    int connected, opened, created, enabled, in_on, out_on;
    port_info_t ctl_info, in_info, out_info;
    uint32_t msg[128];
    const char *waiting;
    uint32_t pci_alloc_swi, pci_free_swi;
    int use_pool, pool_stuck;
    char why[160];
    uint32_t armop_cci;
    int nin;
    mem_t in[MAX_IN];
    int in_busy[MAX_IN];
    uint32_t in_size, stride_y, stride_c, rows_y, rows_c;
    mem_t out[OUT_BUFS];
    int out_state[OUT_BUFS];
    uint32_t out_size;
    held_t held[OUT_BUFS + 1];
    int nheld;
    uint32_t rx_queued;
    uint8_t *pkt;                          /* the packet being put together, and the last one given out */
    size_t pkt_len, pkt_cap;
    uint64_t pkt_pts;
    unsigned pkt_flags;
    uint8_t *hdr;                          /* SPS and PPS */
    size_t hdr_len;
    uint8_t *row;                          /* (PCI memory: a row built here, then copied in) */
    uint8_t *evbuf;
    uint32_t evsize;
    int sent_any, eos_sent, eos_back, eos_seen, heard_output;
    uint32_t t_first_send, last_heard;
    vcenc_stats stats;
};

static char open_err[200];

/* ---- small things ---- */

static void logf_(vcenc *e, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    if (!e->cfg.log) return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    e->cfg.log(e->cfg.log_handle, b);
}

static int fail(vcenc *e, int code, const char *fmt, ...)
{
    va_list ap;
    if (e->failed) return e->failed;
    va_start(ap, fmt);
    vsnprintf(e->err, sizeof e->err, fmt, ap);
    va_end(ap);
    e->failed = code;
    logf_(e, "%s", e->err);
    return code;
}

static int einval(vcenc *e, const char *what)
{
    snprintf(e->err, sizeof e->err, "%s", what);
    return VCENC_EINVAL;
}

static _kernel_oserror *swi5(int n, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4,
                             uint32_t *o0, uint32_t *o2)
{
    _kernel_swi_regs r;
    _kernel_oserror *er;
    memset(&r, 0, sizeof r);
    r.r[0] = (int)r0; r.r[1] = (int)r1; r.r[2] = (int)r2; r.r[3] = (int)r3; r.r[4] = (int)r4;
    er = vc_swi(n, &r);
    if (!er) {
        if (o0) *o0 = (uint32_t)r.r[0];
        if (o2) *o2 = (uint32_t)r.r[2];
    }
    return er;
}
#define swi(n, a, b, c, d, o0, o2) swi5(n, a, b, c, d, 0, o0, o2)

static uint32_t now_cs(void)
{
    uint32_t t = 0;
    swi(OS_ReadMonotonicTime, 0, 0, 0, 0, &t, NULL);
    return t;
}

static const char *const status_names[] = {
    "SUCCESS", "ENOMEM", "ENOSPC", "EINVAL", "ENOSYS", "ENOENT", "ENXIO", "EIO", "ESPIPE",
    "ECORRUPT", "ENOTREADY", "ECONFIG", "EISCONN", "ENOTCONN", "EAGAIN", "EFAULT" };
static const char *st(uint32_t s) { return s < 16 ? status_names[s] : "?"; }

static unsigned gpu_mem(void)
{
    uint32_t buf[8];
    _kernel_swi_regs r;
    buf[0] = sizeof buf; buf[1] = 0;
    buf[2] = TAG_GET_VC_MEMORY; buf[3] = 8; buf[4] = 0; buf[5] = buf[6] = 0; buf[7] = 0;
    memset(&r, 0, sizeof r);
    r.r[0] = r.r[1] = (int)(uintptr_t)buf;
    if (vc_swi(BCMSupport_SendTempPropertyBuffer, &r) || buf[1] != 0x80000000u) return 0;
    return (unsigned)(buf[6] >> 20);
}

static void bulk_counts(vcenc *e, uint32_t v[2])
{
    vcdec_svc_copy(v, (const void *)(uintptr_t)(e->stub + 64), 8);
}

/* ---- memory: Physical Memory Pools, or PCI memory (as vcdec's) ---- */

static void *pci_alloc(vcenc *e, size_t n)
{
    uint32_t log = 0;
    _kernel_swi_regs r;
    if (!e->pci_alloc_swi) {
        memset(&r, 0, sizeof r);
        r.r[1] = (int)(uintptr_t)"PCI_RAMAlloc";
        if (vc_swi(OS_SWINumberFromString, &r)) return NULL;
        e->pci_alloc_swi = (uint32_t)r.r[0];
        r.r[1] = (int)(uintptr_t)"PCI_RAMFree";
        if (vc_swi(OS_SWINumberFromString, &r)) return NULL;
        e->pci_free_swi = (uint32_t)r.r[0];
    }
    if (swi((int)e->pci_alloc_swi, (uint32_t)n, 4096, 0, 0, &log, NULL) || !log) return NULL;
    return (void *)(uintptr_t)log;
}

static void pci_free(vcenc *e, void *p)
{
    if (p) swi((int)e->pci_free_swi, (uint32_t)(uintptr_t)p, 0, 0, 0, NULL, NULL);
}

static _kernel_oserror *pmp_op(uint32_t reason, uint32_t area, uint32_t *list, uint32_t n)
{
    return swi(OS_DynamicArea, reason, area, (uint32_t)(uintptr_t)list, n, NULL, NULL);
}

static int pmp_free(vcenc *e, uint32_t area, uint32_t pages)
{
    _kernel_oserror *er;
    uint32_t *l = malloc(pages * 12 + 12);
    if (l) {
        for (uint32_t j = 0; j < pages; j++) { l[3 * j] = j; l[3 * j + 1] = 0xFFFFFFFFu; l[3 * j + 2] = 0; }
        if (pages) { pmp_op(22, area, l, pages); pmp_op(21, area, l, pages); }
        free(l);
    }
    if ((er = swi(OS_DynamicArea, 1, area, 0, 0, NULL, NULL)) != NULL) {
        logf_(e, "A buffer pool (area %u) can't be removed: %s", (unsigned)area, er->errmess);
        e->pool_stuck = 1;
        return -1;
    }
    return 0;
}

static void why(vcenc *e, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->why, sizeof e->why, fmt, ap);
    va_end(ap);
}

/* a pool of contiguous pages below 1 GB, user read/write, cacheable (as
   vcdec's pmp_alloc: the program's page at &8000 never claimed) */
static void *pmp_alloc(vcenc *e, uint32_t size, uint32_t *area_out)
{
    uint32_t pages = (size + 4095) >> 12, first, *l, phys[2];
    _kernel_swi_regs r;
    _kernel_oserror *er;
    void *base;
    const char *no;
    contig_app_page app = contig_read_app_page(vc_swi);
    if (!(l = malloc(pages * 12))) { why(e, "Out of memory"); return NULL; }
    memset(&r, 0, sizeof r);
    r.r[0] = 0; r.r[1] = -1; r.r[2] = 0; r.r[3] = -1;
    r.r[4] = (int)(DA_SPECIFIC_PAGES | DA_PMP | DA_NOT_DRAGGABLE);
    r.r[5] = (int)(pages << 12); r.r[6] = (int)(e->stub + 72); r.r[7] = 0;
    r.r[8] = (int)(uintptr_t)"vcenc buffer"; r.r[9] = (int)pages;
    if ((er = vc_swi(OS_DynamicArea, &r)) != NULL) { free(l); why(e, "A Physical Memory Pool: %s", er->errmess); return NULL; }
    *area_out = (uint32_t)r.r[1];
    base = (void *)(uintptr_t)(uint32_t)r.r[3];
    if ((no = contig_recommend(vc_swi, pages, VC_RAM_TOP - 1, &app, &first)) != NULL) {
        free(l); pmp_free(e, *area_out, 0);
        why(e, "%s", no);
        return NULL;
    }
    for (uint32_t j = 0; j < pages; j++) { l[3 * j] = j; l[3 * j + 1] = first + j; l[3 * j + 2] = PAGE_LOCK; }
    if ((er = pmp_op(21, *area_out, l, pages)) != NULL) {
        free(l); pmp_free(e, *area_out, pages);
        why(e, "Claiming %u pages from %u: %s", (unsigned)pages, (unsigned)first, er->errmess);
        return NULL;
    }
    for (uint32_t j = 0; j < pages; j++) { l[3 * j] = j; l[3 * j + 1] = j; l[3 * j + 2] = PAGE_LOCK; }
    if ((er = pmp_op(22, *area_out, l, pages)) != NULL) {
        free(l); pmp_free(e, *area_out, pages);
        why(e, "Mapping %u pages: %s", (unsigned)pages, er->errmess);
        return NULL;
    }
    free(l);
    if (contig_app_page_moved(vc_swi, &app)) {
        e->stats.app_page_moves++;
        logf_(e, "Claiming a pool's pages moved the program's page at &8000 (ARMEABISupport will lose the program)");
    }
    for (int k = 0; k < 2; k++) {
        uint32_t blk[3] = { 0, (uint32_t)(uintptr_t)base + (k ? (pages - 1) << 12 : 0), 0 };
        if ((er = swi(OS_Memory, 0x2200, (uint32_t)(uintptr_t)blk, 1, 0, NULL, NULL)) != NULL) {
            pmp_free(e, *area_out, pages);
            why(e, "OS_Memory 0: %s", er->errmess);
            return NULL;
        }
        phys[k] = blk[2];
    }
    if (phys[1] != phys[0] + ((pages - 1) << 12) || phys[1] > VC_RAM_TOP - 4096) {
        pmp_free(e, *area_out, pages);
        why(e, "The pool's pages aren't contiguous below 1 GB (&%08X..&%08X)", (unsigned)phys[0], (unsigned)phys[1]);
        return NULL;
    }
    {   /* user readable and writable? (if not, PCI memory instead: simpler than copying in SVC mode) */
        uint32_t acc = 0;
        memset(&r, 0, sizeof r);
        r.r[0] = 24; r.r[1] = (int)(uintptr_t)base; r.r[2] = (int)((uintptr_t)base + (pages << 12));
        if (!vc_swi(OS_Memory, &r)) acc = (uint32_t)r.r[1];
        if ((acc & 3) != 3) {
            pmp_free(e, *area_out, pages);
            why(e, "The pool isn't user read/write (OS_Memory 24: &%X)", (unsigned)acc);
            return NULL;
        }
    }
    return base;
}

static void mem_free(vcenc *e, mem_t *m)
{
    if (!m->p) return;
    if (m->pool) pmp_free(e, m->area, (m->cap + 4095) >> 12);
    else pci_free(e, m->p);
    memset(m, 0, sizeof *m);
}

static int mem_alloc(vcenc *e, mem_t *m, uint32_t size, const char *what)
{
    mem_free(e, m);
    if (e->use_pool) {
        if ((m->p = pmp_alloc(e, size, &m->area)) != NULL) m->pool = 1;
        else {
            logf_(e, "No buffer pool (%s): PCI memory instead", e->why);
            e->use_pool = 0;
        }
    }
    if (!m->p && !(m->p = pci_alloc(e, size)))
        return fail(e, VCENC_ERROR, "No memory the VideoCore can reach for %s (%u bytes)", what, (unsigned)size);
    m->cap = size;
    return 0;
}

/* a cacheable pool's range cleaned (written out) and invalidated */
static void cache_cci(vcenc *e, const mem_t *m, uint32_t n)
{
    if (m->pool && e->armop_cci)
        vcdec_svc_call(e->armop_cci, (uint32_t)(uintptr_t)m->p, ((uint32_t)(uintptr_t)m->p + n + 63) & ~63u);
}

/* ---- messages (as vcdec's) ---- */

static int send_msg(vcenc *e, uint32_t type, const void *payload, uint32_t len)
{
    uint32_t m[128];
    hdr_t *h = (hdr_t *)m;
    _kernel_oserror *er;
    memset(m, 0, sizeof(hdr_t));
    h->magic = MMAL_MAGIC;
    h->type = type;
    h->context = ++e->context;
    memcpy(h + 1, payload, len);
    swi(VCHIQ_ServiceUse, e->handle, 0, 0, 0, NULL, NULL);
    er = swi(VCHIQ_MsgQueue, e->handle, (uint32_t)(uintptr_t)m, (uint32_t)sizeof *h + len, 0, NULL, NULL);
    swi(VCHIQ_ServiceRelease, e->handle, 0, 0, 0, NULL, NULL);
    if (er) return fail(e, VCENC_ERROR, "VCHIQ_MsgQueue: %s", er->errmess);
    return 0;
}

static int queue_receive(vcenc *e, uint8_t *dst, uint32_t n, uint32_t *seq)
{
    _kernel_oserror *er;
    swi(VCHIQ_ServiceUse, e->handle, 0, 0, 0, NULL, NULL);
    er = swi5(VCHIQ_BulkQueueReceive, e->handle, (uint32_t)(uintptr_t)dst, (n + 3) & ~3u,
              VCHI_FLAGS_CALLBACK_WHEN_DONE, 0, NULL, NULL);
    swi(VCHIQ_ServiceRelease, e->handle, 0, 0, 0, NULL, NULL);
    if (er) return fail(e, VCENC_ERROR, "VCHIQ_BulkQueueReceive: %s", er->errmess);
    *seq = ++e->rx_queued;
    return 0;
}

static int received(vcenc *e, uint32_t seq)
{
    uint32_t v[2];
    if (!seq) return 1;                    /* (the data came in the message) */
    bulk_counts(e, v);
    if (v[1]) return -1;
    return (int32_t)(v[0] - seq) >= 0;
}

static int wait_received(vcenc *e, uint32_t seq)
{
    uint32_t t0 = now_cs();
    int r;
    while (!(r = received(e, seq)))
        if (now_cs() - t0 > REPLY_CS) return fail(e, VCENC_ERROR, "A bulk receive didn't finish in %d cs", REPLY_CS);
    if (r < 0) return fail(e, VCENC_ERROR, "A bulk receive was aborted");
    return 0;
}

static int buffer_to_vc(vcenc *e, port_info_t *pi, int idx, const mem_t *m, uint32_t len, uint32_t flags,
                        uint64_t pts)
{
    buffer_msg_t b;
    memset(&b, 0, sizeof b);
    b.drvbuf.magic = MMAL_MAGIC;
    b.drvbuf.component_handle = e->comp;
    b.drvbuf.port_handle = pi->port_handle;
    b.drvbuf.client_context = (uint32_t)idx + CTX_BASE;
    b.data = (uint32_t)(uintptr_t)m->p;
    b.alloc_size = m->cap;
    b.length = len;
    b.flags = flags;
    b.pts = pts;
    b.dts = pts;
    if (send_msg(e, T_BUFFER_FROM_HOST, &b, sizeof b)) return -1;
    if (e->cfg.flags & VCENC_LOG_MESSAGES)
        logf_(e, "tx: %s buffer %d, %u bytes, flags &%X, pts %lld", pi == &e->in_info ? "input" : "output", idx,
              (unsigned)len, (unsigned)flags, (long long)pts);
    if (len) {
        _kernel_oserror *er;
        swi(VCHIQ_ServiceUse, e->handle, 0, 0, 0, NULL, NULL);
        er = swi5(VCHIQ_BulkQueueTransmit, e->handle, (uint32_t)(uintptr_t)m->p, (len + 3) & ~3u,
                  VCHI_FLAGS_BLOCK_UNTIL_QUEUED, 0, NULL, NULL);
        swi(VCHIQ_ServiceRelease, e->handle, 0, 0, 0, NULL, NULL);
        if (er) return fail(e, VCENC_ERROR, "VCHIQ_BulkQueueTransmit: %s", er->errmess);
    }
    return 0;
}

/* a message that isn't the reply being waited for */
static void handle_msg(vcenc *e, uint32_t got)
{
    hdr_t *h = (hdr_t *)e->msg;
    if (h->type == T_BUFFER_TO_HOST) {
        buffer_msg_t *b = (buffer_msg_t *)(h + 1);
        uint32_t k = b->drvbuf.client_context - CTX_BASE;
        if (b->drvbuf.magic != MMAL_MAGIC || b->drvbuf.client_context < CTX_BASE) {
            logf_(e, "A returned buffer we don't know");
            return;
        }
        if (e->cfg.flags & VCENC_LOG_MESSAGES)
            logf_(e, "rx%s: buffer back, status %u, port %u, buffer %u, length %u, flags &%X, pts %lld%s", e->waiting,
                  (unsigned)h->status, (unsigned)b->drvbuf.port_handle, (unsigned)k, (unsigned)b->length,
                  (unsigned)b->flags, (long long)b->pts, b->payload_in_message ? " (in the message)" : "");
        if (b->drvbuf.port_handle == e->in_info.port_handle && k < (uint32_t)e->nin) {
            e->in_busy[k] = 0;
            return;
        }
        if (b->drvbuf.port_handle != e->out_info.port_handle || k >= OUT_BUFS || e->out_state[k] != OB_VC) {
            logf_(e, "A buffer back from port %u (buffer %u) that isn't with the encoder", (unsigned)b->drvbuf.port_handle,
                  (unsigned)k);
            return;
        }
        if (h->status || (!b->length && !(b->flags & FLAG_EOS))) {   /* empty: back with us */
            if (h->status) logf_(e, "Output buffer back with status %s", st(h->status));
            e->out_state[k] = OB_FREE;
            return;
        }
        {
            held_t *p = &e->held[e->nheld];
            if (e->nheld >= OUT_BUFS + 1) { fail(e, VCENC_ERROR, "More packets back than buffers"); return; }
            memset(p, 0, sizeof *p);
            p->buf = (int)k;
            p->length = b->length;
            p->flags = b->flags;
            p->pts = b->pts;
            if (b->length > e->out[k].cap) {
                fail(e, VCENC_ERROR, "A %u byte packet for a %u byte buffer", (unsigned)b->length, (unsigned)e->out[k].cap);
                return;
            }
            if (b->length && b->payload_in_message) {   /* (short: in the message itself) */
                if (b->length > SHORT_DATA) { fail(e, VCENC_ERROR, "A %u byte packet in a message", (unsigned)b->length); return; }
                memcpy(p->short_data, b->short_data, b->length);
                p->seq = 0;
            } else if (queue_receive(e, e->out[k].p, b->length ? b->length : 8, &p->seq)) {
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
            logf_(e, "rx%s: event &%08X, %u bytes, port type %u", e->waiting, (unsigned)ev->cmd, (unsigned)ev->length,
                  (unsigned)ev->port_type);
        if (!ev->cmd) return;
        if (ev->length > sizeof ev->data) {
            uint32_t seq;
            if (ev->length > e->evsize) {
                pci_free(e, e->evbuf);
                e->evbuf = pci_alloc(e, ev->length + 4);
                e->evsize = e->evbuf ? ev->length : 0;
                if (!e->evbuf) { fail(e, VCENC_ERROR, "No PCI memory for a %u byte event", (unsigned)ev->length); return; }
            }
            if (queue_receive(e, e->evbuf, ev->length, &seq) || wait_received(e, seq)) return;
        }
        if (ev->cmd == EV_ERROR) fail(e, VCENC_ERROR, "The encoder reports an error (port type %u)", (unsigned)ev->port_type);
        else logf_(e, "Event &%08X (%u bytes) on port type %u", (unsigned)ev->cmd, (unsigned)ev->length, (unsigned)ev->port_type);
        return;
    }
    logf_(e, "(a type %u message, %u bytes)", (unsigned)h->type, (unsigned)got);
}

static uint32_t dequeue(vcenc *e)
{
    uint32_t got = 0;
    if (swi(VCHIQ_MsgDequeue, e->handle, (uint32_t)(uintptr_t)e->msg, sizeof e->msg, 0, NULL, &got)) return 0;
    if (got < sizeof(hdr_t) || ((hdr_t *)e->msg)->magic != MMAL_MAGIC) {
        logf_(e, "(a %u byte message that isn't MMAL's)", (unsigned)got);
        return 0;
    }
    e->last_heard = now_cs();
    return got;
}

static int transact(vcenc *e, uint32_t type, const void *payload, uint32_t len, const char *what)
{
    uint32_t t0, got;
    if (e->failed || send_msg(e, type, payload, len)) return -1;
    t0 = now_cs();
    e->waiting = " (waiting)";
    while (now_cs() - t0 < REPLY_CS) {
        if (!(got = dequeue(e))) continue;
        if (((hdr_t *)e->msg)->type == type) { e->waiting = ""; return (int)(got - sizeof(hdr_t)); }
        handle_msg(e, got);
        if (e->failed) { e->waiting = ""; return -1; }
    }
    e->waiting = "";
    fail(e, VCENC_ERROR, "%s: no reply from the encoder in %d cs", what, REPLY_CS);
    return -1;
}

static uint32_t *reply(vcenc *e) { return (uint32_t *)((hdr_t *)e->msg + 1); }

static int status_ok(vcenc *e, int n, const char *what)
{
    if (n < 4) return n < 0 ? -1 : fail(e, VCENC_ERROR, "%s: a short reply", what);
    if (reply(e)[0]) return fail(e, VCENC_ERROR, "%s: %s", what, st(reply(e)[0]));
    return 0;
}

static int port_get(vcenc *e, uint32_t type, port_info_t *pi, const char *what)
{
    uint32_t req[3] = { e->comp, type, 0 };
    int n = transact(e, T_PORT_INFO_GET, req, sizeof req, what);
    if (status_ok(e, n, what)) return -1;
    if (n < (int)offsetof(port_info_t, extradata)) return fail(e, VCENC_ERROR, "%s: a short reply", what);
    memcpy(pi, reply(e), sizeof *pi);
    return 0;
}

static int port_set(vcenc *e, uint32_t type, port_info_t *pi, const char *what)
{
    port_set_t s;
    int n;
    memset(&s, 0, sizeof s);
    s.component_handle = e->comp;
    s.port_type = type;
    s.port = pi->port;
    s.format = pi->format;
    s.format.extradata_size = 0;
    s.video = pi->video;
    n = transact(e, T_PORT_INFO_SET, &s, sizeof s, what);
    if (status_ok(e, n, what)) return -1;
    if (n >= (int)offsetof(port_info_t, extradata)) {
        port_info_t *r = (port_info_t *)reply(e);
        pi->port = r->port;
        pi->format = r->format;
        pi->video = r->video;
    }
    return 0;
}

static int port_action(vcenc *e, port_info_t *pi, uint32_t action, const char *what)
{
    uint32_t req[3 + 16];
    memset(req, 0, sizeof req);
    req[0] = e->comp; req[1] = pi->port_handle; req[2] = action;
    memcpy(&req[3], &pi->port, sizeof pi->port);
    return status_ok(e, transact(e, T_PORT_ACTION, req, sizeof req, what), what);
}

static int simple(vcenc *e, uint32_t type, const char *what)
{
    return status_ok(e, transact(e, type, &e->comp, 4, what), what);
}

/* a port parameter set: 0; -1 if it failed (needed: then the encoder
   fails; otherwise only said in the log) */
static int param_set(vcenc *e, port_info_t *pi, uint32_t id, const uint32_t *v, int nv, int needed, const char *what)
{
    uint32_t req[4 + 4];
    int n;
    req[0] = e->comp; req[1] = pi->port_handle; req[2] = id; req[3] = 8 + 4 * (uint32_t)nv;
    memcpy(&req[4], v, 4 * (size_t)nv);
    n = transact(e, T_PORT_PARAMETER_SET, req, (uint32_t)(16 + 4 * nv), what);
    if (n < 0) return -1;
    if (n < 4 || reply(e)[0]) {
        if (needed) return fail(e, VCENC_ERROR, "%s: %s", what, n >= 4 ? st(reply(e)[0]) : "a short reply");
        logf_(e, "%s: %s (carrying on without it)", what, n >= 4 ? st(reply(e)[0]) : "a short reply");
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
    uint32_t req[1 + 32 + 1], v[2];
    int n;
    memset(req, 0, sizeof req);
    req[0] = 1;
    strcpy((char *)&req[1], "ril.video_encode");
    n = transact(e, T_COMPONENT_CREATE, req, sizeof req, "Create");
    if (n < 0) {
        if (strstr(e->err, "no reply"))
            snprintf(e->err, sizeof e->err, "The VideoCore's MMAL service isn't answering (if something stopped "
                     "part way, it can stay stuck until the machine is restarted)");
        return -1;
    }
    if (n < 8 || reply(e)[0]) return fail(e, VCENC_ERROR, "Create: %s", n >= 4 ? st(reply(e)[0]) : "a short reply");
    e->comp = reply(e)[1];
    e->created = 1;
    if (port_get(e, PORT_CONTROL, &e->ctl_info, "Control port") || port_get(e, PORT_INPUT, &e->in_info, "Input port") ||
        port_get(e, PORT_OUTPUT, &e->out_info, "Output port"))
        return -1;
    logf_(e, "Output port: %u buffers of %u bytes recommended (at least %u of %u)",
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
    if (port_set(e, PORT_INPUT, &e->in_info, "Input format")) return -1;
    if (e->in_info.port.buffer_size_min > e->in_size)
        return fail(e, VCENC_ERROR, "The encoder wants %u byte input buffers (%u made)",
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
    if (port_set(e, PORT_OUTPUT, &e->out_info, "Output format")) return -1;
    if (e->out_info.port.buffer_size_min > e->out_size) {   /* (larger buffers, then the port told again) */
        e->out_size = (e->out_info.port.buffer_size_min + 4095) & ~4095u;
        logf_(e, "The encoder wants %u byte output buffers", (unsigned)e->out_info.port.buffer_size_min);
        for (int i = 0; i < OUT_BUFS; i++) if (mem_alloc(e, &e->out[i], e->out_size, "the output")) return -1;
        e->out_info.port.buffer_size = e->out_size;
        if (port_set(e, PORT_OUTPUT, &e->out_info, "Output format")) return -1;
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
    if (e->failed) return -1;
    if (simple(e, T_COMPONENT_ENABLE, "Enable")) return -1;
    e->enabled = 1;
    if (port_action(e, &e->in_info, ACTION_ENABLE, "Input enable")) return -1;
    e->in_on = 1;
    if (port_action(e, &e->out_info, ACTION_ENABLE, "Output enable")) return -1;
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
            if (e->failed) return -1;
        }
    }
    return give_outputs(e);
}

static void destroy_component(vcenc *e)
{
    e->failed = 0;                         /* (every step tried: a component left on the VideoCore keeps its memory) */
    if (e->out_on && !port_action(e, &e->out_info, ACTION_DISABLE, "Output disable")) e->out_on = 0;
    e->failed = 0;
    if (e->in_on && !port_action(e, &e->in_info, ACTION_DISABLE, "Input disable")) e->in_on = 0;
    e->failed = 0;
    if (e->enabled && !simple(e, T_COMPONENT_DISABLE, "Disable")) e->enabled = 0;
    e->failed = 0;
    if (e->created && !simple(e, T_COMPONENT_DESTROY, "Destroy")) e->created = 0;
}

/* ---- the API ---- */

void vcenc_config_init(vcenc_config *c)
{
    memset(c, 0, sizeof *c);
}

const char *vcenc_open_error(void) { return open_err; }
const char *vcenc_error(const vcenc *e) { return e ? e->err : open_err; }

void vcenc_get_stats(const vcenc *e, vcenc_stats *s)
{
    *s = e->stats;
    s->in_pools = s->out_pools = 0;
    for (int i = 0; i < e->nin; i++) s->in_pools += e->in[i].pool;
    for (int i = 0; i < OUT_BUFS; i++) s->out_pools += e->out[i].pool;
    s->in_size = e->in_size;
    s->out_size = e->out_size;
    s->out_buffers = OUT_BUFS;
}

int vcenc_open(vcenc **out, const vcenc_config *c)
{
    static const uint32_t code[9] = { 0xe3510004u, 0x05903000u, 0x02833001u, 0x05803000u, 0xe3510012u,
                                      0x05903004u, 0x02833001u, 0x05803004u, 0xe1a0f00eu };
    /* (the callback, as vcdec's: R1 = 4, a receive done: [R0] += 1; R1 = 18,
       aborted: [R0 + 4] += 1) */
    vcenc *e;
    unsigned mb;
    uint32_t setup[11];
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
    mb = gpu_mem();
    if (mb && mb < 128 && c->width * c->height > 1280 * 720 && !(c->flags & VCENC_NO_GPU_MEM_CHECK)) {
        snprintf(open_err, sizeof open_err, "The VideoCore has %u MB (gpu_mem): %dx%d needs gpu_mem=128 or more "
                 "in CONFIG/TXT", mb, c->width, c->height);
        return VCENC_UNSUPPORTED;
    }
    if (!(e = calloc(1, sizeof *e))) {
        snprintf(open_err, sizeof open_err, "Out of memory");
        return VCENC_ERROR;
    }
    e->cfg = *c;
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
    e->waiting = "";
    logf_(e, "vcenc %s: %dx%d at %d/%d a second, %u bit/s; the VideoCore has %u MB (gpu_mem)", VCENC_VERSION, c->width,
          c->height, e->cfg.fps_num, e->cfg.fps_den, e->cfg.bitrate, mb);
    if ((er = swi(OS_Module, 6, 0, 0, 80, NULL, &e->stub)) != NULL) { fail(e, VCENC_ERROR, "No RMA: %s", er->errmess); goto bad; }
    {
        uint32_t img[20];
        _kernel_swi_regs rr;
        memset(img, 0, sizeof img);
        memcpy(img, code, sizeof code);
        img[18] = 0xe1a0f00eu;             /* +72: the pools' dynamic area handler: MOV PC, R14 */
        vcdec_svc_copy((void *)(uintptr_t)e->stub, img, sizeof img);
        memset(&rr, 0, sizeof rr);
        rr.r[0] = 1; rr.r[1] = (int)e->stub; rr.r[2] = (int)(e->stub + 76);
        vc_swi(OS_SynchroniseCodeAreas, &rr);
    }
    if (e->use_pool && (er = swi(OS_MMUControl, 2 | ARMOP_CACHE_CLEAN_INVALIDATE_RANGE << 8, 0, 0, 0, &e->armop_cci, NULL)) != NULL) {
        e->armop_cci = 0;
        logf_(e, "No buffer pools (OS_MMUControl 2, RISC OS 5.23 or later: %s): PCI memory instead", er->errmess);
        e->use_pool = 0;
    }
    for (int i = 0; i < e->nin; i++) if (mem_alloc(e, &e->in[i], e->in_size, "an input picture")) goto bad;
    for (int i = 0; i < OUT_BUFS; i++) if (mem_alloc(e, &e->out[i], e->out_size, "the output")) goto bad;
    if (!(e->row = malloc(e->stride_y))) { fail(e, VCENC_ERROR, "Out of memory"); goto bad; }
    if ((er = swi(VCHIQ_Initialise, 0, 0, 0, 0, &e->instance, NULL)) != NULL) { fail(e, VCENC_ERROR, "VCHIQ_Initialise: %s", er->errmess); goto bad; }
    if ((er = swi(VCHIQ_Connect, 0, 0, e->instance, 0, NULL, NULL)) != NULL) { fail(e, VCENC_ERROR, "VCHIQ_Connect: %s", er->errmess); goto bad; }
    e->connected = 1;
    memset(setup, 0, sizeof setup);
    setup[0] = 15; setup[1] = 10; setup[2] = MMAL_SERVICE;
    setup[6] = e->stub; setup[7] = e->stub + 64;
    if ((er = swi(VCHIQ_ServiceOpen, e->instance, (uint32_t)(uintptr_t)setup, 0, 0, &e->handle, NULL)) != NULL) {
        fail(e, VCENC_ERROR, "VCHIQ_ServiceOpen \"mmal\": %s", er->errmess);
        goto bad;
    }
    e->opened = 1;
    swi(VCHIQ_ServiceRelease, e->handle, 0, 0, 0, NULL, NULL);
    /* constant bit rate: variable at the enables, then constant (2, else 4)
       on the running encoder. (The Pi 4's firmware refused 2 and 4 at the
       input's enable, after which it refused every enable on that
       encoder, and refused them while running too, VCEncTest 0.1.2: so
       vcenc 0.1.3 no longer makes three encoders to find that out.) */
    if (create_component(e, 1, (c->flags & VCENC_CBR) != 0)) goto bad;
    *out = e;
    return VCENC_OK;
bad:
    r = e->failed ? e->failed : VCENC_ERROR;
    snprintf(open_err, sizeof open_err, "%s", e->err);
    vcenc_close(e);
    return r;
}

void vcenc_close(vcenc *e)
{
    _kernel_oserror *er;
    if (!e) return;
    if (e->opened) destroy_component(e);
    /* (the receives queued before: finished before their memory goes) */
    {
        uint32_t last = 0;
        for (int i = 0; i < e->nheld; i++) if (e->held[i].seq) last = e->held[i].seq;
        if (last) wait_received(e, last);
    }
    if (e->opened && (er = swi(VCHIQ_ServiceClose, e->handle, 0, 0, 0, NULL, NULL)) != NULL) {
        logf_(e, "VCHIQ_ServiceClose: %s (the callback and buffers are left)", er->errmess);
        e->stub = 0;
        free(e);                           /* (the memory the VideoCore may still reach is kept) */
        return;
    }
    if (e->connected) swi(VCHIQ_Disconnect, e->instance, 0, 0, 0, NULL, NULL);
    for (int i = 0; i < MAX_IN; i++) mem_free(e, &e->in[i]);
    for (int i = 0; i < OUT_BUFS; i++) mem_free(e, &e->out[i]);
    pci_free(e, e->evbuf);
    if (e->stub && !e->pool_stuck) swi(OS_Module, 7, 0, e->stub, 0, NULL, NULL);
    free(e->row);
    free(e->pkt);
    free(e->hdr);
    free(e);
}

int vcenc_poll(vcenc *e)
{
    uint32_t got, now;
    if (e->failed) return e->failed;
    while ((got = dequeue(e)) != 0) {
        handle_msg(e, got);
        if (e->failed) return e->failed;
    }
    if (e->out_on && !e->eos_seen && give_outputs(e)) return e->failed;
    now = now_cs();
    if (e->sent_any && !e->heard_output && now - e->t_first_send > STALL_CS)
        return fail(e, VCENC_ERROR, "Nothing from the encoder in %d cs (the VideoCore has %u MB: 1080p needs "
                    "gpu_mem=128)", STALL_CS, gpu_mem());
    if (e->eos_sent && !e->eos_seen && now - e->last_heard > STALL_CS)
        return fail(e, VCENC_ERROR, "Nothing from the encoder for %d cs after the EOS", STALL_CS);
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
    if (e->failed) return e->failed;
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
        if (e->failed) return e->failed;
    }
    t0 = now_cs();
    copy_in(e, &e->in[i], planes, strides);
    cache_cci(e, &e->in[i], e->in_size);   /* (a cacheable pool: written out to memory first) */
    e->stats.cs_copy += now_cs() - t0;
    if (!e->sent_any) { e->sent_any = 1; e->t_first_send = now_cs(); }
    if (buffer_to_vc(e, &e->in_info, i, &e->in[i], e->in_size, FLAG_FRAME_END | (flags & VCENC_FORCE_KEY ? FLAG_KEYFRAME : 0),
                     (uint64_t)pts))
        return e->failed;
    e->in_busy[i] = 1;
    e->last_heard = now_cs();
    e->stats.pictures++;
    return VCENC_OK;
}

int vcenc_send_eos(vcenc *e)
{
    int r;
    if (e->failed) return e->failed;
    if (e->eos_sent) return VCENC_OK;
    if ((r = vcenc_poll(e)) != VCENC_OK) return r;
    if (!e->sent_any) {                    /* nothing sent: the end at once */
        e->eos_sent = e->eos_back = 1;
        return VCENC_OK;
    }
    for (int i = 0; i < e->nin; i++)
        if (!e->in_busy[i]) {
            if (buffer_to_vc(e, &e->in_info, i, &e->in[i], 0, FLAG_EOS, TIME_UNKNOWN)) return e->failed;
            e->in_busy[i] = 1;
            e->eos_sent = 1;
            e->last_heard = now_cs();
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
        if (!n) return fail(e, VCENC_ERROR, "Out of memory for a %u byte packet", (unsigned)need);
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
        if ((r = received(e, p->seq)) < 0) return fail(e, VCENC_ERROR, "A bulk receive was aborted");
        if (!r) return VCENC_AGAIN;
        if (p->length && pkt_add(e, p)) return e->failed;
        end = (p->flags & (FLAG_FRAME_END | FLAG_CONFIG | FLAG_EOS)) != 0;
        release_head(e);
        if (!end || !e->pkt_len) continue;
        if (e->pkt_flags & VCENC_PKT_CONFIG) {   /* SPS and PPS: kept for vcenc_headers */
            uint8_t *h = realloc(e->hdr, e->pkt_len);
            int again = e->hdr_len != 0;
            if (!h) return fail(e, VCENC_ERROR, "Out of memory");
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
                if (!n) return fail(e, VCENC_ERROR, "Out of memory");
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
