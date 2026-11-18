/*
 * fake_rpivid.c - (0.1.15) a fake of Linux's rpivid driver (Raspberry
 * Pi's V4L2 stateless HEVC decoder, drivers/staging/media/rpivid in the
 * 6.6 kernel) and the media request API, as RISC OS on Linux reaches them:
 * it takes the place of hevcdec_lsys.c, so every Linux call hevcdec makes
 * comes here (open, close, ioctl, mmap2, munmap, poll), with the 32-bit
 * structures a 32-bit ARM program passes.
 *
 * It behaves as the driver does (rpivid_video.c, rpivid.c, and the m2m
 * and vb2 cores it uses):
 *  - /dev/video19 answers VIDIOC_QUERYCAP with driver "rpivid" (and
 *    /dev/video10 is another driver's), /dev/media0 has model "rpivid";
 *  - formats: OUTPUT HEVC slices (sizeimage at least 4K, as given),
 *    CAPTURE NV12 or 10-bit in 128-byte columns, width up to whole columns
 *    (96 samples for 10-bit), height to 16, bytesperline the column height
 *    (height x 3/2) - and setting OUTPUT's clears CAPTURE's;
 *  - OUTPUT buffers need requests; a request needs one buffer and the
 *    decode and slice parameters, and can't be reinitialised until done;
 *  - a job runs when a slice and a frame are both ready and both queues
 *    stream: the frame is the first CAPTURE buffer queued (or the one held
 *    by the last slice, if it had V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF and
 *    this slice has the same timestamp); the timestamp is copied to it;
 *  - references are found among the CAPTURE buffers by their timestamp
 *    (nanoseconds): each must be there (not the frame being written: one
 *    queued again keeps its timestamp and picture until a job takes it,
 *    as vb2_find_buffer finds it) and hold the
 *    picture with that POC;
 *  - a picture (its slices' bytes hashed, fake_hevc.c's way) is "decoded"
 *    by writing the test's picture for that hash into the frame, as the
 *    block writes it (fake_hevc.c's layout).
 * Jobs take fake_rpivid.ticks calls to finish (time passing).
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hevcdec_lsys.h"
#include "hevc_ctrls.h"
#include "fake_rpivid.h"

fake_rpivid_state fake_rpivid;

#define CHECK(c, ...) do { if (!(c)) { fake_rpivid.fails++; if (!fake_rpivid.quiet) { printf("FAIL (fake rpivid): " __VA_ARGS__); printf("\n"); } } } while (0)

/* the 32-bit structures (as hevcdec_linux.c) */
struct capability { char driver[16], card[32], bus_info[32]; uint32_t version, capabilities, device_caps, reserved[3]; };
struct plane_fmt { uint32_t sizeimage, bytesperline; uint16_t reserved[6]; };
struct pix_mp {
    uint32_t width, height, pixelformat, field, colorspace;
    struct plane_fmt plane_fmt[8];
    uint8_t num_planes, flags, ycbcr_enc, quantization, xfer_func, reserved[7];
};
struct format { uint32_t type; union { struct pix_mp pix_mp; uint8_t raw[200]; } fmt; };
struct requestbuffers { uint32_t count, type, memory, capabilities; uint8_t flags, reserved[3]; };
struct plane { uint32_t bytesused, length, m, data_offset, reserved[11]; };
struct buffer {
    uint32_t index, type, bytesused, flags, field;
    int32_t tv_sec, tv_usec;
    uint32_t timecode[4];
    uint32_t sequence, memory, m, length, reserved2;
    int32_t request_fd;
};
struct create_buffers { uint32_t index, count, memory; struct format format; uint32_t capabilities, flags, reserved[6]; };
struct exportbuffer { uint32_t type, index, plane, flags; int32_t fd; uint32_t reserved[11]; };
struct __attribute__((packed)) ext_control { uint32_t id, size, reserved2, ptr, ptr_hi; };
struct ext_controls { uint32_t which, count, error_idx; int32_t request_fd; uint32_t reserved, controls; };
struct media_info { char driver[16], model[32], serial[40], bus_info[32]; uint32_t media_version, hw_revision, driver_version, reserved[31]; };
struct pollfd_ { int32_t fd; int16_t events, revents; };

#define IOC(dir, type, nr, size) (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)(type) << 8) | (uint32_t)(nr))
#define CAP_BUFS 32
#define OUT_BUFS 32
#define NFILES 128
#define NC12 0x3231434eu
#define NC30 0x3033434eu
#define S265 0x35363253u
#define HOLD 0x200u
#define REQ_FD 0x00800000u
#define ERR_FLAG 0x40u
#define EBADF 9
#define ENOMEM 12
#define EBUSY 16
#define EINVAL 22
#define ENOTTY 25
#define EBADR 53

enum { F_NONE, F_VIDEO, F_OTHER_VIDEO, F_MEDIA, F_REQUEST, F_DMABUF };
static struct { int kind, req; } files[NFILES];

