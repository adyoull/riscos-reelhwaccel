/*
 * hevcdec_linux.c - (0.1.15) hevcdec on RISC OS on Linux (ROL): the HEVC
 * block is Linux's there, so hevcdec drives Linux's own driver for it,
 * Raspberry Pi's rpivid (a V4L2 stateless decoder, with the media request
 * API), through Linux's system calls (hevcdec_lsys). rpivid runs the same
 * rpivid_h265.c that hevcdec.c runs on RISC OS, and takes the same
 * controls (hevc_ctrls.h), so a picture goes to it as it comes:
 *
 *  - one slice a request, each in its own OUTPUT buffer, every one but the
 *    picture's last with V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF (the driver
 *    then finishes the picture on its last slice), as hevcdec.c gives
 *    rpivid_h265.c the slices;
 *  - each frame one of the driver's CAPTURE buffers (VIDIOC_CREATE_BUFS,
 *    one at a time, mapped into RISC OS's memory), NV12 in 128-byte
 *    columns as hevcdec.c's frames, so the conversions are the same;
 *  - a picture's OUTPUT buffers carry its number as their timestamp (in
 *    microseconds: the driver's is in nanoseconds, so the DPB's references
 *    are multiplied by 1000), which the driver copies to the CAPTURE
 *    buffer and finds references by;
 *  - the driver overlaps its phases itself: pipelined is buffers queued
 *    ahead.
 *
 * The CAPTURE queue takes buffers in the order they're queued, so a
 * frame's buffer is queued only once its first slice is ready to go; and
 * a decode that fails part way (a slice refused after the frame was
 * queued) stops the decoder, as a phase that doesn't finish does natively.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hevcdec_int.h"
#include "hevcdec_lsys.h"

/* ---- Linux's structures, as a 32-bit ARM program passes them (time32) ---- */

struct lx_capability {
    char driver[16], card[32], bus_info[32];
    uint32_t version, capabilities, device_caps, reserved[3];
};
struct lx_plane_fmt { uint32_t sizeimage, bytesperline; uint16_t reserved[6]; };
struct lx_pix_mp {
    uint32_t width, height, pixelformat, field, colorspace;
    struct lx_plane_fmt plane_fmt[8];
    uint8_t num_planes, flags, ycbcr_enc, quantization, xfer_func, reserved[7];
};
struct lx_format { uint32_t type; union { struct lx_pix_mp pix_mp; uint8_t raw[200]; } fmt; };
struct lx_requestbuffers { uint32_t count, type, memory, capabilities; uint8_t flags, reserved[3]; };
struct lx_plane { uint32_t bytesused, length, m, data_offset, reserved[11]; };
struct lx_buffer {
    uint32_t index, type, bytesused, flags, field;
    int32_t tv_sec, tv_usec;
    uint32_t timecode[4];
    uint32_t sequence, memory, m, length, reserved2;
    int32_t request_fd;
};
struct lx_create_buffers {
    uint32_t index, count, memory;
    struct lx_format format;
    uint32_t capabilities, flags, reserved[6];
};
struct lx_exportbuffer { uint32_t type, index, plane, flags; int32_t fd; uint32_t reserved[11]; };
struct __attribute__((packed)) lx_ext_control { uint32_t id, size, reserved2, ptr, ptr_hi; };
struct lx_ext_controls { uint32_t which, count, error_idx; int32_t request_fd; uint32_t reserved, controls; };
struct lx_media_info {
    char driver[16], model[32], serial[40], bus_info[32];
    uint32_t media_version, hw_revision, driver_version, reserved[31];
};
struct lx_pollfd { int32_t fd; int16_t events, revents; };

#define LX_SIZE_OK(t, n) typedef char t##_size_ok[sizeof(struct t) == (n) ? 1 : -1]
LX_SIZE_OK(lx_capability, 104);
LX_SIZE_OK(lx_format, 204);
LX_SIZE_OK(lx_requestbuffers, 20);
LX_SIZE_OK(lx_plane, 60);
LX_SIZE_OK(lx_buffer, 68);
LX_SIZE_OK(lx_create_buffers, 248);
LX_SIZE_OK(lx_exportbuffer, 64);
LX_SIZE_OK(lx_ext_control, 20);
LX_SIZE_OK(lx_ext_controls, 24);
LX_SIZE_OK(lx_media_info, 256);

