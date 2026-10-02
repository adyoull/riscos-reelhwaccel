/*
 * vcdec - H.264 decoded by the VideoCore, from RISC OS (see vcdec.h).
 *
 * MMAL over VCHIQ to the firmware's ril.video_decode, as MMALDecode 0.12 to
 * 0.17 found it works on a Pi 4:
 *
 *   1. buffers are numbered from 1 for the VideoCore (client_context; 0 is
 *      "no buffer" to it);
 *   2. the output's buffers are handed over only after the decoder's first
 *      format change (EFCH), or 200 cs after the first input anyway; a
 *      cmd 0 "event" carries no data;
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
 *      decoded come back during it and are dropped. After an EOS a flush
 *      isn't enough (the next EOS loses the reorder-delay pictures), so the
 *      component is destroyed and created again;
 *   7. gpu_mem must be 128 or more for 1080p;
 *   8. pictures are copied out of PCI memory by LDM/STM (vcdec_copy.S).
 *
 * Message layouts (no code copied): Raspberry Pi userland's MMAL client,
 * interface/mmal/vc/mmal_vc_msgs.h and mmal_buffer.h (Broadcom,
 * BSD-3-Clause), and Linux's vchiq-mmal, mmal-msg*.h (GPL-2.0).
 * Part of riscos-reelhwaccel. GPL version 2 or later (see COPYING).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "vcdec.h"

#define OS_Module                 0x1E
#define OS_SWINumberFromString    0x39
#define OS_ReadMonotonicTime      0x42
#define OS_DynamicArea            0x66
#define OS_Memory                 0x68
#define OS_MMUControl             0x6B
#define DA_NOT_DRAGGABLE          (1u << 7)
#define DA_SPECIFIC_PAGES         (1u << 8)
#define DA_PMP                    (1u << 20)   /* (RISC OS 5: a Physical Memory Pool) */
#define PAGE_NOT_BUFFERABLE       (1u << 4)
#define PAGE_NOT_CACHEABLE        (1u << 5)
#define PAGE_LOCK                 (1u << 15)
#define VC_RAM_TOP                0x40000000u  /* the VideoCore reaches the first 1 GB */
#define ARMOP_CACHE_CLEAN_INVALIDATE_RANGE 21
#define OS_SynchroniseCodeAreas   0x6E
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
#define EV_FORMAT_CHANGED FOURCC_LE('E', 'F', 'C', 'H')
#define EV_ERROR       FOURCC_LE('E', 'R', 'R', 'O')

enum { T_COMPONENT_CREATE = 4, T_COMPONENT_DESTROY, T_COMPONENT_ENABLE, T_COMPONENT_DISABLE, T_PORT_INFO_GET,
       T_PORT_INFO_SET, T_PORT_ACTION, T_BUFFER_FROM_HOST, T_BUFFER_TO_HOST, T_EVENT_TO_HOST = 16 };
enum { PORT_INPUT = 2, PORT_OUTPUT = 3 };
enum { ACTION_ENABLE = 1, ACTION_DISABLE = 2, ACTION_FLUSH = 3 };
enum { ES_VIDEO = 3 };
#define FLAG_EOS           1u
#define FLAG_FRAME_START   2u
#define FLAG_FRAME_END     4u
#define FLAG_KEYFRAME      8u
#define FLAG_DISCONTINUITY 0x10u
#define TIME_UNKNOWN   0x8000000000000000ull
#define SHORT_DATA     128

#define IN_BUFS        20
#define IN_SIZE        (64 * 1024)
#define OUT_BUFS_DEFAULT 3                 /* (MMALDecode's; the decoder asks for 1) */
#define MAX_OUT        8
#define CTX_BASE       1
#define REPLY_CS       300
#define EFCH_WAIT_CS   200
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
typedef struct {
    uint32_t buffer_size_min, buffer_num_min, buffer_size_recommended, buffer_num_recommended, es_ptr;
    fmt_t format;
    video_t video;
} format_changed_t;

typedef char chk_hdr[sizeof(hdr_t) == 24 ? 1 : -1];
typedef char chk_port[sizeof(port_t) == 64 ? 1 : -1];
typedef char chk_video[sizeof(video_t) == 44 ? 1 : -1];
typedef char chk_buf[offsetof(buffer_msg_t, pts) == 64 && sizeof(buffer_msg_t) == 272 ? 1 : -1];
typedef char chk_set[sizeof(port_set_t) == 280 ? 1 : -1];

#ifdef VCDEC_HOST
_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r);   /* the host tests' fake RISC OS */
#define vc_swi probe_swi
#else
static _kernel_oserror *vc_swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }
#endif
void vcdec_svc_copy(void *dst, const void *src, size_t n);                       /* vcdec_copy.S */
void vcdec_copy_rows(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride, int width, int rows, int mode);
void vcdec_svc_call(uint32_t fn, uint32_t r0, uint32_t r1);
#define COPY_SVC 4                         /* (vcdec_copy_rows's mode: in SVC mode) */

/* the output buffers: with us and empty, with the decoder, or holding a
   picture (or the EOS) that is still arriving or not yet taken */
enum { OB_FREE, OB_VC, OB_HELD };

typedef struct {                           /* a picture (or the EOS) that has come back */
    int buf;
    uint32_t seq;                          /* its receive: done when the callback's count reaches it */
    uint32_t length, flags;
    uint64_t pts;
    uint32_t offsets[3], pitch[3];         /* where its planes are in the buffer */
    int width, height;                     /* visible */
} held_t;

struct vcdec {
    vcdec_config cfg;
    char err[200];
    int failed;                            /* VCDEC_ERROR or VCDEC_UNSUPPORTED from now on (0: none) */
    uint32_t instance, handle, stub, comp, context;
    int connected, opened, created, enabled, in_on, out_on;
    port_info_t in_info, out_info;
    uint32_t msg[128];
    const char *waiting;                   /* (for the log) */
    uint32_t pci_alloc_swi, pci_free_swi;
    uint8_t *in_buf[IN_BUFS];
    int in_busy[IN_BUFS];
    int nout;                              /* output buffers (cfg.out_buffers, or 3) */
    uint8_t *out_buf[MAX_OUT];
    uint32_t out_cap[MAX_OUT], out_need;
    int out_state[MAX_OUT];
    uint32_t out_area[MAX_OUT];            /* VCDEC_OUT_PMP: each buffer's pool (dynamic area number) */
    uint32_t armop_cci;                    /* VCDEC_OUT_PMP, cached: the kernel's Cache_CleanInvalidateRange */
    int copy_way;                          /* vcdec_copy_rows's way: 0 LDM 4, 1 LDM 8, 2 NEON */
    int pmp_svc;                           /* a pool not readable in USR mode (OS_Memory 24): copied in SVC */
    int pool_stuck;                        /* a pool couldn't be removed: the RMA block (its handler) is kept */
    held_t held[MAX_OUT + 1];
    held_t last_ok;                        /* the last picture copied out (its buffer, layout) */
    int nheld;
    uint32_t rx_queued;                    /* receives queued (the callback counts them done) */
    uint8_t *evbuf;
    uint32_t evsize;
    int outputs_given, sent_any, eos_sent, eos_back;
    int eos_seen;                          /* the EOS buffer has come back (not yet taken) */
    int eos_vc;                            /* an EOS has reached the decoder (a flush must create it again) */
    int heard_output;                      /* a format change or a picture since the first input */
    uint32_t t_first_send, last_heard;
    int reformat;                          /* an EFCH to another format, to act on at the top */
    format_changed_t fc;
    vcdec_stats stats;
};