/* the requests: each holds its controls and (once queued) its one buffer */
#define NREQ 64
static struct {
    int used, queued, complete, has_buf, buf;
    int has[5];
    struct v4l2_ctrl_hevc_sps sps;
    struct v4l2_ctrl_hevc_pps pps;
    struct v4l2_ctrl_hevc_scaling_matrix sm;
    struct v4l2_ctrl_hevc_decode_params dec;
    struct v4l2_ctrl_hevc_slice_params sp;
} reqs[NREQ];

static struct {
    int open;                                  /* the video device's file */
    struct pix_mp src_fmt, dst_fmt;
    int dst_set, src_on, dst_on;
    struct { uint8_t *p; uint32_t len; int mapped, state, req; uint64_t ts; uint32_t bytesused, flags; } out[OUT_BUFS];
    int nout;
    struct { uint8_t *p; uint32_t len; int mapped, state, err, bad; uint64_t ts; int32_t poc; int has_pic; uint32_t hash; int nslices; } cap[CAP_BUFS];
    int ncap;
    int out_ready[OUT_BUFS], nout_ready, out_done[OUT_BUFS], nout_done;
    int cap_ready[CAP_BUFS], ncap_ready, cap_done[CAP_BUFS], ncap_done;
    int held;                                   /* the CAPTURE buffer held for more slices, or -1 */
    int busy, left, job_out, job_cap;           /* a job running: its buffers, calls left */
    int decode_mode, start_code;
} v;

enum { B_DEQUEUED, B_QUEUED, B_DONE };
static int nslice_ctrls;                        /* (requests' controls set: for refuse_slice) */          /* (states: with the program, with the driver, finished) */

void fake_rpivid_reset(void)
{
    int present = fake_rpivid.present;
    for (int i = 0; i < OUT_BUFS; i++) free(v.out[i].p);
    for (int i = 0; i < CAP_BUFS; i++) free(v.cap[i].p);
    memset(&v, 0, sizeof v);
    v.held = -1;
    memset(files, 0, sizeof files);
    memset(reqs, 0, sizeof reqs);
    nslice_ctrls = 0;
    memset(&fake_rpivid, 0, sizeof fake_rpivid);
    fake_rpivid.present = present;
    fake_rpivid.ticks = 1;
}

int fake_rpivid_live(void)
{
    int n = 0;
    for (int i = 0; i < NFILES; i++) n += files[i].kind != F_NONE;
    return n + fake_rpivid.maps - fake_rpivid.unmaps;
}

int hevcdec_lsys_available(void) { return fake_rpivid.present; }

static int new_fd(int kind)
{
    for (int i = 3; i < NFILES; i++)
        if (!files[i].kind) {
            files[i].kind = kind;
            fake_rpivid.opens++;
            return i;
        }
    return -24;                                     /* (EMFILE) */
}

static void free_video(void)
{
    for (int i = 0; i < OUT_BUFS; i++) { free(v.out[i].p); v.out[i].p = NULL; }
    for (int i = 0; i < CAP_BUFS; i++) { free(v.cap[i].p); v.cap[i].p = NULL; }
    memset(&v, 0, sizeof v);
    v.held = -1;
}

/* ---- formats (rpivid_video.c) ---- */

static void prepare_dst(struct pix_mp *f)
{
    uint32_t w = f->width ? f->width : 1920, h = f->height ? f->height : 1088, bpl = f->plane_fmt[0].bytesperline,
             si = f->plane_fmt[0].sizeimage, min;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    if (f->pixelformat != NC30) f->pixelformat = NC12;
    h = (h + 15) & ~15u;
    if (f->pixelformat == NC12) w = (w + 127) & ~127u;
    else w = ((w + 2) / 3 + 31) / 32 * 32 * 3;
    min = h * 3 / 2;
    bpl = bpl >= min && bpl < 2 * min ? bpl : min;
    min = f->pixelformat == NC12 ? bpl * w : bpl * w * 4 / 3;
    si = si >= min && si < 2 * min ? si : min;
    f->width = w; f->height = h;
    f->num_planes = 1;
    f->plane_fmt[0].bytesperline = bpl;
    f->plane_fmt[0].sizeimage = si;
}

/* ---- the job: one slice into its frame (rpivid_h265.c's setup and trigger, the m2m core's choices) ---- */

static int cap_by_ts(uint64_t ts)
{
    for (int i = 0; i < v.ncap; i++) if (v.cap[i].p && v.cap[i].ts == ts && v.cap[i].has_pic) return i;
    return -1;
}

