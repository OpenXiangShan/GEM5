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

#include <algorithm>

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

bool
SpecStoreFwdUnit::hasAddrReadyStoreDependency(
    const DynInstPtr &load_inst, LSQ::LSQRequest *request) const
{
    if (!lsqUnit || !load_inst || !request || !request->isNormalLd()) {
        return false;
    }

    const Addr load_start = request->mainReq()->getPaddr();
    const Addr load_end = load_start + request->mainReq()->getSize();
    auto store_it = load_inst->sqIt;

    // Match read()'s outstanding-SQ window. Any known-address overlap must go
    // through normal forwarding/replay handling instead of Spec-STLF.
    while (lsqUnit->storeWBIt.dereferenceable() &&
           store_it != lsqUnit->storeWBIt) {
        --store_it;
        if (!store_it->valid() || !store_it->instruction() ||
            store_it->completed() || !store_it->addrReady() ||
            store_it->size() == 0) {
            continue;
        }

        const auto &store_inst = store_it->instruction();
        if (store_inst->seqNum >= load_inst->seqNum) {
            continue;
        }

        const Addr store_start = store_inst->physEffAddr;
        const Addr store_end = store_start + store_it->size();
        if (load_start < store_end && store_start < load_end) {
            DPRINTF(SPECFwd,
                    "Reject Spec-STLF: load[sn:%llu] overlaps "
                    "address-ready store[sn:%llu]\n",
                    load_inst->seqNum, store_inst->seqNum);
            return true;
        }
    }

    return false;
}

void
SpecStoreFwdUnit::init(LSQUnit *lsq_unit, bool enable, size_t table_size,
                       unsigned ctr_bits, bool allow_no_mdp)
{
    lsqUnit = lsq_unit;
    allowNoMdp_ = allow_no_mdp;
    pred.init(enable, table_size, ctr_bits);
}

bool
SpecStoreFwdUnit::trySpecStoreFwd(const DynInstPtr &load_inst,
                                  LSQ::LSQRequest *request,
                                  const std::vector<size_t> &wait_store_idxs)
{
    DPRINTF(SPECFwd,
            "Try Spec-STLF at trySpecStoreFwd: load[sn:%llu] PC %#lx\n",
            load_inst->seqNum, load_inst->pcState().instAddr());
    if (!lsqUnit || !pred.ready()) {
        return false;
    }
    if (!request || !request->isNormalLd()) {
        return false;
    }
    if (hasAddrReadyStoreDependency(load_inst, request)) {
        return false;
    }
    if (wait_store_idxs.empty()) {
        return false;
    }

    const Addr ld_pc = load_inst->pcState().instAddr();
    const auto pred_meta = pred.predict(ld_pc);
    if (!pred_meta) {
        return false;
    }

    const uint16_t pred_distance = pred_meta->first;
    const uint16_t pred_shift = pred_meta->second;
    const unsigned load_size = request->mainReq()->getSize();

    const size_t ld_sq_boundary = load_inst->sqIt.idx();

    for (const auto st_idx : wait_store_idxs) {
        DPRINTF(SPECFwd,
                "try waiting stores, st:%ld\n",
                st_idx);
        if (!lsqUnit->storeQueue.isValidIdx(st_idx)) {
            continue;
        }
        const auto st_it = lsqUnit->storeQueue.getIterator(st_idx);
        if (!st_it->valid() || !st_it->instruction()) {
            continue;
        }

        const auto &st_inst = st_it->instruction();
        if (st_inst->seqNum >= load_inst->seqNum) {
            continue;
        }
        // The address-unknown SQ state does not carry enough information to
        // validate a dynamic vector mask or a store-conditional outcome.
        // Such stores cannot safely provide speculative bytes.
        if (st_inst->isVector() || st_inst->isAtomic() ||
            st_inst->isStoreConditional()) {
            continue;
        }
        DPRINTF(SPECFwd,
                "try Spec FWD: load[sn:%llu] PC %#lx, try store[sn:%llu] "
                "(pred distance=%u, real distance=%u, shift=%u, storeSize=%u, loadSize=%u, "
                "loadAddr=%llx, storeAddr=%llx, storeAddrValid=%u, storeDataValid=%u)\n",
                load_inst->seqNum, ld_pc, st_inst->seqNum, pred_distance,
                static_cast<uint16_t>(ld_sq_boundary - st_it.idx()),
                pred_shift, static_cast<unsigned>(st_inst->operWid() / 8), load_size,
                load_inst->physEffAddr, st_inst->physEffAddr,
                st_it->addrReady(), st_it->dataReady());
        // Only speculate when store data is ready but address is not.
        if (st_it->addrReady() || !st_it->dataReady()) {
            continue;
        }

        if (ld_sq_boundary <= st_it.idx()) {
            continue;
        }
        const uint16_t distance = static_cast<uint16_t>(
            ld_sq_boundary - st_it.idx());
        if (distance != pred_distance) {
            continue;
        }

        const int32_t st_op_wid = st_inst->operWid();
        if (st_op_wid <= 0) {
            continue;
        }
        const unsigned store_size = static_cast<unsigned>(st_op_wid / 8);

        if (pred_shift >= store_size) {
            continue;
        }
        if (load_size > (store_size - pred_shift)) {
            continue;
        }

        // Allocate memory if this is the first time a load is issued.
        if (!load_inst->memData) {
            load_inst->memData = new uint8_t[load_size];
        }

        request->SQforwardPackets.clear();
        for (unsigned i = 0; i < load_size; i++) {
            const uint8_t byte =
                static_cast<uint8_t>(st_it->data()[pred_shift + i]);
            request->SQforwardPackets.push_back(
                LSQ::LSQRequest::FWDPacket{
                    static_cast<int>(i),
                    byte
                });
        }

        // Record forwarding meta for training and failure recovery.
        load_inst->specStoreFwd = true;
        load_inst->stlfFromStoreQueue = true;
        load_inst->stlfStoreSeqNum = st_inst->seqNum;
        load_inst->stlfDistance = distance;
        load_inst->stlfShiftAmt = pred_shift;

        load_inst->setFullForward();
        ++lsqUnit->stats.forwLoads;

        DPRINTF(SPECFwd,
                "Spec-STLF: load[sn:%llu] PC %#lx forward from store[sn:%llu] "
                "(pred distance=%u, shift=%u, size=%u)\n",
                load_inst->seqNum, ld_pc, st_inst->seqNum, pred_distance,
                pred_shift, load_size);
        return true;
    }

    return false;
}