static char open_err[200];

/* ---- small things ---- */

static void logf_(vcdec *d, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    if (!d->cfg.log) return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    d->cfg.log(d->cfg.log_handle, b);
}

/* the decoder has failed: every call says so from now on */
static int fail(vcdec *d, int code, const char *fmt, ...)
{
    va_list ap;
    if (d->failed) return d->failed;
    va_start(ap, fmt);
    vsnprintf(d->err, sizeof d->err, fmt, ap);
    va_end(ap);
    d->failed = code;
    logf_(d, "%s", d->err);
    return code;
}

/* a bad call: said, but nothing broken */
static int einval(vcdec *d, const char *what)
{
    snprintf(d->err, sizeof d->err, "%s", what);
    return VCDEC_EINVAL;
}

static _kernel_oserror *swi5(int n, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4,
                             uint32_t *o0, uint32_t *o2)
{
    _kernel_swi_regs r;
    _kernel_oserror *e;
    memset(&r, 0, sizeof r);
    r.r[0] = (int)r0; r.r[1] = (int)r1; r.r[2] = (int)r2; r.r[3] = (int)r3; r.r[4] = (int)r4;
    e = vc_swi(n, &r);
    if (!e) {
        if (o0) *o0 = (uint32_t)r.r[0];
        if (o2) *o2 = (uint32_t)r.r[2];
    }
    return e;
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

unsigned vcdec_gpu_mem(void)
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

/* the callback's counters, in the RMA after its code: receives done, aborted */
static void bulk_counts(vcdec *d, uint32_t v[2])
{
    vcdec_svc_copy(v, (const void *)(uintptr_t)(d->stub + 64), 8);
}

/* ---- PCI memory ---- */

static void *pci_alloc(vcdec *d, size_t n)
{
    uint32_t log = 0;
    _kernel_swi_regs r;
    if (!d->pci_alloc_swi) {
        memset(&r, 0, sizeof r);
        r.r[1] = (int)(uintptr_t)"PCI_RAMAlloc";
        if (vc_swi(OS_SWINumberFromString, &r)) return NULL;
        d->pci_alloc_swi = (uint32_t)r.r[0];
        r.r[1] = (int)(uintptr_t)"PCI_RAMFree";
        if (vc_swi(OS_SWINumberFromString, &r)) return NULL;
        d->pci_free_swi = (uint32_t)r.r[0];
    }
    if (swi((int)d->pci_alloc_swi, (uint32_t)n, 4096, 0, 0, &log, NULL) || !log) return NULL;
    return (void *)(uintptr_t)log;
}

static void pci_free(vcdec *d, void *p)
{
    if (p) swi((int)d->pci_free_swi, (uint32_t)(uintptr_t)p, 0, 0, 0, NULL, NULL);
}

/* ---- a Physical Memory Pool (VCDEC_OUT_PMP) ----
   One dynamic area per picture buffer: pages that OS_Memory 12 recommends
   (physically contiguous, below 1 GB, as VCHIQ's bulk transfers need),
   claimed (OS_DynamicArea 21, locked) and mapped in (22) user readable,
   cacheable or not. PRM: OS_DynamicArea 0/21/22, OS_Memory 12. */

static _kernel_oserror *pmp_op(vcdec *d, uint32_t reason, uint32_t area, uint32_t *list, uint32_t n)
{
    (void)d;
    return swi(OS_DynamicArea, reason, area, (uint32_t)(uintptr_t)list, n, NULL, NULL);
}

static int pmp_free(vcdec *d, uint32_t area, uint32_t pages)
{
    _kernel_oserror *e;
    uint32_t *l = malloc(pages * 12);
    if (l) {
        for (uint32_t j = 0; j < pages; j++) { l[3 * j] = j; l[3 * j + 1] = 0xFFFFFFFFu; l[3 * j + 2] = 0; }
        pmp_op(d, 22, area, l, pages);     /* unmapped, */
        pmp_op(d, 21, area, l, pages);     /* released, */
        free(l);
    }
    if ((e = swi(OS_DynamicArea, 1, area, 0, 0, NULL, NULL)) != NULL) {   /* removed */
        logf_(d, "A picture pool (area %u) can't be removed: %s", (unsigned)area, e->errmess);
        d->pool_stuck = 1;                 /* (its handler, in the RMA block, must stay) */
        return -1;
    }
    return 0;
}

static void *pmp_alloc(vcdec *d, uint32_t size, uint32_t *area_out)
{
    uint32_t pages = (size + 4095) >> 12, first, *l, phys[2];
    int uncached = (d->cfg.flags & VCDEC_OUT_UNCACHED) != 0;
    _kernel_swi_regs r;
    _kernel_oserror *e;
    char name[32];
    void *base;
    /* the pool and the page list first: OS_Memory 12's pages are only sure
       to be free if claiming them is the next thing that takes pages */
    if (!(l = malloc(pages * 12))) { fail(d, VCDEC_ERROR, "Out of memory"); return NULL; }
    snprintf(name, sizeof name, "vcdec picture");
    memset(&r, 0, sizeof r);
    r.r[0] = 0; r.r[1] = -1; r.r[2] = 0; r.r[3] = -1;
    r.r[4] = (int)(DA_SPECIFIC_PAGES | DA_PMP | DA_NOT_DRAGGABLE);   /* (access 0: user read/write) */
    r.r[5] = (int)(pages << 12); r.r[6] = (int)(d->stub + 72); r.r[7] = 0;
    r.r[8] = (int)(uintptr_t)name; r.r[9] = (int)pages;
    if ((e = vc_swi(OS_DynamicArea, &r)) != NULL) { free(l); fail(d, VCDEC_ERROR, "A Physical Memory Pool: %s", e->errmess); return NULL; }
    *area_out = (uint32_t)r.r[1];
    base = (void *)(uintptr_t)(uint32_t)r.r[3];
    memset(&r, 0, sizeof r);               /* contiguous pages for DMA below 1 GB (R4-R7: RISC OS 5.29+) */
    r.r[0] = 12 | 1 << 8 | 1 << 9; r.r[1] = (int)(pages << 12); r.r[2] = 12;
    r.r[4] = 0; r.r[5] = 0; r.r[6] = (int)(VC_RAM_TOP - 1); r.r[7] = 0;
    if ((e = vc_swi(OS_Memory, &r)) != NULL) {
        free(l); pmp_free(d, *area_out, 0);
        fail(d, VCDEC_ERROR, "OS_Memory 12 (%u pages): %s", (unsigned)pages, e->errmess);
        return NULL;
    }
    first = (uint32_t)r.r[3];
    for (uint32_t j = 0; j < pages; j++) { l[3 * j] = j; l[3 * j + 1] = first + j; l[3 * j + 2] = PAGE_LOCK; }
    if ((e = pmp_op(d, 21, *area_out, l, pages)) != NULL) {
        free(l); pmp_free(d, *area_out, pages);
        fail(d, VCDEC_ERROR, "Claiming %u pages from %u: %s", (unsigned)pages, (unsigned)first, e->errmess);
        return NULL;
    }
    for (uint32_t j = 0; j < pages; j++) {
        l[3 * j] = j; l[3 * j + 1] = j;
        l[3 * j + 2] = PAGE_LOCK | (uncached ? PAGE_NOT_CACHEABLE : 0);   /* (access 0; uncached: still bufferable, */
                                                                           /* as PCI_RAMAlloc memory) */
    }
    if ((e = pmp_op(d, 22, *area_out, l, pages)) != NULL) {
        free(l); pmp_free(d, *area_out, pages);
        fail(d, VCDEC_ERROR, "Mapping %u pages: %s", (unsigned)pages, e->errmess);
        return NULL;
    }
    free(l);
    /* (checked: the first and last pages' physical addresses, as VCHIQ will see them) */
    for (int k = 0; k < 2; k++) {
        uint32_t blk[3] = { 0, (uint32_t)(uintptr_t)base + (k ? (pages - 1) << 12 : 0), 0 };
        if ((e = swi(OS_Memory, 0x2200, (uint32_t)(uintptr_t)blk, 1, 0, NULL, NULL)) != NULL) {
            pmp_free(d, *area_out, pages);
            fail(d, VCDEC_ERROR, "OS_Memory 0: %s", e->errmess);
            return NULL;
        }
        phys[k] = blk[2];
    }
    if (phys[1] != phys[0] + ((pages - 1) << 12) || phys[1] > VC_RAM_TOP - 4096) {
        pmp_free(d, *area_out, pages);
        fail(d, VCDEC_ERROR, "The pool's pages aren't contiguous below 1 GB (&%08X..&%08X)", (unsigned)phys[0],
             (unsigned)phys[1]);
        return NULL;
    }
    {   /* readable in USR mode? (if not, it's copied in SVC mode: no abort part way) */
        uint32_t acc = 0;
        memset(&r, 0, sizeof r);
        r.r[0] = 24; r.r[1] = (int)(uintptr_t)base; r.r[2] = (int)((uintptr_t)base + (pages << 12));
        if (!vc_swi(OS_Memory, &r)) acc = (uint32_t)r.r[1];   /* (the flags come back in R1) */
        if (!(acc & 1)) {
            if (!d->pmp_svc) logf_(d, "The pool isn't readable in USR mode (OS_Memory 24: &%X): copied in SVC mode", (unsigned)acc);
            d->pmp_svc = 1;
        }
    }
    logf_(d, "A picture buffer in a Physical Memory Pool: area %u, %u pages at &%08X, physical &%08X, %s",
          (unsigned)*area_out, (unsigned)pages, (unsigned)(uintptr_t)base, (unsigned)phys[0],
          uncached ? "not cacheable" : "cacheable");
    return base;
}

/* output buffer i made `size` bytes (any old one freed): 0, or -1 failed */
static int out_alloc(vcdec *d, int i, uint32_t size)
{
    if (d->out_buf[i]) {
        if (d->cfg.flags & VCDEC_OUT_PMP) pmp_free(d, d->out_area[i], (d->out_cap[i] + 4095) >> 12);
        else pci_free(d, d->out_buf[i]);
        d->out_buf[i] = NULL;
        d->out_cap[i] = 0;
    }
    if (d->cfg.flags & VCDEC_OUT_PMP) d->out_buf[i] = pmp_alloc(d, size, &d->out_area[i]);
    else if (!(d->out_buf[i] = pci_alloc(d, size)))
        fail(d, VCDEC_ERROR, "No PCI memory (PCI_RAMAlloc) for %u byte pictures", (unsigned)size);
    if (!d->out_buf[i]) return -1;
    d->out_cap[i] = size;
    return 0;
}

/* ---- messages ---- */

static int send_msg(vcdec *d, uint32_t type, const void *payload, uint32_t len)
{
    uint32_t m[128];
    hdr_t *h = (hdr_t *)m;
    _kernel_oserror *e;
    memset(m, 0, sizeof(hdr_t));
    h->magic = MMAL_MAGIC;
    h->type = type;
    h->context = ++d->context;
    memcpy(h + 1, payload, len);
    swi(VCHIQ_ServiceUse, d->handle, 0, 0, 0, NULL, NULL);
    e = swi(VCHIQ_MsgQueue, d->handle, (uint32_t)(uintptr_t)m, (uint32_t)sizeof *h + len, 0, NULL, NULL);
    swi(VCHIQ_ServiceRelease, d->handle, 0, 0, 0, NULL, NULL);
    if (e) return fail(d, VCDEC_ERROR, "VCHIQ_MsgQueue: %s", e->errmess);
    return 0;
}

/* a bulk receive of n bytes into dst, queued now; its number (the
   callback's count when it's done) */
static int queue_receive(vcdec *d, uint8_t *dst, uint32_t n, uint32_t *seq)
{
    _kernel_oserror *e;
    swi(VCHIQ_ServiceUse, d->handle, 0, 0, 0, NULL, NULL);
    e = swi5(VCHIQ_BulkQueueReceive, d->handle, (uint32_t)(uintptr_t)dst, (n + 3) & ~3u,
             VCHI_FLAGS_CALLBACK_WHEN_DONE, 0, NULL, NULL);
    swi(VCHIQ_ServiceRelease, d->handle, 0, 0, 0, NULL, NULL);
    if (e) return fail(d, VCDEC_ERROR, "VCHIQ_BulkQueueReceive: %s", e->errmess);
    *seq = ++d->rx_queued;
    return 0;
}

/* receive seq done? (-1: one was aborted) */
static int received(vcdec *d, uint32_t seq)
{
    uint32_t v[2];
    bulk_counts(d, v);                     /* (both in one copy: one SVC entry) */
    if (v[1]) return -1;
    return (int32_t)(v[0] - seq) >= 0;
}

/* waits for receive seq (and so every one before it) */
static int wait_received(vcdec *d, uint32_t seq)
{
    uint32_t t0 = now_cs();
    int r;
    while (!(r = received(d, seq)))
        if (now_cs() - t0 > REPLY_CS) return fail(d, VCDEC_ERROR, "A bulk receive didn't finish in %d cs", REPLY_CS);
    if (r < 0) return fail(d, VCDEC_ERROR, "A bulk receive was aborted");
    return 0;
}

static int give_outputs(vcdec *d);

static int buffer_to_vc(vcdec *d, port_info_t *pi, int idx, uint8_t *data, uint32_t alloc, uint32_t len,
                        uint32_t flags, uint64_t pts, uint64_t dts)
{
    buffer_msg_t b;
    memset(&b, 0, sizeof b);
    b.drvbuf.magic = MMAL_MAGIC;
    b.drvbuf.component_handle = d->comp;
    b.drvbuf.port_handle = pi->port_handle;
    b.drvbuf.client_context = (uint32_t)idx + CTX_BASE;
    b.data = (uint32_t)(uintptr_t)data;
    b.alloc_size = alloc;
    b.length = len;
    b.flags = flags;
    b.pts = pts;
    b.dts = dts;
    if (send_msg(d, T_BUFFER_FROM_HOST, &b, sizeof b)) return -1;
    if (d->cfg.flags & VCDEC_LOG_MESSAGES)
        logf_(d, "tx: %s buffer %d, %u bytes, flags &%X, pts %lld", pi == &d->in_info ? "input" : "output", idx,
              (unsigned)len, (unsigned)flags, (long long)pts);
    if (len) {
        _kernel_oserror *e;
        swi(VCHIQ_ServiceUse, d->handle, 0, 0, 0, NULL, NULL);
        e = swi5(VCHIQ_BulkQueueTransmit, d->handle, (uint32_t)(uintptr_t)data, (len + 3) & ~3u,
                 VCHI_FLAGS_BLOCK_UNTIL_QUEUED, 0, NULL, NULL);
        swi(VCHIQ_ServiceRelease, d->handle, 0, 0, 0, NULL, NULL);
        if (e) return fail(d, VCDEC_ERROR, "VCHIQ_BulkQueueTransmit: %s", e->errmess);
    }
    return 0;
}

/* One message that isn't the reply being waited for: buffers back, pictures
   (their receives queued at once: rule 3), events. Never waits for the
   decoder (a format change to another size is left for top_level). */
static void handle_msg(vcdec *d, uint32_t got)
{
    hdr_t *h = (hdr_t *)d->msg;
    if (h->type == T_BUFFER_TO_HOST) {
        buffer_msg_t *b = (buffer_msg_t *)(h + 1);
        uint32_t k = b->drvbuf.client_context - CTX_BASE;
        if (b->drvbuf.magic != MMAL_MAGIC || b->drvbuf.client_context < CTX_BASE) {
            logf_(d, "A returned buffer we don't know");
            return;
        }
        if (d->cfg.flags & VCDEC_LOG_MESSAGES)
            logf_(d, "rx%s: buffer back, status %u, port %u, buffer %u, length %u, flags &%X, pts %lld", d->waiting,
                  (unsigned)h->status, (unsigned)b->drvbuf.port_handle, (unsigned)k, (unsigned)b->length,
                  (unsigned)b->flags, (long long)b->pts);
        if (b->drvbuf.port_handle == d->in_info.port_handle && k < IN_BUFS) {
            d->in_busy[k] = 0;
            return;
        }
        if (b->drvbuf.port_handle != d->out_info.port_handle || k >= (uint32_t)d->nout) {
            logf_(d, "A buffer back from port %u", (unsigned)b->drvbuf.port_handle);
            return;
        }
        if (h->status || (!b->length && !(b->flags & FLAG_EOS))) {   /* empty: back with us */
            if (h->status) logf_(d, "Output buffer back with status %s", st(h->status));
            d->out_state[k] = OB_FREE;     /* (handed over again by vcdec_poll, unless it's stopping) */
            return;
        }
        {
            held_t *p = &d->held[d->nheld];
            uint32_t w = d->out_info.video.width, rows = d->out_info.video.height;
            if (d->nheld >= d->nout + 1) { fail(d, VCDEC_ERROR, "More pictures back than buffers"); return; }
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
                fail(d, VCDEC_ERROR, "A %u byte picture for a %u byte buffer", (unsigned)b->length, (unsigned)d->out_cap[k]);
                return;
            }
            /* the picture's bytes, or an empty EOS's 8 (as Linux's driver, to keep order) */
            if (queue_receive(d, d->out_buf[k], b->length ? b->length : 8, &p->seq)) return;
            d->out_state[k] = OB_HELD;
            d->nheld++;
            d->heard_output = 1;
            if (b->flags & FLAG_EOS) d->eos_seen = 1;
            if (d->cfg.flags & VCDEC_SYNC_RECEIVE) wait_received(d, p->seq);
        }
        return;
    }
    if (h->type == T_EVENT_TO_HOST) {
        event_msg_t *ev = (event_msg_t *)(h + 1);
        if (d->cfg.flags & VCDEC_LOG_MESSAGES)
            logf_(d, "rx%s: event &%08X, %u bytes, port type %u", d->waiting, (unsigned)ev->cmd, (unsigned)ev->length,
                  (unsigned)ev->port_type);
        if (!ev->cmd) return;              /* not an event: no data follows (rule 2) */
        if (ev->length > sizeof ev->data) {
            /* its data follows by bulk transfer: received now, and waited for (rare) */
            uint32_t seq;
            if (ev->length > d->evsize) {
                pci_free(d, d->evbuf);
                d->evbuf = pci_alloc(d, ev->length + 4);
                d->evsize = d->evbuf ? ev->length : 0;
                if (!d->evbuf) { fail(d, VCDEC_ERROR, "No PCI memory for a %u byte event", (unsigned)ev->length); return; }
            }
            if (queue_receive(d, d->evbuf, ev->length, &seq) || wait_received(d, seq)) return;
            vcdec_svc_copy(ev->data, d->evbuf, sizeof ev->data);
        }
        if (ev->cmd == EV_FORMAT_CHANGED && ev->port_type == PORT_OUTPUT) {
            format_changed_t fc;
            memcpy(&fc, ev->data, sizeof fc);
            d->stats.format_changes++;
            d->heard_output = 1;
            logf_(d, "Format changed: %ux%u (crop %dx%d), buffers %u x %u bytes", (unsigned)fc.video.width,
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
            fail(d, VCDEC_ERROR, "The decoder reports an error (port type %u)", (unsigned)ev->port_type);
        } else {
            logf_(d, "Event &%08X (%u bytes) on port type %u", (unsigned)ev->cmd, (unsigned)ev->length,
                  (unsigned)ev->port_type);
        }
        return;
    }
    logf_(d, "(a type %u message, %u bytes)", (unsigned)h->type, (unsigned)got);
}

/* one message into d->msg: its length, or 0 if none */
static uint32_t dequeue(vcdec *d)
{
    uint32_t got = 0;
    if (swi(VCHIQ_MsgDequeue, d->handle, (uint32_t)(uintptr_t)d->msg, sizeof d->msg, 0, NULL, &got)) return 0;
    if (got < sizeof(hdr_t) || ((hdr_t *)d->msg)->magic != MMAL_MAGIC) {
        logf_(d, "(a %u byte message that isn't MMAL's)", (unsigned)got);
        return 0;
    }
    d->last_heard = now_cs();
    return got;
}

/* sends and waits for the reply of the same type, handling what else comes */
static int transact(vcdec *d, uint32_t type, const void *payload, uint32_t len, const char *what)
{
    uint32_t t0, got;
    if (d->failed || send_msg(d, type, payload, len)) return -1;
    t0 = now_cs();
    d->waiting = " (waiting)";
    while (now_cs() - t0 < REPLY_CS) {
        if (!(got = dequeue(d))) continue;
        if (((hdr_t *)d->msg)->type == type) { d->waiting = ""; return (int)(got - sizeof(hdr_t)); }
        handle_msg(d, got);
        if (d->failed) { d->waiting = ""; return -1; }
    }
    d->waiting = "";
    fail(d, VCDEC_ERROR, "%s: no reply from the decoder in %d cs", what, REPLY_CS);
    return -1;
}

static uint32_t *reply(vcdec *d) { return (uint32_t *)((hdr_t *)d->msg + 1); }

static int status_ok(vcdec *d, int n, const char *what)
{
    if (n < 4) return n < 0 ? -1 : fail(d, VCDEC_ERROR, "%s: a short reply", what);
    if (reply(d)[0]) return fail(d, VCDEC_ERROR, "%s: %s", what, st(reply(d)[0]));
    return 0;
}

static int port_get(vcdec *d, uint32_t type, port_info_t *pi, const char *what)
{
    uint32_t req[3] = { d->comp, type, 0 };
    int n = transact(d, T_PORT_INFO_GET, req, sizeof req, what);
    if (status_ok(d, n, what)) return -1;
    if (n < (int)offsetof(port_info_t, extradata)) return fail(d, VCDEC_ERROR, "%s: a short reply", what);
    memcpy(pi, reply(d), sizeof *pi);
    return 0;
}

static int port_set(vcdec *d, uint32_t type, port_info_t *pi, const char *what)
{
    port_set_t s;
    int n;
    memset(&s, 0, sizeof s);
    s.component_handle = d->comp;
    s.port_type = type;
    s.port = pi->port;
    s.format = pi->format;
    s.format.extradata_size = 0;
    s.video = pi->video;
    n = transact(d, T_PORT_INFO_SET, &s, sizeof s, what);
    if (status_ok(d, n, what)) return -1;
    if (n >= (int)offsetof(port_info_t, extradata)) {
        port_info_t *r = (port_info_t *)reply(d);
        pi->port = r->port;                /* the firmware's view (sizes may grow) */
        pi->format = r->format;
        pi->video = r->video;
    }
    return 0;
}

static int port_action(vcdec *d, port_info_t *pi, uint32_t action, const char *what)
{
    uint32_t req[3 + 16];
    memset(req, 0, sizeof req);
    req[0] = d->comp; req[1] = pi->port_handle; req[2] = action;
    memcpy(&req[3], &pi->port, sizeof pi->port);
    return status_ok(d, transact(d, T_PORT_ACTION, req, sizeof req, what), what);
}

static int simple(vcdec *d, uint32_t type, const char *what)
{
    return status_ok(d, transact(d, type, &d->comp, 4, what), what);
}

/* ---- the output ---- */

static int give_outputs(vcdec *d)
{
    for (int i = 0; i < d->nout; i++)
        if (d->out_state[i] == OB_FREE) {
            if (d->out_cap[i] < d->out_need && out_alloc(d, i, d->out_need))   /* (a picture was in it */
                return d->failed;                                              /* when the size grew) */
            if (buffer_to_vc(d, &d->out_info, i, d->out_buf[i], d->out_cap[i], 0, 0, TIME_UNKNOWN, TIME_UNKNOWN))
                return -1;
            d->out_state[i] = OB_VC;
        }
    return 0;
}

/* the output port's format set, and its buffers big enough (those holding a
   picture are grown when it's been taken) */
static int configure_output(vcdec *d)
{
    uint32_t need;
    if (port_set(d, PORT_OUTPUT, &d->out_info, "Output format")) return -1;
    need = d->out_info.port.buffer_size;
    if (d->out_info.port.buffer_size_recommended > need) need = d->out_info.port.buffer_size_recommended;
    if (d->out_info.port.buffer_size_min > need) need = d->out_info.port.buffer_size_min;
    d->out_need = need;
    for (int i = 0; i < d->nout; i++)
        if (d->out_state[i] == OB_FREE && d->out_cap[i] < need && out_alloc(d, i, need))
            return d->failed;
    if (d->out_info.port.buffer_size != need) {
        d->out_info.port.buffer_size = need;
        d->out_info.port.buffer_num = d->nout;
        if (port_set(d, PORT_OUTPUT, &d->out_info, "Output buffers")) return -1;
    }
    return 0;
}

/* an EFCH to another format: the output disabled (its buffers come back),
   the new format set, enabled, the buffers handed over again */
static int do_reformat(vcdec *d)
{
    format_changed_t *fc = &d->fc;
    d->reformat = 0;
    if (port_action(d, &d->out_info, ACTION_DISABLE, "Output disable")) return -1;
    d->out_on = 0;
    for (int i = 0; i < d->nout; i++) if (d->out_state[i] == OB_VC) d->out_state[i] = OB_FREE;
    d->out_info.format = fc->format;
    d->out_info.video = fc->video;
    d->out_info.port.buffer_num = d->nout;
    d->out_info.port.buffer_size = fc->buffer_size_recommended > fc->buffer_size_min ?
                                   fc->buffer_size_recommended : fc->buffer_size_min;
    if (configure_output(d) || port_action(d, &d->out_info, ACTION_ENABLE, "Output enable")) return -1;
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
    uint32_t req[1 + 32 + 1];
    int n;
    memset(req, 0, sizeof req);
    req[0] = 1;
    strcpy((char *)&req[1], "ril.video_decode");
    n = transact(d, T_COMPONENT_CREATE, req, sizeof req, "Create");
    if (n < 0) {
        if (strstr(d->err, "no reply"))
            snprintf(d->err, sizeof d->err, "The VideoCore's MMAL service isn't answering (if something stopped "
                     "part way, it can stay stuck until the machine is restarted)");
        return -1;
    }
    if (n < 8 || reply(d)[0]) return fail(d, VCDEC_ERROR, "Create: %s", n >= 4 ? st(reply(d)[0]) : "a short reply");
    d->comp = reply(d)[1];
    d->created = 1;
    if (port_get(d, PORT_INPUT, &d->in_info, "Input port") || port_get(d, PORT_OUTPUT, &d->out_info, "Output port"))
        return -1;
    d->in_info.format.type = ES_VIDEO;
    d->in_info.format.encoding = ENC_H264;
    d->in_info.video.width = (uint32_t)d->cfg.width;
    d->in_info.video.height = (uint32_t)d->cfg.height;
    d->in_info.port.buffer_num = IN_BUFS;
    d->in_info.port.buffer_size = IN_SIZE;
    if (port_set(d, PORT_INPUT, &d->in_info, "Input format")) return -1;
    d->out_info.format.type = ES_VIDEO;
    d->out_info.format.encoding = ENC_I420;
    d->out_info.video.width = ((uint32_t)d->cfg.width + 31) & ~31u;
    d->out_info.video.height = ((uint32_t)d->cfg.height + 15) & ~15u;
    d->out_info.video.crop[0] = d->out_info.video.crop[1] = 0;
    d->out_info.video.crop[2] = d->cfg.width;
    d->out_info.video.crop[3] = d->cfg.height;
    d->out_info.port.buffer_num = d->nout;
    d->out_info.port.buffer_size = d->out_info.video.width * d->out_info.video.height * 3 / 2;
    if (configure_output(d)) return -1;
    if (simple(d, T_COMPONENT_ENABLE, "Enable")) return -1;
    d->enabled = 1;
    if (port_action(d, &d->in_info, ACTION_ENABLE, "Input enable")) return -1;
    d->in_on = 1;
    if (port_action(d, &d->out_info, ACTION_ENABLE, "Output enable")) return -1;
    d->out_on = 1;
    /* (the output's buffers: after the decoder's first format change) */
    d->outputs_given = d->sent_any = d->eos_sent = d->eos_back = d->eos_seen = d->heard_output = d->reformat = 0;
    d->eos_vc = 0;
    return 0;
}

/* ports off, the component off and gone (each step only if it was done).
   closing: every step is tried even if one before it failed (a component
   left on the VideoCore keeps its memory) */
static void destroy_component(vcdec *d, int closing)
{
    if (closing) d->failed = 0;
    if (d->out_on && !port_action(d, &d->out_info, ACTION_DISABLE, "Output disable")) d->out_on = 0;
    if (closing) d->failed = 0;
    if (d->in_on && !port_action(d, &d->in_info, ACTION_DISABLE, "Input disable")) d->in_on = 0;
    if (closing) d->failed = 0;
    if (d->enabled && !simple(d, T_COMPONENT_DISABLE, "Disable")) d->enabled = 0;
    if (closing) d->failed = 0;
    if (d->created && !simple(d, T_COMPONENT_DESTROY, "Destroy")) d->created = 0;
}

/* ---- the API ---- */

void vcdec_config_init(vcdec_config *c)
{
    memset(c, 0, sizeof *c);
}

const char *vcdec_open_error(void) { return open_err; }
const char *vcdec_error(const vcdec *d) { return d ? d->err : open_err; }

void vcdec_get_stats(const vcdec *d, vcdec_stats *s) { *s = d->stats; }

int vcdec_open(vcdec **out, const vcdec_config *c)
{
    static const uint32_t code[9] = { 0xe3510004u, 0x05903000u, 0x02833001u, 0x05803000u, 0xe3510012u,
                                      0x05903004u, 0x02833001u, 0x05803004u, 0xe1a0f00eu };
    /* (the callback: R1 = 4, a receive done: [R0] += 1; R1 = 18, aborted:
       [R0 + 4] += 1; R0 is its param, the two words after the code) */
    vcdec *d;
    unsigned mb;
    uint32_t setup[11];
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
    if (!(d = calloc(1, sizeof *d))) {
        snprintf(open_err, sizeof open_err, "Out of memory");
        return VCDEC_ERROR;
    }
    d->cfg = *c;
    d->nout = c->out_buffers > 0 ? (c->out_buffers < MAX_OUT ? c->out_buffers : MAX_OUT) : OUT_BUFS_DEFAULT;
    d->copy_way = c->flags & VCDEC_COPY_NEON ? 2 : c->flags & VCDEC_COPY_LDM8 ? 1 : 0;
    if (d->copy_way == 2 && !(c->flags & VCDEC_OUT_PMP))
        logf_(d, "NEON is only used in USR mode: pictures from PCI memory are copied by LDM 8 instead");
    d->waiting = "";
    logf_(d, "vcdec %s: %dx%d; the VideoCore has %u MB (gpu_mem)", VCDEC_VERSION, c->width, c->height, mb);
    for (int i = 0; i < IN_BUFS; i++)
        if (!(d->in_buf[i] = pci_alloc(d, IN_SIZE))) {
            fail(d, VCDEC_ERROR, "No physically contiguous memory (PCI_RAMAlloc; is the PCI module loaded?)");
            goto bad;
        }
    if ((e = swi(OS_Module, 6, 0, 0, 80, NULL, &d->stub)) != NULL) { fail(d, VCDEC_ERROR, "No RMA: %s", e->errmess); goto bad; }
    {
        uint32_t img[20];
        _kernel_swi_regs rr;
        memset(img, 0, sizeof img);
        memcpy(img, code, sizeof code);
        img[18] = 0xe1a0f00eu;             /* +72: the pools' dynamic area handler: MOV PC, R14 */
        vcdec_svc_copy((void *)(uintptr_t)d->stub, img, sizeof img);
        memset(&rr, 0, sizeof rr);
        rr.r[0] = 1; rr.r[1] = (int)d->stub; rr.r[2] = (int)(d->stub + 76);
        vc_swi(OS_SynchroniseCodeAreas, &rr);
    }
    if ((c->flags & (VCDEC_OUT_PMP | VCDEC_OUT_UNCACHED)) == VCDEC_OUT_PMP &&
        (e = swi(OS_MMUControl, 2 | ARMOP_CACHE_CLEAN_INVALIDATE_RANGE << 8, 0, 0, 0, &d->armop_cci, NULL)) != NULL) {
        fail(d, VCDEC_ERROR, "OS_MMUControl 2 (Cache_CleanInvalidateRange; RISC OS 5.23 or later): %s", e->errmess);
        goto bad;
    }
    if ((e = swi(VCHIQ_Initialise, 0, 0, 0, 0, &d->instance, NULL)) != NULL) { fail(d, VCDEC_ERROR, "VCHIQ_Initialise: %s", e->errmess); goto bad; }
    if ((e = swi(VCHIQ_Connect, 0, 0, d->instance, 0, NULL, NULL)) != NULL) { fail(d, VCDEC_ERROR, "VCHIQ_Connect: %s", e->errmess); goto bad; }
    d->connected = 1;
    memset(setup, 0, sizeof setup);
    setup[0] = 15; setup[1] = 10; setup[2] = MMAL_SERVICE;
    setup[6] = d->stub; setup[7] = d->stub + 64;
    if ((e = swi(VCHIQ_ServiceOpen, d->instance, (uint32_t)(uintptr_t)setup, 0, 0, &d->handle, NULL)) != NULL) {
        fail(d, VCDEC_ERROR, "VCHIQ_ServiceOpen \"mmal\": %s", e->errmess);
        goto bad;
    }
    d->opened = 1;
    swi(VCHIQ_ServiceRelease, d->handle, 0, 0, 0, NULL, NULL);   /* (opening leaves it in use) */
    if (create_component(d)) goto bad;
    *out = d;
    return VCDEC_OK;
bad:
    r = d->failed ? d->failed : VCDEC_ERROR;
    snprintf(open_err, sizeof open_err, "%s", d->err);
    vcdec_close(d);
    return r;
}

void vcdec_close(vcdec *d)
{
    _kernel_oserror *e;
    if (!d) return;
    destroy_component(d, 1);
    if (d->opened && (e = swi(VCHIQ_ServiceClose, d->handle, 0, 0, 0, NULL, NULL)) != NULL) {
        logf_(d, "VCHIQ_ServiceClose: %s (the callback is left in the RMA)", e->errmess);
        d->stub = 0;                       /* (it may still be called) */
    }
    if (d->connected) swi(VCHIQ_Disconnect, d->instance, 0, 0, 0, NULL, NULL);
    for (int i = 0; i < IN_BUFS; i++) pci_free(d, d->in_buf[i]);
    for (int i = 0; i < d->nout; i++)      /* (the pools before the RMA block: it holds their handler) */
        if (d->out_buf[i]) {
            if (d->cfg.flags & VCDEC_OUT_PMP) pmp_free(d, d->out_area[i], (d->out_cap[i] + 4095) >> 12);
            else pci_free(d, d->out_buf[i]);
        }
    pci_free(d, d->evbuf);
    if (d->stub && !d->pool_stuck) swi(OS_Module, 7, 0, d->stub, 0, NULL, NULL);
    free(d);
}

/* takes in whatever the decoder has sent; acts on a format change; hands
   the output buffers over if the decoder hasn't said its format in time;
   notices a decoder that has stopped */
int vcdec_poll(vcdec *d)
{
    uint32_t got, now;
    if (d->failed) return d->failed;
    for (;;) {
        /* (a format change can also have come while waiting for a reply) */
        if (d->reformat && do_reformat(d)) return d->failed ? d->failed : VCDEC_ERROR;
        if (!(got = dequeue(d))) break;
        handle_msg(d, got);
        if (d->failed) return d->failed;
    }
    now = now_cs();
    if (d->sent_any && !d->outputs_given && now - d->t_first_send > EFCH_WAIT_CS) {
        logf_(d, "No format change from the decoder in %d cs: the output buffers handed over anyway", EFCH_WAIT_CS);
        d->outputs_given = 1;
    }
    /* empty buffers (back unused, or after a format change) to the decoder */
    if (d->outputs_given && d->out_on && !d->eos_seen && give_outputs(d)) return d->failed;
    /* A decoder that has stopped: nothing at all since the first input (as
       at gpu_mem=64 with 1080p), or nothing after the EOS with a buffer to
       fill. (Not part way: a decoder can keep input while it waits for
       more, and a paused player sends none.) */
    if (d->sent_any && !d->heard_output && now - d->t_first_send > STALL_CS)
        return fail(d, VCDEC_ERROR, "Nothing from the decoder in %d cs (the VideoCore has %u MB: 1080p needs "
                    "gpu_mem=128)", STALL_CS, vcdec_gpu_mem());
    if (d->eos_vc && !d->eos_seen && now - d->last_heard > STALL_CS) {
        int outs = 0;
        for (int i = 0; i < d->nout; i++) outs += d->out_state[i] == OB_VC;
        if (outs) return fail(d, VCDEC_ERROR, "Nothing from the decoder for %d cs after the EOS", STALL_CS);
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
    if (d->failed) return d->failed;
    if (!len) return einval(d, "An empty access unit");
    if (d->eos_sent) return einval(d, "Sent after the EOS (flush first)");
    padded = ((uint32_t)len + 3) & ~3u;
    pieces = (padded + IN_SIZE - 1) / IN_SIZE;
    if (len > (size_t)IN_BUFS * IN_SIZE) return einval(d, "An access unit larger than the decoder's input");
    if (vcdec_check_stream(data, len) == VCDEC_UNSUPPORTED)
        return fail(d, VCDEC_UNSUPPORTED, "The stream's profile isn't one the VideoCore decodes (Baseline, Main, High)");
    if ((r = vcdec_poll(d)) != VCDEC_OK) return r;
    for (int i = 0; i < IN_BUFS; i++) free_n += !d->in_busy[i];
    if ((uint32_t)free_n < pieces) return VCDEC_AGAIN;
    if (!d->sent_any) { d->sent_any = 1; d->t_first_send = now_cs(); }
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
        if (buffer_to_vc(d, &d->in_info, i, d->in_buf[i], IN_SIZE, n, fl, p, q)) return d->failed;
        d->in_busy[i] = 1;
        d->last_heard = now_cs();          /* (the decoder has work: the stall clock starts) */
        off += n;
    }
    d->stats.sent++;
    return VCDEC_OK;
}

int vcdec_send_eos(vcdec *d)
{
    int r;
    if (d->failed) return d->failed;
    if (d->eos_sent) return VCDEC_OK;
    if ((r = vcdec_poll(d)) != VCDEC_OK) return r;
    if (!d->sent_any) {                    /* nothing sent: the end at once, the decoder not told */
        d->eos_sent = d->eos_back = 1;
        return VCDEC_OK;
    }
    for (int i = 0; i < IN_BUFS; i++)
        if (!d->in_busy[i]) {
            if (!d->sent_any) { d->sent_any = 1; d->t_first_send = now_cs(); }
            if (buffer_to_vc(d, &d->in_info, i, d->in_buf[i], IN_SIZE, 0, FLAG_EOS, TIME_UNKNOWN, TIME_UNKNOWN))
                return d->failed;
            d->in_busy[i] = 1;
            d->eos_sent = d->eos_vc = 1;
            d->last_heard = now_cs();
            return VCDEC_OK;
        }
    return VCDEC_AGAIN;
}

/* the head picture, if its receive is done: 1; 0 if none yet; VCDEC_EOF;
   or the failure */
static int head_ready(vcdec *d)
{
    int r;
    for (;;) {
        if (d->failed) return d->failed;
        if (!d->nheld) return d->eos_back ? VCDEC_EOF : 0;
        if ((r = received(d, d->held[0].seq)) < 0) return fail(d, VCDEC_ERROR, "A bulk receive was aborted");
        if (!r) return 0;
        if (!(d->held[0].flags & FLAG_EOS) || d->held[0].length) return 1;
        release_head(d);                   /* an empty EOS: the end */
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
static void copy_picture(vcdec *d, const held_t *p, uint8_t *const planes[3], const int strides[3], int way)
{
    const uint8_t *b = d->out_buf[p->buf];
    int cw = (p->width + 1) / 2, ch = (p->height + 1) / 2;
    int svc = !(d->cfg.flags & VCDEC_OUT_PMP) || d->pmp_svc;
    /* (NEON only in USR mode: in SVC mode an IRQ could leave the VFP context
       lazily switched off, and a trap there isn't VFPSupport's to mend) */
    int mode = (svc && way == 2 ? 1 : way) | (svc ? COPY_SVC : 0);
    if (d->armop_cci)
        vcdec_svc_call(d->armop_cci, (uint32_t)(uintptr_t)b, ((uint32_t)(uintptr_t)b + p->length + 63) & ~63u);
    vcdec_copy_rows(planes[0], strides[0], b + p->offsets[0], (int)p->pitch[0], p->width, p->height, mode);
    vcdec_copy_rows(planes[1], strides[1], b + p->offsets[1], (int)p->pitch[1], cw, ch, mode);
    vcdec_copy_rows(planes[2], strides[2], b + p->offsets[2], (int)p->pitch[2], cw, ch, mode);
}

int vcdec_copy_benchmark(vcdec *d, unsigned way, int reps, uint8_t *const planes[3], const int strides[3],
                         unsigned *cs)
{
    uint32_t t0;
    if (!d->last_ok.length || !d->out_buf[d->last_ok.buf]) return einval(d, "No picture received yet to time");
    t0 = now_cs();
    for (int i = 0; i < reps; i++)
        copy_picture(d, &d->last_ok, planes, strides, way & VCDEC_COPY_NEON ? 2 : way & VCDEC_COPY_LDM8 ? 1 : 0);
    *cs = now_cs() - t0;
    return VCDEC_OK;
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
        int cw = (p->width + 1) / 2, ch = (p->height + 1) / 2;
        if ((uint32_t)p->width > p->pitch[0] || (uint32_t)cw > p->pitch[1] || (uint32_t)cw > p->pitch[2] ||
            p->offsets[0] + p->pitch[0] * (uint32_t)p->height > p->length ||
            p->offsets[1] + p->pitch[1] * (uint32_t)ch > p->length || p->offsets[2] + p->pitch[2] * (uint32_t)ch > p->length) {
            release_head(d);
            return fail(d, VCDEC_ERROR, "A %u byte picture too small for %dx%d", (unsigned)p->length, p->width, p->height);
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
    int r;
    if ((r = vcdec_poll(d)) != VCDEC_OK) return r;
    d->stats.flushes++;
    if (d->eos_sent && !d->eos_vc) {       /* an EOS with nothing before it: nothing to undo */
        d->eos_sent = d->eos_back = 0;
        return VCDEC_OK;
    }
    if (d->eos_sent) {
        /* after an EOS a flush leaves the decoder losing its last pictures
           at the next EOS (0.14-0.17 on the Pi): a new one instead */
        destroy_component(d, 0);
        if (d->failed) return d->failed;
        for (int i = 0; i < IN_BUFS; i++) d->in_busy[i] = 0;
    } else {
        /* the pictures already decoded come back during these (rule 6) */
        if (port_action(d, &d->in_info, ACTION_FLUSH, "Input flush") ||
            port_action(d, &d->out_info, ACTION_FLUSH, "Output flush"))
            return d->failed;
    }
    /* every picture held dropped, once its receive is done */
    if (d->nheld && wait_received(d, d->held[d->nheld - 1].seq)) return d->failed;
    d->stats.discarded += (unsigned)d->nheld;
    d->nheld = 0;
    for (int i = 0; i < d->nout; i++)     /* (the flush or the disables sent back the decoder's) */
        if (d->out_state[i] == OB_HELD || d->eos_sent) d->out_state[i] = OB_FREE;
    d->eos_back = d->eos_seen = 0;
    if (d->eos_sent) {
        d->stats.recreated++;
        logf_(d, "Flush after the EOS: the decoder created again");
        if (create_component(d)) return d->failed;
        return VCDEC_OK;
    }
    /* (vcdec_poll acts on a format change that came during the flush) */
    return vcdec_poll(d);
}