#define IOC(dir, type, nr, size) (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)(type) << 8) | (uint32_t)(nr))
#define VIDIOC_QUERYCAP     IOC(2, 'V', 0, sizeof(struct lx_capability))
#define VIDIOC_G_FMT        IOC(3, 'V', 4, sizeof(struct lx_format))
#define VIDIOC_S_FMT        IOC(3, 'V', 5, sizeof(struct lx_format))
#define VIDIOC_REQBUFS      IOC(3, 'V', 8, sizeof(struct lx_requestbuffers))
#define VIDIOC_QUERYBUF     IOC(3, 'V', 9, sizeof(struct lx_buffer))
#define VIDIOC_QBUF         IOC(3, 'V', 15, sizeof(struct lx_buffer))
#define VIDIOC_EXPBUF       IOC(3, 'V', 16, sizeof(struct lx_exportbuffer))
#define VIDIOC_DQBUF        IOC(3, 'V', 17, sizeof(struct lx_buffer))
#define VIDIOC_STREAMON     IOC(1, 'V', 18, 4)
#define VIDIOC_STREAMOFF    IOC(1, 'V', 19, 4)
#define VIDIOC_S_EXT_CTRLS  IOC(3, 'V', 72, sizeof(struct lx_ext_controls))
#define VIDIOC_CREATE_BUFS  IOC(3, 'V', 92, sizeof(struct lx_create_buffers))
#define MEDIA_IOC_DEVICE_INFO      IOC(3, '|', 0, sizeof(struct lx_media_info))
#define MEDIA_IOC_REQUEST_ALLOC    IOC(2, '|', 5, 4)
#define MEDIA_REQUEST_IOC_QUEUE    IOC(0, '|', 0x80, 0)
#define MEDIA_REQUEST_IOC_REINIT   IOC(0, '|', 0x81, 0)

#define TYPE_CAPTURE  9                      /* V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE */
#define TYPE_OUTPUT   10                     /* V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE */
#define MEMORY_MMAP   1
#define CAP_M2M_MPLANE 0x00004000u
#define FLAG_ERROR     0x00000040u
#define FLAG_REQUEST_FD 0x00800000u
#define WHICH_CUR_VAL     0
#define WHICH_REQUEST_VAL 0x0f010000u
#define PIX_FMT_HEVC_SLICE 0x35363253u       /* ('S','2','6','5') */
#define CID_HEVC_SPS            0xa40a90u
#define CID_HEVC_PPS            0xa40a91u
#define CID_HEVC_SLICE_PARAMS   0xa40a92u
#define CID_HEVC_SCALING_MATRIX 0xa40a93u
#define CID_HEVC_DECODE_PARAMS  0xa40a94u
#define CID_HEVC_DECODE_MODE    0xa40a95u
#define CID_HEVC_START_CODE     0xa40a96u
#define O_RDWR_    2
#define O_NONBLOCK_ 04000
#define O_CLOEXEC_ 02000000
#define POLLIN_  1
#define POLLPRI_ 2
#define POLLOUT_ 4
#define LX_EBUSY 16
#define LX_EINTR 4
#define DRM_NV12 0x3231564eu                 /* ('N','V','1','2') */
#define DRM_P030 0x30333050u                 /* ('P','0','3','0') */

#define LX_SRCS  8                           /* OUTPUT buffers (slices in flight), each with its request */
#define LX_WAIT_CS 200                       /* nothing back from the driver in this: it's stopped */

struct hevcdec_lx {
    int vfd, mfd;
    int streaming;
    struct lx_format cap_fmt;                /* the CAPTURE format (for CREATE_BUFS) */
    struct { void *p; uint32_t len; int req, busy, used; } src[LX_SRCS];
    int nsrc, srcs_busy;
    struct { void *p; uint32_t len; int expfd; hevcdec_frame *f; } cap[VB2_MAX_FRAME];
    int ncap;
    unsigned queued;                         /* CAPTURE buffers with the driver */
    struct v4l2_ctrl_hevc_decode_params dec; /* (a picture's, its references' timestamps in ns) */
};

static int lsys_ioctl(int fd, uint32_t req, void *arg)
{
    return hevcdec_lsys(LSYS_ioctl, (uint32_t)fd, req, (uint32_t)(uintptr_t)arg, 0, 0, 0);
}

static void lsys_close(int fd)
{
    if (fd >= 0) hevcdec_lsys(LSYS_close, (uint32_t)fd, 0, 0, 0, 0, 0);
}

static int lsys_open(const char *path)
{
    return hevcdec_lsys(LSYS_open, (uint32_t)(uintptr_t)path, O_RDWR_ | O_NONBLOCK_ | O_CLOEXEC_, 0, 0, 0, 0);
}

/* the buffer's mapping into RISC OS's memory, or NULL */
static void *lsys_mmap(int fd, uint32_t len, uint32_t offset)
{
    int r = hevcdec_lsys(LSYS_mmap2, 0, len, 3 /* PROT_READ | PROT_WRITE */, 1 /* MAP_SHARED */, (uint32_t)fd, offset >> 12);
    return (uint32_t)r >= 0xFFFFF001u ? NULL : (void *)(uintptr_t)(uint32_t)r;
}

