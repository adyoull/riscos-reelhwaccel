/*
 * vcmmal - the MMAL connection vcdec and vcenc share (see vcmmal.h).
 * This is the code each had a copy of until vcdec 0.4.4 / vcenc 0.1.5,
 * moved here as it was (Pi-proven), with what differed between them made
 * a parameter: the component's name in messages, the pools' name and
 * flags, the port's name in the message log.
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcmmal.h"
#include "../common/contig.h"

#define DA_NOT_DRAGGABLE          (1u << 7)
#define DA_SPECIFIC_PAGES         (1u << 8)
#define DA_PMP                    (1u << 20)   /* (RISC OS 5: a Physical Memory Pool) */
#define PAGE_NOT_CACHEABLE        (1u << 5)
#define PAGE_LOCK                 (1u << 15)
#define VC_RAM_TOP                0x40000000u  /* the VideoCore reaches the first 1 GB */
#define ARMOP_CACHE_CLEAN_INVALIDATE_RANGE 21
#define TAG_GET_VC_MEMORY         0x00010006
#define VCHI_FLAGS_CALLBACK_WHEN_DONE 6
#define VCHI_FLAGS_BLOCK_UNTIL_QUEUED 4

typedef char chk_hdr[sizeof(hdr_t) == 24 ? 1 : -1];
typedef char chk_port[sizeof(port_t) == 64 ? 1 : -1];
typedef char chk_video[sizeof(video_t) == 44 ? 1 : -1];
typedef char chk_buf[offsetof(buffer_msg_t, pts) == 64 && sizeof(buffer_msg_t) == 272 ? 1 : -1];
typedef char chk_set[sizeof(port_set_t) == 280 ? 1 : -1];

/* ---- SWIs, time, small things ---- */

#ifdef VCDEC_HOST
_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r);   /* the host tests' fake RISC OS */
_kernel_oserror *vcm_swi(int n, _kernel_swi_regs *r) { return probe_swi(n, r); }
#else
_kernel_oserror *vcm_swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }
#endif

_kernel_oserror *vcm_swi5(int n, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4, uint32_t *o0, uint32_t *o2)
{
    _kernel_swi_regs r;
    _kernel_oserror *e;
    memset(&r, 0, sizeof r);
    r.r[0] = (int)r0; r.r[1] = (int)r1; r.r[2] = (int)r2; r.r[3] = (int)r3; r.r[4] = (int)r4;
    e = vcm_swi(n, &r);
    if (!e) {
        if (o0) *o0 = (uint32_t)r.r[0];
        if (o2) *o2 = (uint32_t)r.r[2];
    }
    return e;
}

uint32_t vcm_now_cs(void)
{
    uint32_t t = 0;
    vcm_swi4(OS_ReadMonotonicTime, 0, 0, 0, 0, &t, NULL);
    return t;
}

static const char *const status_names[] = {
    "SUCCESS", "ENOMEM", "ENOSPC", "EINVAL", "ENOSYS", "ENOENT", "ENXIO", "EIO", "ESPIPE",
    "ECORRUPT", "ENOTREADY", "ECONFIG", "EISCONN", "ENOTCONN", "EAGAIN", "EFAULT" };
const char *vcm_status(uint32_t s) { return s < 16 ? status_names[s] : "?"; }

unsigned vcm_gpu_mem(void)
{
    uint32_t buf[8];
    _kernel_swi_regs r;
    buf[0] = sizeof buf; buf[1] = 0;
    buf[2] = TAG_GET_VC_MEMORY; buf[3] = 8; buf[4] = 0; buf[5] = buf[6] = 0; buf[7] = 0;
    memset(&r, 0, sizeof r);
    r.r[0] = r.r[1] = (int)(uintptr_t)buf;
    if (vcm_swi(BCMSupport_SendTempPropertyBuffer, &r) || buf[1] != 0x80000000u) return 0;
    return (unsigned)(buf[6] >> 20);
}

void vcm_logf(vcm *m, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    if (!m->log) return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    m->log(m->log_handle, b);
}

int vcm_fail(vcm *m, int code, const char *fmt, ...)
{
    va_list ap;
    if (m->failed) return m->failed;
    va_start(ap, fmt);
    vsnprintf(m->err, sizeof m->err, fmt, ap);
    va_end(ap);
    m->failed = code;
    vcm_logf(m, "%s", m->err);
    return code;
}

