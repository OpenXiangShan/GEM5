/*
 * Copyright (c) 2026 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the disclaimer; redistributions in
 * binary form must reproduce the above copyright notice, this list of
 * conditions and the disclaimer in the documentation and/or other materials
 * provided with the distribution; neither the name of the copyright holders
 * nor the names of any contributors may be used to endorse or promote
 * products derived from this software without specific written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_TRACE_CBP_REG_MAP_HH__
#define __CPU_O3_TRACE_CBP_REG_MAP_HH__

#include <cstdint>

namespace gem5
{
namespace o3
{

/**
 * Pure CBP2025 register-classification and mapping decisions, extracted so
 * the anchored unit tests (cbp_reg_map.test.cc) exercise the production
 * table itself. CBP2025TraceReader::extractRegisterDeps and
 * CBP2025TraceReader::regIsInt call these.
 *
 * Tombstone (x5 mapping chain, guards the three-commit fix-of-fix series):
 *   - 89b00fb712 normalized CBP register deps but kept the CALL_IND x5
 *     dummy source as-is, which still carried RISC-V alt-RA (x5) semantics;
 *   - a28e34a2cc then mapped it to x0, silently dropping the dependency
 *     edge (x0 is constant zero);
 *   - 9d97cbb232 finally mapped it to the neutral GPR x28 (t3), preserving
 *     the dependency edge without alt-RA semantics.
 */
namespace CBPRegMap
{

/** CBP2025 register classes: 0-31 GPRs (31=SP, 30=LR), 64=flags, 65=zero;
 *  32-63 are SIMD/FP. */
inline bool
isIntReg(uint8_t reg)
{
    return reg < 32 || reg == 64 || reg == 65;
}

/**
 * Map a CBP2025 integer register to the RISC-V GPR used by the synthetic
 * instruction encoding.
 *
 * Table (order-sensitive, mirrors the production lambda exactly):
 *   0 / 65 (zero) -> x0
 *   64 (flags)    -> x0 (best effort)
 *   31 (SP)       -> x2
 *   30 (LR)       -> x1 (RA)
 *   CALL_IND x5   -> x28 (t3): ARM-origin traces carry x5 as a dummy
 *                    source, not a link register; x0 would drop the
 *                    dependency edge and x5 would carry alt-RA semantics
 *   26 (IP)       -> x0 (not a GPR)
 *   < 32          -> identity
 *   otherwise     -> x0
 */
inline uint8_t
mapIntReg(uint8_t r, bool call_indirect)
{
    if (r == 0 || r == 65) return 0;
    if (r == 64) return 0;   // flags best-effort to x0
    if (r == 31) return 2;   // SP
    if (r == 30) return 1;   // LR (AArch64 x30) -> RISC-V RA(x1)
    if (call_indirect && r == 5) {
        constexpr uint8_t harmless = 28;  // x28 = t3
        return harmless;
    }
    if (r == 26) return 0;   // IP not defined as GPR; keep 0
    if (r < 32)  return r;   // general purpose
    return 0;
}

/**
 * Map a CBP2025 SIMD/FP register (32-63) to f0..f31. Anything else maps
 * to f0 (the isIntReg gate keeps non-FP registers off this path).
 */
inline uint8_t
mapFpReg(uint8_t r)
{
    if (r >= 32 && r < 64) return static_cast<uint8_t>(r - 32);  // f0..f31
    return 0;
}

} // namespace CBPRegMap
} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_CBP_REG_MAP_HH__
