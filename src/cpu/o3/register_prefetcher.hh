/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of
 * Sciences
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution; neither the name of
 * the copyright holders nor the names of its contributors may be used to
 * endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_REGISTER_PREFETCHER_HH__
#define __CPU_O3_REGISTER_PREFETCHER_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "arch/generic/mmu.hh"
#include "base/statistics.hh"
#include "base/stats/group.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/rfp_stride_table.hh"
#include "cpu/o3/rfp_wakeup_state.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{

class ThreadContext;
struct BaseO3CPUParams;

namespace o3
{

class CPU;
class LSQ;
class Scheduler;

class RegisterPrefetcher
{
  public:
    static constexpr unsigned MaxRfpBytes = 8;

    enum class OperandStatus
    {
        Uncontrolled,
        Waiting,
        Cancel,
        Ready
    };

    enum class S0RejectReason
    {
        TranslationPending,
        Fault,
        Unsupported,
        Forwarding,
        Mdp,
        StoreAddressPending,
        Nuke,
        RarFull,
        RawFull
    };

    enum class NormalRecoveryReason
    {
        StoreLoadRaw,
        LoadLoadOrder,
        ExternalSnoop,
        NumReasons
    };

    class RfpRequest final : public BaseMMU::Translation,
                             public Packet::SenderState
    {
      public:
        RfpRequest(RegisterPrefetcher *owner, uint64_t serial);

        void markDelayed() override;
        void finish(const Fault &fault, const RequestPtr &req,
                    ThreadContext *tc, BaseMMU::Mode mode) override;
        bool squashed() const override;

        RegisterPrefetcher *owner() const { return _owner; }
        uint64_t serial() const { return _serial; }

      private:
        RegisterPrefetcher *_owner;
        const uint64_t _serial;
    };

    RegisterPrefetcher(CPU *cpu, const BaseO3CPUParams &params);

    bool enabled() const { return enable; }
    void setLSQ(LSQ *new_lsq) { lsq = new_lsq; }
    void setScheduler(Scheduler *new_scheduler) { scheduler = new_scheduler; }

    void onRenamedInstruction(const DynInstPtr &inst);
    void onProducerIssue(const DynInstPtr &inst);
    bool onProducerIssueCanceled(const DynInstPtr &inst,
                                 bool consumers_already_canceled = false);
    void trainCommittedLoad(const DynInstPtr &inst);
    void tick();
    void recvReqRetry();
    void recvTimingResp(PacketPtr pkt, RfpRequest &request);
    void recvTimingSnoopReq(PacketPtr pkt);
    void observeLocalWrite(Addr address, unsigned size);
    void squash(ThreadID tid, InstSeqNum squash_seq_num);
    void invalidateGeneration(ThreadID tid);
    void flushThreadForTeardown(ThreadID tid);
    void takeOverFrom();

    bool hasCandidateForS0(const DynInstPtr &inst) const;
    void observeLoadS0(const DynInstPtr &inst);
    void recordNormalDemandRead(const DynInstPtr &inst, unsigned size);
    bool acceptAtS0(const DynInstPtr &inst, const RequestPtr &normal_req,
                    bool rar_reserved, bool raw_reserved);
    void rejectAtS0(const DynInstPtr &inst, S0RejectReason reason);
    void completeReuse(const DynInstPtr &inst);
    void rejectForForwarding(const DynInstPtr &inst);
    void rejectForMdp(const DynInstPtr &inst);
    void rejectForNuke(const DynInstPtr &inst);
    void rejectForRarRaw(const DynInstPtr &inst);
    void onNormalCompletion(const DynInstPtr &inst);

    OperandStatus operandStatus(RegIndex flat_idx, ThreadID tid,
                                InstSeqNum consumer_seq,
                                DynInstPtr *producer);
    void recordConsumerUse(const DynInstPtr &producer,
                           const DynInstPtr &consumer);
    void recordConsumerAtFu(const DynInstPtr &consumer);
    void cancelForConsumer(const DynInstPtr &producer);
    void recordNormalMemOrderSquash(const DynInstPtr &producer,
                                    NormalRecoveryReason reason);

    bool isDrained() const;
    void drainSanityCheck() const;

  private:
    enum class State
    {
        LaunchQueued,
        Translating,
        CacheQueued,
        Inflight,
        ResponseReady,
        S0Validated,
        Reused,
        FallbackNormal,
        Discarded,
        NumStates
    };

