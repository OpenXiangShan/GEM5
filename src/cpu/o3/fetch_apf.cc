#include <algorithm>
#include <cstring>

#include "arch/generic/tlb.hh"
#include "arch/riscv/decoder.hh"
#include "arch/riscv/insts/unknown.hh"
#include "arch/riscv/regs/misc.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/decode.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/fetch.hh"
#include "debug/APF.hh"
#include "debug/Fetch.hh"
#include "params/BaseO3CPU.hh"
#include "params/RiscvDecoder.hh"
#include "sim/system.hh"

namespace gem5::o3
{
using namespace branch_prediction::btb_pred;

APFStats::APFStats(statistics::Group *parent)
    : statistics::Group(parent, "apf"),
      ADD_STAT(candidates, statistics::units::Count::get(), "APF candidates selected"),
      ADD_STAT(started, statistics::units::Count::get(), "Alternate paths started"),
      ADD_STAT(buffered, statistics::units::Count::get(), "Paths moved to saved buffers"),
      ADD_STAT(generatedUops, statistics::units::Count::get(), "Alternate uops generated"),
      ADD_STAT(replayedUops, statistics::units::Count::get(), "Uops delivered to Rename"),
      ADD_STAT(committedUops, statistics::units::Count::get(), "Replayed uops committed"),
      ADD_STAT(recovered, statistics::units::Count::get(), "Paths promoted after redirect"),
      ADD_STAT(recoveredCommitted, statistics::units::Count::get(), "Recovery source branches committed"),
      ADD_STAT(committedConditionalMisses, statistics::units::Count::get(), "Committed conditional mispredictions"),
      ADD_STAT(partialRecoveries, statistics::units::Count::get(), "Recoveries from active prefixes"),
      ADD_STAT(discarded, statistics::units::Count::get(), "Unused paths discarded"),
      ADD_STAT(fullCycles, statistics::units::Cycle::get(), "Active path waiting for a saved buffer"),
      ADD_STAT(ftqWaitCycles, statistics::units::Cycle::get(), "Recovery waiting for FTQ space"),
      ADD_STAT(replayBlockedCycles, statistics::units::Cycle::get(), "Replay blocked at Rename"),
      ADD_STAT(replayBlockedWithRecoveryCycles, statistics::units::Cycle::get(),
               "Replay blocked at Rename while path promotion is incomplete"),
      ADD_STAT(activeCycles, statistics::units::Cycle::get(), "Cycles with an occupied active slot"),
      ADD_STAT(sampledCycles, statistics::units::Cycle::get(), "APF sampling cycles"),
      ADD_STAT(bufferOccupancy, statistics::units::Count::get(), "Sum of saved buffer occupancy"),
      ADD_STAT(bufferIdleCycles, statistics::units::Cycle::get(), "Cycles with no saved paths"),
      ADD_STAT(predictorQueries, statistics::units::Count::get(), "Isolated final-stage predictor queries"),
      ADD_STAT(retiredPaths, statistics::units::Count::get(), "Paths discarded when their source target retires"),
      ADD_STAT(generatedMicroops, statistics::units::Count::get(), "Cached macro-op expansion uops"),
      ADD_STAT(crossPageReads, statistics::units::Count::get(), "Instruction reads crossing 4 KiB boundaries"),
      ADD_STAT(compressedUops, statistics::units::Count::get(), "Cached compressed instructions"),
      ADD_STAT(stops, statistics::units::Count::get(), "Path generation stop reasons"),
      ADD_STAT(fallbacks, statistics::units::Count::get(), "Redirect fallback reasons"),
      ADD_STAT(savedOccupancy, statistics::units::Cycle::get(), "Cycles at each saved-buffer occupancy"),
      ADD_STAT(replayCommitRatio, statistics::units::Ratio::get(),
               "Committed replay uops / delivered replay uops", committedUops / replayedUops),
      ADD_STAT(bufferIdleRatio, statistics::units::Ratio::get(),
               "Saved-buffer empty cycle fraction", bufferIdleCycles / sampledCycles),
      ADD_STAT(recoveryCoverage, statistics::units::Ratio::get(),
               "Committed recovered sources / committed conditional misses",
               recoveredCommitted / committedConditionalMisses),
      ADD_STAT(activeOccupancyRatio, statistics::units::Ratio::get(),
               "Active-slot occupied cycle fraction", activeCycles / sampledCycles),
      ADD_STAT(meanSavedOccupancy, statistics::units::Ratio::get(),
               "Mean number of occupied saved buffers", bufferOccupancy / sampledCycles)
{
    stops.init(5);
    stops.subname(0, "capacity_or_time");
    stops.subname(1, "translation_or_memory");
    stops.subname(2, "unsupported_instruction");
    stops.subname(3, "indirect_branch");
    stops.subname(4, "branch_capacity");
    fallbacks.init(4);
    fallbacks.subname(0, "no_path");
    fallbacks.subname(1, "empty_prefix");
    fallbacks.subname(2, "target_mismatch");
    fallbacks.subname(3, "context_changed");
}

void
Fetch::initAPF(const BaseO3CPUParams &params)
{
    apfEnabled = params.enableAPF;
    if (!apfEnabled)
        return;
    fatal_if(numThreads != 1 || params.enableTraceMode ||
             params.enable_loadFusion || params.enableConstantFolding ||
             valuePred,
             "APF v1 requires single-thread instruction-driven execution "
             "with load fusion, constant folding and value prediction disabled");
    apfBufferEntries = params.apfBufferEntries;
    apfBufferUops = params.apfBufferUops;
    apfWidth = params.apfWidth;
    apfGenerationCycles = params.apfGenerationCycles;
    apfBranchEntries = params.apfBranchEntries;
    apfReplayWidth = params.renameWidth;
    apfFetchLatency = params.apfFetchLatency;
    fatal_if(!apfBufferEntries || !apfBufferUops || !apfWidth ||
             !apfGenerationCycles || !apfBranchEntries ||
             apfFetchLatency < Cycles(1), "APF parameters must be nonzero");
    apfDecoderParams = std::make_unique<RiscvDecoderParams>();
    apfDecoderParams->name = cpu->name() + ".apf_decoder";
    apfDecoderParams->isa = params.isa[0];
    apfDecoderParams->eventq_index = params.eventq_index;
    apfDecoder = std::make_unique<RiscvISA::Decoder>(*apfDecoderParams);
    apfStats = std::make_unique<APFStats>(cpu);
    apfStats->savedOccupancy.init(apfBufferEntries + 1);
    dbpbtb->enableAlternatePaths();
}

bool
Fetch::readAPFBytes(Addr pc, uint32_t &bits)
{
    bits = 0;
    // Read only the bytes of the instruction, including a possible page split.
    for (unsigned offset = 0; offset < 4; offset += 2) {
        auto req = std::make_shared<Request>(pc + offset, 2,
            Request::INST_FETCH, cpu->instRequestorId(), pc,
            cpu->thread[0]->contextId());
        auto fault = cpu->mmu->translateFunctional(
            req, cpu->thread[0]->getTC(), BaseMMU::Execute);
        if (fault != NoFault || req->isUncacheable() ||
            req->isStrictlyOrdered() || req->isLocalAccess() ||
            !cpu->system->isMemAddr(req->getPaddr()) ||
            !cpu->system->isMemAddr(req->getPaddr() + 1))
            return false;
        Packet packet(req, MemCmd::ReadReq);
        uint8_t bytes[2] = {};
        packet.dataStatic(bytes);
        icachePort.sendFunctional(&packet);
        if (packet.isError())
            return false;
        bits |= (uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8)) << (8 * offset);
        if (offset == 0 && (bits & 3) != 3)
            break;
    }
    if ((pc & 4095) == 4094 && (bits & 3) == 3)
        ++apfStats->crossPageReads;
    return true;
}