/* (vcm_pmp_alloc's reason for giving up: the caller falls back, or fails) */
void vcm_why(vcm *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->why, sizeof m->why, fmt, ap);
    va_end(ap);
}

/* ---- PCI memory ---- */

void *vcm_pci_alloc(vcm *m, size_t n)
{
    uint32_t log = 0;
    _kernel_swi_regs r;
    if (!m->pci_alloc_swi) {
        memset(&r, 0, sizeof r);
        r.r[1] = (int)(uintptr_t)"PCI_RAMAlloc";
        if (vcm_swi(OS_SWINumberFromString, &r)) return NULL;
        m->pci_alloc_swi = (uint32_t)r.r[0];
        r.r[1] = (int)(uintptr_t)"PCI_RAMFree";
        if (vcm_swi(OS_SWINumberFromString, &r)) return NULL;
        m->pci_free_swi = (uint32_t)r.r[0];
    }
    if (vcm_swi4((int)m->pci_alloc_swi, (uint32_t)n, 4096, 0, 0, &log, NULL) || !log) return NULL;
    return (void *)(uintptr_t)log;
}

void vcm_pci_free(vcm *m, void *p)
{
    if (p) vcm_swi4((int)m->pci_free_swi, (uint32_t)(uintptr_t)p, 0, 0, 0, NULL, NULL);
}

/* ---- Physical Memory Pools ----
   One dynamic area per buffer: pages that OS_Memory 12 recommends
   (physically contiguous, below 1 GB, as VCHIQ's bulk transfers need),
   claimed (OS_DynamicArea 21, locked) and mapped in (22) user readable,
   cacheable or not. The program's page at &8000 is never claimed
   (contig.h). PRM: OS_DynamicArea 0/21/22, OS_Memory 12. */

static _kernel_oserror *pmp_op(uint32_t reason, uint32_t area, uint32_t *list, uint32_t n)
{
    return vcm_swi4(OS_DynamicArea, reason, area, (uint32_t)(uintptr_t)list, n, NULL, NULL);
}

int vcm_pmp_free(vcm *m, uint32_t area, uint32_t pages)
{
    _kernel_oserror *e;
    uint32_t *l = malloc(pages * 12 + 12);
    if (l) {
        for (uint32_t j = 0; j < pages; j++) { l[3 * j] = j; l[3 * j + 1] = 0xFFFFFFFFu; l[3 * j + 2] = 0; }
        if (pages) {
            pmp_op(22, area, l, pages);    /* unmapped, */
            pmp_op(21, area, l, pages);    /* released, */
        }
        free(l);
    }
    if ((e = vcm_swi4(OS_DynamicArea, 1, area, 0, 0, NULL, NULL)) != NULL) {   /* removed */
        vcm_logf(m, "A %s pool (area %u) can't be removed: %s", m->pool_name, (unsigned)area, e->errmess);
        m->pool_stuck = 1;                 /* (its handler, in the RMA block, must stay) */
        return -1;
    }
    return 0;
}