static void write_picture(int c)
{
    fake_hevc_picture_fn fn = fake_hevc_get_pictures();
    const void *py = NULL, *pu = NULL, *pv = NULL;
    int pw = 0, ph = 0, bytes = 0, ten = v.dst_fmt.pixelformat == NC30;
    uint8_t *out = v.cap[c].p;
    size_t col = (size_t)v.dst_fmt.plane_fmt[0].bytesperline * 128, coff = (size_t)v.dst_fmt.height * 128;
    memset(out, 0x80, v.cap[c].len);
    if (v.cap[c].nslices && fn && fn(v.cap[c].hash, &py, &pu, &pv, &pw, &ph, &bytes) == 0 && bytes == (ten ? 2 : 1)) {
        int cw = (pw + 1) / 2, ch = (ph + 1) / 2;
        CHECK(pw <= (int)v.dst_fmt.width && ph <= (int)v.dst_fmt.height, "a %dx%d picture in a %ux%u frame", pw, ph,
              (unsigned)v.dst_fmt.width, (unsigned)v.dst_fmt.height);
        if (ten) {
            const uint16_t *sy = py, *su = pu, *sv = pv;
            for (int x0 = 0; x0 < pw; x0 += 96) {
                uint32_t *cy = (uint32_t *)(void *)(out + (size_t)(x0 / 96) * col), *cc = (uint32_t *)(void *)(out + coff + (size_t)(x0 / 96) * col);
                for (int r = 0; r < ph; r++)
                    for (int k = 0; k < 32; k++) {
                        uint32_t wd = 0;
                        for (int j = 0; j < 3; j++) {
                            int x = x0 + 3 * k + j;
                            wd |= (uint32_t)(x < pw ? sy[(size_t)r * pw + x] & 0x3FF : 0) << (10 * j);
                        }
                        cy[(size_t)r * 32 + k] = wd;
                    }
                for (int r = 0; r < ch; r++)
                    for (int k = 0; k < 32; k++) {
                        uint32_t wd = 0;
                        for (int j = 0; j < 3; j++) {
                            int i = 3 * k + j, x = x0 / 2 + i / 2;
                            const uint16_t *pl = i & 1 ? sv : su;
                            wd |= (uint32_t)(x < cw ? pl[(size_t)r * cw + x] & 0x3FF : 0) << (10 * j);
                        }
                        cc[(size_t)r * 32 + k] = wd;
                    }
            }
        } else
            for (int x0 = 0; x0 < pw; x0 += 128) {
                uint8_t *cy = out + (size_t)(x0 / 128) * col, *cc = out + coff + (size_t)(x0 / 128) * col;
                int n = pw - x0 < 128 ? pw - x0 : 128, nc = cw - x0 / 2 < 64 ? cw - x0 / 2 : 64;
                for (int r = 0; r < ph; r++) memcpy(cy + (size_t)r * 128, (const uint8_t *)py + (size_t)r * pw + x0, (size_t)n);
                for (int r = 0; r < ch; r++)
                    for (int x = 0; x < nc; x++) {
                        cc[(size_t)r * 128 + 2 * x] = ((const uint8_t *)pu)[(size_t)r * cw + x0 / 2 + x];
                        cc[(size_t)r * 128 + 2 * x + 1] = ((const uint8_t *)pv)[(size_t)r * cw + x0 / 2 + x];
                    }
            }
    } else {
        fake_rpivid.unknown_pictures++;
        memset(out, 0x10, v.cap[c].len);
    }
}

static void cap_finish(int c, int err)
{
    v.cap[c].state = B_DONE;
    v.cap[c].err = err;
    v.cap_done[v.ncap_done++] = c;
    if (!err) {
        fake_rpivid.pictures++;
        if (fake_rpivid.fail_pic && fake_rpivid.pictures == fake_rpivid.fail_pic) {
            v.cap[c].err = 1;
            fake_rpivid.errors_given++;
        }
    }
    if (!v.cap[c].err) write_picture(c);
}

/* the next job, if one can run (v4l2_m2m_try_schedule) */
static void start_job(void)
{
    int o, c;
    if (v.busy || !v.src_on || !v.dst_on || !v.nout_ready) return;
    o = v.out_ready[0];
    if (v.held >= 0 && v.cap[v.held].ts != v.out[o].ts) {   /* (a new picture: the held frame's done) */
        cap_finish(v.held, v.cap[v.held].bad);
        v.held = -1;
    }
    if (v.held < 0 && !v.ncap_ready) return;
    memmove(v.out_ready, v.out_ready + 1, sizeof v.out_ready[0] * (size_t)--v.nout_ready);
    if (v.held >= 0) c = v.held;
    else {
        c = v.cap_ready[0];
        memmove(v.cap_ready, v.cap_ready + 1, sizeof v.cap_ready[0] * (size_t)--v.ncap_ready);
        v.cap[c].ts = v.out[o].ts;                  /* (the timestamp copied) */
        v.cap[c].hash = 2166136261u;
        v.cap[c].nslices = 0;
        v.cap[c].has_pic = 0;
        v.cap[c].bad = 0;
    }
    v.busy = 1;
    v.left = fake_rpivid.ticks;
    v.job_out = o;
    v.job_cap = c;
}

