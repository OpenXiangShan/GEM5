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

#ifndef __CPU_O3_SPEC_STORE_FWD_UNIT_HH__
#define __CPU_O3_SPEC_STORE_FWD_UNIT_HH__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/spec_store_fwd.hh"

namespace gem5
{

namespace o3
{

class LSQUnit;

/**
 * Spec-STLF unit: glue logic between LSQ and SpecStoreFwdPredictor.
 *
 * This module owns the predictor and the Spec-STLF execute-time speculation
 * and store-addr-ready validation logic. LSQUnit should only call into it at:
 * - read(): attempt speculative forwarding
 * - storeDoWriteSQ(): validate and squash on misprediction
 * - commit: commit-time stats/training
 */
class SpecStoreFwdUnit
{
  public:
    enum class AttemptResult
    {
        Miss,
        WaitingData,
        Forwarded,
        CorrectedFail,
    };

    SpecStoreFwdUnit() = default;

    void init(LSQUnit *lsq_unit, bool enable, size_t table_size,
              unsigned ctr_bits, bool allow_no_mdp);

    bool allowNoMdp() const { return allowNoMdp_; }

    /**
     * Try speculative forwarding for a non-strict MDP-wait load.
     *
     * @param wait_store_idxs SQ indices of predicted producing stores whose
     *                        addresses are not ready (candidate set).
     */
    AttemptResult trySpecStoreFwd(
        const DynInstPtr &load_inst, LSQ::LSQRequest *request,
        const std::vector<size_t> &wait_store_idxs);

    /**
     * Try speculative forwarding without an MDP producing-store candidate set.
     *
     * In all-load mode this ignores StoreSet metadata, checks the single store
     * at the predictor's predicted distance, and either forwards data or waits
     * for that store's data without requiring its address to be ready.
     */
    AttemptResult trySpecStoreFwd(
        const DynInstPtr &load_inst, LSQ::LSQRequest *request);

    void checkSpecStoreFwdMispred(const DynInstPtr &store_inst);

    void commitLoad(const DynInstPtr &load_inst);

    void commitStore(size_t store_idx);

    void beginLoadAttempt(const DynInstPtr &inst);
    void cancelLoadAttempt(const DynInstPtr &inst);
    void markSqConfirmed(const DynInstPtr &inst);
    void markSqCorrected(const DynInstPtr &inst);
    void markSpecWonOverSq(const DynInstPtr &inst);
    void markAddrValidationFail(const DynInstPtr &inst);

    bool hasPrediction(const DynInstPtr &inst) const;
    InstSeqNum predictedStoreSeq(const DynInstPtr &inst) const;

  private:
    AttemptResult tryCandidate(const DynInstPtr &load_inst,
                               LSQ::LSQRequest *request, size_t store_idx,
                               uint16_t distance, uint16_t shift,
                               bool saved_prediction);
    void clearCurrentForward(const DynInstPtr &inst);
    void clearPrediction(const DynInstPtr &inst);
    void resetPredictorMeta(const DynInstPtr &load_inst);

    LSQUnit *lsqUnit = nullptr;
    bool allowNoMdp_ = false;
    SpecStoreFwdPredictor pred;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_SPEC_STORE_FWD_UNIT_HH__
