#ifndef __ARCH_RISCV_REGS_RENAMEABLE_MISC_HH__
#define __ARCH_RISCV_REGS_RENAMEABLE_MISC_HH__

#include "base/types.hh"
#include "config/gem5_enable_matrix.hh"

namespace gem5
{

namespace RiscvISA
{

namespace rmisc_reg
{

enum : RegIndex
{
    _VlIdx,
    _VtypeIdx,
    _FuseTmp,
#if GEM5_ENABLE_MATRIX
    _MtilemIdx,
    _MtilenIdx,
    _MtilekIdx,
    NumRegs
#else
    NumRegs = 10
#endif
};

}

}

}

#endif
