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
      ADD_STAT(started, statistics::units::Count::get(), "Alternate paths started"),
      ADD_STAT(generatedUops, statistics::units::Count::get(), "Alternate uops generated"),
      ADD_STAT(replayedUops, statistics::units::Count::get(), "Uops delivered to Rename"),
      ADD_STAT(committedUops, statistics::units::Count::get(), "Replayed uops committed"),
      ADD_STAT(recovered, statistics::units::Count::get(), "Paths promoted after redirect"),
      ADD_STAT(recoveredCommitted, statistics::units::Count::get(), "Recovery source branches committed"),
      ADD_STAT(fullCycles, statistics::units::Cycle::get(), "Active path waiting for a saved buffer"),
      ADD_STAT(ftqWaitCycles, statistics::units::Cycle::get(), "Recovery waiting for FTQ space"),
      ADD_STAT(replayBlockedCycles, statistics::units::Cycle::get(), "Replay blocked at Rename"),
      ADD_STAT(predictorResourceBlocked, statistics::units::Count::get(),
               "APF predictor queries blocked by modeled resources"),
      ADD_STAT(decodeBlockedCycles, statistics::units::Cycle::get(),
               "Cycles where APF predecode had no Decode slot"),
      ADD_STAT(stops, statistics::units::Count::get(), "Path generation stop reasons"),
      ADD_STAT(fallbacks, statistics::units::Count::get(), "Redirect fallback reasons")
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
             params.enableDecodeFusionCompaction ||
             valuePred,
             "APF v1 requires single-thread instruction-driven execution "
             "with load fusion, constant folding and value prediction disabled");
    apfBufferEntries = params.apfBufferEntries;
    apfBufferUops = params.apfBufferUops;
    apfWidth = params.apfWidth;
    apfShareDecode = params.apfShareDecode;
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
    return true;
}

void
Fetch::generateAPF(AlternatePathContext &path)
{
    const auto finishCycle = [&](unsigned stop = 0, bool done = false) {
        ++path.cycles;
        path.ready = cpu->clockEdge(apfFetchLatency);
        path.complete = done || path.cycles >= apfGenerationCycles;
        if (path.complete)
            ++apfStats->stops[stop];
    };
    if (!path.blockPrediction) {
        path.blockStart = path.predictor;
        auto prediction = dbpbtb->predictAlternate(
            path.blockStart, path.pc.instAddr());
        path.blockFirstUop = path.uops.size();
        path.blockIndex = path.predictions.size();
        path.blockBranches.clear();
        if (prediction.resourceBlocked) {
            ++apfStats->predictorResourceBlocked;
            finishCycle();
            return;
        }
        path.blockPrediction = std::move(prediction);
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
    const unsigned decodeGrant = apfShareDecode ?
        cpu->getDecode()->reserveAPFDecodeSlots(apfWidth) : apfWidth;
    if (!decodeGrant) {
        ++apfStats->decodeBlockedCycles;
        finishCycle();
        return;
    }
    unsigned stop = 0;
    StaticInstPtr macro;
    while (path.uops.size() - initialSize < decodeGrant &&
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
            const Addr branchPC = pc.instAddr();
            const auto predicted = std::find_if(
                pred.btbEntries.begin(), pred.btbEntries.end(),
                [branchPC](const auto &entry) {
                    return entry.valid && entry.branch.pc == branchPC;
                });
            Addr target = predicted != pred.btbEntries.end() ?
                predicted->branch.target : 0;
            if (instruction->isDirectCtrl())
                target = instruction->branchTarget(pc)->instAddr();
            taken = instruction->isUncondCtrl() ||
                (predicted != pred.btbEntries.end() && predicted->taken);
            BranchInfo info(pc.instAddr(), target, instruction,
                            pc.compressed() ? 2 : 4);
            path.blockBranches.emplace_back(info);
            path.blockBranches.back().taken = taken;
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
        pred.btbEntries = path.blockBranches;
        prediction.predEndPC = path.uops.back().pc.getFallThruPC();
        prediction.fetchInstNum = path.uops.size() - path.blockFirstUop;
        // Rebuild the endpoint from the block's starting history, not the
        // previous cycle's prefix: otherwise older branches shift in twice.
        auto prefixState = path.blockStart;
        prediction.nextPC = path.pc.instAddr();
        dbpbtb->advanceAlternate(prefixState, prediction);
        path.predictor = std::move(prefixState);
        if (path.blockIndex == path.predictions.size())
            path.predictions.push_back(std::move(prediction));
        else
            path.predictions[path.blockIndex] = std::move(prediction);
        DPRINTF(APF, "APF prefix source %lu:%#lx block %u uops %u "
                "start %#lx end %#lx\n", path.source.ftqId,
                path.source.branch.pc, path.blockIndex,
                path.uops.size() - path.blockFirstUop,
                path.predictions.back().prediction.bbStart, path.pc.instAddr());
        apfStats->generatedUops += count;
    }
    if (!stop && path.branches >= apfBranchEntries)
        stop = 4;
    finishCycle(stop, stop || count == 0 || path.uops.size() >= apfBufferUops);
    if (path.complete || path.pc.instAddr() >= blockEnd ||
        (count && path.uops.back().taken)) {
        path.blockPrediction.reset();
        path.blockStart = {};
        path.blockBranches.clear();
    }
    if (path.complete)
        path.predictor = {};
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
    }
    for (auto it = apfBuffers.begin(); it != apfBuffers.end();) {
        if (!dbpbtb->alternateSourceLive((*it)->source)) {
            it = apfBuffers.erase(it);
        } else {
            ++it;
        }
    }

    assert(apfBuffers.size() <= apfBufferEntries);
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
    if (!apfReplay.empty() && apfReplayTick != curTick())
        ++apfStats->replayBlockedCycles;
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
        } else {
            ++apfStats->fullCycles;
        }
    }
    if (!apfActive && !cpu->isDraining() && !interruptPending && !redirectPending[0] &&
        fetchStatus[0] != Squashing && fetchStatus[0] != TrapPending &&
        fetchStatus[0] != Idle) {
        AlternateCandidate candidate;
        if (dbpbtb->nextAlternateCandidate(candidate)) {
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
            path->source.branch.pc == branch.branch.pc;
    };
    if (matches(apfActive))
        apfActive.reset();
    for (auto it = apfBuffers.begin(); it != apfBuffers.end();) {
        if (matches(*it)) {
            it = apfBuffers.erase(it);
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
        const auto id = dbpbtb->promoteAlternate(path.predictions[index]);
        if (index == 0) {
            ++apfStats->recovered;
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