    enum class DeadlineState
    {
        NoCandidate,
        LaunchQueued,
        Translating,
        CacheQueued,
        Inflight,
        ResponseReady,
        S0Validated,
        TerminalUnavailable,
        NumStates
    };

    enum class FailureReason
    {
        None,
        QueueFull,
        TranslationFault,
        TranslationInvalid,
        RetryLimit,
        ResponseError,
        ResponseInvalidate,
        ResponseMalformed,
        PublishFault,
        Squashed,
        PregRecycle,
        Generation,
        NoData,
        TokenMismatch,
        AddressMismatch,
        PhysicalAddressMismatch,
        FlagsMismatch,
        Forwarding,
        Ordering,
        Mdp,
        StoreAddressPending,
        Nuke,
        RarFull,
        RawFull,
        LocalWrite,
        SnoopInvalidate,
        DemandTranslationPending,
        DemandFault,
        Unsupported,
        NormalCompletion,
        ThreadTeardown,
        NumReasons
    };

    struct Candidate
    {
        uint64_t serial = 0;
        ThreadID tid = InvalidThreadID;
        ContextID contextId = InvalidContextID;
        InstSeqNum seqNum = 0;
        RegIndex destinationFlatIdx = 0;
        uint64_t predictorVersion = 0;
        uint64_t lookahead = 0;
        DynInstPtr producer;
        Addr pc = 0;
        Addr predictedVa = 0;
        Addr translatedPa = 0;
        unsigned size = 0;
        Request::Flags originalFlags;
        uint64_t generation = 0;
        Tick lookupTick = 0;
        Tick translationStartTick = 0;
        Tick translationDoneTick = 0;
        Tick admissionTick = 0;
        Tick responseTick = 0;
        Tick producerIssueTick = 0;
        unsigned retryCycles = 0;
        State state = State::LaunchQueued;
        FailureReason failure = FailureReason::None;
        RequestPtr request;
        PacketPtr packet = nullptr;
        std::unique_ptr<RfpRequest> senderState;
        std::array<uint8_t, MaxRfpBytes> data = {};
        bool translationDelayed = false;
        bool translationOutstanding = false;
        bool packetInflight = false;
        bool responseHasData = false;
        bool everAdmitted = false;
        bool fanoutSampled = false;
        RfpWakeupState wakeup;
        bool orphaned = false;
        std::unordered_set<InstSeqNum> consumerGateSeen;
        std::unordered_map<InstSeqNum, Tick> consumerWaitStart;
        std::unordered_set<InstSeqNum> canceledConsumers;
        std::unordered_set<InstSeqNum> issuedConsumers;
    };

    struct RfpStats : public statistics::Group
    {
        RfpStats(statistics::Group *parent, unsigned num_threads,
                 unsigned issue_width, unsigned candidate_capacity,
                 unsigned max_retry_cycles);

        statistics::Scalar lookup;
        statistics::Scalar tableHit;
        statistics::Scalar confidentHit;
        statistics::Scalar rejectLowConfidence;
        statistics::Scalar rejectStride;
        statistics::Scalar rejectPage;
        statistics::Scalar rejectUnsupported;
        statistics::Scalar rejectDuplicate;
        statistics::Scalar trainFirstSample;
        statistics::Scalar trainStrideMatch;
        statistics::Scalar trainStrideChange;
        statistics::Scalar trainConfidenceInc;
        statistics::Scalar trainConfidenceDec;
        statistics::Scalar entryEvict;

        statistics::Scalar launchQueued;
        statistics::Scalar launchDroppedQueueFull;
        statistics::Scalar translationStarted;
        statistics::Scalar translationHit;
        statistics::Scalar translationDelayed;
        statistics::Scalar translationFault;
        statistics::Scalar admissionAttempt;
        statistics::Scalar admissionAccepted;
        statistics::Scalar rejectDemandPriority;
        statistics::Scalar rejectPortBusy;
        statistics::Scalar rejectBankConflict;
        statistics::Scalar rejectCacheBlocked;
        statistics::Scalar rejectMshrArb;
        statistics::Scalar rejectMshrAlias;
        statistics::Scalar rejectTagRead;
        statistics::Scalar retryCount;
        statistics::Scalar retryDropped;
        statistics::Average inflightOccupancy;
        statistics::Scalar responseReceived;
        statistics::Scalar responseError;
        statistics::Scalar responseInvalidate;
        statistics::Scalar responsePublishFault;
        statistics::Scalar responseOrphaned;
        statistics::Scalar localWriteInvalidate;

