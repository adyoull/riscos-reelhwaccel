# common/toolchain.sh - sourced by every build script (with TOP set to the
# top of the repository): CROSS and ELF2AIF, the RISC OS compiler prefix
# and elf2aif, unless they're set already.
#   1. riscos-crossdev's toolchain, $RISCOS_TOOLCHAIN (set in its container
#      image, ghcr.io/adyoull/riscos-crossdev, which GitHub builds in): its
#      arm-riscos-gnueabihf-* and elf2aif;
#   2. otherwise a GCCSDK environment at ~/gccsdk/env (CROSS), with this
#      repository's tools/elf2aif built (make -C tools/elf2aif GCCSDK_SRC=...)
#      or elf2aif on PATH.
# Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
if [ -z "${CROSS:-}" ]; then
  if [ -n "${RISCOS_TOOLCHAIN:-}" ]; then
    CROSS=$RISCOS_TOOLCHAIN/bin/arm-riscos-gnueabihf-
  elif [ -x "$HOME/gccsdk/env/bin/arm-riscos-gnueabihf-gcc" ]; then
    CROSS=$HOME/gccsdk/env/bin/arm-riscos-gnueabihf-
  else
    echo "No RISC OS toolchain: set RISCOS_TOOLCHAIN (riscos-crossdev) or CROSS (GCCSDK's prefix)" >&2
    exit 1
  fi
fi
if [ -z "${ELF2AIF:-}" ]; then
  if [ -n "${RISCOS_TOOLCHAIN:-}" ] && [ -x "$RISCOS_TOOLCHAIN/bin/elf2aif" ]; then
    ELF2AIF=$RISCOS_TOOLCHAIN/bin/elf2aif
  elif [ -x "$TOP/tools/elf2aif/elf2aif" ]; then
    ELF2AIF=$TOP/tools/elf2aif/elf2aif
  else
    ELF2AIF=$(command -v elf2aif || echo elf2aif)
  fi
fi
[ -x "${CROSS}gcc" ] || { echo "No RISC OS compiler at ${CROSS}gcc" >&2; exit 1; }
export CROSS ELF2AIF