static int lx_fail(hevcdec *d, const char *what, int r)
{
    return hevcdec_fail(d, "Linux's rpivid: %s failed (Linux error %d)", what, -r);
}

/* ---- opening: the device found, the formats set, the OUTPUT buffers made ---- */

/* the device whose driver is rpivid (as it's built into ROL's kernel: /dev/video19), or -1 */
static int find_video(void)
{
    for (int k = -1; k < 64; k++) {
        char path[24];
        struct lx_capability c;
        int fd, n = k < 0 ? 19 : k;
        if (k == 19) continue;
        snprintf(path, sizeof path, "/dev/video%d", n);
        if ((fd = lsys_open(path)) < 0) {
            if (fd == -LSYS_ENOSYS) return -1;
            continue;
        }
        memset(&c, 0, sizeof c);
        if (lsys_ioctl(fd, VIDIOC_QUERYCAP, &c) == 0 && !strncmp(c.driver, "rpivid", sizeof c.driver) &&
            ((c.device_caps ? c.device_caps : c.capabilities) & CAP_M2M_MPLANE))
            return fd;
        lsys_close(fd);
    }
    return -1;
}

/* its media device (its model "rpivid"), for the requests, or -1 */
static int find_media(void)
{
    for (int n = 0; n < 16; n++) {
        char path[24];
        struct lx_media_info mi;
        int fd;
        snprintf(path, sizeof path, "/dev/media%d", n);
        if ((fd = lsys_open(path)) < 0) continue;
        memset(&mi, 0, sizeof mi);
        if (lsys_ioctl(fd, MEDIA_IOC_DEVICE_INFO, &mi) == 0 && !strncmp(mi.model, "rpivid", sizeof mi.model)) return fd;
        lsys_close(fd);
    }
    return -1;
}

/* the bytes a slice's OUTPUT buffer holds: rpivid's own size for a
   picture's bitstream (rpivid_video.c's rpivid_bit_buf_size) */
static uint32_t src_size(unsigned w, unsigned h, int bits_minus8)
{
    uint32_t wxh = w * h, n;
    n = wxh < 983040 ? wxh * 3 / 4 : wxh < 983040 * 2 ? 983040 * 3 / 4 : wxh * 3 / 8;
    n += n * (uint32_t)bits_minus8 / 8;
    n = (n + 0xFFFF) & ~0xFFFFu;
    return n < 0x10000 ? 0x10000 : n;
}

static int set_ctrl_now(struct hevcdec_lx *lx, uint32_t id, int32_t value)
{
    struct lx_ext_control c;
    struct lx_ext_controls cs;
    memset(&c, 0, sizeof c);
    memset(&cs, 0, sizeof cs);
    c.id = id;
    c.ptr = (uint32_t)value;
    cs.which = WHICH_CUR_VAL;
    cs.count = 1;
    cs.controls = (uint32_t)(uintptr_t)&c;
    return lsys_ioctl(lx->vfd, VIDIOC_S_EXT_CTRLS, &cs);
}

static int open_fail(hevcdec *d, char *err, size_t errlen, const char *what, int r)
{
    snprintf(err, errlen, "Linux's rpivid: %s failed (Linux error %d)", what, -r);
    hevcdec_lx_close(d);
    return -1;
}