static void run_job(void)
{
    int o = v.job_out, c = v.job_cap, r = v.out[o].req, err = 0;
    const struct v4l2_ctrl_hevc_slice_params *sp = &reqs[r].sp;
    const struct v4l2_ctrl_hevc_decode_params *dec = &reqs[r].dec;
    const struct v4l2_ctrl_hevc_sps *sps = &reqs[r].sps;
    v.busy = 0;
    fake_rpivid.slices++;
    /* what rpivid_h265.c's setup checks */
    CHECK(v.out[o].bytesused >= (sp->bit_size + 7) / 8, "slice: to bit %u, %u bytes in the buffer", (unsigned)sp->bit_size,
          (unsigned)v.out[o].bytesused);
    if (sp->data_byte_offset >= (sp->bit_size + 7) / 8 || v.out[o].bytesused < (sp->bit_size + 7) / 8) {
        fake_rpivid.slices_refused++;            /* (setup refuses it: the picture fails) */
        err = 1;
    } else {
        v.cap[c].hash = (v.cap[c].hash ^ fake_hevc_hash(v.out[o].p + sp->data_byte_offset,
                                                       (sp->bit_size + 7) / 8 - sp->data_byte_offset)) * 16777619u;
        v.cap[c].nslices++;
    }
    CHECK(sps->pic_width_in_luma_samples <= v.dst_fmt.width && sps->pic_height_in_luma_samples <= v.dst_fmt.height &&
          (sps->bit_depth_luma_minus8 == 2) == (v.dst_fmt.pixelformat == NC30),
          "SPS %ux%u, %u-bit for frames %ux%u %s", (unsigned)sps->pic_width_in_luma_samples,
          (unsigned)sps->pic_height_in_luma_samples, sps->bit_depth_luma_minus8 + 8u, (unsigned)v.dst_fmt.width,
          (unsigned)v.dst_fmt.height, v.dst_fmt.pixelformat == NC30 ? "10-bit" : "8-bit");
    if (reqs[r].has[2]) {
        fake_rpivid.scaling_sent++;
        for (size_t k = 0; k < sizeof reqs[r].sm; k++)
            if (((const uint8_t *)&reqs[r].sm)[k] != 16) { fake_rpivid.scaling_not_flat++; break; }
    }
    for (int i = 0; i < dec->num_active_dpb_entries && i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) {   /* (vb2_find_buffer) */
        int k = cap_by_ts(dec->dpb[i].timestamp);
        /* (counted: the tests require none. The driver only warns, and decodes the picture anyway) */
        if (k < 0 || k == c) {
            fake_rpivid.ref_errors++;
            if (dec->dpb[i].timestamp % 1000) fake_rpivid.dpb_ns_wrong++;
            if (!fake_rpivid.quiet) fprintf(stderr, "fake rpivid: Missing DPB ent %d, timestamp=%llu\n", i, (unsigned long long)dec->dpb[i].timestamp);
        } else if (v.cap[k].poc != dec->dpb[i].pic_order_cnt_val) {
            fake_rpivid.ref_errors++;
            if (!fake_rpivid.quiet) fprintf(stderr, "fake rpivid: DPB ent %d (timestamp %llu): frame %d holds poc %d, not %d\n", i,
                  (unsigned long long)dec->dpb[i].timestamp, k, (int)v.cap[k].poc, (int)dec->dpb[i].pic_order_cnt_val);
        }
    }
    v.cap[c].poc = dec->pic_order_cnt_val;
    if (v.out[o].flags & HOLD) fake_rpivid.held++;
    /* the slice done, its request complete */
    v.out[o].state = B_DONE;
    v.out_done[v.nout_done++] = o;
    reqs[r].complete = 1;
    if (err) v.cap[c].bad = 1;
    if (!(v.out[o].flags & HOLD)) {                 /* (the picture's last slice: the frame done) */
        if (v.held == c) v.held = -1;
        v.cap[c].has_pic = 1;
        cap_finish(c, v.cap[c].bad);
    } else {
        v.held = c;
        v.cap[c].has_pic = 1;
    }
}

/* time passing: a call */
static void tick(void)
{
    start_job();
    if (v.busy && !fake_rpivid.hang && --v.left <= 0) { run_job(); start_job(); }
}

/* ---- ioctls ---- */

static int ctrl_size(uint32_t id)
{
    switch (id) {
    case 0xa40a90: return sizeof(struct v4l2_ctrl_hevc_sps);
    case 0xa40a91: return sizeof(struct v4l2_ctrl_hevc_pps);
    case 0xa40a92: return sizeof(struct v4l2_ctrl_hevc_slice_params);
    case 0xa40a93: return sizeof(struct v4l2_ctrl_hevc_scaling_matrix);
    case 0xa40a94: return sizeof(struct v4l2_ctrl_hevc_decode_params);
    }
    return -1;
}

