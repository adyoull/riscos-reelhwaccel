/*
 * hevcdec_int.h - hevcdec's own structures, shared by its two back ends:
 * hevcdec.c (the Pi 4's HEVC block, driven directly, on RISC OS) and
 * (0.1.15) hevcdec_linux.c (Linux's rpivid driver, through V4L2, on RISC
 * OS on Linux). Not part of the API (hwhevcdec.h is).
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#ifndef HEVCDEC_INT_H
#define HEVCDEC_INT_H

#include "hwhevcdec.h"
#include "rpivid.h"
#include "rpivid_hw.h"

#define MAX_FRAMES  VB2_MAX_FRAME
#define WAIT_CS     100                    /* a phase not done in this: an error */
#define GUARD       16384                  /* bytes of pattern after each buffer the block uses */
#define GUARD_BYTE  0xA5
#define MAX_GUARDED 120

struct hevcdec_frame {
    struct vb2_v4l2_buffer vb;           /* (first: rpivid_h265.c sees this) */
    int used;
    int cached;                          /* cacheable: invalidated before it's read */
    int submitted;                       /* given to the block: decoded when vb.state is set */
    int clean;                           /* (0.1.11) cleaned and invalidated since the block wrote it */
    int free_pending;                    /* (0.1.13) hevcdec_frame_free'd: goes once the block is past it */
    unsigned free_after;                 /* (0.1.13) ... once pics_done reaches this */
    char err[160];                       /* why the block failed it */
    hevcdec *d;                          /* (its decoder) */
    /* (0.1.15) Linux's rpivid: its CAPTURE buffer (vb.vaddr maps it), queued
       to the driver and not yet back */
    int cap, lx_queued;
};

struct hevcdec {
    hevcdec_config cfg;
    hevcdec_hw *hw;
    struct rpivid_dev dev;
    struct rpivid_ctx ctx;
    struct v4l2_m2m_ctx m2m;
    struct vb2_queue vq;
    int started;
    int dead;                            /* a phase never finished: the block may still be at work */
    /* For a stream without scaling lists. The block keeps its scaling
       factors from one picture (and one decoder) to the next, and uses
       them: SPS1's scaling bit is set anyway when FFmpeg's PCM fields
       (255, 253 without PCM) overflow into it, and rpivid_h265.c loads the
       factors only when the SPS enables scaling lists. So such pictures
       came out with whatever factors were left (HEVCTest 0.1 on a Pi 4);
       given flat lists (all 16, what "no scaling lists" means) with the
       SPS copied to enable them, every picture was right (0.1.1). */
    struct { void *p; size_t size; const char *what; int reported, cached; } guarded[MAX_GUARDED];
    int cached_frames;                   /* frames cacheable (config, and the machine can) */
    unsigned overruns, overrun_max;     /* buffers the block wrote past, and by how much at most */
    const char *overrun_what;
    struct v4l2_ctrl_hevc_sps sps_flat;
    struct v4l2_ctrl_hevc_scaling_matrix flat;
    char err[256];
    hevcdec_frame *frames[MAX_FRAMES];
    unsigned pics_given, pics_done;      /* (0.1.13) pictures given to the block, and finished by it */
    int frees_pending;
    /* the slices sent, contiguous copies: a picture's last slice is read by
       phase 1 from here, so a ring (as rpivid's bitstream copies), one a
       picture, each free again once that picture's phase 1 is done */
    struct vb2_v4l2_buffer srcs[RPIVID_P1BUF_COUNT];
    size_t src_caps[RPIVID_P1BUF_COUNT];
    unsigned src_next;
    unsigned pic_w, pic_h;               /* (0.1.10) the last picture's size (a change restarts rpivid) */
    struct vb2_v4l2_buffer *cur_src, *cur_dst;
    int job_state;                       /* trigger's verdict on the last slice sent */
    /* phase 1's command lists, each picture's copied for the block and kept
       until rpivid unmaps it (its decode environment gone: phase 1 done) */
    struct { void *p; uint64_t bus; size_t cap; int mapped; } cmds[RPIVID_DEC_ENV_COUNT];
    hevcdec_stats stats;
    struct hevcdec_lx *lx;               /* (0.1.15) Linux's rpivid instead of the block (hevcdec_linux.c) */
};

/* (0.1.15) hevcdec_linux.c: Linux's rpivid through V4L2's stateless
   decoder interface, when RISC OS runs on Linux (ROL) and Linux has the
   driver. hevcdec_lx_open: 0 opened (d->lx set, d->ctx.dst_fmt the
   driver's frames), 1 not here (use the block), -1 failed (err says why) */
int hevcdec_lx_open(hevcdec *d, char *err, size_t errlen);
int hevcdec_lx_frame_new(hevcdec *d, hevcdec_frame *f);
void hevcdec_lx_frame_gone(hevcdec *d, hevcdec_frame *f);
int hevcdec_lx_decode(hevcdec *d, const hevcdec_picture *pic, const struct v4l2_ctrl_hevc_sps *sps,
                      const struct v4l2_ctrl_hevc_scaling_matrix *scaling, hevcdec_frame *f, uint64_t number);
int hevcdec_lx_poll(hevcdec *d);
int hevcdec_lx_wait(hevcdec *d, hevcdec_frame *f);   /* f NULL: every picture given */
int hevcdec_lx_idle(const hevcdec *d);
int hevcdec_lx_export(hevcdec *d, const hevcdec_frame *f, hevcdec_export *e);
void hevcdec_lx_close(hevcdec *d);
int hevcdec_fail(hevcdec *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void hevcdec_logf(hevcdec *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
