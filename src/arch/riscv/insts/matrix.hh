/*
 * Minimal AME helpers for XS-GEM5 bring-up.
 */

#ifndef __ARCH_RISCV_INSTS_MATRIX_HH__
#define __ARCH_RISCV_INSTS_MATRIX_HH__

#include <cstdint>

namespace gem5
{

namespace RiscvISA
{

static constexpr uint32_t MatrixMaxM = 128;
static constexpr uint32_t MatrixMaxK = 64;
static constexpr uint32_t MatrixMaxN = 128;

static constexpr uint32_t MatrixTileABytes = MatrixMaxM * MatrixMaxK;
static constexpr uint32_t MatrixTileBBytes = MatrixMaxN * MatrixMaxK;
static constexpr uint32_t MatrixAccElems = MatrixMaxM * MatrixMaxN;

uint32_t clampMatrixTileM(uint64_t value);
uint32_t clampMatrixTileK(uint64_t value);
uint32_t clampMatrixTileN(uint64_t value);

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_INSTS_MATRIX_HH__