void
Fetch::generateAPF(AlternatePathContext &path)
{
    if (!path.blockPrediction) {
        path.blockStart = path.predictor;
        path.blockPrediction = dbpbtb->predictAlternate(
            path.blockStart, path.pc.instAddr());
        path.blockFirstUop = path.uops.size();
        path.blockIndex = path.predictions.size();
        path.blockBranches.clear();
        ++apfStats->predictorQueries;
        DPRINTF(APF, "APF query source %lu:%#lx block %u start %#lx\n",
                path.source.ftqId, path.source.branch.pc,
                path.blockIndex, path.pc.instAddr());
    }
    // Work on a copy: trimming the cached prefix must not discard predictions
    // for instructions that will be decoded in a later generation cycle.
    auto prediction = *path.blockPrediction;
    auto &pred = prediction.prediction;
    const Addr blockEnd = pred.getFallThrough(dbpbtb->alternatePredictWidth());
    const auto initialSize = path.uops.size();
    unsigned stop = 0;
    StaticInstPtr macro;
    while (path.uops.size() - initialSize < apfWidth &&
           path.uops.size() < apfBufferUops &&
           path.pc.instAddr() < blockEnd) {
        auto pc = path.pc;
        StaticInstPtr instruction;
        if (macro) {
            instruction = macro->fetchMicroop(pc.microPC());
        } else {
            uint32_t bits;
            if (!readAPFBytes(pc.instAddr(), bits)) {
                stop = 1;
                break;
            }
            bits = htole(bits);
            std::memcpy(apfDecoder->moreBytesPtr(), &bits, sizeof(bits));
            apfDecoder->moreBytes(pc, pc.instAddr());
            instruction = apfDecoder->decode(pc);
            if (instruction && instruction->isMacroop()) {
                macro = instruction;
                instruction = macro->fetchMicroop(pc.microPC());
            }
        }
        if (!instruction || dynamic_cast<RiscvISA::Unknown *>(instruction.get()) ||
            instruction->isVector() ||
            instruction->isVectorConfig() || instruction->isNonSpeculative() ||
            instruction->isSerializing() || instruction->isSquashAfter() ||
            instruction->isSyscall() || instruction->isQuiesce()) {
            stop = 2;
            break;
        }
        if (instruction->isIndirectCtrl() && !instruction->isReturn()) {
            stop = 3;
            break;
        }
        auto next = pc;
        instruction->advancePC(next);
        bool taken = false;
        if (instruction->isControl()) {
            if (path.branches == apfBranchEntries) {
                stop = 4;
                break;
            }
            Addr target = pred.returnTarget;
            if (instruction->isDirectCtrl())
                target = instruction->branchTarget(pc)->instAddr();
            const Addr branchPC = pc.instAddr();
            const auto direction = CondTakens_find(pred.condTakens, branchPC);
            taken = instruction->isUncondCtrl() ||
                (direction != pred.condTakens.end() && direction->second);
            BranchInfo info(pc.instAddr(), target, instruction,
                            pc.compressed() ? 2 : 4);
            path.blockBranches.emplace_back(info);
            ++path.branches;
            if (taken)
                next.set(target);
        }
        path.uops.push_back({instruction, macro, pc, next, taken,
                             path.blockIndex});
        path.pc = next;
        if (macro && instruction->isLastMicroop())
            macro = nullptr;
        if (taken)
            break;
    }
    // Never retain half of a macro-op when the bounded packet ends.
    if (macro) {
        while (path.uops.size() > initialSize && path.uops.back().macroop == macro) {
            path.pc = path.uops.back().pc;
            path.uops.pop_back();
        }
        stop = 2;
    }
    const auto count = path.uops.size() - initialSize;
    if (count) {
        for (auto i = initialSize; i < path.uops.size(); ++i) {
            apfStats->generatedMicroops += path.uops[i].instruction->isMicroop();
            apfStats->compressedUops += path.uops[i].pc.compressed();
        }
        pred.btbEntries = path.blockBranches;
        auto &target = prediction.target;
        const auto retained = [&](Addr pc) {
            return std::any_of(pred.btbEntries.begin(), pred.btbEntries.end(),
                [pc](const auto &branch) { return branch.pc == pc; });
        };
        for (auto *pcs : {&target.h2pTableBranchPCs, &target.h2pBranchPCs,
                          &target.h2pAllocateBranchPCs}) {
            pcs->erase(std::remove_if(pcs->begin(), pcs->end(),
                [&](Addr pc) { return !retained(pc); }), pcs->end());
        }
        target.apfBranches.erase(std::remove_if(target.apfBranches.begin(),
            target.apfBranches.end(), [&](const auto &branch) {
                return !retained(branch.pc);
            }), target.apfBranches.end());
        for (auto &branch : target.apfBranches) {
            branch = *std::find_if(pred.btbEntries.begin(), pred.btbEntries.end(),
                [&](const auto &actual) { return actual.pc == branch.pc; });
        }
        target.setPredictedBranches(pred.btbEntries);
        target.predTaken = path.uops.back().taken;
        target.predBranchInfo = target.predTaken ?
            pred.getTakenEntry().getBranchInfo() : BranchInfo();
        if (target.predTaken)
            target.predBranchInfo.target = path.pc.instAddr();
        target.predEndPC = path.uops.back().pc.getFallThruPC();
        target.fetchInstNum = path.uops.size() - path.blockFirstUop;
        // Rebuild the endpoint from the block's starting history, not the
        // previous cycle's prefix: otherwise older branches shift in twice.
        auto prefixState = path.blockStart;
        dbpbtb->advanceAlternate(prefixState, prediction, path.pc.instAddr());
        path.predictor = std::move(prefixState);
        if (path.blockIndex == path.predictions.size()) {
            path.predictions.push_back(std::move(prediction));
            path.nextPCs.push_back(path.pc.instAddr());
        } else {
            path.predictions[path.blockIndex] = std::move(prediction);
            path.nextPCs[path.blockIndex] = path.pc.instAddr();
        }
        DPRINTF(APF, "APF prefix source %lu:%#lx block %u uops %u "
                "start %#lx end %#lx\n", path.source.ftqId,
                path.source.branch.pc, path.blockIndex,
                path.uops.size() - path.blockFirstUop,
                path.predictions.back().target.startPC, path.pc.instAddr());
        apfStats->generatedUops += count;
    }
    ++path.cycles;
    path.ready = cpu->clockEdge(apfFetchLatency);
    if (!stop && path.branches >= apfBranchEntries)
        stop = 4;
    path.complete = stop || count == 0 || path.uops.size() >= apfBufferUops ||
                    path.cycles >= apfGenerationCycles;
    if (path.complete)
        apfStats->stops[stop]++;
    if (path.complete || path.pc.instAddr() >= blockEnd ||
        (count && path.uops.back().taken)) {
        path.blockPrediction.reset();
        path.blockStart = {};
        path.blockBranches.clear();
    }
    assert(path.uops.size() <= apfBufferUops);
    assert(path.cycles <= apfGenerationCycles);
}