void *vcm_pmp_alloc(vcm *m, uint32_t size, uint32_t *area_out, unsigned flags)
{
    uint32_t pages = (size + 4095) >> 12, first, *l, phys[2];
    int uncached = (flags & VCM_POOL_UNCACHED) != 0;
    _kernel_swi_regs r;
    _kernel_oserror *e;
    void *base;
    const char *no;
    contig_app_page app = contig_read_app_page(vcm_swi);   /* (never claim the program's page at &8000: contig.h) */
    /* the pool and the page list first: OS_Memory 12's pages are only sure
       to be free if claiming them is the next thing that takes pages */
    if (!(l = malloc(pages * 12))) { vcm_why(m, "Out of memory"); return NULL; }
    memset(&r, 0, sizeof r);
    r.r[0] = 0; r.r[1] = -1; r.r[2] = 0; r.r[3] = -1;
    r.r[4] = (int)(DA_SPECIFIC_PAGES | DA_PMP | DA_NOT_DRAGGABLE);   /* (access 0: user read/write) */
    r.r[5] = (int)(pages << 12); r.r[6] = (int)(m->stub + 72); r.r[7] = 0;
    r.r[8] = (int)(uintptr_t)m->pool_name; r.r[9] = (int)pages;
    if ((e = vcm_swi(OS_DynamicArea, &r)) != NULL) { free(l); vcm_why(m, "A Physical Memory Pool: %s", e->errmess); return NULL; }
    *area_out = (uint32_t)r.r[1];
    base = (void *)(uintptr_t)(uint32_t)r.r[3];
    /* contiguous pages for DMA below 1 GB (R4-R7: RISC OS 5.29+), not the program's page at &8000 */
    if ((no = contig_recommend(vcm_swi, pages, VC_RAM_TOP - 1, &app, &first)) != NULL) {
        free(l); vcm_pmp_free(m, *area_out, 0);
        vcm_why(m, "%s", no);
        return NULL;
    }
    for (uint32_t j = 0; j < pages; j++) { l[3 * j] = j; l[3 * j + 1] = first + j; l[3 * j + 2] = PAGE_LOCK; }
    if ((e = pmp_op(21, *area_out, l, pages)) != NULL) {
        free(l); vcm_pmp_free(m, *area_out, pages);
        vcm_why(m, "Claiming %u pages from %u: %s", (unsigned)pages, (unsigned)first, e->errmess);
        return NULL;
    }
    for (uint32_t j = 0; j < pages; j++) {
        l[3 * j] = j; l[3 * j + 1] = j;
        l[3 * j + 2] = PAGE_LOCK | (uncached ? PAGE_NOT_CACHEABLE : 0);   /* (access 0; uncached: still bufferable, */
                                                                           /* as PCI_RAMAlloc memory) */
    }
    if ((e = pmp_op(22, *area_out, l, pages)) != NULL) {
        free(l); vcm_pmp_free(m, *area_out, pages);
        vcm_why(m, "Mapping %u pages: %s", (unsigned)pages, e->errmess);
        return NULL;
    }
    free(l);
    if (contig_app_page_moved(vcm_swi, &app)) {
        m->app_page_moves++;
        vcm_logf(m, "Claiming a pool's pages moved the program's page at &8000 (ARMEABISupport will lose the program)");
    }
    /* (checked: the first and last pages' physical addresses, as VCHIQ will see them) */
    for (int k = 0; k < 2; k++) {
        uint32_t blk[3] = { 0, (uint32_t)(uintptr_t)base + (k ? (pages - 1) << 12 : 0), 0 };
        if ((e = vcm_swi4(OS_Memory, 0x2200, (uint32_t)(uintptr_t)blk, 1, 0, NULL, NULL)) != NULL) {
            vcm_pmp_free(m, *area_out, pages);
            vcm_why(m, "OS_Memory 0: %s", e->errmess);
            return NULL;
        }
        phys[k] = blk[2];
    }
    if (phys[1] != phys[0] + ((pages - 1) << 12) || phys[1] > VC_RAM_TOP - 4096) {
        vcm_pmp_free(m, *area_out, pages);
        vcm_why(m, "The pool's pages aren't contiguous below 1 GB (&%08X..&%08X)", (unsigned)phys[0], (unsigned)phys[1]);
        return NULL;
    }
    {   /* user readable (and writable, if asked)? */
        uint32_t acc = 0;
        memset(&r, 0, sizeof r);
        r.r[0] = 24; r.r[1] = (int)(uintptr_t)base; r.r[2] = (int)((uintptr_t)base + (pages << 12));
        if (!vcm_swi(OS_Memory, &r)) acc = (uint32_t)r.r[1];   /* (the flags come back in R1) */
        if ((flags & VCM_POOL_RW) && (acc & 3) != 3) {   /* (refused: simpler than writing in SVC mode) */
            vcm_pmp_free(m, *area_out, pages);
            vcm_why(m, "The pool isn't user read/write (OS_Memory 24: &%X)", (unsigned)acc);
            return NULL;
        }
        if (!(acc & 1)) {                  /* (read in SVC mode then: no abort part way) */
            if (!m->pmp_svc) vcm_logf(m, "The pool isn't readable in USR mode (OS_Memory 24: &%X): copied in SVC mode", (unsigned)acc);
            m->pmp_svc = 1;
        }
    }
    if (flags & VCM_POOL_LOG)
        vcm_logf(m, "A picture buffer in a Physical Memory Pool: area %u, %u pages at &%08X, physical &%08X, %s",
                 (unsigned)*area_out, (unsigned)pages, (unsigned)(uintptr_t)base, (unsigned)phys[0],
                 uncached ? "not cacheable" : "cacheable");
    return base;
}

void vcm_cache_cci(const vcm *m, const void *p, uint32_t n)
{
    if (m->armop_cci)
        vcdec_svc_call(m->armop_cci, (uint32_t)(uintptr_t)p, ((uint32_t)(uintptr_t)p + n + 63) & ~63u);
}