        statistics::Scalar specWake;
        statistics::Scalar producerIssueAttempts;
        statistics::Scalar producerIssueWakeOnIssue;
        statistics::Scalar responseDataReadyAfterProducerIssue;
        statistics::Scalar producerIssueWakeRollback;
        statistics::Scalar consumerGateBeforeS0;
        statistics::Scalar consumerWait;
        statistics::Scalar consumerEarlyCancel;
        statistics::Scalar consumerIssuedWithData;
        statistics::Scalar cancelNoData;
        statistics::Scalar cancelValidationPending;
        statistics::Scalar cancelTokenMismatch;
        statistics::Scalar cancelAddressMismatch;
        statistics::Scalar cancelGenerationMismatch;
        statistics::Scalar cancelOrderingConflict;
        statistics::Scalar issuedConsumerSquashFallback;
        statistics::Scalar validationPass;
        statistics::Scalar validationFailVa;
        statistics::Scalar validationFailPa;
        statistics::Scalar validationFailFlags;
        statistics::Scalar validationFailForwarding;
        statistics::Scalar validationFailMdp;
        statistics::Scalar validationFailNuke;
        statistics::Scalar validationFailRarRaw;
        statistics::Scalar s0ValidationAttempt;
        statistics::Scalar s0ValidationPass;
        statistics::Scalar s0RejectTranslationPending;
        statistics::Scalar s0RejectFault;
        statistics::Scalar s0RejectUnsupported;
        statistics::Scalar s0RejectNoData;
        statistics::Scalar s0RejectIdentity;
        statistics::Scalar s0RejectAddress;
        statistics::Scalar s0RejectAttributes;
        statistics::Scalar s0RejectForwarding;
        statistics::Scalar s0RejectMdp;
        statistics::Scalar s0RejectStoreAddressPending;
        statistics::Scalar s0RejectNuke;
        statistics::Scalar s0RejectRarFull;
        statistics::Scalar s0RejectRawFull;
        statistics::Scalar s0RarReserved;
        statistics::Scalar s0RawReserved;
        statistics::Scalar consumerBackToBack;
        statistics::Scalar consumerAtFu;
        statistics::Scalar consumerAtFuBackToBack;
        statistics::Scalar s0OwnerRecycled;
        statistics::Scalar postS0RfpInvariantFailure;
        statistics::Scalar normalMemOrderSquashAfterRfp;
        statistics::Scalar reused;
        statistics::Scalar fallbackNormal;
        statistics::Scalar duplicateDemandAvoided;

        statistics::Scalar lookupMiss;
        statistics::Scalar ineligibleLoadRename;
        statistics::Scalar rejectActiveCandidate;
        statistics::Scalar rejectClaim;
        statistics::Scalar rejectMisalignedPrediction;
        statistics::Scalar rejectCrossLinePrediction;
        statistics::Scalar trainSamples;
        statistics::Scalar trainStrideMismatch;
        statistics::Scalar trainIllegalStride;
        statistics::Scalar streamOccurrencesRenamed;
        statistics::Scalar streamOccurrencesCommitted;
        statistics::Scalar streamOccurrencesSquashed;
        statistics::Scalar streamOccurrencesTeardown;
        statistics::Scalar predictionsBeyondNextOccurrence;
        statistics::Scalar predictionIncorrectWithStrideMatch;
        statistics::Scalar streamHighWatermark;

        statistics::Scalar eligibleLoadS0;
        statistics::Scalar predictionResolved;
        statistics::Scalar predictionCorrect;
        statistics::Scalar predictionIncorrect;
        statistics::Scalar predictionLineCorrect;
        statistics::Scalar selectedPredictionResolved;
        statistics::Scalar selectedPredictionCorrect;
        statistics::Scalar selectedPredictionIncorrect;
        statistics::Scalar issuedPredictionResolved;
        statistics::Scalar issuedPredictionCorrect;
        statistics::Scalar issuedPredictionIncorrect;
        statistics::Scalar committedLoads;
        statistics::Scalar committedEligibleLoads;
        statistics::Scalar committedPredictedLoads;
        statistics::Scalar committedCorrectPredictions;
        statistics::Scalar committedIncorrectPredictions;
        statistics::Scalar committedRfpReused;
        statistics::Scalar committedRfpReuseWithConsumer;
        statistics::Scalar committedRfpReuseWithoutConsumer;
        statistics::Scalar committedDemandReadsAvoided;
        statistics::Scalar committedEligibleNormalDemandLoads;
        statistics::Scalar committedRfpBytesUseful;

