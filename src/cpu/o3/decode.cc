/*
 * Copyright (c) 2012, 2014 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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
#include "cpu/o3/decode.hh"

#include <cstring>
#include <initializer_list>
#include <limits>
#include <queue>
#include <utility>

#include "arch/generic/pcstate.hh"
#include "arch/riscv/insts/fusion.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Activity.hh"
#include "debug/Counters.hh"
#include "debug/Decode.hh"
#include "debug/DecoupleBP.hh"
#include "debug/O3PipeView.hh"
#include "debug/UC.hh"
#include "params/BaseO3CPU.hh"
#include "sim/full_system.hh"

// clang complains about std::set being overloaded with Packet::set if
// we open up the entire namespace std
using std::list;

namespace gem5
{

namespace o3
{

namespace
{

enum FusionPairSource
{
    FusionNormalNormal,
    FusionUopUopSameEntry,
    FusionUopUopDifferentEntry,
    FusionNormalToUop,
    FusionUopToNormal,
    NumFusionPairSources
};

const char *fusionPairSourceNames[] = {
    "normal_normal",
    "uop_uop_same_entry",
    "uop_uop_different_entry",
    "normal_to_uop",
    "uop_to_normal",
};

enum FusionRejectReason
{
    FusionRejectMixedSource,
    FusionRejectDifferentUopEntry,
    FusionRejectFaulted,
    FusionRejectLoadFusionDisabled,
    FusionRejectIgnoredPC,
    FusionRejectFirstMapMiss,
    FusionRejectSecondMapMiss,
    FusionRejectCreatorRejected,
    NumFusionRejectReasons
};

const char *fusionRejectReasonNames[] = {
    "mixed_source",
    "different_uop_entry",
    "faulted",
    "load_fusion_disabled",
    "ignored_pc",
    "first_map_miss",
    "second_map_miss",
    "creator_rejected",
};

enum FusionDiagnosticPair
{
    FusionDiagOther,
    FusionDiagSll4Add,
    FusionDiagAddwByte,
};

std::type_index
normalizedFusionType(const StaticInstPtr &inst)
{
    const StaticInst *static_inst = inst.get();
    auto type = std::type_index(typeid(*static_inst));
    auto it = RiscvISA::deCompressMap.find(type);
    return it == RiscvISA::deCompressMap.end() ? type : it->second;
}

bool
makeFusionKey(const StaticInstPtr &inst, RiscvISA::FusionKey &key)
{
    const auto *riscv_inst =
        dynamic_cast<const RiscvISA::RiscvStaticInst *>(inst.get());
    if (!riscv_inst) {
        return false;
    }

    key = RiscvISA::FusionKey(
        normalizedFusionType(inst), riscv_inst->getImm());
    return true;
}

FusionDiagnosticPair
classifyDiagnosticFusionPair(const DynInstPtr &first,
                             const DynInstPtr &second)
{
    auto mnemonic_is = [](const StaticInstPtr &inst,
                          std::initializer_list<const char *> names) {
        const char *mnemonic = inst->getMnemonic();
        for (const char *name : names) {
            if (std::strcmp(mnemonic, name) == 0) {
                return true;
            }
        }
        return false;
    };
    auto imm_is = [](const StaticInstPtr &inst, int imm) {
        RiscvISA::FusionKey key;
        return makeFusionKey(inst, key) && key.imm == imm;
    };

    if (mnemonic_is(first->staticInst, {"slli", "c_slli"}) &&
        mnemonic_is(second->staticInst, {"add", "c_add"}) &&
        imm_is(first->staticInst, 4)) {
        return FusionDiagSll4Add;
    }

    if (mnemonic_is(first->staticInst, {"addw", "c_addw"}) &&
        mnemonic_is(second->staticInst, {"andi", "c_andi"}) &&
        imm_is(second->staticInst, 255)) {
        return FusionDiagAddwByte;
    }

    return FusionDiagOther;
}

} // namespace

Decode::Decode(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      renameToDecodeDelay(params.renameToDecodeDelay),
      iewToDecodeDelay(params.iewToDecodeDelay),
      commitToDecodeDelay(params.commitToDecodeDelay),
      fetchToDecodeDelay(params.fetchToDecodeDelay),
      decodeToFetchDelay(params.decodeToFetchDelay),
      decodeWidth(params.decodeWidth),
      uopCacheBypassQueueSize(params.uopCacheBypassQueueSize),
      enableUopCache(params.hasUopCache),
      numThreads(params.numThreads),
      enableLoadFusion(params.enable_loadFusion),
      stats(_cpu, params.numThreads)
{
    if (decodeWidth > MaxWidth)
        fatal("decodeWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             decodeWidth, static_cast<int>(MaxWidth));
    fatal_if(uopCacheBypassQueueSize == 0,
             "uopCacheBypassQueueSize must be greater than zero");

    // @todo: Make into a parameter
    for (int i=0;i<numThreads;i++) {
        fixedbuffer[i] = boost::circular_buffer<DynInstPtr>(decodeWidth);
    }
    // Preserve the pre-uop-cache pipeline capacity exactly when disabled.
    // Enabled mode may hold normal bundles while an older bypass sequence is
    // selected, so it needs bounded headroom up to the configured Fetch queue;
    // that extra capacity is never present in the baseline A/B path.
    const auto normal_fetch_buffer_groups = enableUopCache ?
        params.fetchQueueSize + fetchToDecodeDelay + 1 :
        fetchToDecodeDelay + 1;
    stallBuffer = boost::circular_buffer<DynInstPtr>(
        decodeWidth * normal_fetch_buffer_groups);
    eachstallSize = boost::circular_buffer<int>(normal_fetch_buffer_groups);


    decodeStalls.resize(decodeWidth, StallReason::NoStall);
    statistics::registerDumpCallback([this]() {
        int idx = 0;
        for (auto it : this->fusionType) {
            this->stats.fusedInsts.subname(idx, it.first);
            this->stats.fusedInsts[idx] = it.second;
            idx++;
        }
        this->fusionType.clear();
    });
}

void
Decode::startupStage()
{
    resetStage();
}

void
Decode::clearStates(ThreadID tid)
{
    if (enableUopCache) {
        // The legacy path owns no per-thread Decode state here.  Fast-path
        // queues must be cleared explicitly because they bypass TimeBuffer.
        fixedbuffer[tid].clear();
        uopCacheBypassQueue[tid].clear();
        lastDecodeCycleTail[tid] = nullptr;
    }
}

void
Decode::resetStage()
{
    _status = Inactive;
    if (enableUopCache) {
        stallBuffer.clear();
        eachstallSize.clear();
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            fixedbuffer[tid].clear();
            uopCacheBypassQueue[tid].clear();
            lastDecodeCycleTail[tid] = nullptr;
        }
    }
}

std::string
Decode::name() const
{
    return cpu->name() + ".decode";
}

Decode::DecodeStats::DecodeStats(CPU *cpu, unsigned num_threads)
    : statistics::Group(cpu, "decode"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is idle"),
      ADD_STAT(smtidleCycles, statistics::units::Cycle::get(),
               "Number of idle cycles per thread"),
      ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is blocked"),
      ADD_STAT(smtblockedCycles, statistics::units::Cycle::get(),
               "Number of blocked cycles per thread"),
      ADD_STAT(smtnotactiveCycles, statistics::units::Cycle::get(),
               "Number of inactive cycles per thread"),
      ADD_STAT(runCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is running"),
      ADD_STAT(unblockCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is unblocking"),
      ADD_STAT(squashCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is squashing"),
      ADD_STAT(branchResolved, statistics::units::Count::get(),
               "Number of times decode resolved a branch"),
      ADD_STAT(branchMispred, statistics::units::Count::get(),
               "Number of times decode detected a branch misprediction"),
      ADD_STAT(numFusedInsts, statistics::units::Count::get(),
               "Number of fused instructions handled by decode"),
      ADD_STAT(fusedInsts, statistics::units::Count::get(),
               "Number of times decode fused instructions by type"),
      ADD_STAT(fusionPairsChecked, statistics::units::Count::get(),
               "Number of adjacent instruction pairs checked for fusion"),
      ADD_STAT(fusionPairSources, statistics::units::Count::get(),
               "Source classes of adjacent instruction pairs checked for "
               "fusion"),
      ADD_STAT(fusionRejectReasons, statistics::units::Count::get(),
               "Reasons adjacent instruction pairs did not fuse"),
      ADD_STAT(fusionSourceBoundaryWouldFuse, statistics::units::Count::get(),
               "Source-boundary rejects that would otherwise pass fusion"),
      ADD_STAT(fusionCycleBoundaryPairsChecked, statistics::units::Count::get(),
               "Pairs checked across adjacent decode cycles"),
      ADD_STAT(fusionCycleBoundaryWouldFuse, statistics::units::Count::get(),
               "Adjacent decode-cycle boundary pairs that would fuse if "
               "decoded in the same cycle"),
      ADD_STAT(fusionSll4AddRejectReasons, statistics::units::Count::get(),
               "Reject reasons for opcode-level sll4add fusion candidates"),
      ADD_STAT(fusionAddwByteRejectReasons, statistics::units::Count::get(),
               "Reject reasons for opcode-level addwbyte fusion candidates"),
      ADD_STAT(controlMispred, statistics::units::Count::get(),
               "Number of times decode detected an instruction incorrectly "
               "predicted as a control"),
      ADD_STAT(decodedInsts, statistics::units::Count::get(),
               "Number of instructions handled by decode"),
      ADD_STAT(squashedInsts, statistics::units::Count::get(),
               "Number of squashed instructions handled by decode"),
      ADD_STAT(mispredictedByPC, statistics::units::Count::get(),
               "Number of instructions that mispredicted due to pc"),
      ADD_STAT(mispredictedByNPC, statistics::units::Count::get(),
               "Number of instructions that mispredicted due to npc")
{
    idleCycles.prereq(idleCycles);
    smtidleCycles.init(num_threads).flags(statistics::total);
    blockedCycles.prereq(blockedCycles);
    smtblockedCycles.init(num_threads).flags(statistics::total);
    smtnotactiveCycles.init(num_threads).flags(statistics::total);
    runCycles.prereq(runCycles);
    unblockCycles.prereq(unblockCycles);
    squashCycles.prereq(squashCycles);
    branchResolved.prereq(branchResolved);
    branchMispred.prereq(branchMispred);
    controlMispred.prereq(controlMispred);
    decodedInsts.prereq(decodedInsts);
    squashedInsts.prereq(squashedInsts);
    mispredictedByPC.flags(statistics::total);
    mispredictedByNPC.flags(statistics::total);
    fusedInsts.init(128).flags(statistics::nozero);
    fusionPairSources.init(NumFusionPairSources).flags(statistics::nozero);
    for (int i = 0; i < NumFusionPairSources; ++i) {
        fusionPairSources.subname(i, fusionPairSourceNames[i]);
    }
    fusionRejectReasons.init(NumFusionRejectReasons)
        .flags(statistics::nozero);
    fusionSourceBoundaryWouldFuse.init(NumFusionRejectReasons)
        .flags(statistics::nozero);
    fusionSll4AddRejectReasons.init(NumFusionRejectReasons)
        .flags(statistics::nozero);
    fusionAddwByteRejectReasons.init(NumFusionRejectReasons)
        .flags(statistics::nozero);
    for (int i = 0; i < NumFusionRejectReasons; ++i) {
        fusionRejectReasons.subname(i, fusionRejectReasonNames[i]);
        fusionSourceBoundaryWouldFuse.subname(i, fusionRejectReasonNames[i]);
        fusionSll4AddRejectReasons.subname(i, fusionRejectReasonNames[i]);
        fusionAddwByteRejectReasons.subname(i, fusionRejectReasonNames[i]);
    }
    fusionPairsChecked.prereq(fusionPairsChecked);
    fusionCycleBoundaryPairsChecked.prereq(fusionCycleBoundaryPairsChecked);
    fusionCycleBoundaryWouldFuse.prereq(fusionCycleBoundaryWouldFuse);
}

void
Decode::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to write information back to fetch.
    toFetch = timeBuffer->getWire(0);

    // Create wires to get information from proper places in time buffer.
    fromRename = timeBuffer->getWire(-renameToDecodeDelay);
    fromIEW = timeBuffer->getWire(-iewToDecodeDelay);
    fromCommit = timeBuffer->getWire(-commitToDecodeDelay);
}

void
Decode::setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)
{
    decodeQueue = dq_ptr;

    // Setup wire to write information to proper place in decode queue.
    toRename = decodeQueue->getWire(0);
}

void
Decode::setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr)
{
    fetchQueue = fq_ptr;

    // Setup wire to read information from fetch queue.
    fromFetch = fetchQueue->getWire(-fetchToDecodeDelay);
}

void
Decode::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
Decode::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        assert(fixedbuffer[tid].empty());
        assert(uopCacheBypassQueue[tid].empty());
    }
}

bool
Decode::isDrained() const
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (!fixedbuffer[tid].empty() || !uopCacheBypassQueue[tid].empty())
            return false;
    }
    return true;
}

void
Decode::enqueueUopCacheBypassInst(const DynInstPtr &inst)
{
    assert(enableUopCache);
    ThreadID tid = inst->threadNumber;
    assert(tid < numThreads);
    assert(inst->isFetchFromUopCache());
    assert(canEnqueueUopCacheBypassInst(tid));

    uopCacheBypassQueue[tid].push_back(inst);
    DPRINTF(UC, "[tid:%i] Enqueued uop-cache bypass inst [sn:%llu] pc=%#lx "
            "queue=%d\n",
            tid, inst->seqNum, inst->getPC(), uopCacheBypassQueue[tid].size());

    if (_status == Inactive) {
        _status = Active;
        cpu->activateStage(CPU::DecodeIdx);
    }
    cpu->activityThisCycle();
}

bool
Decode::canEnqueueUopCacheBypassInst(ThreadID tid) const
{
    assert(tid < numThreads);
    return uopCacheBypassQueue[tid].size() < uopCacheBypassQueueSize;
}

bool
Decode::checkStall(ThreadID tid) const
{
    bool ret_val = false;


    return ret_val;
}

bool
Decode::fetchInstsValid()
{
    return fromFetch->size > 0;
}

void
Decode::selfSquash(const DynInstPtr &inst, ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i] [sn:%llu] Squashing due to incorrect branch "
            "prediction detected at decode.\n", tid, inst->seqNum);

    // Send back mispredict information.
    toFetch->decodeInfo[tid].branchMispredict = true;
    toFetch->decodeInfo[tid].predIncorrect = true;
    toFetch->decodeInfo[tid].mispredictInst = inst;
    toFetch->decodeInfo[tid].squash = true;
    toFetch->decodeInfo[tid].doneSeqNum = inst->seqNum;
    if (inst->isControl()) {
        if (!inst->isReturn()) {
            set(toFetch->decodeInfo[tid].nextPC, *inst->branchTarget());
        } else {
            // if it is return, the target must have already been set in pred target now
            std::unique_ptr<PCStateBase> tgt_ptr(inst->readPredTarg().clone());
            set(toFetch->decodeInfo[tid].nextPC, *tgt_ptr);
        }
    } else {
        std::unique_ptr<PCStateBase> npc_ptr(inst->pcState().clone());
        npc_ptr->as<RiscvISA::PCState>().set(inst->pcState().getFallThruPC());
        set(toFetch->decodeInfo[tid].nextPC, *npc_ptr);
    }

    // Looking at inst->pcState().branching()
    // may yield unexpected results if the branch
    // was predicted taken but aliased in the BTB
    // with a branch jumping to the next instruction (mistarget)
    // Using PCState::branching()  will send execution on the
    // fallthrough and this will not be caught at execution (since
    // branch was correctly predicted taken)
    toFetch->decodeInfo[tid].branchTaken = inst->readPredTaken() ||
                                           inst->isUncondCtrl();

    toFetch->decodeInfo[tid].squashInst = inst;

    InstSeqNum squash_seq_num = inst->seqNum;

    stallSig->blockFetch[tid] = true; // tell fetch don't send new insts

    fixedbuffer[tid].clear();
    uopCacheBypassQueue[tid].clear();
    lastDecodeCycleTail[tid] = nullptr;

    auto delIt = stallBuffer.begin();
    for (auto it0 = eachstallSize.begin(); it0 != eachstallSize.end();) {
        int size = *it0;
        auto start_it = delIt;
        auto end_it = start_it + size;
        if ((*start_it)->threadNumber == tid) {
            delIt = stallBuffer.erase(start_it, end_it);
            it0 = eachstallSize.erase(it0);
        }
        else {
            delIt = end_it;
            it0++;
        }
    }

    // Squash instructions up until this one
    cpu->removeInstsUntil(squash_seq_num, tid);
}

unsigned
Decode::squash(ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i] Squashing.\n",tid);

    fixedbuffer[tid].clear();
    uopCacheBypassQueue[tid].clear();
    lastDecodeCycleTail[tid] = nullptr;

    auto delIt = stallBuffer.begin();
    for (auto it0 = eachstallSize.begin(); it0 != eachstallSize.end();) {
        int size = *it0;
        auto start_it = delIt;
        auto end_it = start_it + size;
        if ((*start_it)->threadNumber == tid) {
            delIt = stallBuffer.erase(start_it, end_it);
            it0 = eachstallSize.erase(it0);
        }
        else {
            delIt = end_it;
            it0++;
        }
    }

    return 0;
}

void
Decode::updateActivate()
{
    bool any_unblocking = false;

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!stallSig->blockDecode[tid]) {
            any_unblocking = true;
            break;
        }
    }

    // Decode will have activity if it's unblocking.
    if (any_unblocking) {
        if (_status == Inactive) {
            _status = Active;

            DPRINTF(Activity, "Activating stage.\n");

            cpu->activateStage(CPU::DecodeIdx);
        }
    } else {
        // If it's not unblocking, then decode will not have any internal
        // activity.  Switch it to inactive.
        if (_status == Active) {
            _status = Inactive;
            DPRINTF(Activity, "Deactivating stage.\n");

            cpu->deactivateStage(CPU::DecodeIdx);
        }
    }
}

unsigned
Decode::moveUopCacheBypassInstsToBuffer(ThreadID tid,
                                        InstSeqNum stopBeforeSeq)
{
    auto &bypass_queue = uopCacheBypassQueue[tid];
    if (bypass_queue.empty()) {
        return 0;
    }
    if (bypass_queue.front()->seqNum >= stopBeforeSeq) {
        return 0;
    }
    if (uopCacheBypassOrderBlocked(bypass_queue.front())) {
        if (fetch_ptr) {
            fetch_ptr->recordUopCacheBypassOrderBlockedEvent();
        }
        return 0;
    }

    int moved = 0;
    unsigned bypassed = 0;
    while (!bypass_queue.empty() &&
           bypass_queue.front()->seqNum < stopBeforeSeq &&
           !fixedbuffer[tid].full() && moved < decodeWidth) {
        if (uopCacheBypassOrderBlocked(bypass_queue.front())) {
            if (fetch_ptr) {
                fetch_ptr->recordUopCacheBypassOrderBlockedEvent();
            }
            break;
        }
        auto inst = bypass_queue.front();
        bypass_queue.pop_front();
        if (inst->isSquashed()) {
            cpu->markInstDecoded(tid, inst->seqNum);
            continue;
        }
        if (localSquashVer[tid].largerThan(inst->getVersion())) {
            inst->setSquashed();
            cpu->markInstDecoded(tid, inst->seqNum);
            continue;
        } else {
            bypassed++;
        }
        fixedbuffer[tid].push_back(inst);
        moved++;
    }

    DPRINTF(UC, "[tid:%i] Moved %d uop-cache bypass insts to decode buffer, "
            "remaining=%d\n",
            tid, moved, bypass_queue.size());

    if (bypassed && fetch_ptr) {
        fetch_ptr->recordUopCacheBypassInsts(bypassed);
    }

    return bypassed;
}

bool
Decode::uopCacheBypassOrderBlocked(const DynInstPtr &inst) const
{
    const ThreadID tid = inst->threadNumber;
    assert(tid < numThreads);

    if (!cpu->hasOlderNonBypassUndecodedInst(inst)) {
        return false;
    }

    const auto &buffer = fixedbuffer[tid];
    if (buffer.empty()) {
        return true;
    }

    // Normal-path instructions moved into fixedbuffer in this same cycle are
    // still globally "undecoded" until decodeInsts() consumes them.  If the
    // bypass instruction is the next sequence after the buffer tail, all older
    // instructions that can block it are already ahead of it in decode order.
    return buffer.back()->seqNum + 1 != inst->seqNum;
}

void
Decode::moveInstsToBuffer()
{
    if (!enableUopCache) {
        // This is the pre-uop-cache algorithm verbatim.  Keeping a dedicated
        // disabled branch prevents bypass ordering and capacity choices from
        // perturbing normal Fetch-to-Decode timing or SMT arbitration.
        auto try_move_head_group = [&]() -> bool {
            if (stallBuffer.empty()) {
                return false;
            }
            ThreadID tid = stallBuffer.front()->threadNumber;
            if (!fixedbuffer[tid].empty()) {
                return false;
            }
            int group_size = eachstallSize.front();
            eachstallSize.pop_front();
            for (int i = 0; i < group_size; ++i) {
                const DynInstPtr &inst = stallBuffer.front();
                assert(tid == inst->threadNumber);
                if (localSquashVer[tid].largerThan(inst->getVersion())) {
                    inst->setSquashed();
                }
                assert(!fixedbuffer[tid].full());
                fixedbuffer[tid].push_back(inst);
                stallBuffer.pop_front();
            }
            return true;
        };

        const bool moved_group = try_move_head_group();
        const int insts_from_fetch = fromFetch->size;
        if (insts_from_fetch != 0) {
            panic_if(eachstallSize.full(),
                     "Decode stallbuffer overflow, has %d stalls\n",
                     eachstallSize.size() + 1);
            eachstallSize.push_back(insts_from_fetch);
            for (int i = 0; i < insts_from_fetch; ++i) {
                stallBuffer.push_back(fromFetch->insts[i]);
            }
        }
        DPRINTF(Decode, "Decode stall buffer has %d stalls\n",
                eachstallSize.size());
        if (!stallBuffer.empty() && !moved_group) {
            try_move_head_group();
        }
        return;
    }

    // Enabled mode merges normal and bypass traffic by sequence number.  A
    // fixedbuffer is filled from only one thread per cycle, preserving the
    // existing per-cycle Decode ownership rule even though queues are per-TID.
    // do not support mixed thread instructions in one fetch group
    int insts_from_fetch = fromFetch->size;
    if (insts_from_fetch != 0) {
        ThreadID tid = fromFetch->insts[0]->threadNumber;

        // move to stallbuffer
        panic_if(eachstallSize.full() ||
                 stallBuffer.size() + insts_from_fetch > stallBuffer.capacity(),
                 "Decode stallbuffer overflow, has %d stalls and %d insts\n",
                 eachstallSize.size() + 1,
                 stallBuffer.size() + insts_from_fetch);
        eachstallSize.push_back(insts_from_fetch);
        for (int i = 0; i < insts_from_fetch; i++) {
            stallBuffer.push_back(fromFetch->insts[i]);
        }
    }

    DPRINTF(Decode, "Decode stall buffer has %d stalls\n",
            eachstallSize.size());

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (!fixedbuffer[tid].empty()) {
            return;
        }
    }

    ThreadID fill_tid = InvalidThreadID;
    unsigned moved_total = 0;

    auto get_oldest_bypass = [&]() {
        ThreadID bypass_tid = InvalidThreadID;
        InstSeqNum bypass_seq = std::numeric_limits<InstSeqNum>::max();
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            if (!uopCacheBypassQueue[tid].empty() &&
                (fill_tid == InvalidThreadID || tid == fill_tid) &&
                uopCacheBypassQueue[tid].front()->seqNum < bypass_seq) {
                bypass_tid = tid;
                bypass_seq = uopCacheBypassQueue[tid].front()->seqNum;
            }
        }
        return std::make_pair(bypass_tid, bypass_seq);
    };

    auto move_normal_insts = [&](InstSeqNum stop_before_seq) {
        if (stallBuffer.empty()) {
            return 0U;
        }

        ThreadID tid = stallBuffer.front()->threadNumber;
        if (fill_tid != InvalidThreadID && fill_tid != tid) {
            return 0U;
        }

        int group_remaining = eachstallSize.front();
        unsigned moved = 0;
        while (group_remaining > 0 &&
               moved_total < decodeWidth &&
               !fixedbuffer[tid].full() &&
               !stallBuffer.empty() &&
               stallBuffer.front()->seqNum < stop_before_seq) {
            const DynInstPtr &inst = stallBuffer.front();
            assert(tid == inst->threadNumber);
            if (localSquashVer[tid].largerThan(inst->getVersion())) {
                inst->setSquashed();
            }

            assert(!fixedbuffer[inst->threadNumber].full());
            fixedbuffer[inst->threadNumber].push_back(inst);
            stallBuffer.pop_front();
            group_remaining--;
            moved++;
            moved_total++;
            fill_tid = tid;
        }

        if (moved == 0) {
            return 0U;
        }
        if (group_remaining == 0) {
            eachstallSize.pop_front();
        } else {
            eachstallSize.front() = group_remaining;
        }

        return moved;
    };

    while (moved_total < decodeWidth) {
        auto [bypass_tid, bypass_seq] = get_oldest_bypass();
        const bool has_bypass = bypass_tid != InvalidThreadID;
        const bool has_normal =
            !stallBuffer.empty() &&
            (fill_tid == InvalidThreadID ||
             stallBuffer.front()->threadNumber == fill_tid);

        if (!has_bypass && !has_normal) {
            break;
        }

        if (has_bypass &&
            (!has_normal || bypass_seq < stallBuffer.front()->seqNum)) {
            const unsigned before_queue =
                uopCacheBypassQueue[bypass_tid].size();
            const unsigned before_buffer = fixedbuffer[bypass_tid].size();
            const unsigned bypassed =
                moveUopCacheBypassInstsToBuffer(
                    bypass_tid,
                    has_normal ? stallBuffer.front()->seqNum :
                                 std::numeric_limits<InstSeqNum>::max());
            const unsigned after_queue =
                uopCacheBypassQueue[bypass_tid].size();
            const unsigned after_buffer = fixedbuffer[bypass_tid].size();
            if (after_buffer > before_buffer) {
                moved_total += after_buffer - before_buffer;
                fill_tid = bypass_tid;
            }
            if (!bypassed && after_queue == before_queue &&
                after_buffer == before_buffer) {
                break;
            }
            continue;
        }

        const unsigned moved = move_normal_insts(
            has_bypass ? bypass_seq : std::numeric_limits<InstSeqNum>::max());
        if (!moved) {
            break;
        }
    }
}

void
Decode::checkSquash()
{
    for (int i = 0;i < numThreads; i++) {
        if (fromCommit->commitInfo[i].squash) {
            DPRINTF(Decode, "[tid:%i] Squashing instructions due to squash "
                    "from commit.\n", i);
            squash(i);
            localSquashVer[i].update(
                fromCommit->commitInfo[i].squashVersion.getVersion());
            DPRINTF(Decode, "Updating squash version to %u\n",
                    localSquashVer[i].getVersion());
            if (enableUopCache) {
                auto *uop_cache = fetch_ptr ? fetch_ptr->getUopCache() : nullptr;
                assert(uop_cache);
                uop_cache->flushCurUopEntry();
            }
        }
    }
}

void
Decode::tick()
{
    if (enableUopCache) {
        auto *uop_cache = fetch_ptr ? fetch_ptr->getUopCache() : nullptr;
        assert(uop_cache);
        uop_cache->tick();
    }

    toRename->fetchStallReason = fromFetch->fetchStallReason;
    wroteToTimeBuffer = false;
    toRenameIndex = 0;
    blockReason = StallReason::NoStall;
    setAllStalls(StallReason::NoStall);

    moveInstsToBuffer();

    checkSquash();

    // Preserve the established SMT arbiter for both modes.  Uop-cache bypass
    // only changes how a selected thread obtains instructions; it must not
    // replace fairness and borrow-priority policy at the stage boundary.
    ThreadID blocked_tid = InvalidThreadID;
    SmtActiveThreadArbiter active_arbiter;
    auto freeze_active_thread = [this](ThreadID tid) {
        stallSig->blockFetch[tid] = true;
        stallSig->fetchBlockReason[tid] = StallReason::OtherFragStall;
        toFetch->decodeInfo[tid].blockReason =
            stallSig->fetchBlockReason[tid];
    };
    const auto fetch_feedback_reserve =
        numThreads > 1 ? fetchToDecodeDelay : decodeToFetchDelay + 1;
    const bool fifo_backpressured =
        !stallBuffer.empty() &&
        eachstallSize.size() + fetch_feedback_reserve >=
            eachstallSize.capacity();
    const ThreadID fifo_head_tid =
        !stallBuffer.empty() ? stallBuffer.front()->threadNumber :
                               InvalidThreadID;
    const StallReason fifo_block_reason =
        (fifo_backpressured && fifo_head_tid != InvalidThreadID &&
         stallSig->blockDecode[fifo_head_tid]) ?
            stallSig->decodeBlockReason[fifo_head_tid] :
            (fifo_backpressured ? StallReason::OtherFragStall :
                                 StallReason::NoStall);
    for (int i = 0; i < numThreads; i++) {
        bool block = stallSig->blockDecode[i];
        bool active = !block && !fixedbuffer[i].empty();

        if (block) {
            ++stats.smtblockedCycles[i];
        }
        if (!active) {
            ++stats.smtnotactiveCycles[i];
        }

        stallSig->blockFetch[i] = block || fifo_backpressured;
        stallSig->fetchBlockReason[i] =
            stallSig->blockFetch[i] ?
                (block ? stallSig->decodeBlockReason[i] : fifo_block_reason) :
                StallReason::NoStall;
        toFetch->decodeInfo[i].blockReason = stallSig->fetchBlockReason[i];
        if (active) {
            const auto freeze = active_arbiter.observe(
                i, smtBorrowPriority(fromIEW->iewInfo[i]));
            if (freeze.previousActive != InvalidThreadID) {
                freeze_active_thread(freeze.previousActive);
            }
            if (freeze.freezeCurrent) {
                freeze_active_thread(i);
            }
        } else if (block && blocked_tid == InvalidThreadID) {
            blocked_tid = i;
        }
    }
    const ThreadID tid = active_arbiter.selected();
    if (tid == InvalidThreadID) {
        // all threads are stalled, no need to process
        if (blocked_tid != InvalidThreadID) {
            setAllStalls(stallSig->fetchBlockReason[blocked_tid]);
            blockReason = stallSig->fetchBlockReason[blocked_tid];
        }
        toRename->decodeStallReason = decodeStalls;
        updateActivate();
        return;
    }
    DPRINTF(Decode,"Processing [tid:%i]\n",tid);

    decodeInsts(tid);
    ++stats.runCycles;
    if (stallSig->blockDecode[tid]) {
        setAllStalls(stallSig->decodeBlockReason[tid]);
    } else if (toRenameIndex > 0 && decodeStalls[0] == StallReason::NoStall) {
        for (int i = 0; i < decodeStalls.size(); i++) {
            if (i < toRenameIndex) {
                decodeStalls.at(i) = StallReason::NoStall;
            } else {
                decodeStalls.at(i) = fromFetch->fetchStallReason.at(i);
            }
        }
    }
    stallSig->fetchBlockReason[tid] =
        stallSig->blockFetch[tid] ? blockReason : StallReason::NoStall;
    toFetch->decodeInfo[tid].blockReason = stallSig->fetchBlockReason[tid];
    updateActivate();

    // if (stalls[tid].rename) {
    //     // stall from rename, pass rename stall
    //     setAllStalls(fromRename->renameInfo[tid].blockReason);
    // } else if (toRenameIndex == 0) {
    //     if (decodeStalls[0] != StallReason::NoStall) {
    //         setAllStalls(decodeStalls[0]);
    //     } else {
    //         // warn("decode have other Stall Reason!");
    //     }
    // } else {
    //     // no stall from decode, pass fetch stall(no stall/FetchFragStall/fetch all stall)
    //     for (int i = 0; i < decodeStalls.size(); i++) {
    //         if (i < toRenameIndex) {    // decode success, no stall
    //             decodeStalls.at(i) = StallReason::NoStall;
    //         } else {    // no insts to decode, pass fetch frag stall
    //             decodeStalls.at(i) = fromFetch->fetchStallReason.at(i);
    //         }
    //     }
    // }

    toRename->decodeStallReason = decodeStalls;

    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");

        cpu->activityThisCycle();
    }
}

void
Decode::decodeInsts(ThreadID tid)
{
    // Instructions can come either from the skid buffer or the list of
    // instructions coming from fetch, depending on decode's status.
    int insts_available = fixedbuffer[tid].size();

    std::queue<StallReason> decode_stalls;

    StallReason breakDecode = StallReason::NoStall;

    if (insts_available == 0) {
        DPRINTF(Decode, "[tid:%i] Nothing to do, breaking out"
                " early.\n",tid);
        // Should I change the status to idle?
        ++stats.idleCycles;
        ++stats.smtidleCycles[tid];

        StallReason stall = StallReason::NoStall;
        for (auto iter : fromFetch->fetchStallReason) {
            if (iter != StallReason::NoStall) {
                stall = iter;
                break;
            }
        }
        setAllStalls(stall);
        return;
    }

    auto& insts_to_decode = fixedbuffer[tid];
    auto *uop_cache = enableUopCache && fetch_ptr ?
        fetch_ptr->getUopCache() : nullptr;
    if (cpu->isTraceMode()) {
        uop_cache = nullptr;
    }

    DPRINTF(Decode, "[tid:%i] Sending instruction to rename.\n",tid);


    bool vec_decode_limit = false;

    if (!insts_to_decode.front()->isVector()) {
        vec_decode_limit = true;
    }

    std::vector<DynInstPtr> fusionInst;
    while (insts_available > 0 && toRenameIndex < decodeWidth) {
        assert(!insts_to_decode.empty());
        if (vec_decode_limit && insts_to_decode.front()->isVector()) {
            break;
        }

        DynInstPtr inst = std::move(insts_to_decode.front());

        insts_to_decode.pop_front();

        DPRINTF(Decode, "[tid:%i] Processing instruction [sn:%lli] with "
                "PC %s\n", tid, inst->seqNum, inst->pcState());

        if (inst->isSquashed()) {
            DPRINTF(Decode, "[tid:%i] Instruction %i with PC %s is "
                    "squashed, skipping.\n",
                    tid, inst->seqNum, inst->pcState());

            if (enableUopCache) {
                cpu->markInstDecoded(tid, inst->seqNum);
            }
            ++stats.squashedInsts;

            --insts_available;

            decode_stalls.push(StallReason::InstSquashed);

            continue;
        }

        if (enableUopCache) {
            // Fetch's bypass-order trackers cover both sources while the
            // feature is enabled; the legacy path does not need this state.
            inst->setDecoded();
            cpu->markInstDecoded(tid, inst->seqNum);
        }

        // Also check if instructions have no source registers.  Mark
        // them as ready to issue at any time.  Not sure if this check
        // should exist here or at a later stage; however it doesn't matter
        // too much for function correctness.
        if (inst->numSrcRegs() == 0) {
            inst->setCanIssue();
        }

        if (uop_cache) {
            if (inst->isFetchFromUopCache() && uop_cache->isBuildMode()) {
                uop_cache->switchToStreamMode();
            } else if (!inst->isFetchFromUopCache() &&
                       uop_cache->isStreamMode()) {
                uop_cache->switchToBuildMode();
            }
        }

        DynInstPtr uop_cache_refill_inst = inst;

        if (fusionInst.empty()) {
            auto &prev_tail = lastDecodeCycleTail[tid];
            if (prev_tail && !prev_tail->isSquashed() &&
                prev_tail->seqNum + 1 == inst->seqNum) {
                stats.fusionCycleBoundaryPairsChecked++;
                if (wouldFuseInstPair(prev_tail, inst, true)) {
                    stats.fusionCycleBoundaryWouldFuse++;
                }
            }
            prev_tail = nullptr;
        }

        // This current instruction is valid, so add it into the decode
        // queue.  The next instruction may not be valid, so check to
        // see if branches were predicted correctly.
        checkAndFuseInsts(fusionInst, inst);
        fusionInst.push_back(inst);

        ++toRenameIndex;
        ++stats.decodedInsts;
        --insts_available;
        cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtDecode);
#if TRACING_ON
        if (debug::O3PipeView) {
            inst->decodeTick = curTick() - inst->fetchTick;
            // DPRINTF(O3PipeView, "Record decode for inst sn:%lu\n",
            //         inst->seqNum);
        }
#endif

        if (inst->staticInst->isVectorConfig()) {
            inst->setSerializeBefore();
            inst->setSerializeAfter();
            decode_stalls.push(StallReason::SerializeStall);
            breakDecode = StallReason::SerializeStall;
            if (uop_cache) {
                uop_cache->addInst(uop_cache_refill_inst);
                uop_cache->setCurrentUopEntryDone();
            }
            DPRINTF(Decode,
                    "[tid:%i] [sn:%llu] Vector config decoded, set serialize barrier and stop decoding younger "
                    "instructions.\n",
                    tid, inst->seqNum);
            break;
        }

        // Ensure that if it was predicted as a branch, it really is a
        // branch.
        if (inst->readPredTaken() && !inst->isControl()) {
            // panic("Instruction predicted as a branch!");

            ++stats.controlMispred;

            // Might want to set some sort of boolean and just do
            // a check at the end
            selfSquash(inst, inst->threadNumber);

            decode_stalls.push(StallReason::InstMisPred);
            breakDecode = StallReason::InstMisPred;
            if (uop_cache) {
                DPRINTF(UC, "Predicted-taken non-control inst, flush UC refill\n");
                uop_cache->flushCurUopEntry();
            }

            break;
        }

        // Go ahead and compute any PC-relative branches.
        // This includes direct unconditional control and
        // direct conditional control that is predicted taken.
        //
        // 在 trace 模式下，如果 trace 已标记该指令会触发 trap/异常等控制流改变
        //（hasTraceCtrlFlowChange），则交由 trap/wrong-path 逻辑处理，不在 decode
        // 再做一次基于静态分支目标的校验，避免把 cond->trap 误统计为普通分支
        // mispredict，或在这里产生“错误”的 redirect。
        if (!(cpu->isTraceMode() && inst->hasTraceCtrlFlowChange()) &&
            inst->isDirectCtrl() &&
            (inst->isUncondCtrl() || inst->readPredTaken()))
        {
            ++stats.branchResolved;

            std::unique_ptr<PCStateBase> target = inst->branchTarget();
            // In trace mode, prefer ground-truth next PC from trace to avoid
            // relying on possibly out-of-range immediates (e.g., JAL 20-bit).
            if (cpu->isTraceMode() && inst->hasTraceBranchInfo()) {
                auto &t_override = target->as<RiscvISA::PCState>();
                Addr trace_next = inst->traceBranchNextPC();
                if (trace_next != t_override.pc()) {
                    DPRINTF(DecoupleBP,
                            "[tid:%i] [sn:%llu] Branch pc %s, Override target by trace: %s -> npc=%#lx\n",
                            tid, inst->seqNum, inst->pcState(), *target, trace_next);
                    t_override.pc(trace_next);
                    // assuming 4-byte instruction for now since we don't have this trace inst
                    t_override.npc(trace_next + 4);
                    DPRINTF(DecoupleBP,
                            "[tid:%i] [sn:%llu] After override target: %s, inst->branchTarget: %s\n",
                            tid, inst->seqNum, *target, *inst->branchTarget());
                }
            }
            auto &t = target->as<RiscvISA::PCState>();
            auto &pred = inst->readPredTarg().as<RiscvISA::PCState>();
            if (t.start_equals(pred) && !t.equals(pred)) {
                DPRINTF(
                    DecoupleBP,
                    "Override useless npc, from %#lx->%#lx to %#lx->%#lx\n",
                    pred.pc(), pred.npc(), t.pc(), t.npc());
                inst->setPredTarg(t);
            }
            if (*target != inst->readPredTarg()) {
                ++stats.branchMispred;

                RiscvISA::PCState cpTarget = target->clone()->as<RiscvISA::PCState>();
                RiscvISA::PCState cpPredTarget = inst->readPredTarg().clone()->as<RiscvISA::PCState>();

                if (cpTarget.instAddr() != cpPredTarget.instAddr() && cpTarget.npc() == cpPredTarget.npc()) {
                    ++stats.mispredictedByPC;
                } else if (cpTarget.instAddr() == cpPredTarget.instAddr() && cpTarget.npc() != cpPredTarget.npc()) {
                    ++stats.mispredictedByNPC;
                }

                // Might want to set some sort of boolean and just do
                // a check at the end
                selfSquash(inst, inst->threadNumber);

                decode_stalls.push(StallReason::InstMisPred);
                breakDecode = StallReason::InstMisPred;
                if (uop_cache) {
                    DPRINTF(UC, "Direct branch target mismatch, flush UC refill\n");
                    uop_cache->flushCurUopEntry();
                }

                DPRINTF(Decode,
                        "[tid:%i] [sn:%llu] Updating predictions:"
                        " Wrong predicted target: %s PredPC: %s\n",
                        tid, inst->seqNum, inst->readPredTarg(), *target);
                //The micro pc after an instruction level branch should be 0
                inst->setPredTarg(*target);
                break;
            }
        }
        // unpredicted return can make use of ras results to get earlier resteer
        if (inst->isReturn() && !inst->isNonSpeculative() && !inst->readPredTaken()) {
            ++stats.branchMispred;
            decode_stalls.push(StallReason::InstMisPred);
            breakDecode = StallReason::InstMisPred;
            // return target cannot be computed in decode stage since it is an indirect branch
            // need to inquire bpu to get the target
            auto return_addr = fetch_ptr->getPreservedReturnAddr(inst);
            auto target = std::make_unique<RiscvISA::PCState>(return_addr);
            DPRINTF(Decode, "[tid:%i] [sn:%llu] Updating predictions:"
                    " Return not identified by bp: predTaken %d, PredPC: %s Now PC %s\n",
                    tid, inst->seqNum, inst->readPredTaken(), inst->readPredTarg(), *target);
            inst->setPredTaken(true);
            inst->setPredTarg(*target);
            // must squash after setting inst real target because it cannot be computed from static inst
            selfSquash(inst, inst->threadNumber);
            if (uop_cache) {
                DPRINTF(UC, "Unpredicted return, flush UC refill\n");
                uop_cache->flushCurUopEntry();
            }
            break;
        }
        if (inst->isNonSpeculative() && inst->readPredTaken()) {
            // TODO: redirect to fall thru
            std::unique_ptr<PCStateBase> npc(inst->pcState().clone());
            npc->as<RiscvISA::PCState>().set(inst->pcState().getFallThruPC());
            inst->setPredTaken(false);
            inst->setPredTarg(*npc);
        }
        if (uop_cache) {
            uop_cache->addInst(uop_cache_refill_inst);
            if (uop_cache_refill_inst->isQuiesce()) {
                uop_cache->flushCurUopEntry();
            }
            if (uop_cache_refill_inst->readPredTaken()) {
                uop_cache->setCurrentUopEntryDone();
            }
        }
    }
    for (auto &fused_inst : fusionInst) {
        toRename->insts[toRename->size++] = fused_inst;
    }
    if (!fusionInst.empty()) {
        lastDecodeCycleTail[tid] = fusionInst.back();
    }

    if (insts_available) {
        // current cycle insts was not all processed, need to block fetch in next cycle
        stallSig->blockFetch[tid] = true;
        if (breakDecode == StallReason::NoStall) {
            breakDecode = StallReason::OtherFragStall;
        }
    }

    // this stage is totally stalled, set all decode stalls
    if (!decode_stalls.empty()) {
        setAllStalls(decode_stalls.front());
        decode_stalls.pop();
    } else if (breakDecode != StallReason::NoStall) {
        setAllStalls(breakDecode);
    }

    // If we didn't process all instructions, then we will need to block
    // and put all those instructions into the skid buffer.
    if (!insts_to_decode.empty()) {
        blockReason = breakDecode;
    }

    // Record that decode has written to the time buffer for activity
    // tracking.
    if (toRenameIndex) {
        wroteToTimeBuffer = true;
    }
}

bool
Decode::wouldFuseInstPair(const DynInstPtr &first_inst,
                          const DynInstPtr &second_inst,
                          bool enforceSourceBoundary) const
{
    const bool first_from_uop_cache = first_inst->isFetchFromUopCache();
    const bool second_from_uop_cache = second_inst->isFetchFromUopCache();

    if (enforceSourceBoundary) {
        if (first_from_uop_cache != second_from_uop_cache) {
            return false;
        }
    }

    if (first_inst->faulted() || second_inst->faulted()) {
        return false;
    }
    if (!enableLoadFusion && (first_inst->isLoad() || second_inst->isLoad())) {
        return false;
    }
    if (first_inst->getPC() >= ignoreFusionPC &&
        first_inst->getPC() < ignoreFusionPC + 8 &&
        cpu->ticksToCycles(curTick() - lastSetIgnoreTick) <=
            keepIgnoreFusionCycles) {
        return false;
    }

    RiscvISA::FusionKey first_key;
    if (!makeFusionKey(first_inst->staticInst, first_key)) {
        return false;
    }

    auto finder = RiscvISA::fusionMap.find(first_key);
    if (finder == RiscvISA::fusionMap.end()) {
        return false;
    }

    assert(finder->second.index() == 1);
    auto map = std::get<1>(finder->second);
    RiscvISA::FusionKey second_key;
    if (!makeFusionKey(second_inst->staticInst, second_key)) {
        return false;
    }
    finder = map->find(second_key);
    if (finder == map->end()) {
        return false;
    }

    assert(finder->second.index() == 0);
    auto creator = std::get<0>(finder->second);
    const std::vector<DynInstPtr> inst_pair = {first_inst, second_inst};
    return static_cast<bool>(creator(inst_pair));
}

void
Decode::checkAndFuseInsts(std::vector<DynInstPtr> &vec, DynInstPtr& cur)
{
    if (vec.empty()) {
        return;
    }
    const bool first_from_uop_cache = vec.back()->isFetchFromUopCache();
    const bool second_from_uop_cache = cur->isFetchFromUopCache();
    const FusionDiagnosticPair diag_pair =
        classifyDiagnosticFusionPair(vec.back(), cur);
    auto record_reject = [&](FusionRejectReason reason) {
        stats.fusionRejectReasons[reason]++;
        if (diag_pair == FusionDiagSll4Add) {
            stats.fusionSll4AddRejectReasons[reason]++;
        } else if (diag_pair == FusionDiagAddwByte) {
            stats.fusionAddwByteRejectReasons[reason]++;
        }
    };
    auto would_fuse_without_source_boundary = [&]() {
        return wouldFuseInstPair(vec.back(), cur, false);
    };

    stats.fusionPairsChecked++;
    if (first_from_uop_cache && second_from_uop_cache) {
        if (vec.back()->getUopCacheFetchAddr() ==
            cur->getUopCacheFetchAddr()) {
            stats.fusionPairSources[FusionUopUopSameEntry]++;
        } else {
            stats.fusionPairSources[FusionUopUopDifferentEntry]++;
        }
    } else if (first_from_uop_cache) {
        stats.fusionPairSources[FusionUopToNormal]++;
    } else if (second_from_uop_cache) {
        stats.fusionPairSources[FusionNormalToUop]++;
    } else {
        stats.fusionPairSources[FusionNormalNormal]++;
    }

    if (first_from_uop_cache != second_from_uop_cache) {
        if (would_fuse_without_source_boundary()) {
            stats.fusionSourceBoundaryWouldFuse[FusionRejectMixedSource]++;
        }
        record_reject(FusionRejectMixedSource);
        return;
    }
    if (vec.back()->faulted() || cur->faulted()) {
        record_reject(FusionRejectFaulted);
        return;
    }
    if (!enableLoadFusion && (vec.back()->isLoad() || cur->isLoad())) {
        record_reject(FusionRejectLoadFusionDisabled);
        return;
    }
    if (vec.back()->getPC() >= ignoreFusionPC && vec.back()->getPC() < ignoreFusionPC + 8) {
        // ignore fusion for this pc range
        if (cpu->ticksToCycles(curTick() - lastSetIgnoreTick) > keepIgnoreFusionCycles) {
            ignoreFusionPC = 0;
        }
        record_reject(FusionRejectIgnoredPC);
        return;
    }

    // first search
    RiscvISA::FusionKey first_key;
    if (!makeFusionKey(vec.back()->staticInst, first_key)) {
        record_reject(FusionRejectFirstMapMiss);
        return; // no fusion
    }

    auto finder = RiscvISA::fusionMap.find(first_key);
    if (finder == RiscvISA::fusionMap.end()) {
        record_reject(FusionRejectFirstMapMiss);
        return; // no fusion
    }

    // second search
    assert(finder->second.index() == 1);

    auto map = std::get<1>(finder->second);
    RiscvISA::FusionKey second_key;
    if (!makeFusionKey(cur->staticInst, second_key)) {
        record_reject(FusionRejectSecondMapMiss);
        return; // no fusion
    }

    finder = map->find(second_key);
    if (finder == map->end()) {
        record_reject(FusionRejectSecondMapMiss);
        return; // no fusion
    }

    assert(finder->second.index() == 0);
    auto creator = std::get<0>(finder->second);

    const std::vector<DynInstPtr> inst_pair = {vec.back(), cur};
    auto fused_inst = creator(inst_pair);
    if (!fused_inst) {
        record_reject(FusionRejectCreatorRejected);
        return;
    }
    vec.pop_back();

    DynInst::Arrays arrays;
    arrays.numSrcs = fused_inst->numSrcRegs();
    arrays.numDests = fused_inst->numDestRegs();

    // ugly but works for now
    RiscvISA::PCState thispc, predPC;
    thispc.set(inst_pair[0]->getPC());
    thispc.setNPC(inst_pair[1]->getNPC());
    predPC.update(thispc);
    predPC.advance();

    // Create a new DynInst from the instruction fetched.
    DynInstPtr instruction = new (arrays) DynInst(
            arrays, fused_inst, fused_inst, thispc, predPC, inst_pair[0]->seqNum, cpu);


    instruction->setVersion(inst_pair[1]->getVersion());
    instruction->setDecoded();
    instruction->setTid(inst_pair[1]->threadNumber);
    instruction->thread = inst_pair[1]->thread;
    instruction->setFtqId(inst_pair[1]->ftqId);
    if (first_from_uop_cache) {
        instruction->setFetchFromUopCache(true);
        instruction->setUopCacheBypass(
            inst_pair[0]->isUopCacheBypass() &&
            inst_pair[1]->isUopCacheBypass());
        instruction->setUopCacheFetchAddr(inst_pair[0]->getUopCacheFetchAddr());
    }

    auto first_it = inst_pair[0]->getInstListIt();
    auto second_it = inst_pair[1]->getInstListIt();
    instruction->setInstListIt(cpu->instList.insert(first_it, instruction));
    inst_pair[0]->clearInstListIt();
    inst_pair[1]->clearInstListIt();
    cpu->instList.erase(first_it);
    cpu->instList.erase(second_it);

    dynamic_cast<RiscvISA::FusionInst*>(fused_inst.get())->setFusedInst(instruction);

    cur = instruction;
    stats.numFusedInsts++;

    if (fusionType.find(fused_inst->getMnemonic()) == fusionType.end()) {
        fusionType[fused_inst->getMnemonic()] = 1;
    } else {
        fusionType[fused_inst->getMnemonic()]++;
    }
}

void
Decode::setAllStalls(StallReason decodeStall)
{
    for (int i = 0;i < decodeStalls.size();i++) {
        decodeStalls.at(i) = decodeStall;
    }
}

} // namespace o3
} // namespace gem5
