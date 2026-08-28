/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_SPEC_STORE_FWD_TYPES_HH__
#define __CPU_O3_SPEC_STORE_FWD_TYPES_HH__

#include <cstdint>

namespace gem5
{

namespace o3
{

enum class SpecStoreFwdState : uint8_t
{
    None,
    WaitingData,
    PendingValidation,
    SqConfirmed,
    SqCorrectedFail,
    AddrValidationFail,
};

enum class SpecStoreFwdSqResult : uint8_t
{
    Miss,
    FullForward,
    PartialForward,
    DataNotReady,
    /** Backward-compatible name for a partial SQ overlap. */
    Conflict = PartialForward,
};

enum class SpecStoreFwdFeedbackReason : uint8_t
{
    RangeMismatch,
    SqYoungerFull,
    SqPartialReplay,
    SqDataNotReadyReplay,
    DataReplayInvalidSource,
    YoungerNukeOrViolation,
    Count,
};

inline constexpr const char *SpecStoreFwdFeedbackReasonNames[] = {
    "rangeMismatch",
    "sqYoungerFull",
    "sqPartialReplay",
    "sqDataNotReadyReplay",
    "dataReplayInvalidSource",
    "youngerNukeOrViolation",
};

/** SFP only learns store-to-load forwarding with identical byte ranges. */
constexpr bool
isSameStoreLoadRange(uint64_t load_addr, uint64_t load_size,
                     uint64_t store_addr, uint64_t store_size)
{
    return load_addr == store_addr && load_size == store_size;
}

enum class SpecStoreFwdDecision : uint8_t
{
    NormalPath,
    KeepSpec,
    ConfirmWithSq,
    CorrectWithSq,
    ReplayForSq,
};

/** Select between a speculative source and one address-known SQ result. */
constexpr SpecStoreFwdDecision
selectSpecStoreFwdSource(bool spec_active, uint64_t predicted_store_seq,
                         SpecStoreFwdSqResult sq_result,
                         uint64_t sq_store_seq = 0,
                         bool predicted_addr_mismatch = false)
{
    if (!spec_active) {
        return (sq_result == SpecStoreFwdSqResult::PartialForward ||
                sq_result == SpecStoreFwdSqResult::DataNotReady) ?
            SpecStoreFwdDecision::ReplayForSq :
            SpecStoreFwdDecision::NormalPath;
    }

    if (sq_result == SpecStoreFwdSqResult::Miss) {
        return predicted_addr_mismatch ?
            SpecStoreFwdDecision::NormalPath :
            SpecStoreFwdDecision::KeepSpec;
    }

    if (sq_store_seq < predicted_store_seq) {
        return SpecStoreFwdDecision::KeepSpec;
    }

    if (sq_result == SpecStoreFwdSqResult::FullForward) {
        return sq_store_seq == predicted_store_seq ?
            SpecStoreFwdDecision::ConfirmWithSq :
            SpecStoreFwdDecision::CorrectWithSq;
    }

    return SpecStoreFwdDecision::ReplayForSq;
}

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_SPEC_STORE_FWD_TYPES_HH__