static int s_ext_ctrls(struct ext_controls *cs)
{
    const struct ext_control *c = (const struct ext_control *)(uintptr_t)cs->controls;
    if (cs->which == 0) {                            /* (now: the mode and start code) */
        for (uint32_t i = 0; i < cs->count; i++) {
            if (c[i].id == 0xa40a95 && c[i].ptr == 0) v.decode_mode = 1;
            else if (c[i].id == 0xa40a96 && c[i].ptr <= 1) v.start_code = (int)c[i].ptr + 1;
            else { cs->error_idx = i; return -EINVAL; }
        }
        return 0;
    }
    if (cs->which != 0x0f010000u) { CHECK(0, "S_EXT_CTRLS which &%x", (unsigned)cs->which); return -EINVAL; }
    if (cs->request_fd < 0 || cs->request_fd >= NFILES || files[cs->request_fd].kind != F_REQUEST) return -EINVAL;
    {
        int r = files[cs->request_fd].req;
        if (reqs[r].queued) return -EBUSY;
        if (fake_rpivid.refuse_slice && ++nslice_ctrls == fake_rpivid.refuse_slice) { cs->error_idx = 0; return -EINVAL; }
        for (uint32_t i = 0; i < cs->count; i++) {
            void *dst = NULL;
            int k = -1;
            if ((int)c[i].size != ctrl_size(c[i].id)) {
                CHECK(0, "control &%x: %u bytes, not %d", (unsigned)c[i].id, (unsigned)c[i].size, ctrl_size(c[i].id));
                cs->error_idx = i;
                return -EINVAL;
            }
            switch (c[i].id) {
            case 0xa40a90: dst = &reqs[r].sps; k = 0; break;
            case 0xa40a91: dst = &reqs[r].pps; k = 1; break;
            case 0xa40a93: dst = &reqs[r].sm; k = 2; break;
            case 0xa40a94: dst = &reqs[r].dec; k = 3; break;
            case 0xa40a92: dst = &reqs[r].sp; k = 4; break;
            }
            memcpy(dst, (const void *)(uintptr_t)c[i].ptr, c[i].size);
            reqs[r].has[k] = 1;
            if (k == 0) {                            /* (try_ctrl_sps) */
                const struct v4l2_ctrl_hevc_sps *s = &reqs[r].sps;
                if (s->chroma_format_idc != 1 || (s->bit_depth_luma_minus8 != 0 && s->bit_depth_luma_minus8 != 2) ||
                    s->bit_depth_luma_minus8 != s->bit_depth_chroma_minus8 ||
                    (v.dst_set && ((s->bit_depth_luma_minus8 == 2) != (v.dst_fmt.pixelformat == NC30) ||
                                   s->pic_width_in_luma_samples > v.dst_fmt.width ||
                                   s->pic_height_in_luma_samples > v.dst_fmt.height))) {
                    cs->error_idx = i;
                    return -EINVAL;
                }
            }
        }
    }
    return 0;
}