void
Fetch::tickAPF()
{
    if (!apfEnabled)
        return;
    if (apfInvalidated)
        return;
    // Predicted entries need not become executed branches. Their FTQ lifetime
    // bounds the lifetime of an otherwise unresolved alternate path.
    if (apfActive && !dbpbtb->alternateSourceLive(apfActive->source)) {
        apfActive.reset();
        ++apfStats->retiredPaths;
    }
    for (auto it = apfBuffers.begin(); it != apfBuffers.end();) {
        if (!dbpbtb->alternateSourceLive((*it)->source)) {
            it = apfBuffers.erase(it);
            ++apfStats->retiredPaths;
        } else {
            ++it;
        }
    }
    ++apfStats->sampledCycles;
    assert(apfBuffers.size() <= apfBufferEntries);
    apfStats->bufferOccupancy += apfBuffers.size();
    ++apfStats->savedOccupancy[apfBuffers.size()];
    if (apfBuffers.empty())
        ++apfStats->bufferIdleCycles;
    if (apfActive)
        ++apfStats->activeCycles;
    const auto satp = cpu->readMiscRegNoEffect(RiscvISA::MISCREG_SATP, 0);
    const auto privilege = cpu->readMiscRegNoEffect(RiscvISA::MISCREG_PRV, 0);
    const auto changed = [&](const auto &path) {
        return path && (path->addressSpace != satp || path->privilege != privilege);
    };
    if (changed(apfActive) || changed(apfRecovery) ||
        std::any_of(apfBuffers.begin(), apfBuffers.end(), changed)) {
        invalidateAPF();
        ++apfStats->fallbacks[3];
        return;
    }
    // Rename ran before Fetch this cycle. Sample existing replay before
    // promotion can append new uops, including when FTQ promotion must wait.
    if (!apfReplay.empty() && apfReplayTick != curTick()) {
        ++apfStats->replayBlockedCycles;
        if (apfRecovery)
            ++apfStats->replayBlockedWithRecoveryCycles;
    }
    if (apfRecovery) {
        promoteAPF();
        return;
    }
    if (!apfReplay.empty()) {
        return;
    }
    if (apfActive && !apfActive->complete && curTick() >= apfActive->ready)
        generateAPF(*apfActive);
    if (apfActive && apfActive->complete) {
        if (apfActive->uops.empty()) {
            apfActive.reset();
        } else if (apfBuffers.size() < apfBufferEntries) {
            apfBuffers.push_back(std::move(apfActive));
            ++apfStats->buffered;
        } else {
            ++apfStats->fullCycles;
        }
    }
    if (!apfActive && !cpu->isDraining() && !interruptPending && !redirectPending[0] &&
        fetchStatus[0] != Squashing && fetchStatus[0] != TrapPending &&
        fetchStatus[0] != Idle) {
        AlternateCandidate candidate;
        if (dbpbtb->nextAlternateCandidate(candidate)) {
            ++apfStats->candidates;
            apfActive = std::make_unique<AlternatePathContext>();
            auto &path = *apfActive;
            path.source = candidate;
            path.startPC = candidate.predictedTaken ?
                candidate.branch.pc + candidate.branch.size : candidate.branch.target;
            path.pc.set(path.startPC);
            path.addressSpace = satp;
            path.privilege = privilege;
            path.epoch = apfEpoch;
            path.predictor = dbpbtb->startAlternate(candidate);
            path.ready = cpu->clockEdge(apfFetchLatency);
            apfDecoder->reset();
            ++apfStats->started;
        }
    }
}