int hevcdec_lx_open(hevcdec *d, char *err, size_t errlen)
{
    struct hevcdec_lx *lx;
    struct lx_format f;
    struct lx_requestbuffers rb;
    struct v4l2_pix_format_mplane *df = &d->ctx.dst_fmt;
    uint32_t want = d->cfg.bit_depth == 10 ? V4L2_PIX_FMT_NV12_10_COL128 : V4L2_PIX_FMT_NV12_COL128;
    int r, vfd;
    if (!hevcdec_lsys_available()) return 1;        /* (not on Linux: the block, driven directly) */
    if ((vfd = find_video()) < 0) {
        hevcdec_logf(d, "RISC OS on Linux, but Linux has no rpivid device (/dev/video*): trying the block directly");
        return 1;
    }
    if (!(lx = calloc(1, sizeof *lx))) { lsys_close(vfd); snprintf(err, errlen, "Out of memory"); return -1; }
    d->lx = lx;
    lx->vfd = vfd;
    for (int i = 0; i < LX_SRCS; i++) lx->src[i].req = -1;
    for (int i = 0; i < VB2_MAX_FRAME; i++) lx->cap[i].expfd = -1;
    if ((lx->mfd = find_media()) < 0) return open_fail(d, err, errlen, "finding its media device (/dev/media*)", -LSYS_ENOENT);
    /* OUTPUT: the slices (setting it clears CAPTURE's format) */
    memset(&f, 0, sizeof f);
    f.type = TYPE_OUTPUT;
    f.fmt.pix_mp.width = (uint32_t)d->cfg.width;
    f.fmt.pix_mp.height = (uint32_t)d->cfg.height;
    f.fmt.pix_mp.pixelformat = PIX_FMT_HEVC_SLICE;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = src_size((unsigned)d->cfg.width, (unsigned)d->cfg.height, d->cfg.bit_depth - 8);
    if ((r = lsys_ioctl(vfd, VIDIOC_S_FMT, &f)) < 0) return open_fail(d, err, errlen, "setting the slices' format", r);
    if (f.fmt.pix_mp.pixelformat != PIX_FMT_HEVC_SLICE)
        return open_fail(d, err, errlen, "setting the slices' format (not HEVC slices)", -22);
    /* CAPTURE: the frames, NV12 (or 10-bit) in 128-byte columns, the size the frames are made at */
    memset(&f, 0, sizeof f);
    f.type = TYPE_CAPTURE;
    f.fmt.pix_mp.width = (uint32_t)d->cfg.width;
    f.fmt.pix_mp.height = (uint32_t)d->cfg.height;
    f.fmt.pix_mp.pixelformat = want;
    f.fmt.pix_mp.num_planes = 1;
    if ((r = lsys_ioctl(vfd, VIDIOC_S_FMT, &f)) < 0) return open_fail(d, err, errlen, "setting the frames' format", r);
    memset(&f, 0, sizeof f);
    f.type = TYPE_CAPTURE;
    if ((r = lsys_ioctl(vfd, VIDIOC_G_FMT, &f)) < 0) return open_fail(d, err, errlen, "reading the frames' format", r);
    if (f.fmt.pix_mp.pixelformat != want || f.fmt.pix_mp.width < (uint32_t)d->cfg.width ||
        f.fmt.pix_mp.height < (uint32_t)d->cfg.height || f.fmt.pix_mp.plane_fmt[0].bytesperline < f.fmt.pix_mp.height * 3 / 2 ||
        f.fmt.pix_mp.plane_fmt[0].sizeimage < f.fmt.pix_mp.plane_fmt[0].bytesperline * (uint64_t)f.fmt.pix_mp.width *
                                               (d->cfg.bit_depth == 10 ? 4 : 3) / 3) {
        snprintf(err, errlen, "Linux's rpivid gave frames %ux%u, format &%08X, columns of %u lines, %u bytes: not ones "
                 "hevcdec reads", (unsigned)f.fmt.pix_mp.width, (unsigned)f.fmt.pix_mp.height, (unsigned)f.fmt.pix_mp.pixelformat,
                 (unsigned)f.fmt.pix_mp.plane_fmt[0].bytesperline, (unsigned)f.fmt.pix_mp.plane_fmt[0].sizeimage);
        hevcdec_lx_close(d);
        return -1;
    }
    lx->cap_fmt = f;
    df->width = f.fmt.pix_mp.width;
    df->height = f.fmt.pix_mp.height;
    df->pixelformat = f.fmt.pix_mp.pixelformat;
    df->plane_fmt[0].bytesperline = f.fmt.pix_mp.plane_fmt[0].bytesperline;
    df->plane_fmt[0].sizeimage = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    d->ctx.dst_fmt_set = 1;
    /* slice by slice, each without a start code (as hevcdec is given them) */
    if ((r = set_ctrl_now(lx, CID_HEVC_DECODE_MODE, 0)) < 0) return open_fail(d, err, errlen, "setting slice-based decoding", r);
    if ((r = set_ctrl_now(lx, CID_HEVC_START_CODE, 0)) < 0) return open_fail(d, err, errlen, "setting no start codes", r);
    /* the OUTPUT buffers, mapped, each with a request */
    memset(&rb, 0, sizeof rb);
    rb.count = LX_SRCS;
    rb.type = TYPE_OUTPUT;
    rb.memory = MEMORY_MMAP;
    if ((r = lsys_ioctl(vfd, VIDIOC_REQBUFS, &rb)) < 0) return open_fail(d, err, errlen, "making the slice buffers", r);
    if (!rb.count) return open_fail(d, err, errlen, "making the slice buffers (none made)", -12);
    lx->nsrc = rb.count < LX_SRCS ? (int)rb.count : LX_SRCS;
    for (int i = 0; i < lx->nsrc; i++) {
        struct lx_buffer b;
        struct lx_plane pl;
        int q;
        memset(&b, 0, sizeof b);
        memset(&pl, 0, sizeof pl);
        b.index = (uint32_t)i;
        b.type = TYPE_OUTPUT;
        b.memory = MEMORY_MMAP;
        b.length = 1;
        b.m = (uint32_t)(uintptr_t)&pl;
        if ((r = lsys_ioctl(vfd, VIDIOC_QUERYBUF, &b)) < 0) return open_fail(d, err, errlen, "finding a slice buffer", r);
        if (!(lx->src[i].p = lsys_mmap(vfd, pl.length, pl.m)))
            return open_fail(d, err, errlen, "mapping a slice buffer", -12);
        lx->src[i].len = pl.length;
        if ((r = lsys_ioctl(lx->mfd, MEDIA_IOC_REQUEST_ALLOC, &q)) < 0)
            return open_fail(d, err, errlen, "making a request", r);
        lx->src[i].req = q;
    }
    return 0;
}