/* ---- the connection ---- */

/* the RMA block: the bulk callback (R1 = 4, a receive done: [R0] += 1;
   R1 = 18, aborted: [R0 + 4] += 1; R0 is its param, the two words after
   the code, at +64 and +68), and at +72 the pools' dynamic area handler
   (MOV PC, R14) */
int vcm_claim_stub(vcm *m)
{
    static const uint32_t code[9] = { 0xe3510004u, 0x05903000u, 0x02833001u, 0x05803000u, 0xe3510012u,
                                      0x05903004u, 0x02833001u, 0x05803004u, 0xe1a0f00eu };
    uint32_t img[20];
    _kernel_swi_regs rr;
    _kernel_oserror *e;
    if ((e = vcm_swi4(OS_Module, 6, 0, 0, 80, NULL, &m->stub)) != NULL) return vcm_fail(m, VCM_ERROR, "No RMA: %s", e->errmess);
    memset(img, 0, sizeof img);
    memcpy(img, code, sizeof code);
    img[18] = 0xe1a0f00eu;
    vcdec_svc_copy((void *)(uintptr_t)m->stub, img, sizeof img);
    memset(&rr, 0, sizeof rr);
    rr.r[0] = 1; rr.r[1] = (int)m->stub; rr.r[2] = (int)(m->stub + 76);
    vcm_swi(OS_SynchroniseCodeAreas, &rr);
    return 0;
}

_kernel_oserror *vcm_find_cci(vcm *m)
{
    _kernel_oserror *e = vcm_swi4(OS_MMUControl, 2 | ARMOP_CACHE_CLEAN_INVALIDATE_RANGE << 8, 0, 0, 0, &m->armop_cci, NULL);
    if (e) m->armop_cci = 0;
    return e;
}

int vcm_connect(vcm *m)
{
    uint32_t setup[11];
    _kernel_oserror *e;
    if ((e = vcm_swi4(VCHIQ_Initialise, 0, 0, 0, 0, &m->instance, NULL)) != NULL)
        return vcm_fail(m, VCM_ERROR, "VCHIQ_Initialise: %s", e->errmess);
    if ((e = vcm_swi4(VCHIQ_Connect, 0, 0, m->instance, 0, NULL, NULL)) != NULL)
        return vcm_fail(m, VCM_ERROR, "VCHIQ_Connect: %s", e->errmess);
    m->connected = 1;
    memset(setup, 0, sizeof setup);
    setup[0] = 15; setup[1] = 10; setup[2] = MMAL_SERVICE;
    setup[6] = m->stub; setup[7] = m->stub + 64;
    if ((e = vcm_swi4(VCHIQ_ServiceOpen, m->instance, (uint32_t)(uintptr_t)setup, 0, 0, &m->handle, NULL)) != NULL)
        return vcm_fail(m, VCM_ERROR, "VCHIQ_ServiceOpen \"mmal\": %s", e->errmess);
    m->opened = 1;
    vcm_swi4(VCHIQ_ServiceRelease, m->handle, 0, 0, 0, NULL, NULL);   /* (opening leaves it in use) */
    m->waiting = "";
    return 0;
}

int vcm_service_close(vcm *m, const char *left)
{
    _kernel_oserror *e;
    if (!m->opened || !(e = vcm_swi4(VCHIQ_ServiceClose, m->handle, 0, 0, 0, NULL, NULL))) return 0;
    vcm_logf(m, "VCHIQ_ServiceClose: %s (%s)", e->errmess, left);
    m->stub = 0;                           /* (it may still be called) */
    return -1;
}

void vcm_disconnect(vcm *m)
{
    if (m->connected) vcm_swi4(VCHIQ_Disconnect, m->instance, 0, 0, 0, NULL, NULL);
    vcm_pci_free(m, m->evbuf);
    m->evbuf = NULL;
    m->evsize = 0;
}

void vcm_free_stub(vcm *m)
{
    if (m->stub && !m->pool_stuck) vcm_swi4(OS_Module, 7, 0, m->stub, 0, NULL, NULL);
}

/* ---- messages ---- */