void
Fetch::resolveAPF(const BranchOutcome &branch)
{
    if (!apfEnabled)
        return;
    dbpbtb->resolveAlternateCandidate(branch);
    if (branch.mispredicted)
        return;
    const auto matches = [&](const auto &path) {
        return path && path->source.ftqId == branch.ftqId &&
            path->source.branch.pc == branch.pc;
    };
    if (matches(apfActive)) {
        apfActive.reset();
        ++apfStats->discarded;
    }
    for (auto it = apfBuffers.begin(); it != apfBuffers.end();) {
        if (matches(*it)) {
            it = apfBuffers.erase(it);
            ++apfStats->discarded;
        } else {
            ++it;
        }
    }
}

void
Fetch::squashAPF(const DynInstPtr &inst, Addr redirect, InstSeqNum seq)
{
    if (!apfEnabled)
        return;
    apfInvalidated = false;
    squashAPFReplay(seq);
    const auto matches = [&](const auto &path) {
        return path && inst && inst->isCondCtrl() &&
            path->epoch == apfEpoch &&
            path->source.ftqId == inst->getFtqId() &&
            path->source.branch.pc == inst->pcState().instAddr();
    };
    if (matches(apfActive))
        apfRecovery = std::move(apfActive);
    for (auto it = apfBuffers.begin(); it != apfBuffers.end();) {
        if (matches(*it)) {
            apfRecovery = std::move(*it);
            it = apfBuffers.erase(it);
        } else {
            ++it;
        }
    }
    ++apfEpoch;
    const auto younger = [&](const auto &path) {
        if (!inst)
            return true;
        return path->source.ftqId > inst->getFtqId() ||
            (path->source.ftqId == inst->getFtqId() &&
             path->source.branch.pc >= inst->pcState().instAddr());
    };
    if (apfActive) {
        if (younger(apfActive))
            apfActive.reset();
        else
            apfActive->epoch = apfEpoch;
    }
    for (auto it = apfBuffers.begin(); it != apfBuffers.end();) {
        if (younger(*it))
            it = apfBuffers.erase(it);
        else {
            (*it)->epoch = apfEpoch;
            ++it;
        }
    }
    if (apfRecovery && apfRecovery->startPC != redirect) {
        ++apfStats->fallbacks[2];
        apfRecovery.reset();
    } else if (apfRecovery && apfRecovery->uops.empty()) {
        ++apfStats->fallbacks[1];
        apfRecovery.reset();
    } else if (!apfRecovery && inst && inst->isCondCtrl()) {
        ++apfStats->fallbacks[0];
    }
    if (apfRecovery) {
        apfRecoverySource = inst;
    }
    dbpbtb->holdForAlternateReplay(apfReplayPending());
}