/* ---- frames: CAPTURE buffers ---- */

int hevcdec_lx_frame_new(hevcdec *d, hevcdec_frame *f)
{
    struct hevcdec_lx *lx = d->lx;
    struct lx_create_buffers cb;
    struct lx_buffer b;
    struct lx_plane pl;
    int i, r;
    for (i = 0; i < lx->ncap; i++)                   /* (a buffer whose frame has gone: kept, and used again) */
        if (lx->cap[i].p && !lx->cap[i].f) break;
    if (i == lx->ncap) {
        memset(&cb, 0, sizeof cb);
        cb.count = 1;
        cb.memory = MEMORY_MMAP;
        cb.format = lx->cap_fmt;
        if ((r = lsys_ioctl(lx->vfd, VIDIOC_CREATE_BUFS, &cb)) < 0) return lx_fail(d, "making a frame", r);
        if (cb.count != 1 || cb.index >= VB2_MAX_FRAME || cb.index != (uint32_t)lx->ncap)
            return hevcdec_fail(d, "Linux's rpivid: making a frame gave %u buffers from %u", (unsigned)cb.count,
                                (unsigned)cb.index);
        i = (int)cb.index;
        memset(&b, 0, sizeof b);
        memset(&pl, 0, sizeof pl);
        b.index = (uint32_t)i;
        b.type = TYPE_CAPTURE;
        b.memory = MEMORY_MMAP;
        b.length = 1;
        b.m = (uint32_t)(uintptr_t)&pl;
        if ((r = lsys_ioctl(lx->vfd, VIDIOC_QUERYBUF, &b)) < 0) return lx_fail(d, "finding a frame", r);
        if (pl.length < d->ctx.dst_fmt.plane_fmt[0].sizeimage)
            return hevcdec_fail(d, "Linux's rpivid: a frame of %u bytes, not %u", (unsigned)pl.length,
                                (unsigned)d->ctx.dst_fmt.plane_fmt[0].sizeimage);
        lx->ncap = i + 1;                            /* (the driver has it now, mapped or not) */
        if (!(lx->cap[i].p = lsys_mmap(lx->vfd, pl.length, pl.m))) return hevcdec_fail(d, "Linux's rpivid: mapping a frame failed");
        lx->cap[i].len = pl.length;
    }
    lx->cap[i].f = f;
    f->cap = i;
    f->lx_queued = 0;
    f->vb.vaddr = lx->cap[i].p;
    f->vb.planes[0].length = lx->cap[i].len;
    f->vb.vb2_buf.index = (unsigned)i;
    return 0;
}

void hevcdec_lx_frame_gone(hevcdec *d, hevcdec_frame *f)
{
    if (f->cap >= 0 && d->lx->cap[f->cap].f == f) d->lx->cap[f->cap].f = NULL;
    f->cap = -1;
}

/* ---- buffers back ---- */

int hevcdec_lx_poll(hevcdec *d)
{
    struct hevcdec_lx *lx = d->lx;
    if (!lx->streaming || d->dead) return 0;
    for (;;) {                                       /* slices done with */
        struct lx_buffer b;
        struct lx_plane pl;
        int r;
        memset(&b, 0, sizeof b);
        b.type = TYPE_OUTPUT;
        b.memory = MEMORY_MMAP;
        b.length = 1;
        b.m = (uint32_t)(uintptr_t)&pl;
        if ((r = lsys_ioctl(lx->vfd, VIDIOC_DQBUF, &b)) == -LSYS_EAGAIN) break;
        if (r < 0) { d->dead = 1; return lx_fail(d, "taking back a slice buffer", r); }
        if (b.index < (uint32_t)lx->nsrc && lx->src[b.index].busy) { lx->src[b.index].busy = 0; lx->srcs_busy--; }
    }
    for (;;) {                                       /* pictures decoded */
        struct lx_buffer b;
        struct lx_plane pl;
        hevcdec_frame *f;
        int r;
        memset(&b, 0, sizeof b);
        b.type = TYPE_CAPTURE;
        b.memory = MEMORY_MMAP;
        b.length = 1;
        b.m = (uint32_t)(uintptr_t)&pl;
        if ((r = lsys_ioctl(lx->vfd, VIDIOC_DQBUF, &b)) == -LSYS_EAGAIN) break;
        if (r < 0) { d->dead = 1; return lx_fail(d, "taking back a frame", r); }
        if (lx->queued) lx->queued--;
        d->pics_done++;
        if (b.index < (uint32_t)lx->ncap && (f = lx->cap[b.index].f) && f->lx_queued) {
            f->lx_queued = 0;
            if (b.flags & FLAG_ERROR) {
                f->vb.state = VB2_BUF_STATE_ERROR;
                snprintf(f->err, sizeof f->err, "Linux's rpivid failed the picture (its log may say why: *IXRun dmesg)");
            } else
                f->vb.state = VB2_BUF_STATE_DONE;
        }
    }
    return 0;
}