int vcm_send(vcm *m, uint32_t type, const void *payload, uint32_t len)
{
    uint32_t msg[128];
    hdr_t *h = (hdr_t *)msg;
    _kernel_oserror *e;
    memset(msg, 0, sizeof(hdr_t));
    h->magic = MMAL_MAGIC;
    h->type = type;
    h->context = ++m->context;
    memcpy(h + 1, payload, len);
    vcm_swi4(VCHIQ_ServiceUse, m->handle, 0, 0, 0, NULL, NULL);
    e = vcm_swi4(VCHIQ_MsgQueue, m->handle, (uint32_t)(uintptr_t)msg, (uint32_t)sizeof *h + len, 0, NULL, NULL);
    vcm_swi4(VCHIQ_ServiceRelease, m->handle, 0, 0, 0, NULL, NULL);
    if (e) return vcm_fail(m, VCM_ERROR, "VCHIQ_MsgQueue: %s", e->errmess);
    return 0;
}

uint32_t vcm_dequeue(vcm *m)
{
    uint32_t got = 0;
    if (vcm_swi4(VCHIQ_MsgDequeue, m->handle, (uint32_t)(uintptr_t)m->msg, sizeof m->msg, 0, NULL, &got)) return 0;
    if (got < sizeof(hdr_t) || ((hdr_t *)m->msg)->magic != MMAL_MAGIC) {
        vcm_logf(m, "(a %u byte message that isn't MMAL's)", (unsigned)got);
        return 0;
    }
    m->last_heard = vcm_now_cs();
    return got;
}

/* sends and waits for the reply of the same type, handing what else comes to on_msg */
int vcm_transact(vcm *m, uint32_t type, const void *payload, uint32_t len, const char *what)
{
    uint32_t t0, got;
    if (m->failed || vcm_send(m, type, payload, len)) return -1;
    t0 = vcm_now_cs();
    m->waiting = " (waiting)";
    while (vcm_now_cs() - t0 < REPLY_CS) {
        if (!(got = vcm_dequeue(m))) continue;
        if (((hdr_t *)m->msg)->type == type) { m->waiting = ""; return (int)(got - sizeof(hdr_t)); }
        m->on_msg(m->owner, got);
        if (m->failed) { m->waiting = ""; return -1; }
    }
    m->waiting = "";
    vcm_fail(m, VCM_ERROR, "%s: no reply from the %s in %d cs", what, m->name, REPLY_CS);
    return -1;
}

uint32_t *vcm_reply(vcm *m) { return (uint32_t *)((hdr_t *)m->msg + 1); }

int vcm_status_ok(vcm *m, int n, const char *what)
{
    if (n < 4) return n < 0 ? -1 : vcm_fail(m, VCM_ERROR, "%s: a short reply", what);
    if (vcm_reply(m)[0]) return vcm_fail(m, VCM_ERROR, "%s: %s", what, vcm_status(vcm_reply(m)[0]));
    return 0;
}

/* a bulk receive of n bytes into dst, queued now; its number (the
   callback's count when it's done) */
int vcm_queue_receive(vcm *m, uint8_t *dst, uint32_t n, uint32_t *seq)
{
    _kernel_oserror *e;
    vcm_swi4(VCHIQ_ServiceUse, m->handle, 0, 0, 0, NULL, NULL);
    e = vcm_swi5(VCHIQ_BulkQueueReceive, m->handle, (uint32_t)(uintptr_t)dst, (n + 3) & ~3u,
                 VCHI_FLAGS_CALLBACK_WHEN_DONE, 0, NULL, NULL);
    vcm_swi4(VCHIQ_ServiceRelease, m->handle, 0, 0, 0, NULL, NULL);
    if (e) return vcm_fail(m, VCM_ERROR, "VCHIQ_BulkQueueReceive: %s", e->errmess);
    *seq = ++m->rx_queued;
    return 0;
}

int vcm_received(vcm *m, uint32_t seq)
{
    uint32_t v[2];
    if (!seq) return 1;                    /* (the data came in the message) */
    vcdec_svc_copy(v, (const void *)(uintptr_t)(m->stub + 64), 8);   /* (the callback's counts: one SVC entry) */
    if (v[1]) return -1;
    return (int32_t)(v[0] - seq) >= 0;
}

/* waits for receive seq (and so every one before it) */
int vcm_wait_received(vcm *m, uint32_t seq)
{
    uint32_t t0 = vcm_now_cs();
    int r;
    while (!(r = vcm_received(m, seq)))
        if (vcm_now_cs() - t0 > REPLY_CS) return vcm_fail(m, VCM_ERROR, "A bulk receive didn't finish in %d cs", REPLY_CS);
    if (r < 0) return vcm_fail(m, VCM_ERROR, "A bulk receive was aborted");
    return 0;
}