        statistics::Scalar normalEligibleDemandLoads;
        statistics::Scalar normalEligibleDemandPackets;
        statistics::Scalar normalEligibleDemandBytes;
        statistics::Scalar prefetchBytesIssued;
        statistics::Scalar prefetchBytesReceived;
        statistics::Scalar prefetchBytesUseful;
        statistics::Scalar prefetchBytesWasted;
        statistics::Scalar responseDataReady;
        statistics::Scalar responseMalformed;
        statistics::Scalar responseFast;
        statistics::Scalar responseSlow;
        statistics::Scalar prefetchOnTimeAtS0;
        statistics::Scalar prefetchLateAtS0;
        statistics::Scalar prefetchUnavailableAtS0;
        statistics::Scalar prefetchNotIssuedAtS0;

        statistics::Scalar candidateDiscarded;
        statistics::Scalar issuedTerminalWaste;
        statistics::Scalar responseReadyTerminalWaste;
        statistics::Scalar wrongPathPrefetchIssued;
        statistics::Scalar snoopInvalidateEvents;
        statistics::Scalar snoopCandidatesInvalidated;
        statistics::Scalar localWriteEvents;
        statistics::Scalar rejectGlobalInflightFull;
        statistics::Scalar rejectThreadInflightFull;
        statistics::Scalar translationWidthDeferred;
        statistics::Scalar admissionWidthDeferred;
        statistics::Scalar candidateHighWatermark;
        statistics::Scalar liveCandidates;
        statistics::Scalar liveIssuedCandidates;
        statistics::Scalar inflightAtDump;

        statistics::Scalar consumerPairsSeenAtGate;
        statistics::Scalar consumerPairsWaited;
        statistics::Scalar consumerPairsCanceled;
        statistics::Scalar consumerPairsWaitedToReady;
        statistics::Scalar consumerPairsWaitedCanceled;
        statistics::Scalar consumerPairsWaitExpired;

        statistics::Vector candidateDiscardReason;
        statistics::Vector candidateDiscardState;
        statistics::Vector predictionDeadlineState;
        statistics::Vector normalRecoveryReason;
        statistics::Vector perThreadInflightFullCycles;

        statistics::Average candidateStorageOccupancy;
        statistics::Average activeCandidateOccupancy;
        statistics::Average launchQueueOccupancy;
        statistics::Average translatingOccupancy;
        statistics::Average cacheQueuedOccupancy;
        statistics::Average responseReadyOccupancy;
        statistics::Average s0ValidatedOccupancy;
        statistics::Average terminalOutstandingOccupancy;
        statistics::Average streamOccurrenceOccupancy;

        statistics::Distribution latencyLookupToAdmission;
        statistics::Distribution latencyAdmissionToResponse;
        statistics::Distribution latencyResponseToReuse;
        statistics::Distribution latencyRenameToConsumerUse;
        statistics::Distribution latencyS0ToConsumerIssue;
        statistics::Distribution latencyS0ToConsumerFu;
        statistics::Distribution latencyLookupToTranslation;
        statistics::Distribution latencyTranslation;
        statistics::Distribution latencyTranslationToAdmission;
        statistics::Distribution latencyPredictionToLoadS0;
        statistics::Distribution latencyAdmissionToLoadS0;
        statistics::Distribution latencyResponseToLoadS0;
        statistics::Distribution latencyLoadS0ToResponse;
        statistics::Distribution latencyCandidateLifetime;
        statistics::Distribution retriesPerCandidate;
        statistics::Distribution consumerWaitToReady;
        statistics::Distribution consumerWaitToCancel;
        statistics::Distribution latencyResponseToConsumerIssue;
        statistics::Distribution latencyProducerIssueToLoadS0;
        statistics::Distribution fanoutPerReusedLoad;
        statistics::Distribution translationsPerCycle;
        statistics::Distribution admissionAttemptsPerCycle;
        statistics::Distribution prefetchesIssuedPerCycle;
        statistics::Distribution predictionLookahead;

