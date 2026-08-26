/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of
 * Sciences
 * All rights reserved.
 *
 * The license is the same as that in register_prefetcher.hh.
 */

#include "cpu/o3/register_prefetcher.hh"

#include <algorithm>
#include <limits>
#include <utility>

#include "arch/riscv/insts/mem.hh"
#include "base/logging.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/o3/lsq.hh"
#include "debug/Rfp.hh"
#include "debug/RfpCancel.hh"
#include "debug/RfpPredictor.hh"
#include "debug/RfpRequest.hh"
#include "debug/RfpValidate.hh"
#include "params/BaseO3CPU.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace o3
{

namespace
{

bool
crossesCacheLine(Addr address, unsigned size, unsigned line_size)
{
    return size == 0 ||
        address / line_size != (address + size - 1) / line_size;
}

} // anonymous namespace

RegisterPrefetcher::RfpStats::RfpStats(statistics::Group *parent)
    : statistics::Group(parent, "rfp"),
      ADD_STAT(lookup, statistics::units::Count::get(),
               "Eligible load predictor lookups"),
      ADD_STAT(tableHit, statistics::units::Count::get(),
               "RFP stride table hits"),
      ADD_STAT(confidentHit, statistics::units::Count::get(),
               "RFP stride predictions above the confidence threshold"),
      ADD_STAT(rejectLowConfidence, statistics::units::Count::get(),
               "RFP lookups rejected for low confidence"),
      ADD_STAT(rejectStride, statistics::units::Count::get(),
               "RFP lookups rejected for an illegal stride"),
      ADD_STAT(rejectPage, statistics::units::Count::get(),
               "RFP lookups rejected by the page policy"),
      ADD_STAT(rejectUnsupported, statistics::units::Count::get(),
               "Loads outside the first-version RFP eligibility contract"),
      ADD_STAT(rejectDuplicate, statistics::units::Count::get(),
               "RFP lookups rejected after the committed sample launched"),
      ADD_STAT(trainFirstSample, statistics::units::Count::get(),
               "RFP predictor first samples"),
      ADD_STAT(trainStrideMatch, statistics::units::Count::get(),
               "Committed strides matching the prediction"),
      ADD_STAT(trainStrideChange, statistics::units::Count::get(),
               "Committed loads changing the stored stride"),
      ADD_STAT(trainConfidenceInc, statistics::units::Count::get(),
               "RFP confidence increments"),
      ADD_STAT(trainConfidenceDec, statistics::units::Count::get(),
               "RFP confidence decrements"),
      ADD_STAT(entryEvict, statistics::units::Count::get(),
               "RFP stride table evictions"),
      ADD_STAT(launchQueued, statistics::units::Count::get(),
               "RFP candidates queued after rename"),
      ADD_STAT(launchDroppedQueueFull, statistics::units::Count::get(),
               "RFP candidates dropped because the launch queue is full"),
      ADD_STAT(translationStarted, statistics::units::Count::get(),
               "RFP timing translations started"),
      ADD_STAT(translationHit, statistics::units::Count::get(),
               "RFP timing translations completed without delay"),
      ADD_STAT(translationDelayed, statistics::units::Count::get(),
               "RFP timing translations delayed"),
      ADD_STAT(translationFault, statistics::units::Count::get(),
               "RFP translations discarded on a fault"),
      ADD_STAT(admissionAttempt, statistics::units::Count::get(),
               "RFP DCache admission attempts"),
      ADD_STAT(admissionAccepted, statistics::units::Count::get(),
               "RFP DCache admissions accepted"),
      ADD_STAT(rejectDemandPriority, statistics::units::Count::get(),
               "RFP admissions deferred behind demand or inflight limits"),
      ADD_STAT(rejectPortBusy, statistics::units::Count::get(),
               "RFP admissions rejected by load-port quota"),
      ADD_STAT(rejectBankConflict, statistics::units::Count::get(),
               "RFP admissions rejected by a DCache bank conflict"),
      ADD_STAT(rejectCacheBlocked, statistics::units::Count::get(),
               "RFP admissions rejected because the DCache is blocked"),
      ADD_STAT(rejectMshrArb, statistics::units::Count::get(),
               "RFP admissions rejected by MSHR arbitration"),
      ADD_STAT(rejectMshrAlias, statistics::units::Count::get(),
               "RFP admissions rejected by an MSHR alias"),
      ADD_STAT(rejectTagRead, statistics::units::Count::get(),
               "RFP admissions rejected by tag-read arbitration"),
      ADD_STAT(retryCount, statistics::units::Count::get(),
               "RFP DCache admission retries"),
      ADD_STAT(retryDropped, statistics::units::Count::get(),
               "RFP candidates dropped after exhausting retries"),
      ADD_STAT(inflightOccupancy, statistics::units::Count::get(),
               "Average RFP packet inflight occupancy"),
      ADD_STAT(responseReceived, statistics::units::Count::get(),
               "RFP DCache responses received"),
      ADD_STAT(responseError, statistics::units::Count::get(),
               "RFP DCache error responses"),
      ADD_STAT(responseInvalidate, statistics::units::Count::get(),
               "RFP responses carrying an invalidate"),
      ADD_STAT(responsePublishFault, statistics::units::Count::get(),
               "RFP values rejected by ISA speculative publication"),
      ADD_STAT(responseOrphaned, statistics::units::Count::get(),
               "Late RFP responses discarded after invalidation"),
      ADD_STAT(localWriteInvalidate, statistics::units::Count::get(),
               "RFP candidates invalidated by same-core cache-line writes"),
      ADD_STAT(specWake, statistics::units::Count::get(),
               "RFP speculative producer wakeups"),
      ADD_STAT(consumerWait, statistics::units::Count::get(),
               "RFP issue-gate checks waiting within the configured window"),
      ADD_STAT(consumerEarlyCancel, statistics::units::Count::get(),
               "Consumers canceled before RFP data became usable"),
      ADD_STAT(consumerIssuedWithData, statistics::units::Count::get(),
               "Consumers passing the RFP issue gate"),
      ADD_STAT(cancelNoData, statistics::units::Count::get(),
               "RFP cancellations because response data is unavailable"),
      ADD_STAT(cancelValidationPending, statistics::units::Count::get(),
               "RFP cancellations while validation is pending"),
      ADD_STAT(cancelTokenMismatch, statistics::units::Count::get(),
               "RFP cancellations because identity tokens mismatch"),
      ADD_STAT(cancelAddressMismatch, statistics::units::Count::get(),
               "RFP cancellations because the predicted address mismatches"),
      ADD_STAT(cancelGenerationMismatch, statistics::units::Count::get(),
               "RFP cancellations after address-space generation changes"),
      ADD_STAT(cancelOrderingConflict, statistics::units::Count::get(),
               "RFP cancellations due to load ordering"),
      ADD_STAT(issuedConsumerSquashFallback, statistics::units::Count::get(),
               "RFP recovery attempts finding an already-issued consumer"),
      ADD_STAT(validationPass, statistics::units::Count::get(),
               "RFP candidates passing complete validation"),
      ADD_STAT(validationFailVa, statistics::units::Count::get(),
               "RFP validation failures due to virtual address"),
      ADD_STAT(validationFailPa, statistics::units::Count::get(),
               "RFP validation failures due to physical address"),
      ADD_STAT(validationFailFlags, statistics::units::Count::get(),
               "RFP validation failures due to access attributes"),
      ADD_STAT(validationFailForwarding, statistics::units::Count::get(),
               "RFP validation failures due to store forwarding"),
      ADD_STAT(validationFailMdp, statistics::units::Count::get(),
               "RFP validation failures due to MDP ordering"),
      ADD_STAT(validationFailNuke, statistics::units::Count::get(),
               "RFP validation failures due to a same-cycle nuke"),
      ADD_STAT(validationFailRarRaw, statistics::units::Count::get(),
               "RFP validation failures due to RAR/RAW tracking"),
      ADD_STAT(reused, statistics::units::Count::get(),
               "Loads completed from an RFP candidate"),
      ADD_STAT(fallbackNormal, statistics::units::Count::get(),
               "RFP candidates falling back to the normal load path"),
      ADD_STAT(duplicateDemandAvoided, statistics::units::Count::get(),
               "Demand DCache reads avoided by RFP reuse"),
      ADD_STAT(latencyLookupToAdmission, statistics::units::Cycle::get(),
               "Cycles from RFP lookup to DCache admission"),
      ADD_STAT(latencyAdmissionToResponse, statistics::units::Cycle::get(),
               "Cycles from RFP DCache admission to response"),
      ADD_STAT(latencyResponseToReuse, statistics::units::Cycle::get(),
               "Cycles from RFP response to load reuse"),
      ADD_STAT(latencyRenameToConsumerUse, statistics::units::Cycle::get(),
               "Cycles from RFP producer rename to consumer issue")
{
    latencyLookupToAdmission.init(0, 255, 8);
    latencyAdmissionToResponse.init(0, 1023, 16);
    latencyResponseToReuse.init(0, 255, 8);
    latencyRenameToConsumerUse.init(0, 255, 8);
}

RegisterPrefetcher::RfpRequest::RfpRequest(
    RegisterPrefetcher *owner, uint64_t serial)
    : _owner(owner), _serial(serial)
{
}

void
RegisterPrefetcher::RfpRequest::markDelayed()
{
    _owner->markTranslationDelayed(_serial);
}

void
RegisterPrefetcher::RfpRequest::finish(
    const Fault &fault, const RequestPtr &req, gem5::ThreadContext *tc,
    BaseMMU::Mode mode)
{
    _owner->finishTranslation(_serial, fault, req);
}

bool
RegisterPrefetcher::RfpRequest::squashed() const
{
    return _owner->requestSquashed(_serial);
}

RegisterPrefetcher::RegisterPrefetcher(
    CPU *cpu_ptr, const BaseO3CPUParams &params)
    : cpu(cpu_ptr), enable(params.enableRegisterPrefetch),
      issueWidth(params.rpfIssueWidth),
      launchQueueEntries(params.rpfLaunchQueueEntries),
      maxInflight(params.rpfMaxInflight),
      perThreadMaxInflight(params.rpfPerThreadMaxInflight),
      demandPriority(params.rpfDemandPriority),
      dropOnPressure(params.rpfDropOnPressure),
      maxRetryCycles(params.rpfMaxRetryCycles),
      reuseMaxWaitCycles(params.rpfReuseMaxWaitCycles),
      enableDebugTrace(params.rpfEnableDebugTrace),
      perThreadInflight(params.numThreads, 0),
      generations(params.numThreads, 1), stats(cpu_ptr)
{
    panic_if(params.rpfTableEntries == 0,
             "rpfTableEntries must be greater than zero");
    panic_if(params.rpfAssociativity == 0 ||
             params.rpfTableEntries % params.rpfAssociativity != 0,
             "rpfTableEntries must be divisible by rpfAssociativity");
    panic_if(params.rpfMaxInflight < params.rpfPerThreadMaxInflight,
             "rpfMaxInflight must be at least rpfPerThreadMaxInflight");
    panic_if(params.rpfConfidenceBits == 0 ||
                 params.rpfConfidenceBits > 8,
             "rpfConfidenceBits must be in [1, 8]");
    panic_if(params.rpfConfidenceThreshold >=
                 (1U << params.rpfConfidenceBits),
             "rpfConfidenceThreshold must fit rpfConfidenceBits");
    panic_if(!demandPriority,
             "This RFP model requires rpfDemandPriority=true");
    panic_if(params.rpfCancelIssuedConsumerPolicy != "SquashFallback",
             "Only the SquashFallback RFP recovery policy is supported");

    if (!enable) {
        return;
    }

    predictors.reserve(params.numThreads);
    for (ThreadID tid = 0; tid < params.numThreads; ++tid) {
        predictors.emplace_back(std::make_unique<RfpStrideTable>(
            params.rpfTableEntries, params.rpfAssociativity,
            params.rpfConfidenceBits, params.rpfConfidenceThreshold,
            params.rpfMaxStrideBytes, params.rpfRequireSamePage));
    }
}

bool
RegisterPrefetcher::eligible(const DynInstPtr &inst, unsigned *size,
                             Request::Flags *flags) const
{
    if (!inst || !inst->isLoad() || inst->isVector() || inst->isAtomic() ||
        inst->isLoadReserved() || inst->isDataPrefetch() ||
        inst->isInstPrefetch() || inst->inHtmTransactionalState() ||
        inst->staticInst->isFusion() || inst->numDestRegs() != 1 ||
        inst->renamedDestIdx(0)->isFixedMapping() ||
        inst->vpApplied || inst->vpResult.speculative) {
        return false;
    }

    const auto *mem_inst =
        dynamic_cast<const RiscvISA::MemInst *>(inst->staticInst.get());
    const int width = inst->operWid();
    if (!mem_inst || width <= 0 || width % 8 != 0) {
        return false;
    }

    *size = static_cast<unsigned>(width / 8);
    if (*size == 0 || *size > MaxRfpBytes) {
        return false;
    }

    *flags = mem_inst->getMemAccessFlags();
    constexpr Request::FlagsType unsupported =
        Request::UNCACHEABLE | Request::STRICT_ORDER | Request::NO_ACCESS |
        Request::LOCKED_RMW | Request::LLSC | Request::MEM_SWAP |
        Request::MEM_SWAP_COND | Request::READ_MODIFY_WRITE |
        Request::ATOMIC_RETURN_OP | Request::ATOMIC_NO_RETURN_OP |
        Request::HTM_CMD;
    return flags->noneSet(unsupported);
}

void
RegisterPrefetcher::recordLookupReject(
    RfpStrideTable::RejectReason reason)
{
    switch (reason) {
      case RfpStrideTable::RejectReason::LowConfidence:
        ++stats.rejectLowConfidence;
        break;
      case RfpStrideTable::RejectReason::CrossPage:
        ++stats.rejectPage;
        break;
      case RfpStrideTable::RejectReason::ZeroStride:
      case RfpStrideTable::RejectReason::StrideRange:
      case RfpStrideTable::RejectReason::AddressOverflow:
        ++stats.rejectStride;
        break;
      default:
        break;
    }
}

void
RegisterPrefetcher::onRenamedInstruction(const DynInstPtr &inst)
{
    if (!enable || !inst) {
        return;
    }

    // A physical register can be recycled after the prior defining
    // instruction retires or is squashed.  Invalidate an old RFP binding for
    // every new definition, including non-loads and loads that are not RFP
    // eligible, before considering a new candidate.
    for (int i = 0; i < inst->numDestRegs(); ++i) {
        const auto destination = inst->renamedDestIdx(i);
        if (destination->isFixedMapping()) {
            continue;
        }
        auto old_owner = pregOwners.find(destination->flatIndex());
        if (old_owner != pregOwners.end()) {
            if (auto *old = findCandidate(old_owner->second)) {
                discardCandidate(*old, FailureReason::TokenMismatch, false);
            } else {
                pregOwners.erase(old_owner);
            }
        }
    }

    unsigned size = 0;
    Request::Flags flags;
    if (!eligible(inst, &size, &flags)) {
        if (inst && inst->isLoad()) {
            ++stats.rejectUnsupported;
        }
        return;
    }
    if (issueWidth == 0) {
        return;
    }

    ++stats.lookup;
    const ThreadID tid = inst->threadNumber;
    auto result = predictors.at(tid)->lookup(
        inst->pcState().instAddr(), generations.at(tid), curTick());
    if (result.tableHit) {
        ++stats.tableHit;
    }
    if (!result.prediction) {
        recordLookupReject(result.reject);
        return;
    }
    ++stats.confidentHit;

    // A committed-only table does not advance its address on rename. Multiple
    // in-flight instances of one static load would therefore request the same
    // next address until an instance commits. Keep one active token per PC so
    // duplicate predictions cannot consume translation and cache bandwidth.
    for (const auto &[serial, active] : candidates) {
        const bool terminal = active->state == State::Reused ||
            active->state == State::FallbackNormal ||
            active->state == State::Discarded;
        if (!terminal && active->tid == tid &&
            active->pc == inst->pcState().instAddr()) {
            ++stats.rejectDuplicate;
            return;
        }
    }

    const Addr predicted = result.prediction->address;
    if (predicted % size != 0 ||
        crossesCacheLine(predicted, size, cpu->cacheLineSize())) {
        ++stats.rejectUnsupported;
        return;
    }

    unsigned launch_occupancy = 0;
    for (const auto &[serial, candidate] : candidates) {
        if (!candidate->packetInflight &&
            candidate->state != State::ResponseReady &&
            candidate->state != State::AwaitingValidation) {
            ++launch_occupancy;
        }
    }
    if (launch_occupancy >= launchQueueEntries ||
        candidates.size() >= launchQueueEntries + maxInflight) {
        ++stats.launchDroppedQueueFull;
        return;
    }
    if (!predictors.at(tid)->claimPrediction(
            inst->pcState().instAddr(), generations.at(tid),
            result.prediction->version)) {
        ++stats.rejectDuplicate;
        return;
    }

    const RegIndex destination = inst->renamedDestIdx(0)->flatIndex();
    auto candidate = std::make_unique<Candidate>();
    candidate->serial = nextSerial++;
    candidate->tid = tid;
    candidate->contextId = inst->contextId();
    candidate->seqNum = inst->seqNum;
    candidate->destinationFlatIdx = destination;
    candidate->predictorVersion = result.prediction->version;
    candidate->producer = inst;
    candidate->pc = inst->pcState().instAddr();
    candidate->predictedVa = predicted;
    candidate->size = size;
    candidate->originalFlags = flags;
    candidate->generation = generations.at(tid);
    candidate->lookupTick = curTick();

    const uint64_t serial = candidate->serial;
    candidates.emplace(serial, std::move(candidate));
    pregOwners[destination] = serial;
    launchQueue.push_back(serial);

    inst->rfpTokenSerial = serial;
    inst->rfpUseState = DynInst::RfpUseState::Offered;
    inst->rfpRenameTick = curTick();
    ++stats.launchQueued;
    DPRINTF(RfpPredictor,
            "[tid:%u] [sn:%llu] token:%llu pc:%#lx predicts va:%#lx\n",
            tid, inst->seqNum, serial, inst->pcState().instAddr(),
            predicted);
}

void
RegisterPrefetcher::trainCommittedLoad(const DynInstPtr &inst)
{
    if (!enable || !inst || inst->threadNumber >= predictors.size()) {
        return;
    }

    unsigned size = 0;
    Request::Flags flags;
    if (!eligible(inst, &size, &flags) || inst->isSquashed() ||
        inst->getFault() != NoFault || !inst->effAddrValid() ||
        !inst->isNormalLd() || !inst->readPredicate()) {
        return;
    }

    const ThreadID tid = inst->threadNumber;
    auto result = predictors[tid]->train(
        inst->pcState().instAddr(), inst->effAddr, generations[tid],
        inst->seqNum, curTick());
    stats.trainFirstSample += result.firstSample;
    stats.trainStrideMatch += result.strideMatch;
    stats.trainStrideChange += result.strideChange;
    stats.trainConfidenceInc += result.confidenceInc;
    stats.trainConfidenceDec += result.confidenceDec;
    stats.entryEvict += result.entryEvict;

    DPRINTF(RfpPredictor,
            "[tid:%u] [sn:%llu] train pc:%#lx va:%#lx first:%d "
            "match:%d change:%d\n",
            tid, inst->seqNum, inst->pcState().instAddr(), inst->effAddr,
            result.firstSample, result.strideMatch, result.strideChange);
}

RegisterPrefetcher::Candidate *
RegisterPrefetcher::findCandidate(uint64_t serial)
{
    auto it = candidates.find(serial);
    return it == candidates.end() ? nullptr : it->second.get();
}

const RegisterPrefetcher::Candidate *
RegisterPrefetcher::findCandidate(uint64_t serial) const
{
    auto it = candidates.find(serial);
    return it == candidates.end() ? nullptr : it->second.get();
}

RegisterPrefetcher::Candidate *
RegisterPrefetcher::findCandidate(const DynInstPtr &inst)
{
    if (!inst || inst->rfpTokenSerial == 0) {
        return nullptr;
    }
    auto *candidate = findCandidate(inst->rfpTokenSerial);
    return candidate && validateIdentity(*candidate, inst) ? candidate :
                                                             nullptr;
}

const RegisterPrefetcher::Candidate *
RegisterPrefetcher::findCandidate(const DynInstPtr &inst) const
{
    if (!inst || inst->rfpTokenSerial == 0) {
        return nullptr;
    }
    const auto *candidate = findCandidate(inst->rfpTokenSerial);
    return candidate && validateIdentity(*candidate, inst) ? candidate :
                                                             nullptr;
}

bool
RegisterPrefetcher::validateIdentity(
    const Candidate &candidate, const DynInstPtr &inst) const
{
    return inst && candidate.serial == inst->rfpTokenSerial &&
        candidate.tid == inst->threadNumber &&
        candidate.contextId == inst->contextId() &&
        candidate.seqNum == inst->seqNum &&
        candidate.pc == inst->pcState().instAddr() &&
        inst->numDestRegs() == 1 &&
        candidate.destinationFlatIdx ==
            inst->renamedDestIdx(0)->flatIndex();
}

void
RegisterPrefetcher::startTranslation(Candidate &candidate)
{
    if (candidate.orphaned || candidate.producer->isSquashed() ||
        candidate.generation != generations[candidate.tid]) {
        discardCandidate(candidate, FailureReason::Generation, false);
        return;
    }

    candidate.request = std::make_shared<Request>(
        candidate.predictedVa, candidate.size, candidate.originalFlags,
        candidate.producer->requestorId(), candidate.pc,
        candidate.contextId);
    candidate.request->setByteEnable(
        std::vector<bool>(candidate.size, true));
    candidate.request->setReqInstSeqNum(candidate.seqNum);
    candidate.request->taskId(cpu->taskId());
    candidate.request->setXsMetadata(
        Request::XsMetadata(candidate.producer->xsMeta));
    candidate.senderState =
        std::make_unique<RfpRequest>(this, candidate.serial);
    candidate.state = State::Translating;
    candidate.translationOutstanding = true;
    ++stats.translationStarted;

    DPRINTF(RfpRequest,
            "[tid:%u] [sn:%llu] token:%llu translate va:%#lx size:%u\n",
            candidate.tid, candidate.seqNum, candidate.serial,
            candidate.predictedVa, candidate.size);
    cpu->mmu->translateTiming(
        candidate.request, cpu->tcBase(candidate.tid),
        candidate.senderState.get(), BaseMMU::Read);
}

void
RegisterPrefetcher::markTranslationDelayed(uint64_t serial)
{
    auto *candidate = findCandidate(serial);
    if (!candidate || !candidate->translationOutstanding) {
        return;
    }
    candidate->translationDelayed = true;
    ++stats.translationDelayed;
    DPRINTF(RfpRequest, "token:%llu translation delayed\n", serial);
}

void
RegisterPrefetcher::finishTranslation(
    uint64_t serial, const Fault &fault, const RequestPtr &req)
{
    auto *candidate = findCandidate(serial);
    if (!candidate) {
        return;
    }
    candidate->translationOutstanding = false;

    if (candidate->orphaned || candidate->producer->isSquashed() ||
        candidate->generation != generations[candidate->tid]) {
        discardCandidate(*candidate, FailureReason::Generation, false);
    } else if (fault != NoFault || !req || !req->hasPaddr()) {
        ++stats.translationFault;
        discardCandidate(*candidate, FailureReason::TranslationFault, false);
    } else if (req->isUncacheable() || req->isStrictlyOrdered() ||
               req->isLLSC() || req->isAtomic() || req->isMemMgmt()) {
        discardCandidate(*candidate, FailureReason::TranslationInvalid,
                         false);
    } else {
        candidate->translatedPa = req->getPaddr();
        candidate->state = State::CacheQueued;
        if (!candidate->translationDelayed) {
            ++stats.translationHit;
        }
        DPRINTF(RfpRequest,
                "[tid:%u] [sn:%llu] token:%llu translated pa:%#lx\n",
                candidate->tid, candidate->seqNum, candidate->serial,
                candidate->translatedPa);
    }
    cpu->wakeCPU();
    cpu->activityThisCycle();
}

bool
RegisterPrefetcher::requestSquashed(uint64_t serial) const
{
    const auto *candidate = findCandidate(serial);
    return !candidate || candidate->orphaned ||
        candidate->producer->isSquashed() ||
        candidate->generation != generations[candidate->tid];
}

uint64_t
RegisterPrefetcher::cyclesBetween(Tick start, Tick end) const
{
    return static_cast<uint64_t>(cpu->ticksToCycles(end - start));
}

bool
RegisterPrefetcher::sendCandidate(Candidate &candidate)
{
    if (candidate.state != State::CacheQueued) {
        return false;
    }
    if (inflight >= maxInflight ||
        perThreadInflight[candidate.tid] >= perThreadMaxInflight) {
        ++stats.rejectDemandPriority;
        return false;
    }

    if (!candidate.packet) {
        candidate.packet = new Packet(candidate.request, MemCmd::ReadReq);
        candidate.packet->dataStatic(candidate.data.data());
        candidate.packet->senderState = candidate.senderState.get();
    }

    ++stats.admissionAttempt;
    const auto result = lsq->trySendRfpPacket(
        candidate.packet, candidate.predictedVa, candidate.size);
    if (result == LSQ::RfpDcacheSendResult::Accepted) {
        candidate.state = State::Inflight;
        candidate.packetInflight = true;
        candidate.admissionTick = curTick();
        ++inflight;
        ++perThreadInflight[candidate.tid];
        ++stats.admissionAccepted;
        stats.latencyLookupToAdmission.sample(
            cyclesBetween(candidate.lookupTick, curTick()));
        scheduler->specWakeUpFromRFP(candidate.producer);
        candidate.specWoken = true;
        candidate.producer->rfpUseState = DynInst::RfpUseState::Launched;
        ++stats.specWake;
        DPRINTF(RfpRequest,
                "[tid:%u] [sn:%llu] token:%llu admitted va:%#lx pa:%#lx\n",
                candidate.tid, candidate.seqNum, candidate.serial,
                candidate.predictedVa, candidate.translatedPa);
        return true;
    }

    switch (result) {
      case LSQ::RfpDcacheSendResult::CacheBlocked:
        ++stats.rejectCacheBlocked;
        break;
      case LSQ::RfpDcacheSendResult::PortBusy:
        ++stats.rejectPortBusy;
        break;
      case LSQ::RfpDcacheSendResult::BankConflict:
        ++stats.rejectBankConflict;
        break;
      case LSQ::RfpDcacheSendResult::MshrArbFail:
        ++stats.rejectMshrArb;
        break;
      case LSQ::RfpDcacheSendResult::MshrAliasFail:
        ++stats.rejectMshrAlias;
        break;
      case LSQ::RfpDcacheSendResult::TagReadFail:
        ++stats.rejectTagRead;
        break;
      case LSQ::RfpDcacheSendResult::HitInWriteBuffer:
        ++stats.rejectCacheBlocked;
        break;
      default:
        break;
    }

    ++candidate.retryCycles;
    ++stats.retryCount;
    delete candidate.packet;
    candidate.packet = nullptr;
    if (dropOnPressure && candidate.retryCycles > maxRetryCycles) {
        ++stats.retryDropped;
        discardCandidate(candidate, FailureReason::RetryLimit, false);
    }
    return false;
}

void
RegisterPrefetcher::tick()
{
    if (!enable) {
        return;
    }

    stats.inflightOccupancy = inflight;
    cleanupTerminalCandidates();

    unsigned translations = 0;
    while (translations < issueWidth && !launchQueue.empty()) {
        const uint64_t serial = launchQueue.front();
        launchQueue.pop_front();
        auto *candidate = findCandidate(serial);
        if (!candidate || candidate->state != State::LaunchQueued) {
            continue;
        }
        startTranslation(*candidate);
        ++translations;
    }

    std::vector<Candidate *> ready;
    ready.reserve(candidates.size());
    for (auto &[serial, candidate] : candidates) {
        if (candidate->state == State::CacheQueued) {
            ready.push_back(candidate.get());
        }
    }
    std::sort(ready.begin(), ready.end(),
              [](const Candidate *lhs, const Candidate *rhs) {
                  return lhs->seqNum < rhs->seqNum;
              });

    unsigned issued = 0;
    for (auto *candidate : ready) {
        if (issued >= issueWidth) {
            break;
        }
        if (sendCandidate(*candidate)) {
            ++issued;
        }
    }

    if (enableDebugTrace) {
        checkInvariants();
    }

    if (!candidates.empty()) {
        cpu->activityThisCycle();
    }
}

void
RegisterPrefetcher::recvReqRetry()
{
    if (enable && !candidates.empty()) {
        cpu->wakeCPU();
        cpu->activityThisCycle();
    }
}

void
RegisterPrefetcher::recvTimingResp(PacketPtr pkt, RfpRequest &request)
{
    auto *candidate = findCandidate(request.serial());
    panic_if(!candidate || request.owner() != this,
             "RFP response has an unknown sender state");
    panic_if(!candidate->packetInflight || candidate->packet != pkt,
             "RFP response does not match its inflight candidate");

    candidate->packetInflight = false;
    // Candidate cleanup owns unsent packets, while the cache response path
    // owns this returned packet.  Detach it before any failure cleanup so the
    // same Packet cannot be deleted by both paths.
    candidate->packet = nullptr;
    assert(inflight > 0);
    assert(perThreadInflight[candidate->tid] > 0);
    --inflight;
    --perThreadInflight[candidate->tid];
    ++stats.responseReceived;

    const bool orphaned = candidate->orphaned ||
        candidate->state != State::Inflight ||
        candidate->producer->isSquashed() ||
        candidate->generation != generations[candidate->tid];
    if (pkt->isInvalidate()) {
        invalidateLine(pkt->getAddr(), candidate->serial);
    }
    if (orphaned) {
        ++stats.responseOrphaned;
        discardCandidate(*candidate, FailureReason::Squashed, false);
    } else if (pkt->isError()) {
        ++stats.responseError;
        discardCandidate(*candidate, FailureReason::ResponseError, true);
    } else if (pkt->isInvalidate()) {
        ++stats.responseInvalidate;
        discardCandidate(*candidate, FailureReason::ResponseInvalidate,
                         true);
    } else if (!pkt->hasData() || pkt->getSize() != candidate->size ||
               pkt->getAddr() != candidate->translatedPa) {
        discardCandidate(*candidate, FailureReason::ResponseMalformed,
                         true);
    } else {
        std::copy_n(pkt->getConstPtr<uint8_t>(), candidate->size,
                    candidate->data.begin());
        candidate->responseHasData = true;
        candidate->responseTick = curTick();
        stats.latencyAdmissionToResponse.sample(
            cyclesBetween(candidate->admissionTick, curTick()));

        const Fault publish_fault =
            candidate->producer->publishRfpValue(pkt);
        if (publish_fault != NoFault) {
            ++stats.responsePublishFault;
            discardCandidate(*candidate, FailureReason::PublishFault, true);
        } else {
            candidate->state = State::ResponseReady;
            candidate->producer->rfpDataPublished = true;
            candidate->producer->rfpUseState =
                DynInst::RfpUseState::DataReady;
            scheduler->rfpDataReady(candidate->producer);
            DPRINTF(RfpRequest,
                    "[tid:%u] [sn:%llu] token:%llu response pa:%#lx "
                    "published preg:%u\n",
                    candidate->tid, candidate->seqNum, candidate->serial,
                    candidate->translatedPa,
                    candidate->destinationFlatIdx);
        }
    }

    delete pkt;
    cpu->wakeCPU();
    cpu->activityThisCycle();
}

void
RegisterPrefetcher::recvTimingSnoopReq(PacketPtr pkt)
{
    if (!enable || !pkt->isInvalidate()) {
        return;
    }

    invalidateLine(pkt->getAddr());
}

void
RegisterPrefetcher::invalidateLine(Addr address, uint64_t excluded_serial)
{

    const Addr block_mask = ~(static_cast<Addr>(cpu->cacheLineSize()) - 1);
    const Addr invalidated = address & block_mask;
    for (auto &[serial, candidate] : candidates) {
        if (serial != excluded_serial && candidate->request &&
            candidate->request->hasPaddr() &&
            (candidate->translatedPa & block_mask) == invalidated &&
            candidate->state != State::Reused &&
            candidate->state != State::Discarded &&
            candidate->state != State::FallbackNormal) {
            discardCandidate(*candidate, FailureReason::ResponseInvalidate,
                             true);
        }
    }
}

void
RegisterPrefetcher::observeLocalWrite(Addr address, unsigned size)
{
    if (!enable || size == 0) {
        return;
    }

    const Addr block_mask = ~(static_cast<Addr>(cpu->cacheLineSize()) - 1);
    const Addr first_block = address & block_mask;
    const Addr max_addr = std::numeric_limits<Addr>::max();
    const Addr last_byte = address > max_addr - (size - 1) ?
        max_addr : address + size - 1;
    const Addr last_block = last_byte & block_mask;

    for (auto &[serial, candidate] : candidates) {
        const bool read_admitted = candidate->state == State::Inflight ||
            candidate->state == State::ResponseReady ||
            candidate->state == State::AwaitingValidation;
        if (!read_admitted) {
            continue;
        }

        const Addr candidate_block = candidate->translatedPa & block_mask;
        if (candidate_block < first_block || candidate_block > last_block) {
            continue;
        }

        ++stats.localWriteInvalidate;
        ++stats.cancelOrderingConflict;
        ++stats.validationFailRarRaw;
        DPRINTF(RfpCancel,
                "[tid:%u] [sn:%llu] token:%llu local write [%#lx, %#lx] "
                "invalidates candidate pa:%#lx\n",
                candidate->tid, candidate->seqNum, candidate->serial,
                address, last_byte, candidate->translatedPa);
        discardCandidate(*candidate, FailureReason::Ordering, true);
    }
}

bool
RegisterPrefetcher::validateAddressAndAttributes(
    Candidate &candidate, const DynInstPtr &inst,
    const RequestPtr &normal_req, FailureReason *failure)
{
    if (!validateIdentity(candidate, inst)) {
        *failure = FailureReason::TokenMismatch;
        return false;
    }
    if (candidate.generation != generations[candidate.tid] ||
        !predictors[candidate.tid]->versionMatches(
            candidate.pc, candidate.generation,
            candidate.predictorVersion)) {
        *failure = FailureReason::Generation;
        return false;
    }
    if (!normal_req || !normal_req->hasVaddr() || !normal_req->hasPaddr() ||
        normal_req->getVaddr() != candidate.predictedVa ||
        inst->effAddr != candidate.predictedVa) {
        *failure = FailureReason::AddressMismatch;
        return false;
    }
    if (normal_req->getPaddr() != candidate.translatedPa ||
        inst->physEffAddr != candidate.translatedPa) {
        *failure = FailureReason::PhysicalAddressMismatch;
        return false;
    }
    if (normal_req->getSize() != candidate.size ||
        static_cast<Request::FlagsType>(normal_req->getFlags()) !=
            static_cast<Request::FlagsType>(candidate.request->getFlags()) ||
        normal_req->contextId() != candidate.contextId ||
        normal_req->isUncacheable() || normal_req->isStrictlyOrdered() ||
        normal_req->isLLSC() || normal_req->isAtomic() ||
        normal_req->isMemMgmt() ||
        normal_req->getByteEnable().size() != candidate.size ||
        !std::all_of(normal_req->getByteEnable().begin(),
                     normal_req->getByteEnable().end(),
                     [](bool enabled) { return enabled; })) {
        *failure = FailureReason::FlagsMismatch;
        return false;
    }
    if (!candidate.responseHasData ||
        (candidate.state != State::ResponseReady &&
         candidate.state != State::AwaitingValidation)) {
        *failure = FailureReason::NoData;
        return false;
    }
    *failure = FailureReason::None;
    return true;
}

bool
RegisterPrefetcher::tryPrepareReuse(
    const DynInstPtr &inst, const RequestPtr &normal_req)
{
    if (!enable || !inst || inst->rfpTokenSerial == 0) {
        return false;
    }

    auto *candidate = findCandidate(inst->rfpTokenSerial);
    if (!candidate || !validateIdentity(*candidate, inst)) {
        ++stats.cancelTokenMismatch;
        if (candidate) {
            discardCandidate(*candidate, FailureReason::TokenMismatch, true);
        } else {
            inst->rfpTokenSerial = 0;
            inst->rfpUseState = DynInst::RfpUseState::Invalid;
        }
        return false;
    }

    FailureReason failure = FailureReason::None;
    if (!validateAddressAndAttributes(
            *candidate, inst, normal_req, &failure)) {
        switch (failure) {
          case FailureReason::AddressMismatch:
            ++stats.validationFailVa;
            ++stats.cancelAddressMismatch;
            break;
          case FailureReason::PhysicalAddressMismatch:
            ++stats.validationFailPa;
            break;
          case FailureReason::FlagsMismatch:
            ++stats.validationFailFlags;
            break;
          case FailureReason::Generation:
            ++stats.cancelGenerationMismatch;
            break;
          case FailureReason::NoData:
            ++stats.cancelNoData;
            break;
          default:
            ++stats.cancelTokenMismatch;
            break;
        }
        discardCandidate(*candidate, failure, true);
        return false;
    }

    if (!inst->memData) {
        inst->memData = new uint8_t[candidate->size];
    }
    std::copy_n(candidate->data.begin(), candidate->size, inst->memData);
    candidate->state = State::AwaitingValidation;
    inst->rfpReusePending = true;
    inst->rfpUseState = DynInst::RfpUseState::ValidationPending;
    DPRINTF(RfpValidate,
            "[tid:%u] [sn:%llu] token:%llu address validation passed\n",
            candidate->tid, candidate->seqNum, candidate->serial);
    return true;
}

bool
RegisterPrefetcher::finalizeReuse(
    const DynInstPtr &inst, const RequestPtr &normal_req)
{
    auto *candidate = findCandidate(inst);
    if (!candidate || !inst->rfpReusePending ||
        inst->rfpFallbackRequired) {
        return false;
    }

    FailureReason failure = FailureReason::None;
    if (!validateAddressAndAttributes(
            *candidate, inst, normal_req, &failure)) {
        discardCandidate(*candidate, failure, true);
        return false;
    }

    inst->rfpValidationPassed = true;
    ++stats.validationPass;
    return true;
}

void
RegisterPrefetcher::completeReuse(const DynInstPtr &inst)
{
    auto *candidate = findCandidate(inst);
    panic_if(!candidate || !inst->rfpValidationPassed ||
             !inst->rfpDataPublished,
             "Completing an RFP reuse without a validated candidate");

    candidate->state = State::Reused;
    inst->rfpReusePending = false;
    inst->rfpReused = true;
    inst->rfpUseState = DynInst::RfpUseState::Reused;
    ++stats.reused;
    ++stats.duplicateDemandAvoided;
    stats.latencyResponseToReuse.sample(
        cyclesBetween(candidate->responseTick, curTick()));

    // Keep the token-to-preg binding through this cycle's issue arbitration.
    // CPU::tick() calls RFP cleanup after IEW issueAndSelect(), so consumers
    // can prove they used a published and validated RFP operand instead of
    // falling through the ordinary bypass-scoreboard path.
    DPRINTF(RfpValidate,
            "[tid:%u] [sn:%llu] token:%llu reused candidate\n",
            candidate->tid, candidate->seqNum, candidate->serial);
}

void
RegisterPrefetcher::rejectForForwarding(const DynInstPtr &inst)
{
    if (!enable || !inst || inst->rfpReused) {
        return;
    }
    if (auto *candidate = findCandidate(inst)) {
        ++stats.validationFailForwarding;
        discardCandidate(*candidate, FailureReason::Forwarding, true);
    }
}

void
RegisterPrefetcher::rejectForMdp(const DynInstPtr &inst)
{
    if (!enable || !inst || inst->rfpReused) {
        return;
    }
    if (auto *candidate = findCandidate(inst)) {
        ++stats.validationFailMdp;
        ++stats.cancelOrderingConflict;
        discardCandidate(*candidate, FailureReason::Ordering, true);
    }
}

void
RegisterPrefetcher::rejectForNuke(const DynInstPtr &inst)
{
    if (!enable || !inst || inst->rfpReused) {
        return;
    }
    if (auto *candidate = findCandidate(inst)) {
        ++stats.validationFailNuke;
        ++stats.cancelOrderingConflict;
        discardCandidate(*candidate, FailureReason::Ordering, true);
    }
}

void
RegisterPrefetcher::rejectForRarRaw(const DynInstPtr &inst)
{
    if (!enable || !inst || inst->rfpReused) {
        return;
    }
    if (auto *candidate = findCandidate(inst)) {
        ++stats.validationFailRarRaw;
        ++stats.cancelOrderingConflict;
        discardCandidate(*candidate, FailureReason::Ordering, true);
    }
}

void
RegisterPrefetcher::onNormalCompletion(const DynInstPtr &inst)
{
    if (!enable || !inst || inst->rfpReused) {
        return;
    }
    if (auto *candidate = findCandidate(inst)) {
        discardCandidate(*candidate, FailureReason::NormalCompletion, true);
    }
}

RegisterPrefetcher::OperandStatus
RegisterPrefetcher::operandStatus(
    RegIndex flat_idx, ThreadID tid, InstSeqNum consumer_seq,
    DynInstPtr *producer)
{
    if (!enable) {
        return OperandStatus::Uncontrolled;
    }
    auto owner = pregOwners.find(flat_idx);
    if (owner == pregOwners.end()) {
        return OperandStatus::Uncontrolled;
    }
    auto *candidate = findCandidate(owner->second);
    if (!candidate || !candidate->specWoken || candidate->tid != tid ||
        candidate->seqNum >= consumer_seq || candidate->orphaned) {
        return OperandStatus::Uncontrolled;
    }

    if (producer) {
        *producer = candidate->producer;
    }
    if (candidate->producer->rfpDataPublished &&
        candidate->producer->rfpValidationPassed) {
        return OperandStatus::Ready;
    }

    auto [wait_it, inserted] =
        candidate->consumerWaitStart.try_emplace(consumer_seq, curTick());
    if (reuseMaxWaitCycles > 0 &&
        cyclesBetween(wait_it->second, curTick()) < reuseMaxWaitCycles) {
        ++stats.consumerWait;
        return OperandStatus::Waiting;
    }
    return OperandStatus::Cancel;
}

void
RegisterPrefetcher::recordConsumerUse(
    const DynInstPtr &producer, InstSeqNum consumer_seq)
{
    auto *candidate = findCandidate(producer);
    panic_if(!candidate || !producer->rfpDataPublished ||
                 !producer->rfpValidationPassed,
             "Recording an unvalidated RFP consumer");
    if (candidate->issuedConsumers.insert(consumer_seq).second) {
        ++stats.consumerIssuedWithData;
        stats.latencyRenameToConsumerUse.sample(
            cyclesBetween(producer->rfpRenameTick, curTick()));
    }
}

void
RegisterPrefetcher::cancelForConsumer(const DynInstPtr &producer)
{
    auto *candidate = findCandidate(producer);
    if (!candidate) {
        return;
    }
    ++stats.consumerEarlyCancel;
    if (candidate->responseHasData) {
        ++stats.cancelValidationPending;
    } else {
        ++stats.cancelNoData;
    }
    discardCandidate(*candidate, FailureReason::NoData, true);
}

void
RegisterPrefetcher::releaseInstructionBinding(
    Candidate &candidate, bool fallback)
{
    auto owner = pregOwners.find(candidate.destinationFlatIdx);
    if (owner != pregOwners.end() && owner->second == candidate.serial) {
        pregOwners.erase(owner);
    }

    auto &inst = candidate.producer;
    if (inst && inst->rfpTokenSerial == candidate.serial) {
        const bool pending = inst->rfpReusePending;
        inst->rfpTokenSerial = 0;
        inst->rfpReusePending = false;
        inst->rfpValidationPassed = false;
        if (fallback) {
            inst->rfpFallbackRequired |= pending;
            inst->rfpDataPublished = false;
            inst->rfpUseState = DynInst::RfpUseState::Fallback;
        }
    }
}

void
RegisterPrefetcher::discardCandidate(
    Candidate &candidate, FailureReason reason, bool cancel_consumers)
{
    if (candidate.state == State::Discarded ||
        candidate.state == State::FallbackNormal ||
        candidate.state == State::Reused) {
        return;
    }

    candidate.failure = reason;
    candidate.orphaned = candidate.packetInflight ||
                         candidate.translationOutstanding;
    const bool fallback = candidate.specWoken ||
        candidate.state == State::ResponseReady ||
        candidate.state == State::AwaitingValidation;
    candidate.state = fallback ? State::FallbackNormal : State::Discarded;
    if (fallback) {
        ++stats.fallbackNormal;
    }

    if (candidate.specWoken && scheduler) {
        if (cancel_consumers) {
            const bool issued = scheduler->loadCancel(
                candidate.producer, SpeculationSource::RegisterPrefetch);
            if (issued) {
                ++stats.issuedConsumerSquashFallback;
                cpu->squashFromRfp(candidate.producer);
            }
        } else {
            scheduler->clearRfpState(candidate.producer);
        }
    }

    if (candidate.packet && !candidate.packetInflight) {
        delete candidate.packet;
        candidate.packet = nullptr;
    }
    releaseInstructionBinding(candidate, fallback);
    DPRINTF(RfpCancel,
            "[tid:%u] [sn:%llu] token:%llu discard reason:%u inflight:%d\n",
            candidate.tid, candidate.seqNum, candidate.serial,
            static_cast<unsigned>(reason), candidate.packetInflight);
}

void
RegisterPrefetcher::cleanupTerminalCandidates()
{
    for (auto it = candidates.begin(); it != candidates.end();) {
        auto &candidate = *it->second;
        const bool terminal = candidate.state == State::Discarded ||
            candidate.state == State::FallbackNormal ||
            candidate.state == State::Reused;
        if (terminal && !candidate.packetInflight &&
            !candidate.translationOutstanding) {
            if (candidate.state == State::Reused) {
                releaseInstructionBinding(candidate, false);
            }
            it = candidates.erase(it);
        } else {
            ++it;
        }
    }
}

void
RegisterPrefetcher::checkInvariants() const
{
    panic_if(candidates.size() > launchQueueEntries + maxInflight,
             "RFP candidate storage exceeded its configured bound");

    unsigned counted_inflight = 0;
    std::vector<unsigned> counted_per_thread(perThreadInflight.size(), 0);
    for (const auto &[serial, candidate_ptr] : candidates) {
        const auto &candidate = *candidate_ptr;
        panic_if(serial != candidate.serial,
                 "RFP candidate map key/token mismatch");
        panic_if(!candidate.producer ||
                     candidate.tid >= counted_per_thread.size(),
                 "RFP candidate has an invalid producer or thread");

        if (candidate.packetInflight) {
            ++counted_inflight;
            ++counted_per_thread[candidate.tid];
            panic_if(!candidate.packet,
                     "RFP inflight candidate lost its packet");
            panic_if(candidate.state != State::Inflight &&
                         candidate.state != State::FallbackNormal &&
                         candidate.state != State::Discarded,
                     "RFP packet is inflight in an illegal state");
        } else {
            panic_if(candidate.state == State::Inflight,
                     "RFP inflight state has no inflight packet");
        }

        panic_if(candidate.translationOutstanding &&
                     (!candidate.request || !candidate.senderState),
                 "RFP timing translation lost request ownership");
        panic_if(candidate.producer->rfpValidationPassed &&
                     !candidate.producer->rfpDataPublished,
                 "RFP validation passed without published data");

        const bool owns_binding =
            candidate.state != State::FallbackNormal &&
            candidate.state != State::Discarded;
        if (owns_binding) {
            panic_if(!validateIdentity(candidate, candidate.producer),
                     "Live RFP candidate lost its DynInst token binding");
            const auto owner = pregOwners.find(candidate.destinationFlatIdx);
            panic_if(owner == pregOwners.end() ||
                         owner->second != candidate.serial,
                     "Live RFP candidate lost its physical-register owner");
        }

        const bool value_state = candidate.state == State::ResponseReady ||
            candidate.state == State::AwaitingValidation ||
            candidate.state == State::Reused;
        panic_if(value_state &&
                     (!candidate.responseHasData ||
                      !candidate.producer->rfpDataPublished),
                 "RFP value state has no published response data");
        panic_if(candidate.state == State::AwaitingValidation &&
                     !candidate.producer->rfpReusePending,
                 "RFP validation state has no pending producer");
        panic_if(candidate.state == State::Reused &&
                     (!candidate.producer->rfpReused ||
                      !candidate.producer->rfpValidationPassed),
                 "RFP reused state is missing producer completion flags");
    }

    panic_if(counted_inflight != inflight || inflight > maxInflight,
             "RFP global inflight accounting mismatch");
    for (ThreadID tid = 0; tid < counted_per_thread.size(); ++tid) {
        panic_if(counted_per_thread[tid] != perThreadInflight[tid] ||
                     perThreadInflight[tid] > perThreadMaxInflight,
                 "RFP per-thread inflight accounting mismatch");
    }

    for (const auto &[flat_idx, serial] : pregOwners) {
        const auto *candidate = findCandidate(serial);
        panic_if(!candidate || candidate->destinationFlatIdx != flat_idx ||
                     candidate->state == State::FallbackNormal ||
                     candidate->state == State::Discarded,
                 "RFP physical-register owner references a dead candidate");
    }
}

void
RegisterPrefetcher::squash(ThreadID tid, InstSeqNum squash_seq_num)
{
    if (!enable) {
        return;
    }
    for (auto &[serial, candidate] : candidates) {
        if (candidate->tid == tid && candidate->seqNum > squash_seq_num) {
            // All consumers are younger than the producer and are covered by
            // the same pipeline squash. Avoid a second selective-cancel walk.
            discardCandidate(*candidate, FailureReason::Squashed, false);
        }
    }
}

void
RegisterPrefetcher::invalidateGeneration(ThreadID tid)
{
    if (!enable || tid >= generations.size()) {
        return;
    }
    ++generations[tid];
    if (generations[tid] == 0) {
        generations[tid] = 1;
    }
    for (auto &[serial, candidate] : candidates) {
        if (candidate->tid == tid) {
            discardCandidate(*candidate, FailureReason::Generation, true);
        }
    }
}

void
RegisterPrefetcher::takeOverFrom()
{
    if (!enable) {
        return;
    }
    panic_if(!isDrained(), "RFP takeover requires a drained prefetcher");
    candidates.clear();
    pregOwners.clear();
    launchQueue.clear();
    inflight = 0;
    std::fill(perThreadInflight.begin(), perThreadInflight.end(), 0);
    for (ThreadID tid = 0; tid < predictors.size(); ++tid) {
        predictors[tid]->reset();
        ++generations[tid];
    }
}

bool
RegisterPrefetcher::isDrained() const
{
    return !enable || (candidates.empty() && inflight == 0);
}

void
RegisterPrefetcher::drainSanityCheck() const
{
    panic_if(!isDrained(), "RFP still owns candidates while CPU is drained");
}

} // namespace o3
} // namespace gem5