int vcm_buffer_to_vc(vcm *m, const port_info_t *pi, const char *port_name, int idx, const void *data, uint32_t alloc,
                     uint32_t len, uint32_t flags, uint64_t pts, uint64_t dts)
{
    buffer_msg_t b;
    memset(&b, 0, sizeof b);
    b.drvbuf.magic = MMAL_MAGIC;
    b.drvbuf.component_handle = m->comp;
    b.drvbuf.port_handle = pi->port_handle;
    b.drvbuf.client_context = (uint32_t)idx + CTX_BASE;
    b.data = (uint32_t)(uintptr_t)data;
    b.alloc_size = alloc;
    b.length = len;
    b.flags = flags;
    b.pts = pts;
    b.dts = dts;
    if (vcm_send(m, T_BUFFER_FROM_HOST, &b, sizeof b)) return -1;
    if (m->log_messages)
        vcm_logf(m, "tx: %s buffer %d, %u bytes, flags &%X, pts %lld", port_name, idx, (unsigned)len, (unsigned)flags,
                 (long long)pts);
    if (len) {
        _kernel_oserror *e;
        vcm_swi4(VCHIQ_ServiceUse, m->handle, 0, 0, 0, NULL, NULL);
        e = vcm_swi5(VCHIQ_BulkQueueTransmit, m->handle, (uint32_t)(uintptr_t)data, (len + 3) & ~3u,
                     VCHI_FLAGS_BLOCK_UNTIL_QUEUED, 0, NULL, NULL);
        vcm_swi4(VCHIQ_ServiceRelease, m->handle, 0, 0, 0, NULL, NULL);
        if (e) return vcm_fail(m, VCM_ERROR, "VCHIQ_BulkQueueTransmit: %s", e->errmess);
    }
    return 0;
}

/* an event whose data is longer than the message's: it follows by bulk
   transfer, received now and waited for (rare) */
int vcm_event_data(vcm *m, event_msg_t *ev)
{
    uint32_t seq;
    if (ev->length <= sizeof ev->data) return 0;
    if (ev->length > m->evsize) {
        vcm_pci_free(m, m->evbuf);
        m->evbuf = vcm_pci_alloc(m, ev->length + 4);
        m->evsize = m->evbuf ? ev->length : 0;
        if (!m->evbuf) return vcm_fail(m, VCM_ERROR, "No PCI memory for a %u byte event", (unsigned)ev->length);
    }
    if (vcm_queue_receive(m, m->evbuf, ev->length, &seq) || vcm_wait_received(m, seq)) return -1;
    vcdec_svc_copy(ev->data, m->evbuf, sizeof ev->data);
    return 0;
}

/* ---- the component ---- */

int vcm_create(vcm *m, const char *component)
{
    uint32_t req[1 + 32 + 1];
    int n;
    memset(req, 0, sizeof req);
    req[0] = 1;
    strcpy((char *)&req[1], component);
    n = vcm_transact(m, T_COMPONENT_CREATE, req, sizeof req, "Create");
    if (n < 0) {
        if (strstr(m->err, "no reply"))
            snprintf(m->err, sizeof m->err, "The VideoCore's MMAL service isn't answering (if something stopped "
                     "part way, it can stay stuck until the machine is restarted)");
        return -1;
    }
    if (n < 8 || vcm_reply(m)[0])
        return vcm_fail(m, VCM_ERROR, "Create: %s", n >= 4 ? vcm_status(vcm_reply(m)[0]) : "a short reply");
    m->comp = vcm_reply(m)[1];
    m->created = 1;
    return 0;
}

int vcm_port_get(vcm *m, uint32_t type, port_info_t *pi, const char *what)
{
    uint32_t req[3] = { m->comp, type, 0 };
    int n = vcm_transact(m, T_PORT_INFO_GET, req, sizeof req, what);
    if (vcm_status_ok(m, n, what)) return -1;
    if (n < (int)offsetof(port_info_t, extradata)) return vcm_fail(m, VCM_ERROR, "%s: a short reply", what);
    memcpy(pi, vcm_reply(m), sizeof *pi);
    return 0;
}

