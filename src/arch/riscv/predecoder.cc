/*
 * Copyright (c) 2026 The OpenXiangShan Contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "arch/riscv/predecoder.hh"

#include <cstdint>

namespace gem5
{
namespace RiscvISA
{

namespace
{

int64_t
signExtend(uint64_t value, unsigned width)
{
    const uint64_t sign = uint64_t(1) << (width - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

void
setRas(PredecodeInfo &info, unsigned rd, unsigned rs1)
{
    info.hasPush = rd == 1 || rd == 5;
    info.hasPop = (rs1 == 1 || rs1 == 5) && rd != rs1;
    if (info.hasPop && info.hasPush)
        info.rasAction = PredecodeInfo::RasAction::PopAndPush;
    else if (info.hasPop)
        info.rasAction = PredecodeInfo::RasAction::Pop;
    else if (info.hasPush)
        info.rasAction = PredecodeInfo::RasAction::Push;
}

} // anonymous namespace

PredecodeInfo
RiscvPredecoder::decode(MachInst rawInst)
{
    PredecodeInfo info;
    const uint16_t half = static_cast<uint16_t>(rawInst & 0xffff);
    info.isRvc = (rawInst & 0x3) != 0x3;
    info.instSize = info.isRvc ? 2 : 4;
    info.valid = true;

    if (info.isRvc) {
        const unsigned quadrant = half & 0x3;
        const unsigned funct3 = (half >> 13) & 0x7;
        const unsigned funct4 = (half >> 12) & 0xf;
        const unsigned rs1 = (half >> 7) & 0x1f;
        const unsigned rs2 = (half >> 2) & 0x1f;

        // C.EBREAK has priority over the C.JALR encoding family.
        if (quadrant == 2 && funct4 == 0x9 && rs1 == 0 && rs2 == 0) {
            return info;
        }

        if (quadrant == 1 && funct3 == 0x5) {
            info.branchType = PredecodeInfo::BranchType::Direct;
            const uint64_t imm = ((half >> 12) & 0x1) << 11 |
                ((half >> 11) & 0x1) << 4 |
                ((half >> 9) & 0x3) << 8 |
                ((half >> 8) & 0x1) << 10 |
                ((half >> 7) & 0x1) << 6 |
                ((half >> 6) & 0x1) << 7 |
                ((half >> 3) & 0x7) << 1 |
                ((half >> 2) & 0x1) << 5;
            info.targetOffset = signExtend(imm, 12);
            // C.J is the RV64/RV128 form. It is not C.JAL in RV64C.
            return info;
        }

        if (quadrant == 2 && (funct4 == 0x8 || funct4 == 0x9) &&
            rs2 == 0 && rs1 != 0) {
            info.branchType = PredecodeInfo::BranchType::Indirect;
            const unsigned rd = funct4 == 0x9 ? 1 : 0;
            setRas(info, rd, rs1);
            info.isCall = info.hasPush;
            info.isReturn = info.hasPop;
            return info;
        }

        if (quadrant == 1 && (funct3 == 0x6 || funct3 == 0x7)) {
            info.branchType = PredecodeInfo::BranchType::Conditional;
            const uint64_t imm = ((half >> 12) & 0x1) << 8 |
                ((half >> 10) & 0x3) << 3 |
                ((half >> 5) & 0x3) << 6 |
                ((half >> 3) & 0x3) << 1 |
                ((half >> 2) & 0x1) << 5;
            info.targetOffset = signExtend(imm, 9);
            return info;
        }
        return info;
    }

    const unsigned opcode = rawInst & 0x7f;
    const unsigned funct3 = (rawInst >> 12) & 0x7;
    const unsigned rd = (rawInst >> 7) & 0x1f;
    const unsigned rs1 = (rawInst >> 15) & 0x1f;

    if (opcode == 0x6f) {
        info.branchType = PredecodeInfo::BranchType::Direct;
        const uint64_t imm = ((rawInst >> 31) & 0x1) << 20 |
            ((rawInst >> 21) & 0x3ff) << 1 |
            ((rawInst >> 20) & 0x1) << 11 |
            ((rawInst >> 12) & 0xff) << 12;
        info.targetOffset = signExtend(imm, 21);
        setRas(info, rd, 0);
        info.isCall = info.hasPush;
    } else if (opcode == 0x67 && funct3 == 0) {
        info.branchType = PredecodeInfo::BranchType::Indirect;
        setRas(info, rd, rs1);
        info.isCall = info.hasPush;
        info.isReturn = info.hasPop;
    } else if (opcode == 0x63) {
        info.branchType = PredecodeInfo::BranchType::Conditional;
        const uint64_t imm = ((rawInst >> 31) & 0x1) << 12 |
            ((rawInst >> 7) & 0x1) << 11 |
            ((rawInst >> 25) & 0x3f) << 5 |
            ((rawInst >> 8) & 0xf) << 1;
        info.targetOffset = signExtend(imm, 13);
    }

    return info;
}

} // namespace RiscvISA
} // namespace gem5
