/*
 * hevcdec_lsys.c - (0.1.15) Linux's system calls from RISC OS on Linux
 * (ROL), through IXSupport_LinuxSyscall: r0-r5 the arguments, r7 the
 * call's number, the result (or -errno) back in r0. As ROL's own
 * LinuxCodec module calls them.
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#include "kernel.h"
#include "hevcdec_lsys.h"

#define IXSupport_LinuxSyscall 0x59EC0
#define OS_Module              0x1E

int hevcdec_lsys(int nr, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    _kernel_swi_regs r;
    r.r[0] = (int)a; r.r[1] = (int)b; r.r[2] = (int)c; r.r[3] = (int)d; r.r[4] = (int)e; r.r[5] = (int)f;
    r.r[6] = 0; r.r[7] = nr;
    if (_kernel_swi(IXSupport_LinuxSyscall, &r, &r)) return -LSYS_ENOSYS;
    return r.r[0];
}

int hevcdec_lsys_available(void)
{
    static int known = -1;
    _kernel_swi_regs r;
    if (known >= 0) return known;
    r.r[0] = 18;                             /* (OS_Module 18: is NativeSupport there) */
    r.r[1] = (int)"NativeSupport";
    known = !_kernel_swi(OS_Module, &r, &r) && hevcdec_lsys(LSYS_close, 0xFFFFFFFFu, 0, 0, 0, 0, 0) != -LSYS_ENOSYS;
    return known;
}