int vcm_port_set(vcm *m, uint32_t type, port_info_t *pi, const char *what)
{
    port_set_t s;
    int n;
    memset(&s, 0, sizeof s);
    s.component_handle = m->comp;
    s.port_type = type;
    s.port = pi->port;
    s.format = pi->format;
    s.format.extradata_size = 0;
    s.video = pi->video;
    n = vcm_transact(m, T_PORT_INFO_SET, &s, sizeof s, what);
    if (vcm_status_ok(m, n, what)) return -1;
    if (n >= (int)offsetof(port_info_t, extradata)) {
        port_info_t *r = (port_info_t *)vcm_reply(m);
        pi->port = r->port;                /* the firmware's view (sizes may grow) */
        pi->format = r->format;
        pi->video = r->video;
    }
    return 0;
}

int vcm_port_action(vcm *m, const port_info_t *pi, uint32_t action, const char *what)
{
    uint32_t req[3 + 16];
    memset(req, 0, sizeof req);
    req[0] = m->comp; req[1] = pi->port_handle; req[2] = action;
    memcpy(&req[3], &pi->port, sizeof pi->port);
    return vcm_status_ok(m, vcm_transact(m, T_PORT_ACTION, req, sizeof req, what), what);
}

int vcm_simple(vcm *m, uint32_t type, const char *what)
{
    return vcm_status_ok(m, vcm_transact(m, type, &m->comp, 4, what), what);
}

/* ports off, the component off and gone (each step only if it was done).
   closing: every step is tried even if one before it failed (a component
   left on the VideoCore keeps its memory) */
void vcm_destroy(vcm *m, const port_info_t *in, int *in_on, const port_info_t *out, int *out_on, int closing)
{
    if (closing) m->failed = 0;
    if (*out_on && !vcm_port_action(m, out, ACTION_DISABLE, "Output disable")) *out_on = 0;
    if (closing) m->failed = 0;
    if (*in_on && !vcm_port_action(m, in, ACTION_DISABLE, "Input disable")) *in_on = 0;
    if (closing) m->failed = 0;
    if (m->enabled && !vcm_simple(m, T_COMPONENT_DISABLE, "Disable")) m->enabled = 0;
    if (closing) m->failed = 0;
    if (m->created && !vcm_simple(m, T_COMPONENT_DESTROY, "Destroy")) m->created = 0;
}

int vcm_param_set(vcm *m, const port_info_t *pi, uint32_t id, uint32_t value, const char *what)
{
    uint32_t req[5];
    int n;
    req[0] = m->comp; req[1] = pi->port_handle; req[2] = id; req[3] = 12; req[4] = value;   /* (12: id, size, value) */
    if ((n = vcm_transact(m, T_PORT_PARAMETER_SET, req, sizeof req, what)) < 0) return -1;
    if (n < 4) return vcm_fail(m, VCM_ERROR, "%s: a short reply", what);
    if (vcm_reply(m)[0]) {
        vcm_why(m, "%s: %s", what, vcm_status(vcm_reply(m)[0]));
        return (int)vcm_reply(m)[0];
    }
    return 0;
}

/* ---- (0.4.5) the shared memory service ----
   vc-sm-cma's protocol, as SMEMProbe 0.1 found it works from RISC OS (no
   code copied: Linux rpi-6.6.y, drivers/staging/vc04_services/vc-sm-cma,
   vc_sm_defs.h and vc_sm_cma_vchi.c, GPL-2.0). A message is {type,
   transaction} and its body; a reply starts with the transaction. The
   VideoCore's own messages (the top bit of their transaction set) and any
   reply to a FREE are dropped. */
#define SMEM_SERVICE   FOURCC_BE('S', 'M', 'E', 'M')
#define SM_FREE        4
#define SM_IMPORT      8
#define SM_NON_CACHED  1                   /* (the VideoCore's uncached alias: as Linux imports dma-bufs) */
#define SM_ALLOCATOR   0x524F4D53u         /* (ours to choose: it comes back in the VideoCore's messages) */

static uint32_t smem_dequeue(vcm *m)
{
    uint32_t got = 0;
    if (vcm_swi4(VCHIQ_MsgDequeue, m->smem_handle, (uint32_t)(uintptr_t)m->smem_msg, sizeof m->smem_msg, 0, NULL, &got))
        return 0;
    return got;
}

