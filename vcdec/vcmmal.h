/*
 * vcmmal - what vcdec and vcenc share: the firmware's MMAL service over
 * RISC OS's VCHIQ module (its messages, replies and bulk transfers, the
 * RMA callback that counts finished receives), MMAL components and their
 * ports, and memory the VideoCore can reach (PCI_RAMAlloc memory and
 * Physical Memory Pools). Internal to libvcdec.a: not part of the devkit's
 * API (vcdec.h and vcenc.h are).
 *
 * How MMAL is driven here was found on a Pi 4 (MMALDecode 0.12-0.17,
 * VCDecTest, VCEncTest; vcdec.c's rules 1-8). In short:
 *   - buffers are numbered from 1 for the VideoCore (client_context; 0 is
 *     "no buffer" to it): CTX_BASE;
 *   - every bulk receive is queued the moment its message arrives: the
 *     VideoCore answers nothing more until the data it's sending has a
 *     receive. They run on, counted done by the RMA callback;
 *   - bulk transfers need physically contiguous memory below 1 GB.
 *
 * Message layouts (no code copied): Raspberry Pi userland's MMAL client,
 * interface/mmal/vc/mmal_vc_msgs.h and mmal_buffer.h (Broadcom,
 * BSD-3-Clause), and Linux's vchiq-mmal, mmal-msg*.h (GPL-2.0).
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#ifndef VCMMAL_H
#define VCMMAL_H

#include <stddef.h>
#include <stdint.h>
#include "kernel.h"

/* ---- SWIs ---- */
#define OS_Module                 0x1E
#define OS_SWINumberFromString    0x39
#define OS_ReadMonotonicTime      0x42
#define OS_DynamicArea            0x66
#define OS_Memory                 0x68
#define OS_MMUControl             0x6B
#define OS_SynchroniseCodeAreas   0x6E
#define BCMSupport_SendTempPropertyBuffer 0x591C5
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

/* ---- MMAL ---- */
#define FOURCC_BE(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define FOURCC_LE(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define MMAL_SERVICE   FOURCC_BE('m', 'm', 'a', 'l')
#define MMAL_MAGIC     FOURCC_LE('m', 'm', 'a', 'l')
#define ENC_H264       FOURCC_LE('H', '2', '6', '4')
#define ENC_I420       FOURCC_LE('I', '4', '2', '0')
#define EV_FORMAT_CHANGED FOURCC_LE('E', 'F', 'C', 'H')
#define EV_ERROR       FOURCC_LE('E', 'R', 'R', 'O')

enum { T_COMPONENT_CREATE = 4, T_COMPONENT_DESTROY, T_COMPONENT_ENABLE, T_COMPONENT_DISABLE, T_PORT_INFO_GET,
       T_PORT_INFO_SET, T_PORT_ACTION, T_BUFFER_FROM_HOST, T_BUFFER_TO_HOST, T_PORT_PARAMETER_SET = 14,
       T_EVENT_TO_HOST = 16 };
enum { PORT_CONTROL = 1, PORT_INPUT = 2, PORT_OUTPUT = 3 };
enum { ACTION_ENABLE = 1, ACTION_DISABLE = 2, ACTION_FLUSH = 3 };
enum { ES_VIDEO = 3 };
#define FLAG_EOS           1u
#define FLAG_FRAME_START   2u
#define FLAG_FRAME_END     4u
#define FLAG_KEYFRAME      8u
#define FLAG_DISCONTINUITY 0x10u
#define FLAG_CONFIG        0x20u
#define TIME_UNKNOWN   0x8000000000000000ull
#define SHORT_DATA     128
#define CTX_BASE       1                   /* (rule 1: 0 is "no buffer" to the VideoCore) */
#define REPLY_CS       300                 /* a reply waited for this long at most */
#define STALL_CS       500                 /* nothing from the component for this long: it has stopped */

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

/* ---- one MMAL component's connection ---- */
#define VCM_ERROR (-1)                     /* (= VCDEC_ERROR = VCENC_ERROR) */