void
Fetch::promoteAPF()
{
    auto &path = *apfRecovery;
    while (path.promotedPredictions < path.predictions.size()) {
        if (!dbpbtb->canPromoteAlternate(1)) {
            ++apfStats->ftqWaitCycles;
            return;
        }
        const auto index = path.promotedPredictions;
        const auto id = dbpbtb->promoteAlternate(path.predictions[index],
                                               path.nextPCs[index]);
        if (index == 0) {
            ++apfStats->recovered;
            if (!path.complete)
                ++apfStats->partialRecoveries;
            if (apfRecoverySource->isCommitted())
                ++apfStats->recoveredCommitted;
            else
                apfRecoverySource->apfRecoverySource = true;
        }
        while (path.promotedUops < path.uops.size() &&
               path.uops[path.promotedUops].prediction == index) {
            const auto &uop = path.uops[path.promotedUops++];
            DynInst::Arrays arrays;
            arrays.numSrcs = uop.instruction->numSrcRegs();
            arrays.numDests = uop.instruction->numDestRegs();
            auto seq = cpu->getAndIncrementInstSeq();
            DynInstPtr inst = new (arrays) DynInst(arrays, uop.instruction,
                uop.macroop, uop.pc, uop.nextPC, seq, cpu);
            inst->setTid(0);
            inst->setThreadState(cpu->thread[0]);
            inst->setFtqId(id);
            inst->setLoopIteration(0);
            inst->setVersion(localSquashVer[0]);
            inst->setPredTaken(uop.taken);
            inst->setPredTarg(uop.nextPC);
            inst->fallThruPC = uop.pc.getFallThruPC();
            inst->setPredecodeChecked();
            inst->apfReplayed = true;
            if (inst->numSrcRegs() == 0)
                inst->setCanIssue();
            inst->setInstListIt(cpu->addInst(inst));
            cpu->perfCCT->createMeta(inst);
            cpu->perfCCT->updateInstPos(seq, PerfRecord::AtDecode);
            apfReplay.push_back(inst);
            assert(apfReplay.size() <= apfBufferUops);
        }
        ++path.promotedPredictions;
    }
    set(threads[0].fetchpc, path.pc);
    threads[0].valid = false;
    macroop[0] = nullptr;
    decoder[0]->reset();
    DPRINTF(Fetch, "APF restored %u uops, next PC %#lx\n",
            path.uops.size(), path.pc.instAddr());
    apfRecovery.reset();
    apfRecoverySource = nullptr;
}