int hevcdec_lx_idle(const hevcdec *d)
{
    return !d->lx->queued && !d->lx->srcs_busy;
}

/* waits for the driver: until done(), polling (POLLIN: a frame back, POLLOUT: a slice buffer) */
static int lx_wait_until(hevcdec *d, int (*done)(hevcdec *, void *), void *arg, int16_t events)
{
    struct hevcdec_lx *lx = d->lx;
    uint32_t t0 = hevcdec_hw_now_cs(), last = t0;
    int r = HEVCDEC_OK;
    if (!done(d, arg)) d->stats.waits++;
    while (!done(d, arg)) {
        struct lx_pollfd p;
        int n;
        if (d->dead || hevcdec_lx_poll(d)) { r = HEVCDEC_ERROR; break; }
        if (done(d, arg)) break;
        if (hevcdec_lx_idle(d)) { r = hevcdec_fail(d, "The decode stopped: nothing with Linux's rpivid"); break; }
        p.fd = lx->vfd;
        p.events = events;
        p.revents = 0;
        n = hevcdec_lsys(LSYS_poll, (uint32_t)(uintptr_t)&p, 1, 20, 0, 0, 0);
        if (n < 0 && n != -LX_EINTR) { d->dead = 1; r = lx_fail(d, "waiting", n); break; }
        if (n > 0) last = hevcdec_hw_now_cs();
        else if (hevcdec_hw_now_cs() - last > LX_WAIT_CS) {
            d->dead = 1;
            r = hevcdec_fail(d, "Linux's rpivid gave nothing back in %d cs (%u frames, %d slices with it): decoder "
                             "stopped", LX_WAIT_CS, lx->queued, lx->srcs_busy);
            break;
        }
    }
    d->stats.cs_wait += hevcdec_hw_now_cs() - t0;
    return r;
}

static int frame_back(hevcdec *d, void *a)
{
    const hevcdec_frame *f = a;
    (void)d;
    return !f->submitted || f->vb.state || !f->lx_queued;
}
static int all_back(hevcdec *d, void *a) { (void)a; return hevcdec_lx_idle(d); }
static int src_free(hevcdec *d, void *a) { (void)a; return d->lx->srcs_busy < d->lx->nsrc; }

int hevcdec_lx_wait(hevcdec *d, hevcdec_frame *f)
{
    if (f) return f->lx_queued ? lx_wait_until(d, frame_back, f, POLLIN_) : HEVCDEC_OK;
    return lx_wait_until(d, all_back, NULL, POLLIN_ | POLLOUT_);
}

/* ---- decoding ---- */

/* a slice buffer free, its request ready to use again */
static int src_get(hevcdec *d)
{
    struct hevcdec_lx *lx = d->lx;
    for (;;) {
        int r;
        if (hevcdec_lx_poll(d)) return -1;
        if (!src_free(d, NULL) && lx_wait_until(d, src_free, NULL, POLLOUT_) != HEVCDEC_OK) return -1;
        for (int i = 0; i < lx->nsrc; i++) {
            if (lx->src[i].busy) continue;
            if (!lx->src[i].used) return i;
            if ((r = lsys_ioctl(lx->src[i].req, MEDIA_REQUEST_IOC_REINIT, NULL)) == 0) { lx->src[i].used = 0; return i; }
            if (r != -LX_EBUSY) return lx_fail(d, "reusing a request", r), -1;
        }
        {                                            /* (requests not yet complete: a moment) */
            struct lx_pollfd p = { lx->src[0].req, POLLPRI_, 0 };
            hevcdec_lsys(LSYS_poll, (uint32_t)(uintptr_t)&p, 1, 2, 0, 0, 0);
        }
        if (d->dead) return -1;
    }
}

