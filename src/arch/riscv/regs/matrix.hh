#ifndef __ARCH_RISCV_REGS_MATRIX_HH__
#define __ARCH_RISCV_REGS_MATRIX_HH__

#include "arch/riscv/regs/renameable_misc.hh"
#include "cpu/reg_class.hh"

namespace gem5
{

namespace RiscvISA
{

inline const auto MatrixRenamedTileMReg =
    RegId(RMiscRegClass, rmisc_reg::_MtilemIdx);
inline const auto MatrixRenamedTileNReg =
    RegId(RMiscRegClass, rmisc_reg::_MtilenIdx);
inline const auto MatrixRenamedTileKReg =
    RegId(RMiscRegClass, rmisc_reg::_MtilekIdx);

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_REGS_MATRIX_HH__
