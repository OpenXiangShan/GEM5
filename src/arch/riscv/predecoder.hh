/*
 * Copyright (c) 2026 The OpenXiangShan Contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __ARCH_RISCV_PREDECODER_HH__
#define __ARCH_RISCV_PREDECODER_HH__

#include <cstdint>

#include "arch/riscv/types.hh"

namespace gem5
{
namespace RiscvISA
{

/** Metadata needed by the frontend before full ISA decode. */
struct PredecodeInfo
{
    enum class BranchType : uint8_t
    {
        None,
        Conditional,
        Direct,
        Indirect
    };

    enum class RasAction : uint8_t
    {
        None,
        Pop,
        Push,
        PopAndPush
    };

    bool valid = false;
    bool isRvc = false;
    uint8_t instSize = 0;
    BranchType branchType = BranchType::None;
    RasAction rasAction = RasAction::None;
    bool isCall = false;
    bool isReturn = false;
    bool hasPop = false;
    bool hasPush = false;
    int64_t targetOffset = 0;
};

/**
 * Decode only frontend-visible control-flow metadata.
 *
 * This deliberately does not inspect prediction state, fetch-block
 * boundaries, or register values. Those belong to Fetch/Decode owners.
 */
class RiscvPredecoder
{
  public:
    static PredecodeInfo decode(MachInst rawInst);
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_PREDECODER_HH__