static void ctrl(struct lx_ext_control *c, uint32_t id, const void *p, size_t size)
{
    memset(c, 0, sizeof *c);
    c->id = id;
    c->size = (uint32_t)size;
    c->ptr = (uint32_t)(uintptr_t)p;
}

static int stream_on(hevcdec *d)
{
    struct hevcdec_lx *lx = d->lx;
    uint32_t t;
    int r;
    if (lx->streaming) return 0;
    t = TYPE_CAPTURE;
    if ((r = lsys_ioctl(lx->vfd, VIDIOC_STREAMON, &t)) < 0) return lx_fail(d, "starting the frames' queue", r);
    t = TYPE_OUTPUT;
    if ((r = lsys_ioctl(lx->vfd, VIDIOC_STREAMON, &t)) < 0) {
        t = TYPE_CAPTURE;
        lsys_ioctl(lx->vfd, VIDIOC_STREAMOFF, &t);
        return lx_fail(d, "starting the decoder", r);
    }
    lx->streaming = 1;
    return 0;
}

int hevcdec_lx_decode(hevcdec *d, const hevcdec_picture *pic, const struct v4l2_ctrl_hevc_sps *sps,
                      const struct v4l2_ctrl_hevc_scaling_matrix *scaling, hevcdec_frame *f, uint64_t number)
{
    struct hevcdec_lx *lx = d->lx;
    int r;
    if (f->cap < 0 || lx->cap[f->cap].f != f) return hevcdec_fail(d, "Not one of this decoder's frames");
    for (unsigned i = 0; i < pic->nslices; i++)      /* (refused before anything goes to the driver) */
        if (pic->slices[i].size > lx->src[0].len)
            return hevcdec_fail(d, "Slice %u: %u bytes, more than Linux's rpivid takes (%u)", i,
                                (unsigned)pic->slices[i].size, (unsigned)lx->src[0].len);
    if (number > 0x7FFFFFFFull * 1000000ull)
        return hevcdec_fail(d, "Picture number %llu: too big for Linux's timestamps", (unsigned long long)number);
    /* the frame finished with, if it was being decoded */
    if (hevcdec_lx_wait(d, f) != HEVCDEC_OK) return HEVCDEC_ERROR;
    if (stream_on(d)) return HEVCDEC_ERROR;
    d->err[0] = 0;
    f->used = 1;
    f->submitted = 0;
    f->clean = 0;
    f->err[0] = 0;
    f->vb.timestamp = number;
    f->vb.state = 0;
    lx->dec = *pic->dec;                             /* (references by timestamp: the driver's, in ns) */
    for (unsigned i = 0; i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) lx->dec.dpb[i].timestamp *= 1000;
    for (unsigned i = 0; i < pic->nslices; i++) {
        const hevcdec_slice *sl = &pic->slices[i];
        struct lx_ext_control c[5];
        struct lx_ext_controls cs;
        struct lx_buffer b;
        struct lx_plane pl;
        int s = src_get(d);
        if (s < 0) {
            if (i) d->dead = 1;
            return HEVCDEC_ERROR;
        }
        memcpy(lx->src[s].p, sl->data, sl->size);
        ctrl(&c[0], CID_HEVC_SPS, sps, sizeof *sps);
        ctrl(&c[1], CID_HEVC_PPS, pic->pps, sizeof *pic->pps);
        ctrl(&c[2], CID_HEVC_SCALING_MATRIX, scaling, sizeof *scaling);
        ctrl(&c[3], CID_HEVC_DECODE_PARAMS, &lx->dec, sizeof lx->dec);
        ctrl(&c[4], CID_HEVC_SLICE_PARAMS, sl->params, sizeof *sl->params);
        memset(&cs, 0, sizeof cs);
        cs.which = WHICH_REQUEST_VAL;
        cs.count = 5;
        cs.request_fd = lx->src[s].req;
        cs.controls = (uint32_t)(uintptr_t)c;
        lx->src[s].used = 1;                         /* (the request has values now: reinit before it's used again) */
        if ((r = lsys_ioctl(lx->vfd, VIDIOC_S_EXT_CTRLS, &cs)) < 0) {
            if (i) d->dead = 1;                      /* (the frame is with the driver, held for slices that won't come) */
            return hevcdec_fail(d, "Slice %u: Linux's rpivid refused it (control %u of 5, Linux error %d)%s", i,
                                (unsigned)cs.error_idx + 1, -r, i ? ": decoder stopped" : "");
        }
        if (!i) {                                    /* the frame: queued once the first slice is ready to go */
            memset(&b, 0, sizeof b);
            memset(&pl, 0, sizeof pl);
            b.index = (uint32_t)f->cap;
            b.type = TYPE_CAPTURE;
            b.memory = MEMORY_MMAP;
            b.length = 1;
            b.m = (uint32_t)(uintptr_t)&pl;
            if ((r = lsys_ioctl(lx->vfd, VIDIOC_QBUF, &b)) < 0) return lx_fail(d, "giving it the frame", r);
            f->lx_queued = 1;
            f->submitted = 1;                        /* (from here the driver has it: it comes back, done or failed) */
            lx->queued++;
            d->pics_given++;
        }
        memset(&b, 0, sizeof b);
        memset(&pl, 0, sizeof pl);
        pl.bytesused = (uint32_t)sl->size;
        pl.length = lx->src[s].len;
        b.index = (uint32_t)s;
        b.type = TYPE_OUTPUT;
        b.memory = MEMORY_MMAP;
        b.length = 1;
        b.m = (uint32_t)(uintptr_t)&pl;
        b.flags = FLAG_REQUEST_FD | (i + 1 < pic->nslices ? V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF : 0);
        b.request_fd = lx->src[s].req;
        b.tv_sec = (int32_t)(number / 1000000);
        b.tv_usec = (int32_t)(number % 1000000);
        if ((r = lsys_ioctl(lx->vfd, VIDIOC_QBUF, &b)) < 0) {
            d->dead = 1;
            return lx_fail(d, "giving it a slice", r);
        }
        if ((r = lsys_ioctl(lx->src[s].req, MEDIA_REQUEST_IOC_QUEUE, NULL)) < 0) {
            d->dead = 1;
            return lx_fail(d, "queuing a slice's request", r);
        }
        lx->src[s].busy = 1;
        lx->srcs_busy++;
    }
    d->stats.pictures++;
    if (d->cfg.pipelined) return hevcdec_lx_poll(d) ? HEVCDEC_ERROR : HEVCDEC_OK;
    return hevcdec_frame_wait(d, f);
}

