/*
 * hevcdec_lsys.h - (0.1.15) Linux's system calls, for hevcdec_linux.c,
 * when RISC OS runs on Linux (ROL): through ROL's IXSupport_LinuxSyscall
 * SWI. On RISC OS, hevcdec_lsys.c; in the host tests, a fake rpivid
 * driver (tests/host/fake_rpivid.c).
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#ifndef HEVCDEC_LSYS_H
#define HEVCDEC_LSYS_H

#include <stdint.h>

/* Linux, 32-bit ARM EABI */
#define LSYS_open   5
#define LSYS_close  6
#define LSYS_ioctl  54
#define LSYS_munmap 91
#define LSYS_poll   168
#define LSYS_mmap2  192

#define LSYS_ENOENT 2
#define LSYS_EAGAIN 11
#define LSYS_ENOSYS 38

/* the call nr with a..f in r0-r5: its result, or -errno (-ENOSYS when
   there's no Linux underneath) */
int hevcdec_lsys(int nr, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f);
/* Linux underneath, with its devices' ioctls passed through: ROL's
   IXSupport, and NativeSupport loaded (it is when ROL runs natively on
   the machine, not under QEMU) */
int hevcdec_lsys_available(void);

#endif