        statistics::Formula prefetchIssued;
        statistics::Formula prefetchUseful;
        statistics::Formula prefetchUnused;
        statistics::Formula tableHitRate;
        statistics::Formula predictionGenerationRate;
        statistics::Formula dynamicPredictionAccuracy;
        statistics::Formula committedPredictionAccuracy;
        statistics::Formula predictionCoverage;
        statistics::Formula correctPredictionCoverage;
        statistics::Formula selectedPredictionAccuracy;
        statistics::Formula issuedPredictionAccuracy;
        statistics::Formula launchConversion;
        statistics::Formula admissionConversion;
        statistics::Formula prefetchAccuracy;
        statistics::Formula resolvedPrefetchAccuracy;
        statistics::Formula committedPrefetchAccuracy;
        statistics::Formula prefetchCoverage;
        statistics::Formula allLoadPrefetchCoverage;
        statistics::Formula demandReadCoverage;
        statistics::Formula responseUseRate;
        statistics::Formula prefetchByteAccuracy;
        statistics::Formula resolvedPrefetchByteAccuracy;
        statistics::Formula committedPrefetchByteAccuracy;
        statistics::Formula prefetchTimeliness;
        statistics::Formula validationYield;
        statistics::Formula reuseConversion;
        statistics::Formula consumerBackToBackRate;
        statistics::Formula consumerAtFuBackToBackRate;
    };

    Candidate *findCandidate(const DynInstPtr &inst);
    const Candidate *findCandidate(const DynInstPtr &inst) const;
    Candidate *findCandidate(uint64_t serial);
    const Candidate *findCandidate(uint64_t serial) const;
    bool eligible(const DynInstPtr &inst, unsigned *size,
                  Request::Flags *flags) const;
    void startTranslation(Candidate &candidate);
    void finishTranslation(uint64_t serial, const Fault &fault,
                           const RequestPtr &req);
    void markTranslationDelayed(uint64_t serial);
    bool requestSquashed(uint64_t serial) const;
    bool sendCandidate(Candidate &candidate);
    void applyWakeAction(Candidate &candidate, RfpWakeupState::Action action,
                         bool consumers_already_canceled = false);
    bool validateIdentity(const Candidate &candidate,
                          const DynInstPtr &inst) const;
    bool validateAddressAndAttributes(Candidate &candidate,
                                      const DynInstPtr &inst,
                                      const RequestPtr &normal_req,
                                      FailureReason *failure);
    void discardCandidate(Candidate &candidate, FailureReason reason,
                          bool cancel_consumers);
    void releaseInstructionBinding(Candidate &candidate, bool fallback);
    void cleanupTerminalCandidates();
    void invalidateLine(Addr address, uint64_t excluded_serial = 0,
                        FailureReason reason = FailureReason::SnoopInvalidate);
    DeadlineState deadlineState(const Candidate *candidate) const;
    void checkInvariants() const;
    void recordLookupReject(RfpStrideTable::RejectReason reason);
    uint64_t cyclesBetween(Tick start, Tick end) const;

    CPU *cpu;
    LSQ *lsq = nullptr;
    Scheduler *scheduler = nullptr;
    const bool enable;
    const unsigned issueWidth;
    const unsigned launchQueueEntries;
    const unsigned maxInflight;
    const unsigned perThreadMaxInflight;
    const bool demandPriority;
    const bool dropOnPressure;
    const unsigned maxRetryCycles;
    const unsigned reuseMaxWaitCycles;
    const bool enableDebugTrace;

    uint64_t nextSerial = 1;
    unsigned inflight = 0;
    unsigned candidateHighWatermarkValue = 0;
    unsigned streamHighWatermarkValue = 0;
    unsigned admissionAttemptsThisCycle = 0;
    unsigned prefetchesIssuedThisCycle = 0;
    std::vector<unsigned> perThreadInflight;
    std::vector<uint64_t> generations;
    std::vector<std::unique_ptr<RfpStrideTable>> predictors;
    std::vector<RfpStreamTracker> streamTrackers;
    std::deque<uint64_t> launchQueue;
    std::unordered_map<uint64_t, std::unique_ptr<Candidate>> candidates;
    std::unordered_map<RegIndex, uint64_t> pregOwners;
    RfpStats stats;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_REGISTER_PREFETCHER_HH__