static int smem_send(vcm *m, uint32_t type, const uint32_t *body, uint32_t len)
{
    uint32_t b[2 + 13];
    _kernel_oserror *e;
    while (smem_dequeue(m)) {}             /* (anything left: dropped) */
    b[0] = type;
    b[1] = m->smem_trans = (m->smem_trans + 1) & 0x7FFFFFFFu;
    memcpy(&b[2], body, len);
    vcm_swi4(VCHIQ_ServiceUse, m->smem_handle, 0, 0, 0, NULL, NULL);
    e = vcm_swi4(VCHIQ_MsgQueue, m->smem_handle, (uint32_t)(uintptr_t)b, 8 + len, 0, NULL, NULL);
    vcm_swi4(VCHIQ_ServiceRelease, m->smem_handle, 0, 0, 0, NULL, NULL);
    if (e) { vcm_why(m, "SMEM: VCHIQ_MsgQueue: %s", e->errmess); return -1; }
    if (m->log_messages) vcm_logf(m, "SMEM tx: type %u, transaction %u", (unsigned)type, (unsigned)b[1]);
    return 0;
}

int vcm_smem_open(vcm *m)
{
    uint32_t setup[11];
    _kernel_oserror *e;
    memset(setup, 0, sizeof setup);
    setup[0] = 1; setup[1] = 0; setup[2] = SMEM_SERVICE;   /* (vc-sm-cma's version 1, at least 0) */
    setup[6] = m->stub; setup[7] = m->stub + 64;
    if ((e = vcm_swi4(VCHIQ_ServiceOpen, m->instance, (uint32_t)(uintptr_t)setup, 0, 0, &m->smem_handle, NULL)) != NULL) {
        vcm_why(m, "VCHIQ_ServiceOpen \"SMEM\": %s", e->errmess);
        return -1;
    }
    vcm_swi4(VCHIQ_ServiceRelease, m->smem_handle, 0, 0, 0, NULL, NULL);   /* (opening leaves it in use) */
    m->smem_open = 1;
    return 0;
}

uint32_t vcm_smem_import(vcm *m, const void *p, uint32_t size, const char *name)
{
    uint32_t b[5 + 8], blk[3], t0, got;
    _kernel_oserror *e;
    if (!m->smem_open) { vcm_why(m, "the SMEM service isn't open"); return 0; }
    blk[0] = 0; blk[1] = (uint32_t)(uintptr_t)p; blk[2] = 0;
    if ((e = vcm_swi4(OS_Memory, 0x2200, (uint32_t)(uintptr_t)blk, 1, 0, NULL, NULL)) != NULL) {
        vcm_why(m, "OS_Memory 0: %s", e->errmess);
        return 0;
    }
    memset(b, 0, sizeof b);
    b[0] = SM_NON_CACHED; b[1] = blk[2] | 0xC0000000u; b[2] = size; b[3] = ++m->smem_ids; b[4] = SM_ALLOCATOR;
    snprintf((char *)&b[5], 32, "%s", name);
    if (smem_send(m, SM_IMPORT, b, sizeof b)) return 0;
    for (t0 = vcm_now_cs(); vcm_now_cs() - t0 < REPLY_CS;) {
        if (!(got = smem_dequeue(m))) continue;
        if (m->smem_msg[0] != m->smem_trans) continue;   /* (not this reply: dropped) */
        if (got < 8 || !m->smem_msg[1]) {
            vcm_why(m, "SMEM: the import of %u bytes at &%08X was refused", (unsigned)size, (unsigned)blk[2]);
            return 0;
        }
        if (m->log_messages) vcm_logf(m, "SMEM: %u bytes at &%08X imported: handle &%08X", (unsigned)size,
                                      (unsigned)blk[2], (unsigned)m->smem_msg[1]);
        return m->smem_msg[1];
    }
    vcm_why(m, "SMEM: no reply to an import in %d cs", REPLY_CS);
    return 0;
}

void vcm_smem_free(vcm *m, uint32_t handle)
{
    uint32_t b[2];
    if (!m->smem_open || !handle) return;
    b[0] = handle; b[1] = 0;
    smem_send(m, SM_FREE, b, sizeof b);
}

void vcm_smem_close(vcm *m)
{
    _kernel_oserror *e;
    if (!m->smem_open) return;
    while (smem_dequeue(m)) {}
    if ((e = vcm_swi4(VCHIQ_ServiceClose, m->smem_handle, 0, 0, 0, NULL, NULL)) != NULL)
        vcm_logf(m, "SMEM: VCHIQ_ServiceClose: %s", e->errmess);
    m->smem_open = 0;
}
