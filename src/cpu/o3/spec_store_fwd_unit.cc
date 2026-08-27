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

#include "cpu/o3/spec_store_fwd_unit.hh"

#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/lsq_unit.hh"
#include "debug/LSQUnit.hh"
#include "debug/LoadPipeline.hh"
#include "debug/SPECFwd.hh"

namespace gem5
{

namespace o3
{

void
SpecStoreFwdUnit::init(LSQUnit *lsq_unit, bool enable, size_t table_size,
                       unsigned ctr_bits, bool allow_no_mdp)
{
    lsqUnit = lsq_unit;
    allowNoMdp_ = allow_no_mdp;
    pred.init(enable, table_size, ctr_bits);
}

SpecStoreFwdUnit::AttemptResult
SpecStoreFwdUnit::trySpecStoreFwd(
    const DynInstPtr &load_inst, LSQ::LSQRequest *request,
    const std::vector<size_t> &wait_store_idxs)
{
    if (!lsqUnit || !pred.ready() || !request || !request->isNormalLd()) {
        return AttemptResult::Miss;
    }

    if (load_inst->specStoreFwdState == SpecStoreFwdState::WaitingData) {
        const size_t boundary = load_inst->sqIt.idx();
        const auto distance = load_inst->specStoreFwdDistance;
        if (boundary < distance) {
            feedbackDataReplayInvalidSource(load_inst);
            markSqCorrected(load_inst);
            return AttemptResult::CorrectedFail;
        }
        return tryCandidate(load_inst, request, boundary - distance, distance,
                            load_inst->specStoreFwdShiftAmt, true);
    }
    if (load_inst->specStoreFwdState != SpecStoreFwdState::None ||
        wait_store_idxs.empty()) {
        return AttemptResult::Miss;
    }

    const auto pred_meta = pred.predict(load_inst->pcState().instAddr());
    if (!pred_meta) {
        return AttemptResult::Miss;
    }

    const size_t boundary = load_inst->sqIt.idx();
    for (const auto store_idx : wait_store_idxs) {
        if (boundary <= store_idx) {
            continue;
        }
        const auto distance = static_cast<uint16_t>(boundary - store_idx);
        if (distance == pred_meta->first) {
            return tryCandidate(load_inst, request, store_idx, distance,
                                pred_meta->second, false);
        }
    }
    return AttemptResult::Miss;
}

SpecStoreFwdUnit::AttemptResult
SpecStoreFwdUnit::trySpecStoreFwd(const DynInstPtr &load_inst,
                                  LSQ::LSQRequest *request)
{
    if (!allowNoMdp_ || !lsqUnit || !pred.ready() || !request ||
        !request->isNormalLd()) {
        return AttemptResult::Miss;
    }

    if (load_inst->specStoreFwdState == SpecStoreFwdState::WaitingData) {
        const size_t boundary = load_inst->sqIt.idx();
        const auto distance = load_inst->specStoreFwdDistance;
        if (boundary < distance) {
            feedbackDataReplayInvalidSource(load_inst);
            markSqCorrected(load_inst);
            return AttemptResult::CorrectedFail;
        }
        return tryCandidate(load_inst, request, boundary - distance, distance,
                            load_inst->specStoreFwdShiftAmt, true);
    }
    if (load_inst->specStoreFwdState != SpecStoreFwdState::None) {
        return AttemptResult::Miss;
    }

    const auto pred_meta = pred.predict(load_inst->pcState().instAddr());
    if (!pred_meta) {
        return AttemptResult::Miss;
    }

    const size_t boundary = load_inst->sqIt.idx();
    if (boundary < pred_meta->first) {
        return AttemptResult::Miss;
    }
    return tryCandidate(load_inst, request, boundary - pred_meta->first,
                        pred_meta->first, pred_meta->second, false);
}

SpecStoreFwdUnit::AttemptResult
SpecStoreFwdUnit::tryCandidate(const DynInstPtr &load_inst,
                               LSQ::LSQRequest *request, size_t store_idx,
                               uint16_t distance, uint16_t shift,
                               bool saved_prediction)
{
    if (!lsqUnit->storeQueue.isValidIdx(store_idx)) {
        if (saved_prediction) {
            feedbackDataReplayInvalidSource(load_inst);
            markSqCorrected(load_inst);
            return AttemptResult::CorrectedFail;
        }
        return AttemptResult::Miss;
    }

    const auto store_it = lsqUnit->storeQueue.getIterator(store_idx);
    if (!store_it->valid() || !store_it->instruction()) {
        if (saved_prediction) {
            feedbackDataReplayInvalidSource(load_inst);
            markSqCorrected(load_inst);
            return AttemptResult::CorrectedFail;
        }
        return AttemptResult::Miss;
    }

    const auto &store_inst = store_it->instruction();
    const bool invalid_source =
        store_inst->seqNum >= load_inst->seqNum ||
        (saved_prediction &&
         store_inst->seqNum != load_inst->specStoreFwdStoreSeqNum) ||
        store_inst->isVector() || store_inst->isAtomic() ||
        store_inst->isStoreConditional();
    const auto *store_request = store_it->request();
    const bool masked = store_request && store_request->mainReq() &&
        store_request->mainReq()->isMasked();
    if (invalid_source || masked) {
        if (saved_prediction) {
            feedbackDataReplayInvalidSource(load_inst);
            markSqCorrected(load_inst);
            return AttemptResult::CorrectedFail;
        }
        return AttemptResult::Miss;
    }

    const int32_t store_width = store_inst->operWid();
    if (store_width <= 0) {
        return AttemptResult::Miss;
    }
    const unsigned store_size = static_cast<unsigned>(store_width / 8);
    const unsigned load_size = request->mainReq()->getSize();
    if (shift >= store_size || load_size > store_size - shift) {
        if (saved_prediction) {
            feedbackShiftMismatch(load_inst);
            markSqCorrected(load_inst);
            return AttemptResult::CorrectedFail;
        }
        return AttemptResult::Miss;
    }

    if (!saved_prediction) {
        load_inst->specStoreFwdStoreSeqNum = store_inst->seqNum;
        load_inst->specStoreFwdDistance = distance;
        load_inst->specStoreFwdShiftAmt = shift;
    }

    if (store_it->addrReady()) {
        const int64_t actual_shift =
            static_cast<int64_t>(load_inst->physEffAddr) -
            static_cast<int64_t>(store_inst->physEffAddr);
        if (actual_shift < 0 || actual_shift != shift ||
            load_size > store_size - static_cast<unsigned>(actual_shift)) {
            feedbackShiftMismatch(load_inst);
            markSqCorrected(load_inst);
            return AttemptResult::CorrectedFail;
        }
    }

    if (!store_it->dataReady()) {
        load_inst->specStoreFwdState = SpecStoreFwdState::WaitingData;
        load_inst->specStoreFwdDataWaited = true;
        DPRINTF(SPECFwd,
                "Spec-STLF load[sn:%llu] waits for store[sn:%llu] data\n",
                load_inst->seqNum, store_inst->seqNum);
        return AttemptResult::WaitingData;
    }

    if (!load_inst->memData) {
        load_inst->memData = new uint8_t[load_size];
    }

    request->SQforwardPackets.clear();
    for (unsigned i = 0; i < load_size; ++i) {
        const uint8_t byte = store_it->isAllZeros() ? 0 :
            static_cast<uint8_t>(store_it->data()[shift + i]);
        request->SQforwardPackets.push_back(
            LSQ::LSQRequest::FWDPacket{static_cast<int>(i), byte});
    }

    load_inst->specStoreFwd = true;
    load_inst->specStoreFwdState = store_it->addrReady() ?
        SpecStoreFwdState::SqConfirmed :
        SpecStoreFwdState::PendingValidation;
    load_inst->stlfFromStoreQueue = true;
    load_inst->stlfStoreSeqNum = store_inst->seqNum;
    load_inst->stlfDistance = distance;
    load_inst->stlfShiftAmt = shift;
    load_inst->setFullForward();
    ++lsqUnit->stats.forwLoads;

    DPRINTF(SPECFwd,
            "Spec-STLF load[sn:%llu] PC %#lx forwards from store[sn:%llu] "
            "(distance=%u shift=%u size=%u addrReady=%u)\n",
            load_inst->seqNum, load_inst->pcState().instAddr(),
            store_inst->seqNum, distance, shift, load_size,
            store_it->addrReady());
    return AttemptResult::Forwarded;
}

void
SpecStoreFwdUnit::checkSpecStoreFwdMispred(const DynInstPtr &store_inst)
{
    if (!lsqUnit || !pred.ready() || !store_inst || store_inst->isSquashed() ||
        !store_inst->effAddrValid()) {
        return;
    }

    const ssize_t store_idx = store_inst->sqIdx;
    if (store_idx < 0 ||
        !lsqUnit->storeQueue.isValidIdx(static_cast<size_t>(store_idx))) {
        return;
    }
    if (!lsqUnit->storeQueue[store_idx].valid()) {
        return;
    }

    const Addr store_paddr = store_inst->physEffAddr;
    const unsigned store_size = lsqUnit->storeQueue[store_idx].size();
    if (store_size == 0) {
        return;
    }

    DynInstPtr oldest_mispred = nullptr;
    uint32_t mispreds = 0;

    for (auto it = lsqUnit->loadQueue.begin(); it != lsqUnit->loadQueue.end();
         ++it) {
        if (!it->valid() || !it->instruction()) {
            continue;
        }
        const auto &ld_inst = it->instruction();
        if (!ld_inst || ld_inst->isSquashed() ||
            (ld_inst->specStoreFwdState !=
                 SpecStoreFwdState::PendingValidation &&
             ld_inst->specStoreFwdState !=
                 SpecStoreFwdState::SqConfirmed)) {
            continue;
        }
        if (!ld_inst->effAddrValid()) {
            continue;
        }

        // This scan only validates the predicted source. Other stores must go
        // through checkViolations(), whose load iterator enforces program age
        // and prevents a younger store from squashing an already retired load.
        if (ld_inst->specStoreFwdStoreSeqNum != store_inst->seqNum) {
            continue;
        }

        const unsigned load_size = ld_inst->effSize;
        const int64_t actual_shift =
            static_cast<int64_t>(ld_inst->physEffAddr) -
            static_cast<int64_t>(store_paddr);
        const bool shift_in_range = actual_shift >= 0 &&
            static_cast<uint64_t>(actual_shift) <= store_size;
        const bool mispred = !shift_in_range ||
            actual_shift != ld_inst->specStoreFwdShiftAmt ||
            (shift_in_range &&
             load_size > store_size - static_cast<unsigned>(actual_shift));
        if (!mispred) {
            ld_inst->specStoreFwdState = SpecStoreFwdState::SqConfirmed;
            continue;
        }

        feedbackShiftMismatch(ld_inst);
        markAddrValidationFail(ld_inst);

        mispreds++;
        if (!oldest_mispred || ld_inst->seqNum < oldest_mispred->seqNum) {
            oldest_mispred = ld_inst;
        }
    }

    if (mispreds == 0) {
        return;
    }

    lsqUnit->storeQueue[store_idx].specStoreFwdMispreds() += mispreds;

    if (!lsqUnit->memDepViolator ||
        (oldest_mispred &&
         oldest_mispred->seqNum < lsqUnit->memDepViolator->seqNum)) {
        lsqUnit->memDepViolator = oldest_mispred;
        lsqUnit->memDepViolationCause = ViolationCause::SpecStoreFwd;
    }

    DPRINTF(SPECFwd,
            "Spec-STLF mispred: store[sn:%llu] triggers squash from load[sn:%llu] "
            "(mispreds=%u)\n",
            store_inst->seqNum, oldest_mispred ? oldest_mispred->seqNum : 0,
            mispreds);
}

void
SpecStoreFwdUnit::commitLoad(const DynInstPtr &inst)
{
    if (!lsqUnit || !inst) {
        return;
    }

    // Commit-time accounting (avoid wrong-path effects).
    if (inst->isNormalLd()) {
        lsqUnit->stats.specStoreFwdTotalLoads++;
        if (inst->mdpNonStrictWait) {
            lsqUnit->stats.specStoreFwdMdpWaitLoads++;
        }
    }

    const bool success =
        inst->specStoreFwdState == SpecStoreFwdState::PendingValidation ||
        inst->specStoreFwdState == SpecStoreFwdState::SqConfirmed;
    const bool fail =
        inst->specStoreFwdState == SpecStoreFwdState::SqCorrectedFail;
    if (success || fail) {
        lsqUnit->stats.specStoreFwdPredicted++;
    }
    if (success) {
        lsqUnit->stats.specStoreFwdSuccess++;
        if (inst->mdpNonStrictWait) {
            lsqUnit->stats.specStoreFwdMdpWaitSuccess++;
        }
    }
    if (fail) {
        lsqUnit->stats.specStoreFwdFail++;
    }
    if (inst->specStoreFwdDataWaited) {
        lsqUnit->stats.specStoreFwdDataWait++;
    }
    if (inst->specStoreFwdSameEntry) {
        lsqUnit->stats.specStoreFwdSqSameEntry++;
    }
    if (inst->specStoreFwdWonOverSq) {
        lsqUnit->stats.specStoreFwdSpecWinsSq++;
    }
    if (inst->specStoreFwdSqCorrected) {
        lsqUnit->stats.specStoreFwdSqCorrectsSpec++;
    }
    // Predictor training is independent of MDP and only observes committed
    // full forwarding from the live store queue.
    if (inst->stlfFromStoreQueue && inst->fullForward()) {
        pred.train(inst->pcState().instAddr(), inst->stlfDistance,
                   inst->stlfShiftAmt);
        if (pred.ready()) {
            lsqUnit->stats.specStoreFwdTrainEvents++;
        }
        DPRINTF(SPECFwd,
                "Spec-STLF train load[sn:%llu] PC %#lx distance=%u shift=%u\n",
                inst->seqNum, inst->pcState().instAddr(), inst->stlfDistance,
                inst->stlfShiftAmt);
    }
}

void
SpecStoreFwdUnit::commitStore(size_t store_idx)
{
    if (!lsqUnit) {
        return;
    }
    if (!lsqUnit->storeQueue.isValidIdx(store_idx) ||
        !lsqUnit->storeQueue[store_idx].valid()) {
        return;
    }

    auto &entry = lsqUnit->storeQueue[store_idx];
    if (entry.specStoreFwdMispreds() == 0) {
        return;
    }

    // Failed Spec-STLF predictions are attributed to the (older) store so they
    // can be counted at commit time.
    lsqUnit->stats.specStoreFwdPredicted += entry.specStoreFwdMispreds();
    lsqUnit->stats.specStoreFwdFail += entry.specStoreFwdMispreds();
    lsqUnit->stats.specStoreFwdAddrValidationFail +=
        entry.specStoreFwdMispreds();
    entry.specStoreFwdMispreds() = 0;
}

void
SpecStoreFwdUnit::clearCurrentForward(const DynInstPtr &inst)
{
    if (!inst) {
        return;
    }
    inst->specStoreFwd = false;
    inst->stlfFromStoreQueue = false;
    inst->stlfStoreSeqNum = 0;
    inst->stlfDistance = 0;
    inst->stlfShiftAmt = 0;
}

void
SpecStoreFwdUnit::clearPrediction(const DynInstPtr &inst)
{
    if (!inst) {
        return;
    }
    inst->specStoreFwdState = SpecStoreFwdState::None;
    inst->specStoreFwdStoreSeqNum = 0;
    inst->specStoreFwdDistance = 0;
    inst->specStoreFwdShiftAmt = 0;
    inst->specStoreFwdDataWaited = false;
    inst->specStoreFwdSameEntry = false;
    inst->specStoreFwdWonOverSq = false;
    inst->specStoreFwdSqCorrected = false;
}

void
SpecStoreFwdUnit::beginLoadAttempt(const DynInstPtr &inst)
{
    if (!lsqUnit || !inst || !pred.enabled()) {
        return;
    }
    clearCurrentForward(inst);
    if (inst->specStoreFwdState != SpecStoreFwdState::WaitingData &&
        inst->specStoreFwdState != SpecStoreFwdState::SqCorrectedFail) {
        clearPrediction(inst);
    }
}

void
SpecStoreFwdUnit::cancelLoadAttempt(const DynInstPtr &inst)
{
    if (!lsqUnit || !inst || !pred.enabled()) {
        return;
    }
    clearCurrentForward(inst);
    if (inst->specStoreFwdState != SpecStoreFwdState::WaitingData &&
        inst->specStoreFwdState != SpecStoreFwdState::SqCorrectedFail) {
        clearPrediction(inst);
    }
}

void
SpecStoreFwdUnit::markSqConfirmed(const DynInstPtr &inst)
{
    inst->specStoreFwdState = SpecStoreFwdState::SqConfirmed;
    inst->specStoreFwdSameEntry = true;
    inst->specStoreFwd = false;
}

void
SpecStoreFwdUnit::markSqCorrected(const DynInstPtr &inst)
{
    inst->specStoreFwdState = SpecStoreFwdState::SqCorrectedFail;
    inst->specStoreFwdSqCorrected = true;
    inst->specStoreFwd = false;
}

void
SpecStoreFwdUnit::markSpecWonOverSq(const DynInstPtr &inst)
{
    inst->specStoreFwdWonOverSq = true;
}

void
SpecStoreFwdUnit::markAddrValidationFail(const DynInstPtr &inst)
{
    inst->specStoreFwdState = SpecStoreFwdState::AddrValidationFail;
    inst->specStoreFwd = false;
}

bool
SpecStoreFwdUnit::hasPrediction(const DynInstPtr &inst) const
{
    if (!inst) {
        return false;
    }
    return inst->specStoreFwdState == SpecStoreFwdState::WaitingData ||
        inst->specStoreFwdState == SpecStoreFwdState::PendingValidation ||
        inst->specStoreFwdState == SpecStoreFwdState::SqConfirmed;
}

InstSeqNum
SpecStoreFwdUnit::predictedStoreSeq(const DynInstPtr &inst) const
{
    return inst ? inst->specStoreFwdStoreSeqNum : 0;
}

void
SpecStoreFwdUnit::applyFeedback(const DynInstPtr &load_inst,
                                SpecStoreFwdFeedbackReason reason,
                                std::optional<uint16_t> distance,
                                std::optional<uint16_t> shift)
{
    if (!lsqUnit || !load_inst || !pred.enabled()) {
        return;
    }

    const Addr pc = load_inst->pcState().instAddr();
    if (distance && shift) {
        pred.updateMetaAndDecrement(pc, *distance, *shift);
    } else if (distance) {
        pred.updateDistanceAndDecrement(pc, *distance);
    } else {
        pred.decrement(pc);
    }
    ++lsqUnit->stats.specStoreFwdCtrDecrements[
        static_cast<unsigned>(reason)];
    DPRINTF(SPECFwd,
            "Spec-STLF feedback load[sn:%llu] reason=%s update=%s "
            "distance=%u shift=%u decrement\n",
            load_inst->seqNum,
            SpecStoreFwdFeedbackReasonNames[static_cast<unsigned>(reason)],
            distance ? (shift ? "meta" : "distance") : "none",
            distance.value_or(0), shift.value_or(0));
}

void
SpecStoreFwdUnit::feedbackShiftMismatch(const DynInstPtr &inst)
{
    applyFeedback(inst, SpecStoreFwdFeedbackReason::ShiftMismatch);
}

void
SpecStoreFwdUnit::feedbackDataReplayInvalidSource(const DynInstPtr &inst)
{
    applyFeedback(inst, SpecStoreFwdFeedbackReason::DataReplayInvalidSource);
}

void
SpecStoreFwdUnit::feedbackSqYoungerFull(const DynInstPtr &inst,
                                        uint16_t distance, uint16_t shift)
{
    applyFeedback(inst, SpecStoreFwdFeedbackReason::SqYoungerFull, distance,
                  shift);
}

void
SpecStoreFwdUnit::feedbackSqPartialReplay(const DynInstPtr &inst)
{
    applyFeedback(inst, SpecStoreFwdFeedbackReason::SqPartialReplay);
}

void
SpecStoreFwdUnit::feedbackSqDataNotReadyReplay(const DynInstPtr &inst,
                                               uint16_t distance)
{
    applyFeedback(inst, SpecStoreFwdFeedbackReason::SqDataNotReadyReplay,
                  distance);
}

void
SpecStoreFwdUnit::feedbackYoungerNukeOrViolation(
    const DynInstPtr &inst, uint16_t distance, std::optional<uint16_t> shift)
{
    if (distance == 0) {
        applyFeedback(inst, SpecStoreFwdFeedbackReason::YoungerNukeOrViolation);
    } else {
        applyFeedback(inst, SpecStoreFwdFeedbackReason::YoungerNukeOrViolation,
                      distance, shift);
    }
}

} // namespace o3
} // namespace gem5