/* ---- a frame for showing ---- */

int hevcdec_lx_export(hevcdec *d, const hevcdec_frame *f, hevcdec_export *e)
{
    struct hevcdec_lx *lx = d->lx;
    if (f->cap < 0 || lx->cap[f->cap].f != f) return hevcdec_fail(d, "Not one of this decoder's frames");
    if (lx->cap[f->cap].expfd < 0) {
        struct lx_exportbuffer x;
        int r;
        memset(&x, 0, sizeof x);
        x.type = TYPE_CAPTURE;
        x.index = (uint32_t)f->cap;
        x.flags = O_RDWR_ | O_CLOEXEC_;
        if ((r = lsys_ioctl(lx->vfd, VIDIOC_EXPBUF, &x)) < 0) return lx_fail(d, "exporting a frame", r);
        lx->cap[f->cap].expfd = x.fd;
    }
    e->fd = lx->cap[f->cap].expfd;
    e->drm_format = d->cfg.bit_depth == 10 ? DRM_P030 : DRM_NV12;
    e->col_height = d->ctx.dst_fmt.plane_fmt[0].bytesperline;
    e->modifier = (7ull << 56) | ((uint64_t)e->col_height << 8) | 4;   /* (DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT) */
    e->width = d->ctx.dst_fmt.width;
    e->height = d->ctx.dst_fmt.height;
    e->size = d->ctx.dst_fmt.plane_fmt[0].sizeimage;
    return HEVCDEC_OK;
}

/* ---- closing: the queues stopped, everything unmapped and closed (the
   driver frees its buffers with its file) ---- */

void hevcdec_lx_close(hevcdec *d)
{
    struct hevcdec_lx *lx = d->lx;
    if (!lx) return;
    if (lx->streaming && !d->dead && !hevcdec_lx_idle(d)) hevcdec_lx_wait(d, NULL);
    if (lx->streaming) {
        uint32_t t = TYPE_OUTPUT;
        lsys_ioctl(lx->vfd, VIDIOC_STREAMOFF, &t);
        t = TYPE_CAPTURE;
        lsys_ioctl(lx->vfd, VIDIOC_STREAMOFF, &t);
        lx->streaming = 0;
    }
    for (int i = 0; i < LX_SRCS; i++) {
        if (lx->src[i].p) hevcdec_lsys(LSYS_munmap, (uint32_t)(uintptr_t)lx->src[i].p, lx->src[i].len, 0, 0, 0, 0);
        lsys_close(lx->src[i].req);
    }
    for (int i = 0; i < VB2_MAX_FRAME; i++) {
        if (lx->cap[i].p) hevcdec_lsys(LSYS_munmap, (uint32_t)(uintptr_t)lx->cap[i].p, lx->cap[i].len, 0, 0, 0, 0);
        lsys_close(lx->cap[i].expfd);
    }
    lsys_close(lx->mfd);
    lsys_close(lx->vfd);
    free(lx);
    d->lx = NULL;
}