bool
Fetch::apfReplayPending() const
{
    return apfEnabled && (apfRecovery || !apfReplay.empty());
}

void
Fetch::squashAPFReplay(InstSeqNum seq)
{
    // Rename observes backend squashes before Fetch. Stop delivery at that
    // earlier boundary, or stale replay can modify an already-restored RAT.
    apfRecovery.reset();
    apfRecoverySource = nullptr;
    for (auto it = apfReplay.begin(); it != apfReplay.end();) {
        if ((*it)->seqNum > seq) {
            (*it)->setSquashed();
            it = apfReplay.erase(it);
        } else {
            ++it;
        }
    }
}

DynInstPtr
Fetch::takeAPFReplay(const SquashVersion &version)
{
    if (apfInvalidated || apfReplay.empty() ||
        version.largerThan(apfReplay.front()->getVersion()))
        return nullptr;
    if (apfReplayTick != curTick()) {
        apfReplayTick = curTick();
        apfReplayThisCycle = 0;
    }
    if (apfReplayThisCycle >= apfReplayWidth)
        return nullptr;
    auto inst = apfReplay.front();
    apfReplay.pop_front();
    ++apfReplayThisCycle;
    ++apfStats->replayedUops;
    cpu->getDecode()->recordAPFBranch(inst);
    if (!apfReplayPending())
        dbpbtb->holdForAlternateReplay(false);
    return inst;
}

void
Fetch::commitAPFUop(const DynInstPtr &inst)
{
    if (apfEnabled) {
        if (inst->isCondCtrl() && inst->mispredicted())
            ++apfStats->committedConditionalMisses;
        if (inst->apfReplayed)
            ++apfStats->committedUops;
        if (inst->apfRecoverySource)
            ++apfStats->recoveredCommitted;
    }
}

bool
Fetch::invalidateAPF()
{
    if (!apfEnabled)
        return false;
    // Already promoted uops require an architectural redirect to undo their
    // FTQ/history and restore the restart PC. Do not silently drop that prefix.
    // Delivered replay may already be in Rename/ROB after the replay FIFO
    // emptied. Those uncommitted uops also need the normal squash/RAT path.
    apfInvalidated = apfReplayPending() || std::any_of(
        cpu->instList.begin(), cpu->instList.end(), [](const auto &inst) {
            return inst->apfReplayed && !inst->isCommitted() &&
                !inst->isSquashed();
        });
    apfActive.reset();
    apfBuffers.clear();
    if (!apfInvalidated)
        dbpbtb->holdForAlternateReplay(false);
    return apfInvalidated;
}

void
Fetch::clearAPF()
{
    if (!apfEnabled)
        return;
    apfInvalidated = false;
    ++apfEpoch;
    apfActive.reset();
    apfBuffers.clear();
    apfRecovery.reset();
    apfRecoverySource = nullptr;
    for (auto &inst : apfReplay) {
        if (!inst->isSquashed()) {
            inst->setSquashed();
            cpu->removeFrontInst(inst);
        }
    }
    apfReplay.clear();
    dbpbtb->holdForAlternateReplay(false);
}

} // namespace gem5::o3
