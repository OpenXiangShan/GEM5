#ifndef __ARCH_RISCV_REGS_RENAMEABLE_MISC_HH__
#define __ARCH_RISCV_REGS_RENAMEABLE_MISC_HH__

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
    NumRegs=10
};

}

}

}

#endif