static int vid_ioctl(uint32_t req, void *arg)
{
    switch (req) {
    case IOC(2, 'V', 0, 104): {
        struct capability *c = arg;
        memset(c, 0, sizeof *c);
        strcpy(c->driver, "rpivid"); strcpy(c->card, "rpivid"); strcpy(c->bus_info, "platform:rpivid");
        c->capabilities = 0x84004000u; c->device_caps = 0x04004000u;
        return 0;
    }
    case IOC(3, 'V', 5, 204): case IOC(3, 'V', 4, 204): {
        struct format *f = arg;
        int set = req == IOC(3, 'V', 5, 204);
        if (f->type == 10) {
            if (!set) { f->fmt.pix_mp = v.src_fmt; return 0; }
            if (v.nout) return -EBUSY;
            if (f->fmt.pix_mp.plane_fmt[0].sizeimage < 4096) f->fmt.pix_mp.plane_fmt[0].sizeimage = 4096;   /* (as given, at least 4K) */
            if (f->fmt.pix_mp.width > 4096) f->fmt.pix_mp.width = 4096;
            if (f->fmt.pix_mp.height > 4096) f->fmt.pix_mp.height = 4096;
            f->fmt.pix_mp.pixelformat = S265;
            f->fmt.pix_mp.num_planes = 1;
            f->fmt.pix_mp.plane_fmt[0].bytesperline = 0;
            v.src_fmt = f->fmt.pix_mp;
            v.dst_set = 0;                           /* (setting src invalidates dst) */
            return 0;
        }
        if (f->type == 9) {
            if (!set) {
                if (!v.dst_set) { memset(&v.dst_fmt, 0, sizeof v.dst_fmt); prepare_dst(&v.dst_fmt); }
                f->fmt.pix_mp = v.dst_fmt;
                return 0;
            }
            if (v.ncap) return -EBUSY;
            prepare_dst(&f->fmt.pix_mp);
            v.dst_fmt = f->fmt.pix_mp;
            v.dst_set = 1;
            return 0;
        }
        return -EINVAL;
    }
    case IOC(3, 'V', 8, 20): {
        struct requestbuffers *rb = arg;
        if (rb->type != 10 || rb->memory != 1) { CHECK(0, "REQBUFS type %u memory %u", (unsigned)rb->type, (unsigned)rb->memory); return -EINVAL; }
        if (v.nout) return -EBUSY;
        if (rb->count > OUT_BUFS) rb->count = OUT_BUFS;
        for (uint32_t i = 0; i < rb->count; i++) {
            v.out[i].len = fake_rpivid.out_size ? fake_rpivid.out_size : v.src_fmt.plane_fmt[0].sizeimage;
            v.out[i].p = calloc(1, v.out[i].len);
        }
        v.nout = (int)rb->count;
        return 0;
    }
    case IOC(3, 'V', 92, 248): {
        struct create_buffers *cb = arg;
        if (cb->format.type != 9 || cb->memory != 1 || cb->count != 1) return -EINVAL;
        if (!v.dst_set) { CHECK(0, "CREATE_BUFS before the frames' format"); return -EINVAL; }
        if (cb->format.fmt.pix_mp.plane_fmt[0].sizeimage < v.dst_fmt.plane_fmt[0].sizeimage) return -EINVAL;
        if (v.ncap >= CAP_BUFS) { cb->count = 0; return -ENOMEM; }
        cb->index = (uint32_t)v.ncap;
        v.cap[v.ncap].len = cb->format.fmt.pix_mp.plane_fmt[0].sizeimage;
        v.cap[v.ncap].p = calloc(1, v.cap[v.ncap].len);
        v.cap[v.ncap].poc = -999;
        v.ncap++;
        fake_rpivid.creates++;
        return 0;
    }
    case IOC(3, 'V', 9, 68): case IOC(3, 'V', 15, 68): case IOC(3, 'V', 17, 68): {
        struct buffer *b = arg;
        struct plane *pl = (struct plane *)(uintptr_t)b->m;
        int out = b->type == 10;
        if ((b->type != 9 && b->type != 10) || b->memory != 1 || b->length != 1 || !pl) {
            CHECK(0, "a buffer: type %u memory %u planes %u", (unsigned)b->type, (unsigned)b->memory, (unsigned)b->length);
            return -EINVAL;
        }
        if (req == IOC(3, 'V', 17, 68)) {            /* DQBUF (the device was opened O_NONBLOCK) */
            int i;
            if (out ? !v.src_on : !v.dst_on) return -EINVAL;
            if (out ? !v.nout_done : !v.ncap_done) return -LSYS_EAGAIN;
            if (out) {
                i = v.out_done[0];
                memmove(v.out_done, v.out_done + 1, sizeof v.out_done[0] * (size_t)--v.nout_done);
                v.out[i].state = B_DEQUEUED;
                b->flags = 0;
            } else {
                i = v.cap_done[0];
                memmove(v.cap_done, v.cap_done + 1, sizeof v.cap_done[0] * (size_t)--v.ncap_done);
                v.cap[i].state = B_DEQUEUED;
                b->flags = v.cap[i].err ? ERR_FLAG : 0;
                b->tv_sec = (int32_t)(v.cap[i].ts / 1000000000u);
                b->tv_usec = (int32_t)(v.cap[i].ts % 1000000000u / 1000);
            }
            b->index = (uint32_t)i;
            return 0;
        }
        if (b->index >= (uint32_t)(out ? v.nout : v.ncap)) return -EINVAL;
        if (req == IOC(3, 'V', 9, 68)) {             /* QUERYBUF */
            pl->length = out ? v.out[b->index].len : v.cap[b->index].len;
            pl->m = (out ? 0x01000000u : 0x40000000u) + b->index * 0x00100000u;   /* (the mmap offset) */
            return 0;
        }
        if (out) {                                   /* QBUF: a slice, with its request */
            int r;
            if (v.out[b->index].state != B_DEQUEUED) return -EINVAL;
            if (!(b->flags & REQ_FD)) { CHECK(0, "a slice without a request"); return -EBADR; }
            if (b->request_fd < 0 || b->request_fd >= NFILES || files[b->request_fd].kind != F_REQUEST) return -EINVAL;
            r = files[b->request_fd].req;
            if (reqs[r].queued || reqs[r].has_buf) return -EBUSY;
            CHECK(pl->bytesused && pl->bytesused <= v.out[b->index].len, "a slice of %u bytes in %u", (unsigned)pl->bytesused,
                  (unsigned)v.out[b->index].len);
            CHECK(b->tv_usec >= 0 && b->tv_usec < 1000000, "timestamp %d.%06d", (int)b->tv_sec, (int)b->tv_usec);
            v.out[b->index].bytesused = pl->bytesused;
            v.out[b->index].flags = b->flags;
            v.out[b->index].ts = (uint64_t)b->tv_sec * 1000000000u + (uint64_t)b->tv_usec * 1000u;
            v.out[b->index].req = r;
            v.out[b->index].state = B_QUEUED;
            reqs[r].has_buf = 1;
            reqs[r].buf = (int)b->index;
            return 0;
        }
        if (v.cap[b->index].state != B_DEQUEUED) return -EINVAL;   /* QBUF: a frame */
        v.cap[b->index].state = B_QUEUED;          /* (its timestamp and picture kept until a job takes it: */
        v.cap_ready[v.ncap_ready++] = (int)b->index; /* a picture before may still read it, as rpivid's phase 2 does) */
        {
            int ahead = v.ncap_ready + (v.held >= 0) + (v.busy && v.held != v.job_cap);
            if (ahead > fake_rpivid.max_ahead) fake_rpivid.max_ahead = ahead;
        }
        return 0;
    }
    case IOC(3, 'V', 16, 64): {
        struct exportbuffer *x = arg;
        int fd;
        if (x->type != 9 || x->index >= (uint32_t)v.ncap || x->plane) return -EINVAL;
        if ((fd = new_fd(F_DMABUF)) < 0) return fd;
        x->fd = fd;
        fake_rpivid.exports++;
        return 0;
    }
    case IOC(1, 'V', 18, 4): case IOC(1, 'V', 19, 4): {
        uint32_t t = *(const uint32_t *)arg;
        int on = req == IOC(1, 'V', 18, 4);
        if (t != 9 && t != 10) return -EINVAL;
        if (on) {
            if (t == 10 && (!v.nout || !v.decode_mode)) { CHECK(0, "streaming slices without buffers or mode"); return -EINVAL; }
            if (t == 9) v.dst_on = 1; else v.src_on = 1;
            fake_rpivid.streamons++;
        } else {                                     /* (everything queued comes back, failed, and is taken) */
            if (t == 9) {
                v.dst_on = 0;
                for (int i = 0; i < v.ncap; i++) v.cap[i].state = B_DEQUEUED;
                v.ncap_ready = v.ncap_done = 0;
                v.held = -1;
            } else {
                v.src_on = 0;
                for (int i = 0; i < v.nout; i++) v.out[i].state = B_DEQUEUED;
                v.nout_ready = v.nout_done = 0;
            }
            v.busy = 0;
            fake_rpivid.streamoffs++;
        }
        return 0;
    }
    case IOC(3, 'V', 72, 24): return s_ext_ctrls(arg);
    }
    CHECK(0, "ioctl &%08X on the video device", (unsigned)req);
    return -ENOTTY;
}