typedef struct vcm {
    const char *name;                      /* "decoder", "encoder": for the messages */
    const char *pool_name;                 /* its pools' dynamic area name */
    void (*log)(void *handle, const char *text);   /* (NULL: no log) */
    void *log_handle;
    int log_messages;                      /* every message logged */
    void (*on_msg)(void *owner, uint32_t got);     /* a message that isn't the reply being waited for */
    void *owner;
    char err[200];
    int failed;                            /* VCM_ERROR (or the owner's code) from now on; 0: none */
    uint32_t instance, handle, stub, comp, context;
    int connected, opened, created, enabled;
    uint32_t msg[128];                     /* the last message dequeued */
    const char *waiting;                   /* (for the log) */
    uint32_t last_heard;                   /* when the last message came */
    uint32_t rx_queued;                    /* receives queued (the callback counts them done) */
    uint32_t pci_alloc_swi, pci_free_swi;
    uint8_t *evbuf;                        /* an event's data, when it comes by bulk transfer */
    uint32_t evsize;
    char why[160];                         /* why a pool couldn't be had */
    uint32_t armop_cci;                    /* cached pools: the kernel's Cache_CleanInvalidateRange */
    int pmp_svc;                           /* a pool not readable in USR mode (OS_Memory 24) */
    int pool_stuck;                        /* a pool couldn't be removed: the RMA block (its handler) is kept */
    unsigned app_page_moves;               /* claims that moved the program's page at &8000 (contig.h) */
} vcm;

/* SWIs (the host tests' fake under VCDEC_HOST) */
_kernel_oserror *vcm_swi(int n, _kernel_swi_regs *r);
_kernel_oserror *vcm_swi5(int n, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4, uint32_t *o0, uint32_t *o2);
#define vcm_swi4(n, a, b, c, d, o0, o2) vcm_swi5(n, a, b, c, d, 0, o0, o2)
uint32_t vcm_now_cs(void);
unsigned vcm_gpu_mem(void);                /* gpu_mem in MB, or 0 */
const char *vcm_status(uint32_t s);        /* an MMAL status's name */

/* vcdec_copy.S */
void vcdec_svc_copy(void *dst, const void *src, size_t n);
void vcdec_copy_rows(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride, int width, int rows, int mode);
void vcdec_svc_call(uint32_t fn, uint32_t r0, uint32_t r1);

/* the log, and failing (every call fails from then on: the code returned) */
void vcm_logf(vcm *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vcm_fail(vcm *m, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void vcm_why(vcm *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* memory the VideoCore reaches */
void *vcm_pci_alloc(vcm *m, size_t n);
void vcm_pci_free(vcm *m, void *p);
#define VCM_POOL_UNCACHED 1u               /* not cacheable (still bufferable) */
#define VCM_POOL_RW       2u               /* user read/write needed (else refused; otherwise read in SVC if need be) */
#define VCM_POOL_LOG      4u               /* each pool made is logged */
void *vcm_pmp_alloc(vcm *m, uint32_t size, uint32_t *area_out, unsigned flags);   /* NULL: vcm_why says why */
int vcm_pmp_free(vcm *m, uint32_t area, uint32_t pages);
void vcm_cache_cci(const vcm *m, const void *p, uint32_t n);   /* a cacheable pool's range cleaned and invalidated */

/* the connection: the RMA callback (also the pools' handler), then VCHIQ */
int vcm_claim_stub(vcm *m);
_kernel_oserror *vcm_find_cci(vcm *m);     /* (RISC OS 5.23+: for cacheable pools) */
int vcm_connect(vcm *m);
int vcm_service_close(vcm *m, const char *left);   /* -1: it failed (said, with what's left) */
void vcm_disconnect(vcm *m);
void vcm_free_stub(vcm *m);

/* messages */
int vcm_send(vcm *m, uint32_t type, const void *payload, uint32_t len);
uint32_t vcm_dequeue(vcm *m);              /* one message into m->msg: its length, or 0 */
int vcm_transact(vcm *m, uint32_t type, const void *payload, uint32_t len, const char *what);
uint32_t *vcm_reply(vcm *m);
int vcm_status_ok(vcm *m, int n, const char *what);
int vcm_queue_receive(vcm *m, uint8_t *dst, uint32_t n, uint32_t *seq);
int vcm_received(vcm *m, uint32_t seq);    /* 1 done, 0 not yet, -1 one was aborted (seq 0: done) */
int vcm_wait_received(vcm *m, uint32_t seq);
int vcm_buffer_to_vc(vcm *m, const port_info_t *pi, const char *port_name, int idx, const void *data, uint32_t alloc,
                     uint32_t len, uint32_t flags, uint64_t pts, uint64_t dts);
int vcm_event_data(vcm *m, event_msg_t *ev);   /* an event's data that comes by bulk transfer, into ev->data */

/* the component and its ports */
int vcm_create(vcm *m, const char *component);
int vcm_port_get(vcm *m, uint32_t type, port_info_t *pi, const char *what);
int vcm_port_set(vcm *m, uint32_t type, port_info_t *pi, const char *what);
int vcm_port_action(vcm *m, const port_info_t *pi, uint32_t action, const char *what);
int vcm_simple(vcm *m, uint32_t type, const char *what);
void vcm_destroy(vcm *m, const port_info_t *in, int *in_on, const port_info_t *out, int *out_on, int closing);

#endif
