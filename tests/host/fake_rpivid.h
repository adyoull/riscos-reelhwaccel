/* fake_rpivid.h - (0.1.15) a fake of Linux's rpivid driver, as RISC OS on
   Linux (ROL) reaches it through IXSupport_LinuxSyscall (fake_rpivid.c
   takes the place of hevcdec_lsys.c), for the host tests.
   Part of riscos-reelhwaccel. GPL version 2. */
#ifndef FAKE_RPIVID_H
#define FAKE_RPIVID_H
#include "fake_hevc.h"

typedef struct {
    /* switches */
    int present;                      /* 0 (default): not on Linux (every call -ENOSYS): the block is used */
    int no_device;                    /* Linux, but no rpivid (only another V4L2 device) */
    int no_media;                     /* the video device but no media device */
    int ticks;                        /* the driver's polls (calls) a job takes (1) */
    int hang;                         /* jobs never finish */
    int fail_pic;                     /* this picture (counting from 1) comes back with V4L2_BUF_FLAG_ERROR */
    int refuse_slice;                 /* this slice (counting from 1, all pictures) refused by S_EXT_CTRLS */
    unsigned out_size;                /* (0: as the driver sizes it) the OUTPUT buffers' bytes */
    int quiet;                        /* failures expected: counted, not printed */
    /* findings */
    int fails;                        /* the fake's own CHECK failures */
    int opens, closes, live_fds, maps, unmaps;
    int pictures, slices, held, slices_refused, ref_errors, unknown_pictures, errors_given;
    int creates, reinits, exports, streamons, streamoffs;
    int max_ahead;                    /* the most pictures queued at once (pipelining) */
    int scaling_sent, scaling_not_flat;
    int dpb_ns_wrong;                 /* a reference's timestamp not one of the frames' (ns) */
} fake_rpivid_state;
extern fake_rpivid_state fake_rpivid;

void fake_rpivid_reset(void);         /* (present kept as it was) */
int fake_rpivid_live(void);           /* files and mappings left open */
#endif