static int media_ioctl(uint32_t req, void *arg)
{
    if (req == IOC(3, '|', 0, 256)) {
        struct media_info *mi = arg;
        memset(mi, 0, sizeof *mi);
        strcpy(mi->driver, "rpivid"); strcpy(mi->model, "rpivid"); strcpy(mi->bus_info, "platform:rpivid");
        return 0;
    }
    if (req == IOC(2, '|', 5, 4)) {
        int fd, r;
        for (r = 0; r < NREQ && reqs[r].used; r++) {}
        if (r == NREQ) return -ENOMEM;
        if ((fd = new_fd(F_REQUEST)) < 0) return fd;
        memset(&reqs[r], 0, sizeof reqs[r]);
        reqs[r].used = 1;
        files[fd].req = r;
        *(int32_t *)arg = fd;
        return 0;
    }
    CHECK(0, "ioctl &%08X on the media device", (unsigned)req);
    return -ENOTTY;
}

static int request_ioctl(int r, uint32_t req)
{
    if (req == IOC(0, '|', 0x80, 0)) {               /* QUEUE (rpivid_request_validate) */
        if (reqs[r].queued) return -EBUSY;
        if (!reqs[r].has_buf) return -LSYS_ENOENT;
        if (!reqs[r].has[3] || !reqs[r].has[4]) { CHECK(0, "a request without decode or slice parameters"); return -LSYS_ENOENT; }
        reqs[r].queued = 1;
        reqs[r].complete = 0;
        v.out_ready[v.nout_ready++] = reqs[r].buf;
        return 0;
    }
    if (req == IOC(0, '|', 0x81, 0)) {               /* REINIT */
        if (reqs[r].queued && !reqs[r].complete) return -EBUSY;
        memset(reqs[r].has, 0, sizeof reqs[r].has);
        reqs[r].queued = reqs[r].complete = reqs[r].has_buf = 0;
        fake_rpivid.reinits++;
        return 0;
    }
    CHECK(0, "ioctl &%08X on a request", (unsigned)req);
    return -ENOTTY;
}