bool
SpecStoreFwdUnit::trySpecStoreFwd(const DynInstPtr &load_inst,
                                  LSQ::LSQRequest *request)
{
    if (!allowNoMdp_) {
        return false;
    }
    if (!lsqUnit || !pred.ready()) {
        return false;
    }
    if (!request || !request->isNormalLd()) {
        return false;
    }

    const Addr ld_pc = load_inst->pcState().instAddr();
    const auto pred_meta = pred.predict(ld_pc);
    if (!pred_meta) {
        return false;
    }

    const uint16_t pred_distance = pred_meta->first;
    const size_t ld_sq_boundary = load_inst->sqIt.idx();
    if (ld_sq_boundary < pred_distance) {
        return false;
    }

    const size_t st_idx = ld_sq_boundary - pred_distance;
    return trySpecStoreFwd(load_inst, request, std::vector<size_t>{st_idx});
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
        if (!ld_inst || ld_inst->isSquashed() || !ld_inst->specStoreFwd ||
            !ld_inst->stlfFromStoreQueue) {
            continue;
        }
        if (ld_inst->stlfStoreSeqNum != store_inst->seqNum) {
            continue;
        }
        if (!ld_inst->effAddrValid()) {
            continue;
        }

        const unsigned load_size = ld_inst->effSize;
        const int64_t actual_shift =
            static_cast<int64_t>(ld_inst->physEffAddr) -
            static_cast<int64_t>(store_paddr);

        const bool mispred =
            actual_shift < 0 ||
            actual_shift != static_cast<int64_t>(ld_inst->stlfShiftAmt) ||
            (load_size >
             (store_size - static_cast<unsigned>(actual_shift)));

        if (!mispred) {
            continue;
        }

        // Failure recovery: reset predictor meta for this load PC.
        resetPredictorMeta(ld_inst);

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

    if (inst->specStoreFwd) {
        lsqUnit->stats.specStoreFwdPredicted++;
        lsqUnit->stats.specStoreFwdSuccess++;
    }
    // DPRINTF(SPECFwd,
    //         "Spec-STLF commit: load[sn:%llu] predicted=%u, inst->mdpPredStrictWait=%u, "
    //         "!inst->mdpProducingStores.empty()=%u, inst->stlfFromStoreQueue=%u, inst->fullForward()=%u\n",
    //         inst->seqNum, inst->specStoreFwd ? 1 : 0, inst->mdpPredStrictWait,
    //         !inst->mdpProducingStores.empty() ? 1 : 0, inst->stlfFromStoreQueue,
    //         inst->fullForward() ? 1 : 0);

    // Training is commit-time to avoid wrong-path effects.
    //
    // Default: only train when the actual forwarding store is one of the
    // predicted producing stores (i.e. within the replay-based MDP scope).
    //
    // When allowNoMdp_ is enabled: ignore MDP scope and train on any committed
    // full STLF from the store queue.
    if ((allowNoMdp_ || !inst->mdpPredStrictWait) &&
        inst->stlfFromStoreQueue && inst->fullForward()) {
        const bool in_mdp_scope =
            !inst->mdpProducingStores.empty() &&
            (std::find(inst->mdpProducingStores.begin(),
                       inst->mdpProducingStores.end(),
                       inst->stlfStoreSeqNum) != inst->mdpProducingStores.end());
        if (allowNoMdp_ || in_mdp_scope) {
            pred.train(inst->pcState().instAddr(), inst->stlfDistance,
                       inst->stlfShiftAmt);
            if (pred.ready()) {
                lsqUnit->stats.specStoreFwdTrainEvents++;
            }
            DPRINTF(SPECFwd,
                    "Spec-STLF train: load[sn:%llu] at PC 0x%llx, "
                    "inst->stlfDistance=%u, inst->stlfShiftAmt=%u, "
                    "in_mdp_scope=%u\n",
                    inst->seqNum, inst->pcState().instAddr(), inst->stlfDistance,
                    inst->stlfShiftAmt, in_mdp_scope ? 1 : 0);
        }
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
    entry.specStoreFwdMispreds() = 0;
}

void
SpecStoreFwdUnit::resetSpecFwdInfo(const DynInstPtr &inst)
{
    if (!lsqUnit || !inst || !pred.enabled()) {
        return;
    }

    // Reset the speculative forwarding information for this instruction.
    inst->specStoreFwd = false;
    inst->stlfFromStoreQueue = false;
    inst->stlfStoreSeqNum = 0;
    inst->stlfDistance = 0;
    inst->stlfShiftAmt = 0;
}

void
SpecStoreFwdUnit::resetPredictorMeta(const DynInstPtr &load_inst)
{
    if (!lsqUnit || !load_inst || !pred.enabled()) {
        return;
    }

    pred.reset(load_inst->pcState().instAddr());
}

} // namespace o3
} // namespace gem5
