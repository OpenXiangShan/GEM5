// Copyright (c) 2026 The gem5 contributors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_DIFFTEST_FS_HH__
#define __CPU_DIFFTEST_FS_HH__

#include <cstdint>
#include <optional>

namespace gem5
{
namespace difftest
{

constexpr uint64_t FsMask = uint64_t{3} << 13;
constexpr uint64_t SdMask = uint64_t{1} << 63;

inline unsigned
fs(uint64_t status)
{
    return (status >> 13) & 3;
}

// RV64 SD is the OR of the Dirty states of FS, VS and XS. It is derived
// state, so an FS difference does not justify ignoring an inconsistent SD.
inline bool
validSd(uint64_t status)
{
    const bool dirty = fs(status) == 3 || ((status >> 9) & 3) == 3 ||
                       ((status >> 15) & 3) == 3;
    return bool(status & SdMask) == dirty;
}

// RISC-V Privileged ISA, "Extension Context Status in mstatus Register":
// implementations may track dirtiness imprecisely, and FS is WARL. Thus
// Initial/Clean may become Dirty even without an observable FP change.
// Actual FPR/FCSR changes require Dirty; only explicit CSR writes may clear
// Dirty or change Off. Initial and Clean are not interchangeable.
inline bool
legalFsTransition(unsigned before, unsigned after, bool fp_changed,
                  std::optional<unsigned> requested = std::nullopt)
{
    const unsigned expected = fp_changed ? 3 : requested.value_or(before);
    return after == expected ||
           ((expected == 1 || expected == 2) && after == 3);
}

// Call only after validating each side's transition. Accept differences
// involving conservative Dirty, while comparing every unrelated status bit.
inline bool
equivalentFsStatus(uint64_t dut, uint64_t ref)
{
    if (!validSd(dut) || !validSd(ref) ||
        ((dut ^ ref) & ~(FsMask | SdMask))) {
        return false;
    }
    const unsigned a = fs(dut);
    const unsigned b = fs(ref);
    return a == b || (a != 0 && b != 0 && (a == 3 || b == 3));
}

// Decode successful RV64 mstatus/sstatus CSR writes using the pre-step GPR.
// The caller keeps virtualized execution on the strict comparison path.
inline std::optional<unsigned>
requestedFs(uint32_t inst, uint64_t source, uint64_t status)
{
    if ((inst & 0x7f) != 0x73) {
        return std::nullopt;
    }
    const unsigned csr = inst >> 20;
    const unsigned op = (inst >> 12) & 7;
    const unsigned rs1 = (inst >> 15) & 31;
    if ((csr != 0x300 && csr != 0x100) || op == 0 || op == 4) {
        return std::nullopt;
    }
    const unsigned operation = op & 3;
    if (operation != 1 && rs1 == 0) {
        return std::nullopt;
    }
    const uint64_t value = op & 4 ? rs1 : source;
    const uint64_t result = operation == 1 ? value :
                            operation == 2 ? status | value : status & ~value;
    return fs(result);
}

inline bool
accessesFp(uint32_t inst, bool floating)
{
    const unsigned opcode = inst & 0x7f;
    const unsigned funct3 = (inst >> 12) & 7;
    const unsigned csr = inst >> 20;
    return floating ||
           (opcode == 0x57 && (funct3 == 1 || funct3 == 5)) ||
           (opcode == 0x73 && funct3 != 0 && funct3 != 4 &&
            csr >= 1 && csr <= 3);
}

} // namespace difftest
} // namespace gem5

#endif // __CPU_DIFFTEST_FS_HH__