int hevcdec_lsys(int nr, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    (void)d; (void)e;
    if (!fake_rpivid.present) return -LSYS_ENOSYS;
    tick();
    switch (nr) {
    case LSYS_open: {
        const char *path = (const char *)(uintptr_t)a;
        int fd;
        CHECK(b == (2 | 04000 | 02000000), "open %s: flags &%x", path, (unsigned)b);
        if (!strcmp(path, "/dev/video10")) return new_fd(F_OTHER_VIDEO);
        if (!strcmp(path, "/dev/video19") && !fake_rpivid.no_device) {
            if (v.open) return -EBUSY;
            if ((fd = new_fd(F_VIDEO)) < 0) return fd;
            free_video();
            v.open = 1;
            return fd;
        }
        if (!strcmp(path, "/dev/media0") && !fake_rpivid.no_device && !fake_rpivid.no_media) return new_fd(F_MEDIA);
        return -LSYS_ENOENT;
    }
    case LSYS_close:
        if ((int)a < 0 || (int)a >= NFILES || !files[a].kind) return -EBADF;
        if (files[a].kind == F_VIDEO) {
            CHECK(fake_rpivid.maps == fake_rpivid.unmaps, "the device closed with %d buffers still mapped",
                  fake_rpivid.maps - fake_rpivid.unmaps);
            free_video();
        }
        if (files[a].kind == F_REQUEST) reqs[files[a].req].used = 0;
        files[a].kind = F_NONE;
        fake_rpivid.closes++;
        return 0;
    case LSYS_ioctl: {
        int fd = (int)a;
        if (fd < 0 || fd >= NFILES || !files[fd].kind) return -EBADF;
        switch (files[fd].kind) {
        case F_VIDEO: return vid_ioctl(b, (void *)(uintptr_t)c);
        case F_OTHER_VIDEO:
            if (b == IOC(2, 'V', 0, 104)) {
                struct capability *cp = (void *)(uintptr_t)c;
                memset(cp, 0, sizeof *cp);
                strcpy(cp->driver, "bcm2835-codec");
                cp->device_caps = 0x04004000u;
                return 0;
            }
            CHECK(0, "ioctl &%08X on another driver's device", (unsigned)b);
            return -ENOTTY;
        case F_MEDIA: return media_ioctl(b, (void *)(uintptr_t)c);
        case F_REQUEST: return request_ioctl(files[fd].req, b);
        }
        return -ENOTTY;
    }
    case LSYS_mmap2: {
        uint32_t off = f << 12;
        CHECK(a == 0 && c == 3 && d == 1, "mmap2 (%x, prot %u, flags %u)", (unsigned)a, (unsigned)c, (unsigned)d);
        if ((int)e < 0 || (int)e >= NFILES || files[e].kind != F_VIDEO) return -EBADF;
        if (off >= 0x40000000u) {
            uint32_t i = (off - 0x40000000u) / 0x00100000u;
            if (i >= (uint32_t)v.ncap || b > v.cap[i].len || v.cap[i].mapped) return -EINVAL;
            v.cap[i].mapped = 1;
            fake_rpivid.maps++;
            return (int)(uintptr_t)v.cap[i].p;
        } else {
            uint32_t i = (off - 0x01000000u) / 0x00100000u;
            if (off < 0x01000000u || i >= (uint32_t)v.nout || b > v.out[i].len || v.out[i].mapped) return -EINVAL;
            v.out[i].mapped = 1;
            fake_rpivid.maps++;
            return (int)(uintptr_t)v.out[i].p;
        }
    }
    case LSYS_munmap:
        for (int i = 0; i < v.ncap; i++)
            if (v.cap[i].mapped && (uint32_t)(uintptr_t)v.cap[i].p == a) { v.cap[i].mapped = 0; fake_rpivid.unmaps++; return 0; }
        for (int i = 0; i < v.nout; i++)
            if (v.out[i].mapped && (uint32_t)(uintptr_t)v.out[i].p == a) { v.out[i].mapped = 0; fake_rpivid.unmaps++; return 0; }
        CHECK(0, "munmap &%x: not mapped", (unsigned)a);
        return -EINVAL;
    case LSYS_poll: {
        struct pollfd_ *p = (struct pollfd_ *)(uintptr_t)a;
        int n = 0;
        for (uint32_t i = 0; i < b; i++) {
            int fd = p[i].fd;
            p[i].revents = 0;
            if (fd < 0 || fd >= NFILES || !files[fd].kind) { p[i].revents = 0x20; n++; continue; }   /* (POLLNVAL) */
            if (files[fd].kind == F_VIDEO) {
                if (v.ncap_done && (p[i].events & 1)) p[i].revents |= 1;
                if (v.nout_done && (p[i].events & 4)) p[i].revents |= 4;
            } else if (files[fd].kind == F_REQUEST) {
                if (reqs[files[fd].req].complete && (p[i].events & 2)) p[i].revents |= 2;
            }
            n += p[i].revents != 0;
        }
        return n;
    }
    }
    CHECK(0, "Linux call %d", nr);
    return -LSYS_ENOSYS;
}
