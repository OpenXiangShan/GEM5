/*
 * Minimal AME helpers for XS-GEM5 bring-up.
 */

#include "arch/riscv/insts/matrix.hh"

namespace gem5
{

namespace RiscvISA
{

uint32_t
clampMatrixTileM(uint64_t value)
{
    return value > MatrixMaxM ? MatrixMaxM : static_cast<uint32_t>(value);
}

uint32_t
clampMatrixTileK(uint64_t value)
{
    return value > MatrixMaxK ? MatrixMaxK : static_cast<uint32_t>(value);
}

uint32_t
clampMatrixTileN(uint64_t value)
{
    return value > MatrixMaxN ? MatrixMaxN : static_cast<uint32_t>(value);
}

} // namespace RiscvISA
} // namespace gem5
