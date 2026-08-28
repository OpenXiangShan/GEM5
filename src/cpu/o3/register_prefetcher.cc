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
#include <string>
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

RegisterPrefetcher::RfpStats::RfpStats(
    statistics::Group *parent, unsigned num_threads, unsigned issue_width,
    unsigned candidate_capacity, unsigned max_retry_cycles)
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
               "Committed observed strides matching the stored stride"),
      ADD_STAT(trainStrideChange, statistics::units::Count::get(),
               "Committed loads changing the stored stride"),
      ADD_STAT(trainConfidenceInc, statistics::units::Count::get(),
               "RFP confidence increments"),
      ADD_STAT(trainConfidenceDec, statistics::units::Count::get(),
               "RFP confidence halving events after stride nonmatches"),
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
      ADD_STAT(producerIssueAttempts, statistics::units::Count::get(),
               "RFP producer load issue-stage-0 attempts"),
      ADD_STAT(producerIssueWakeOnIssue, statistics::units::Count::get(),
               "RFP consumer wakeups sent at producer issue stage 0"),
      ADD_STAT(responseDataReadyAfterProducerIssue,
               statistics::units::Count::get(),
               "RFP responses publishing data after producer issue wakeup"),
      ADD_STAT(producerIssueWakeRollback, statistics::units::Count::get(),
               "RFP issue-stage wakeups retracted before demand load S0"),
      ADD_STAT(consumerGateBeforeS0, statistics::units::Count::get(),
               "RFP consumers reaching their final issue gate before load S0"),
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
      ADD_STAT(s0ValidationAttempt, statistics::units::Count::get(),
               "RFP candidates resolved at the load S0 linearization point"),
      ADD_STAT(s0ValidationPass, statistics::units::Count::get(),
               "RFP candidates accepted at load S0"),
      ADD_STAT(s0RejectTranslationPending, statistics::units::Count::get(),
               "RFP S0 rejects because demand translation is incomplete"),
      ADD_STAT(s0RejectFault, statistics::units::Count::get(),
               "RFP S0 rejects because the demand load has a fault"),
      ADD_STAT(s0RejectUnsupported, statistics::units::Count::get(),
               "RFP S0 rejects because the demand access is unsupported"),
      ADD_STAT(s0RejectNoData, statistics::units::Count::get(),
               "RFP S0 rejects because response data is not ready"),
      ADD_STAT(s0RejectIdentity, statistics::units::Count::get(),
               "RFP S0 rejects because candidate identity is stale"),
      ADD_STAT(s0RejectAddress, statistics::units::Count::get(),
               "RFP S0 rejects because VA or PA mismatches"),
      ADD_STAT(s0RejectAttributes, statistics::units::Count::get(),
               "RFP S0 rejects because demand attributes mismatch"),
      ADD_STAT(s0RejectForwarding, statistics::units::Count::get(),
               "RFP S0 rejects because an older store may forward"),
      ADD_STAT(s0RejectMdp, statistics::units::Count::get(),
               "RFP S0 rejects because MDP requires a wait"),
      ADD_STAT(s0RejectStoreAddressPending,
               statistics::units::Count::get(),
               "RFP S0 rejects on an unresolved older store-pipe address"),
      ADD_STAT(s0RejectNuke, statistics::units::Count::get(),
               "RFP S0 rejects because of a visible store-pipe conflict"),
      ADD_STAT(s0RejectRarFull, statistics::units::Count::get(),
               "RFP S0 rejects because a RAR entry cannot be reserved"),
      ADD_STAT(s0RejectRawFull, statistics::units::Count::get(),
               "RFP S0 rejects because a RAW entry cannot be reserved"),
      ADD_STAT(s0RarReserved, statistics::units::Count::get(),
               "RAR queue entries reserved by accepted RFP loads in S0"),
      ADD_STAT(s0RawReserved, statistics::units::Count::get(),
               "RAW queue entries reserved by accepted RFP loads in S0"),
      ADD_STAT(consumerBackToBack, statistics::units::Count::get(),
               "Consumers issued in the producer load S0 validation cycle"),
      ADD_STAT(consumerAtFu, statistics::units::Count::get(),
               "Live RFP producer-consumer pairs reaching the consumer FU"),
      ADD_STAT(consumerAtFuBackToBack, statistics::units::Count::get(),
               "Consumers reaching the FU one cycle after producer S0"),
      ADD_STAT(s0OwnerRecycled, statistics::units::Count::get(),
               "S0 owner bindings displaced by a real destination preg "
               "allocation"),
      ADD_STAT(postS0RfpInvariantFailure, statistics::units::Count::get(),
               "RFP-specific rejection attempted after S0 acceptance"),
      ADD_STAT(normalMemOrderSquashAfterRfp, statistics::units::Count::get(),
               "Normal memory-order squashes after RFP S0 acceptance"),
      ADD_STAT(reused, statistics::units::Count::get(),
               "Loads completed from an RFP candidate"),
      ADD_STAT(fallbackNormal, statistics::units::Count::get(),
               "RFP candidates falling back to the normal load path"),
      ADD_STAT(duplicateDemandAvoided, statistics::units::Count::get(),
               "Demand DCache reads avoided by RFP reuse"),

      ADD_STAT(lookupMiss, statistics::units::Count::get(),
               "Eligible rename lookups missing the stride table"),
      ADD_STAT(ineligibleLoadRename, statistics::units::Count::get(),
               "Renamed loads outside the RFP eligibility contract"),
      ADD_STAT(rejectActiveCandidate, statistics::units::Count::get(),
               "Predictions suppressed by an active candidate for the PC"),
      ADD_STAT(rejectClaim, statistics::units::Count::get(),
               "Predictions rejected by the committed-table claim token"),
      ADD_STAT(rejectMisalignedPrediction, statistics::units::Count::get(),
               "Predictions rejected because the predicted VA is misaligned"),
      ADD_STAT(rejectCrossLinePrediction, statistics::units::Count::get(),
               "Predictions rejected because the access crosses a line"),
      ADD_STAT(trainSamples, statistics::units::Count::get(),
               "Committed eligible loads used to train the predictor"),
      ADD_STAT(trainStrideMismatch, statistics::units::Count::get(),
               "Committed legal strides differing from the stored stride"),
      ADD_STAT(trainIllegalStride, statistics::units::Count::get(),
               "Committed observed strides outside the predictor contract"),
      ADD_STAT(streamOccurrencesRenamed, statistics::units::Count::get(),
               "Eligible load occurrences registered at rename"),
      ADD_STAT(streamOccurrencesCommitted, statistics::units::Count::get(),
               "Eligible load occurrences retired at commit"),
      ADD_STAT(streamOccurrencesSquashed, statistics::units::Count::get(),
               "Eligible load occurrences removed by pipeline squash"),
      ADD_STAT(streamOccurrencesTeardown, statistics::units::Count::get(),
               "Eligible load occurrences removed by thread teardown"),
      ADD_STAT(predictionsBeyondNextOccurrence,
               statistics::units::Count::get(),
               "Generated predictions with a same-PC lookahead above one"),
      ADD_STAT(predictionIncorrectWithStrideMatch,
               statistics::units::Count::get(),
               "Incorrect committed predictions whose observed stride still "
               "matched the stored stride"),
      ADD_STAT(streamHighWatermark, statistics::units::Count::get(),
               "Maximum outstanding eligible load occurrences"),

      ADD_STAT(eligibleLoadS0, statistics::units::Count::get(),
               "Rename-eligible dynamic loads reaching their first S0"),
      ADD_STAT(predictionResolved, statistics::units::Count::get(),
               "Generated predictions compared with the actual S0 VA"),
      ADD_STAT(predictionCorrect, statistics::units::Count::get(),
               "Generated predictions exactly matching the actual S0 VA"),
      ADD_STAT(predictionIncorrect, statistics::units::Count::get(),
               "Generated predictions differing from the actual S0 VA"),
      ADD_STAT(predictionLineCorrect, statistics::units::Count::get(),
               "Generated predictions matching the actual cache line"),
      ADD_STAT(selectedPredictionResolved, statistics::units::Count::get(),
               "Launched candidate predictions compared with actual S0 VA"),
      ADD_STAT(selectedPredictionCorrect, statistics::units::Count::get(),
               "Launched candidate predictions matching actual S0 VA"),
      ADD_STAT(selectedPredictionIncorrect, statistics::units::Count::get(),
               "Launched candidate predictions differing from actual S0 VA"),
      ADD_STAT(issuedPredictionResolved, statistics::units::Count::get(),
               "Issued prefetch predictions compared with the actual S0 VA"),
      ADD_STAT(issuedPredictionCorrect, statistics::units::Count::get(),
               "Issued prefetch predictions matching the actual S0 VA"),
      ADD_STAT(issuedPredictionIncorrect, statistics::units::Count::get(),
               "Issued prefetch predictions differing from the actual S0 VA"),
      ADD_STAT(committedLoads, statistics::units::Count::get(),
               "Non-faulting dynamic loads reaching RFP commit observation"),
      ADD_STAT(committedEligibleLoads, statistics::units::Count::get(),
               "Committed loads with an evaluable RFP ground truth"),
      ADD_STAT(committedPredictedLoads, statistics::units::Count::get(),
               "Committed eligible loads for which rename generated a prediction"),
      ADD_STAT(committedCorrectPredictions, statistics::units::Count::get(),
               "Committed predictions exactly matching the actual VA"),
      ADD_STAT(committedIncorrectPredictions, statistics::units::Count::get(),
               "Committed predictions differing from the actual VA"),
      ADD_STAT(committedRfpReused, statistics::units::Count::get(),
               "Committed loads completed with RFP response data"),
      ADD_STAT(committedRfpReuseWithConsumer, statistics::units::Count::get(),
               "Committed reused loads with at least one issued consumer"),
      ADD_STAT(committedRfpReuseWithoutConsumer, statistics::units::Count::get(),
               "Committed reused loads without an issued consumer"),
      ADD_STAT(committedDemandReadsAvoided, statistics::units::Count::get(),
               "Committed eligible loads whose RFP reuse remained the only "
               "DCache read path"),
      ADD_STAT(committedEligibleNormalDemandLoads,
               statistics::units::Count::get(),
               "Committed eligible loads that sent a normal DCache read"),
      ADD_STAT(committedRfpBytesUseful, statistics::units::Byte::get(),
               "RFP response bytes used by loads that subsequently commit"),

      ADD_STAT(normalEligibleDemandLoads, statistics::units::Count::get(),
               "Eligible loads that sent at least one normal DCache read"),
      ADD_STAT(normalEligibleDemandPackets, statistics::units::Count::get(),
               "Normal DCache read packets sent by eligible loads"),
      ADD_STAT(normalEligibleDemandBytes, statistics::units::Byte::get(),
               "Normal DCache read bytes sent by eligible loads"),
      ADD_STAT(prefetchBytesIssued, statistics::units::Byte::get(),
               "RFP read bytes accepted by the DCache"),
      ADD_STAT(prefetchBytesReceived, statistics::units::Byte::get(),
               "Well-formed RFP response data bytes returned"),
      ADD_STAT(prefetchBytesUseful, statistics::units::Byte::get(),
               "RFP response bytes used for speculative producer completion"),
      ADD_STAT(prefetchBytesWasted, statistics::units::Byte::get(),
               "Issued RFP bytes reaching a terminal non-reuse outcome"),
      ADD_STAT(responseDataReady, statistics::units::Count::get(),
               "RFP responses successfully published to a physical register"),
      ADD_STAT(responseMalformed, statistics::units::Count::get(),
               "RFP responses with missing data, size, or address mismatch"),
      ADD_STAT(responseFast, statistics::units::Count::get(),
               "Well-formed RFP responses arriving before demand load S0"),
      ADD_STAT(responseSlow, statistics::units::Count::get(),
               "Well-formed RFP responses arriving after demand load S0"),
      ADD_STAT(prefetchOnTimeAtS0, statistics::units::Count::get(),
               "Predicted loads with usable response data at demand S0"),
      ADD_STAT(prefetchLateAtS0, statistics::units::Count::get(),
               "Predicted loads whose issued RFP data was unavailable at S0"),
      ADD_STAT(prefetchUnavailableAtS0, statistics::units::Count::get(),
               "Issued predictions neither usable nor still inflight at S0"),
      ADD_STAT(prefetchNotIssuedAtS0, statistics::units::Count::get(),
               "Generated predictions without an issued RFP request at S0"),

      ADD_STAT(candidateDiscarded, statistics::units::Count::get(),
               "Launched candidates reaching a unique non-reuse terminal state"),
      ADD_STAT(issuedTerminalWaste, statistics::units::Count::get(),
               "Issued candidates reaching a terminal non-reuse outcome"),
      ADD_STAT(responseReadyTerminalWaste, statistics::units::Count::get(),
               "Candidates discarded after usable response data was ready"),
      ADD_STAT(wrongPathPrefetchIssued, statistics::units::Count::get(),
               "Issued candidates discarded by a producer pipeline squash"),
      ADD_STAT(snoopInvalidateEvents, statistics::units::Count::get(),
               "External invalidating snoop events observed by RFP"),
      ADD_STAT(snoopCandidatesInvalidated, statistics::units::Count::get(),
               "RFP candidates invalidated by external snoops"),
      ADD_STAT(localWriteEvents, statistics::units::Count::get(),
               "Same-core cache-line write events observed by RFP"),
      ADD_STAT(rejectGlobalInflightFull, statistics::units::Count::get(),
               "Candidate-cycle deferrals at the global inflight limit"),
      ADD_STAT(rejectThreadInflightFull, statistics::units::Count::get(),
               "Candidate-cycle deferrals at a per-thread inflight limit"),
      ADD_STAT(translationWidthDeferred, statistics::units::Count::get(),
               "Launch-queued candidate cycles deferred by translation width"),
      ADD_STAT(admissionWidthDeferred, statistics::units::Count::get(),
               "Cache-queued candidate cycles not examined because admission "
               "width was consumed"),
      ADD_STAT(candidateHighWatermark, statistics::units::Count::get(),
               "Maximum live RFP candidate storage occupancy"),
      ADD_STAT(liveCandidates, statistics::units::Count::get(),
               "Live candidates at the most recent RFP tick"),
      ADD_STAT(liveIssuedCandidates, statistics::units::Count::get(),
               "Issued nonterminal candidates at the most recent RFP tick"),
      ADD_STAT(inflightAtDump, statistics::units::Count::get(),
               "Inflight RFP packets at the most recent RFP tick"),

      ADD_STAT(consumerPairsSeenAtGate, statistics::units::Count::get(),
               "Unique producer-consumer pairs observed at the RFP issue gate"),
      ADD_STAT(consumerPairsWaited, statistics::units::Count::get(),
               "Unique producer-consumer pairs that waited at the issue gate"),
      ADD_STAT(consumerPairsCanceled, statistics::units::Count::get(),
               "Unique waiting producer-consumer pairs canceled"),
      ADD_STAT(consumerPairsWaitedToReady, statistics::units::Count::get(),
               "Unique waiting producer-consumer pairs later issued with data"),
      ADD_STAT(consumerPairsWaitedCanceled, statistics::units::Count::get(),
               "Unique waiting producer-consumer pairs later canceled"),
      ADD_STAT(consumerPairsWaitExpired, statistics::units::Count::get(),
               "Unique waiting pairs that disappeared after producer reuse"),

      ADD_STAT(candidateDiscardReason, statistics::units::Count::get(),
               "Unique candidate discards by terminal reason"),
      ADD_STAT(candidateDiscardState, statistics::units::Count::get(),
               "Unique candidate discards by state at termination"),
      ADD_STAT(predictionDeadlineState, statistics::units::Count::get(),
               "Generated prediction state when the demand load reached S0"),
      ADD_STAT(normalRecoveryReason, statistics::units::Count::get(),
               "Normal memory-order recoveries after RFP acceptance by cause"),
      ADD_STAT(perThreadInflightFullCycles, statistics::units::Cycle::get(),
               "Cycles each thread held its RFP inflight limit"),

      ADD_STAT(candidateStorageOccupancy, statistics::units::Count::get(),
               "Average total RFP candidate storage occupancy"),
      ADD_STAT(activeCandidateOccupancy, statistics::units::Count::get(),
               "Average nonterminal RFP candidate occupancy"),
      ADD_STAT(launchQueueOccupancy, statistics::units::Count::get(),
               "Average RFP launch-queued candidate occupancy"),
      ADD_STAT(translatingOccupancy, statistics::units::Count::get(),
               "Average RFP translating candidate occupancy"),
      ADD_STAT(cacheQueuedOccupancy, statistics::units::Count::get(),
               "Average RFP cache-admission queued occupancy"),
      ADD_STAT(responseReadyOccupancy, statistics::units::Count::get(),
               "Average RFP response-ready candidate occupancy"),
      ADD_STAT(s0ValidatedOccupancy, statistics::units::Count::get(),
               "Average RFP S0-validated candidate occupancy"),
      ADD_STAT(terminalOutstandingOccupancy, statistics::units::Count::get(),
               "Average terminal candidates awaiting an outstanding callback"),
      ADD_STAT(streamOccurrenceOccupancy, statistics::units::Count::get(),
               "Average outstanding eligible load occurrences"),

      ADD_STAT(latencyLookupToAdmission, statistics::units::Cycle::get(),
               "Cycles from RFP lookup to DCache admission"),
      ADD_STAT(latencyAdmissionToResponse, statistics::units::Cycle::get(),
               "Cycles from RFP DCache admission to response"),
      ADD_STAT(latencyResponseToReuse, statistics::units::Cycle::get(),
               "Cycles from RFP response to load reuse"),
      ADD_STAT(latencyRenameToConsumerUse, statistics::units::Cycle::get(),
               "Cycles from RFP producer rename to consumer issue"),
      ADD_STAT(latencyS0ToConsumerIssue, statistics::units::Cycle::get(),
               "Cycles from producer S0 validation to consumer issue"),
      ADD_STAT(latencyS0ToConsumerFu, statistics::units::Cycle::get(),
               "Cycles from producer S0 validation to consumer FU entry"),
      ADD_STAT(latencyLookupToTranslation, statistics::units::Cycle::get(),
               "Cycles from prediction lookup to translation start"),
      ADD_STAT(latencyTranslation, statistics::units::Cycle::get(),
               "Cycles spent in RFP timing translation"),
      ADD_STAT(latencyTranslationToAdmission, statistics::units::Cycle::get(),
               "Cycles from translation completion to DCache admission"),
      ADD_STAT(latencyPredictionToLoadS0, statistics::units::Cycle::get(),
               "Cycles from prediction generation to the demand load S0"),
      ADD_STAT(latencyAdmissionToLoadS0, statistics::units::Cycle::get(),
               "Cycles from RFP admission to the demand load S0"),
      ADD_STAT(latencyResponseToLoadS0, statistics::units::Cycle::get(),
               "Response lead cycles before an on-time demand load S0"),
      ADD_STAT(latencyLoadS0ToResponse, statistics::units::Cycle::get(),
               "Response lateness cycles after the demand load S0"),
      ADD_STAT(latencyCandidateLifetime, statistics::units::Cycle::get(),
               "Cycles from candidate launch to a terminal outcome"),
      ADD_STAT(retriesPerCandidate, statistics::units::Count::get(),
               "DCache admission retries per terminal candidate"),
      ADD_STAT(consumerWaitToReady, statistics::units::Cycle::get(),
               "Cycles unique consumer pairs waited before issue"),
      ADD_STAT(consumerWaitToCancel, statistics::units::Cycle::get(),
               "Cycles unique consumer pairs waited before cancellation"),
      ADD_STAT(latencyResponseToConsumerIssue, statistics::units::Cycle::get(),
               "Cycles from producer response publication to consumer issue"),
      ADD_STAT(latencyProducerIssueToLoadS0,
               statistics::units::Cycle::get(),
               "Cycles from producer issue stage 0 to demand load S0"),
      ADD_STAT(fanoutPerReusedLoad, statistics::units::Count::get(),
               "Unique issued-consumer fanout per reused producer"),
      ADD_STAT(translationsPerCycle, statistics::units::Count::get(),
               "RFP translations started per active CPU cycle"),
      ADD_STAT(admissionAttemptsPerCycle, statistics::units::Count::get(),
               "RFP DCache admission attempts per active CPU cycle"),
      ADD_STAT(prefetchesIssuedPerCycle, statistics::units::Count::get(),
               "RFP DCache requests accepted per active CPU cycle"),
      ADD_STAT(predictionLookahead, statistics::units::Count::get(),
               "Same-PC dynamic occurrence lookahead of generated predictions"),

      ADD_STAT(prefetchIssued, statistics::units::Count::get(),
               "Alias for RFP DCache requests accepted"),
      ADD_STAT(prefetchUseful, statistics::units::Count::get(),
               "Alias for speculative producer loads completed from RFP data"),
      ADD_STAT(prefetchUnused, statistics::units::Count::get(),
               "Issued RFP requests not reused, including live requests"),
      ADD_STAT(tableHitRate, statistics::units::Ratio::get(),
               "Stride table hit rate among eligible rename lookups"),
      ADD_STAT(predictionGenerationRate, statistics::units::Ratio::get(),
               "Generated predictions among eligible rename lookups"),
      ADD_STAT(dynamicPredictionAccuracy, statistics::units::Ratio::get(),
               "Exact-VA accuracy among predictions resolved at S0"),
      ADD_STAT(committedPredictionAccuracy, statistics::units::Ratio::get(),
               "Exact-VA accuracy among committed predicted loads"),
      ADD_STAT(predictionCoverage, statistics::units::Ratio::get(),
               "Predicted fraction of committed eligible loads"),
      ADD_STAT(correctPredictionCoverage, statistics::units::Ratio::get(),
               "Correctly predicted fraction of committed eligible loads"),
      ADD_STAT(selectedPredictionAccuracy, statistics::units::Ratio::get(),
               "Exact-VA accuracy among launched candidate predictions"),
      ADD_STAT(issuedPredictionAccuracy, statistics::units::Ratio::get(),
               "Exact-VA accuracy among issued prefetches resolved at S0"),
      ADD_STAT(launchConversion, statistics::units::Ratio::get(),
               "Candidates launched per generated prediction"),
      ADD_STAT(admissionConversion, statistics::units::Ratio::get(),
               "DCache requests accepted per launched candidate"),
      ADD_STAT(prefetchAccuracy, statistics::units::Ratio::get(),
               "Speculative producer reuses per issued RFP request"),
      ADD_STAT(resolvedPrefetchAccuracy, statistics::units::Ratio::get(),
               "Reuse rate among issued candidates with terminal outcomes"),
      ADD_STAT(committedPrefetchAccuracy, statistics::units::Ratio::get(),
               "Committed reused loads per issued RFP request"),
      ADD_STAT(prefetchCoverage, statistics::units::Ratio::get(),
               "Committed reused fraction of committed eligible loads"),
      ADD_STAT(allLoadPrefetchCoverage, statistics::units::Ratio::get(),
               "Committed reused fraction of all observed committed loads"),
      ADD_STAT(demandReadCoverage, statistics::units::Ratio::get(),
               "Committed eligible DCache-read opportunities served only by "
               "RFP"),
      ADD_STAT(responseUseRate, statistics::units::Ratio::get(),
               "Reused producer loads per successfully published response"),
      ADD_STAT(prefetchByteAccuracy, statistics::units::Ratio::get(),
               "Useful bytes per issued RFP byte"),
      ADD_STAT(resolvedPrefetchByteAccuracy, statistics::units::Ratio::get(),
               "Useful bytes among resolved useful and wasted bytes"),
      ADD_STAT(committedPrefetchByteAccuracy,
               statistics::units::Ratio::get(),
               "Bytes used by committed loads per issued RFP byte"),
      ADD_STAT(prefetchTimeliness, statistics::units::Ratio::get(),
               "Usable-at-S0 fraction among issued predictions reaching "
               "demand S0"),
      ADD_STAT(validationYield, statistics::units::Ratio::get(),
               "S0 validation passes per S0 validation attempt"),
      ADD_STAT(reuseConversion, statistics::units::Ratio::get(),
               "Producer reuses per S0 validation pass"),
      ADD_STAT(consumerBackToBackRate, statistics::units::Ratio::get(),
               "Same-cycle issue fraction of RFP consumer pairs"),
      ADD_STAT(consumerAtFuBackToBackRate, statistics::units::Ratio::get(),
               "Next-cycle FU fraction of live RFP consumer pairs")
{
    using namespace statistics;

    constexpr std::array failure_names = {
        "none", "queue_full", "translation_fault", "translation_invalid",
        "retry_limit", "response_error", "response_invalidate",
        "response_malformed", "publish_fault", "squashed", "preg_recycle",
        "generation", "no_data", "token_mismatch", "address_mismatch",
        "physical_address_mismatch", "flags_mismatch", "forwarding",
        "ordering", "mdp", "store_address_pending", "nuke", "rar_full",
        "raw_full", "local_write", "snoop_invalidate",
        "demand_translation_pending", "demand_fault", "unsupported",
        "normal_completion", "thread_teardown"
    };
    constexpr std::array state_names = {
        "launch_queued", "translating", "cache_queued", "inflight",
        "response_ready", "s0_validated", "reused", "fallback_normal",
        "discarded"
    };
    constexpr std::array deadline_names = {
        "no_candidate", "launch_queued", "translating", "cache_queued",
        "inflight", "response_ready", "s0_validated",
        "terminal_unavailable"
    };
    constexpr std::array recovery_names = {
        "store_load_raw", "load_load_order", "external_snoop"
    };
    static_assert(failure_names.size() ==
                  static_cast<unsigned>(FailureReason::NumReasons));
    static_assert(state_names.size() ==
                  static_cast<unsigned>(State::NumStates));
    static_assert(deadline_names.size() ==
                  static_cast<unsigned>(DeadlineState::NumStates));
    static_assert(recovery_names.size() ==
                  static_cast<unsigned>(NormalRecoveryReason::NumReasons));

    candidateDiscardReason.init(failure_names.size());
    candidateDiscardState.init(state_names.size());
    predictionDeadlineState.init(deadline_names.size());
    normalRecoveryReason.init(recovery_names.size());
    for (unsigned i = 0; i < failure_names.size(); ++i) {
        candidateDiscardReason.subname(i, failure_names[i]);
    }
    for (unsigned i = 0; i < state_names.size(); ++i) {
        candidateDiscardState.subname(i, state_names[i]);
    }
    for (unsigned i = 0; i < deadline_names.size(); ++i) {
        predictionDeadlineState.subname(i, deadline_names[i]);
    }
    for (unsigned i = 0; i < recovery_names.size(); ++i) {
        normalRecoveryReason.subname(i, recovery_names[i]);
    }
    perThreadInflightFullCycles.init(num_threads).flags(total);
    for (ThreadID tid = 0; tid < num_threads; ++tid) {
        perThreadInflightFullCycles.subname(
            tid, "thread" + std::to_string(tid));
    }

    latencyLookupToAdmission.init(0, 255, 8);
    latencyAdmissionToResponse.init(0, 1023, 16);
    latencyResponseToReuse.init(0, 255, 8);
    latencyRenameToConsumerUse.init(0, 255, 8);
    latencyS0ToConsumerIssue.init(0, 255, 8);
    latencyS0ToConsumerFu.init(0, 31, 1);
    latencyLookupToTranslation.init(0, 255, 4);
    latencyTranslation.init(0, 1023, 8);
    latencyTranslationToAdmission.init(0, 1023, 8);
    latencyPredictionToLoadS0.init(0, 1023, 8);
    latencyAdmissionToLoadS0.init(0, 1023, 8);
    latencyResponseToLoadS0.init(0, 1023, 8);
    latencyLoadS0ToResponse.init(0, 1023, 8);
    latencyCandidateLifetime.init(0, 4095, 16);
    retriesPerCandidate.init(
        0, std::max(1U, max_retry_cycles + 1), 1);
    consumerWaitToReady.init(0, 255, 1);
    consumerWaitToCancel.init(0, 255, 1);
    latencyResponseToConsumerIssue.init(0, 255, 1);
    latencyProducerIssueToLoadS0.init(0, 31, 1);
    fanoutPerReusedLoad.init(0, 63, 1);
    translationsPerCycle.init(0, std::max(1U, issue_width), 1);
    admissionAttemptsPerCycle.init(
        0, std::max(1U, candidate_capacity), 1);
    prefetchesIssuedPerCycle.init(0, std::max(1U, issue_width), 1);
    predictionLookahead.init(1, 511, 1);

    prefetchIssued = admissionAccepted;
    prefetchUseful = reused;
    prefetchUnused = admissionAccepted - reused;
    tableHitRate = tableHit / lookup;
    predictionGenerationRate = confidentHit / lookup;
    dynamicPredictionAccuracy = predictionCorrect / predictionResolved;
    committedPredictionAccuracy =
        committedCorrectPredictions / committedPredictedLoads;
    predictionCoverage = committedPredictedLoads / committedEligibleLoads;
    correctPredictionCoverage =
        committedCorrectPredictions / committedEligibleLoads;
    selectedPredictionAccuracy =
        selectedPredictionCorrect / selectedPredictionResolved;
    issuedPredictionAccuracy =
        issuedPredictionCorrect / issuedPredictionResolved;
    launchConversion = launchQueued / confidentHit;
    admissionConversion = admissionAccepted / launchQueued;
    prefetchAccuracy = reused / admissionAccepted;
    resolvedPrefetchAccuracy =
        reused / (reused + issuedTerminalWaste);
    committedPrefetchAccuracy = committedRfpReused / admissionAccepted;
    prefetchCoverage = committedRfpReused / committedEligibleLoads;
    allLoadPrefetchCoverage = committedRfpReused / committedLoads;
    demandReadCoverage = committedDemandReadsAvoided /
        (committedDemandReadsAvoided + committedEligibleNormalDemandLoads);
    responseUseRate = reused / responseDataReady;
    prefetchByteAccuracy = prefetchBytesUseful / prefetchBytesIssued;
    resolvedPrefetchByteAccuracy = prefetchBytesUseful /
        (prefetchBytesUseful + prefetchBytesWasted);
    committedPrefetchByteAccuracy =
        committedRfpBytesUseful / prefetchBytesIssued;
    prefetchTimeliness = prefetchOnTimeAtS0 /
        (prefetchOnTimeAtS0 + prefetchLateAtS0 +
         prefetchUnavailableAtS0);
    validationYield = s0ValidationPass / s0ValidationAttempt;
    reuseConversion = reused / s0ValidationPass;
    consumerBackToBackRate =
        consumerBackToBack / consumerIssuedWithData;
    consumerAtFuBackToBackRate = consumerAtFuBackToBack / consumerAtFu;
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
      generations(params.numThreads, 1),
      streamTrackers(params.numThreads),
      stats(cpu_ptr, params.numThreads, params.rpfIssueWidth,
            params.rpfLaunchQueueEntries + params.rpfMaxInflight,
            params.rpfMaxRetryCycles)
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
    // Eliminated moves/constant folds alias an existing source preg; they do
    // not define a new physical-register value and must retain that preg's RFP
    // producer binding.
    if (!inst->isEliminated()) {
        for (int i = 0; i < inst->numDestRegs(); ++i) {
            const auto destination = inst->renamedDestIdx(i);
            if (destination->isFixedMapping()) {
                continue;
            }
            auto old_owner = pregOwners.find(destination->flatIndex());
            if (old_owner != pregOwners.end()) {
                if (auto *old = findCandidate(old_owner->second)) {
                    const bool validated_recycle =
                        old->state == State::S0Validated;
                    if (validated_recycle) {
                        ++stats.s0OwnerRecycled;
                        DPRINTF(RfpCancel,
                                "[tid:%u] [sn:%llu] token:%llu preg:%u "
                                "recycled by rename [sn:%llu] after S0 "
                                "acceptance\n",
                                old->tid, old->seqNum, old->serial,
                                old->destinationFlatIdx, inst->seqNum);
                    }
                    discardCandidate(
                        *old,
                        validated_recycle ? FailureReason::PregRecycle :
                                            FailureReason::TokenMismatch,
                        false);
                } else {
                    pregOwners.erase(old_owner);
                }
            }
        }
    }

    unsigned size = 0;
    Request::Flags flags;
    if (!eligible(inst, &size, &flags)) {
        if (inst->isLoad()) {
            ++stats.ineligibleLoadRename;
            ++stats.rejectUnsupported;
        }
        return;
    }
    inst->rfpEligibleAtRename = true;
    const ThreadID tid = inst->threadNumber;
    const Addr pc = inst->pcState().instAddr();
    const uint64_t generation = generations.at(tid);
    const uint64_t lookahead =
        streamTrackers.at(tid).onRename(pc, generation, inst->seqNum);
    inst->rfpStreamGeneration = generation;
    inst->rfpStreamLookahead = lookahead;
    ++stats.streamOccurrencesRenamed;
    unsigned stream_occupancy = 0;
    for (const auto &tracker : streamTrackers) {
        stream_occupancy += tracker.size();
    }
    streamHighWatermarkValue = std::max(
        streamHighWatermarkValue, stream_occupancy);
    stats.streamHighWatermark = streamHighWatermarkValue;
    if (issueWidth == 0) {
        return;
    }

    ++stats.lookup;
    auto result = predictors.at(tid)->lookup(
        pc, generation, lookahead, curTick());
    if (result.tableHit) {
        ++stats.tableHit;
    } else {
        ++stats.lookupMiss;
    }
    if (!result.prediction) {
        recordLookupReject(result.reject);
        return;
    }
    ++stats.confidentHit;
    inst->rfpPredictionValid = true;
    inst->rfpPredictedVa = result.prediction->address;
    inst->rfpRenameTick = curTick();
    stats.predictionLookahead.sample(lookahead);
    if (lookahead > 1) {
        ++stats.predictionsBeyondNextOccurrence;
    }

    const Addr predicted = result.prediction->address;
    if (predicted % size != 0) {
        ++stats.rejectMisalignedPrediction;
        ++stats.rejectUnsupported;
        return;
    }
    if (crossesCacheLine(predicted, size, cpu->cacheLineSize())) {
        ++stats.rejectCrossLinePrediction;
        ++stats.rejectUnsupported;
        return;
    }

    unsigned launch_occupancy = 0;
    for (const auto &[serial, candidate] : candidates) {
        if (!candidate->packetInflight &&
            candidate->state != State::ResponseReady &&
            candidate->state != State::S0Validated) {
            ++launch_occupancy;
        }
    }
    if (launch_occupancy >= launchQueueEntries ||
        candidates.size() >= launchQueueEntries + maxInflight) {
        ++stats.launchDroppedQueueFull;
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
    candidate->lookahead = lookahead;
    candidate->producer = inst;
    candidate->pc = pc;
    candidate->predictedVa = predicted;
    candidate->size = size;
    candidate->originalFlags = flags;
    candidate->generation = generation;
    candidate->lookupTick = curTick();

    const uint64_t serial = candidate->serial;
    candidates.emplace(serial, std::move(candidate));
    pregOwners[destination] = serial;
    launchQueue.push_back(serial);
    candidateHighWatermarkValue = std::max(
        candidateHighWatermarkValue,
        static_cast<unsigned>(candidates.size()));
    stats.candidateHighWatermark = candidateHighWatermarkValue;

    inst->rfpTokenSerial = serial;
    inst->rfpPredictionSelected = true;
    inst->rfpUseState = DynInst::RfpUseState::Offered;
    ++stats.launchQueued;
    DPRINTF(RfpPredictor,
            "[tid:%u] [sn:%llu] token:%llu pc:%#lx N:%llu "
            "predicts va:%#lx\n",
            tid, inst->seqNum, serial, pc, lookahead, predicted);
}

void
RegisterPrefetcher::trainCommittedLoad(const DynInstPtr &inst)
{
    if (!enable || !inst || inst->threadNumber >= predictors.size()) {
        return;
    }

    const ThreadID tid = inst->threadNumber;
    if (inst->rfpEligibleAtRename) {
        streamTrackers[tid].onCommit(
            inst->pcState().instAddr(), inst->rfpStreamGeneration,
            inst->seqNum);
        ++stats.streamOccurrencesCommitted;
    }

    ++stats.committedLoads;

    unsigned size = 0;
    Request::Flags flags;
    if (!eligible(inst, &size, &flags) || inst->isSquashed() ||
        inst->getFault() != NoFault || !inst->effAddrValid() ||
        !inst->isNormalLd() || !inst->readPredicate()) {
        return;
    }

    ++stats.committedEligibleLoads;
    if (inst->rfpPredictionValid) {
        ++stats.committedPredictedLoads;
        if (inst->rfpPredictedVa == inst->effAddr) {
            ++stats.committedCorrectPredictions;
        } else {
            ++stats.committedIncorrectPredictions;
        }
    }
    const bool final_rfp_reuse =
        inst->rfpReused && !inst->rfpNormalDemandReadSent;
    if (final_rfp_reuse) {
        ++stats.committedRfpReused;
        stats.committedRfpBytesUseful += size;
        if (inst->rfpConsumerUsed) {
            ++stats.committedRfpReuseWithConsumer;
        } else {
            ++stats.committedRfpReuseWithoutConsumer;
        }
    }
    if (inst->rfpNormalDemandReadSent) {
        ++stats.committedEligibleNormalDemandLoads;
    } else if (final_rfp_reuse) {
        // These sets are intentionally mutually exclusive. A reused load
        // which later replays and sends a normal read did not ultimately
        // avoid that demand transaction.
        ++stats.committedDemandReadsAvoided;
    }

    if (inst->rfpStreamGeneration != generations[tid]) {
        return;
    }

    ++stats.trainSamples;
    auto result = predictors[tid]->train(
        inst->pcState().instAddr(), inst->effAddr,
        inst->rfpStreamGeneration,
        inst->seqNum, curTick());
    stats.trainFirstSample += result.firstSample;
    stats.trainStrideMatch += result.strideMatch;
    stats.trainStrideMismatch += result.strideMismatch;
    stats.trainIllegalStride += result.illegalStride;
    stats.trainStrideChange += result.strideChange;
    stats.trainConfidenceInc += result.confidenceInc;
    stats.trainConfidenceDec += result.confidenceDec;
    stats.entryEvict += result.entryEvict;
    if (inst->rfpPredictionValid &&
        inst->rfpPredictedVa != inst->effAddr && result.strideMatch) {
        ++stats.predictionIncorrectWithStrideMatch;
    }

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
        candidate.generation == inst->rfpStreamGeneration &&
        candidate.lookahead == inst->rfpStreamLookahead &&
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
    candidate.translationStartTick = curTick();
    candidate.translationOutstanding = true;
    ++stats.translationStarted;
    stats.latencyLookupToTranslation.sample(
        cyclesBetween(candidate.lookupTick, candidate.translationStartTick));

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
    if (!candidate || !candidate->translationOutstanding ||
        candidate->translationDelayed) {
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
    candidate->translationDoneTick = curTick();
    if (candidate->translationStartTick != 0) {
        stats.latencyTranslation.sample(cyclesBetween(
            candidate->translationStartTick, candidate->translationDoneTick));
    }

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

void
RegisterPrefetcher::applyWakeAction(
    Candidate &candidate, RfpWakeupState::Action action,
    bool consumers_already_canceled)
{
    if (action == RfpWakeupState::Action::None) {
        return;
    }

    panic_if(!scheduler, "RFP wakeup action has no scheduler");
    if (action == RfpWakeupState::Action::Wake) {
        panic_if(!candidate.wakeup.producerIssued() ||
                     !candidate.wakeup.woken(),
                 "RFP issue-stage wakeup has inconsistent state");
        scheduler->specWakeUpFromRFP(candidate.producer);
        if (candidate.wakeup.hasData()) {
            scheduler->rfpDataReady(candidate.producer);
        }
        ++stats.specWake;
        ++stats.producerIssueWakeOnIssue;
        DPRINTF(RfpRequest,
                "[tid:%u] [sn:%llu] token:%llu wake at producer issue "
                "data:%d\n",
                candidate.tid, candidate.seqNum, candidate.serial,
                candidate.wakeup.hasData());
        return;
    }

    panic_if(action != RfpWakeupState::Action::Retract ||
                 candidate.wakeup.producerIssued() ||
                 candidate.wakeup.woken(),
             "RFP wakeup rollback has inconsistent state");
    ++stats.producerIssueWakeRollback;
    if (!consumers_already_canceled) {
        const bool issued = scheduler->loadCancel(
            candidate.producer, SpeculationSource::RegisterPrefetch);
        if (issued) {
            ++stats.issuedConsumerSquashFallback;
            cpu->squashFromRfp(candidate.producer);
        }
    }
    DPRINTF(RfpCancel,
            "[tid:%u] [sn:%llu] token:%llu retract producer issue wake "
            "consumers_already_canceled:%d\n",
            candidate.tid, candidate.seqNum, candidate.serial,
            consumers_already_canceled);
}

void
RegisterPrefetcher::onProducerIssue(const DynInstPtr &inst)
{
    if (!enable || !inst || inst->isSquashed()) {
        return;
    }
    auto *candidate = findCandidate(inst);
    if (!candidate || candidate->state == State::S0Validated ||
        candidate->state == State::Reused ||
        candidate->state == State::FallbackNormal ||
        candidate->state == State::Discarded ||
        candidate->wakeup.producerIssued()) {
        return;
    }

    candidate->producerIssueTick = curTick();
    ++stats.producerIssueAttempts;
    applyWakeAction(*candidate, candidate->wakeup.onProducerIssue());
}

bool
RegisterPrefetcher::onProducerIssueCanceled(
    const DynInstPtr &inst, bool consumers_already_canceled)
{
    if (!enable || !inst) {
        return false;
    }
    auto *candidate = findCandidate(inst);
    if (!candidate || candidate->state == State::S0Validated ||
        candidate->state == State::Reused ||
        candidate->state == State::FallbackNormal ||
        candidate->state == State::Discarded ||
        !candidate->wakeup.producerIssued()) {
        return false;
    }

    const auto action = candidate->wakeup.onProducerIssueCanceled();
    applyWakeAction(*candidate, action, consumers_already_canceled);
    candidate->producerIssueTick = 0;
    return action == RfpWakeupState::Action::Retract;
}

bool
RegisterPrefetcher::sendCandidate(Candidate &candidate)
{
    if (candidate.state != State::CacheQueued) {
        return false;
    }
    if (inflight >= maxInflight) {
        ++stats.rejectDemandPriority;
        ++stats.rejectGlobalInflightFull;
        return false;
    }
    if (perThreadInflight[candidate.tid] >= perThreadMaxInflight) {
        ++stats.rejectDemandPriority;
        ++stats.rejectThreadInflightFull;
        return false;
    }

    if (!candidate.packet) {
        candidate.packet = new Packet(candidate.request, MemCmd::ReadReq);
        candidate.packet->dataStatic(candidate.data.data());
        candidate.packet->senderState = candidate.senderState.get();
    }

    ++stats.admissionAttempt;
    ++admissionAttemptsThisCycle;
    const auto result = lsq->trySendRfpPacket(
        candidate.packet, candidate.predictedVa, candidate.size);
    if (result == LSQ::RfpDcacheSendResult::Accepted) {
        candidate.state = State::Inflight;
        candidate.packetInflight = true;
        candidate.admissionTick = curTick();
        candidate.everAdmitted = true;
        ++inflight;
        ++perThreadInflight[candidate.tid];
        ++stats.admissionAccepted;
        ++prefetchesIssuedThisCycle;
        stats.prefetchBytesIssued += candidate.size;
        stats.latencyLookupToAdmission.sample(
            cyclesBetween(candidate.lookupTick, curTick()));
        if (candidate.translationDoneTick != 0) {
            stats.latencyTranslationToAdmission.sample(cyclesBetween(
                candidate.translationDoneTick, candidate.admissionTick));
        }
        candidate.producer->rfpPrefetchIssued = true;
        candidate.producer->rfpPrefetchAdmissionTick = curTick();
        candidate.producer->rfpUseState = DynInst::RfpUseState::Launched;
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

    cleanupTerminalCandidates();
    admissionAttemptsThisCycle = 0;
    prefetchesIssuedThisCycle = 0;

    unsigned active = 0;
    unsigned launch_queued = 0;
    unsigned translating = 0;
    unsigned cache_queued = 0;
    unsigned response_ready = 0;
    unsigned s0_validated = 0;
    unsigned terminal_outstanding = 0;
    unsigned live_issued = 0;
    for (const auto &[serial, candidate] : candidates) {
        const bool terminal = candidate->state == State::Reused ||
            candidate->state == State::FallbackNormal ||
            candidate->state == State::Discarded;
        active += !terminal;
        launch_queued += candidate->state == State::LaunchQueued;
        translating += candidate->state == State::Translating;
        cache_queued += candidate->state == State::CacheQueued;
        response_ready += candidate->state == State::ResponseReady;
        s0_validated += candidate->state == State::S0Validated;
        terminal_outstanding += terminal &&
            (candidate->packetInflight || candidate->translationOutstanding);
        live_issued += candidate->everAdmitted && !terminal;
    }
    stats.inflightOccupancy = inflight;
    stats.candidateStorageOccupancy = candidates.size();
    stats.activeCandidateOccupancy = active;
    stats.launchQueueOccupancy = launch_queued;
    stats.translatingOccupancy = translating;
    stats.cacheQueuedOccupancy = cache_queued;
    stats.responseReadyOccupancy = response_ready;
    stats.s0ValidatedOccupancy = s0_validated;
    stats.terminalOutstandingOccupancy = terminal_outstanding;
    unsigned stream_occupancy = 0;
    for (const auto &tracker : streamTrackers) {
        stream_occupancy += tracker.size();
    }
    stats.streamOccurrenceOccupancy = stream_occupancy;
    for (ThreadID tid = 0; tid < perThreadInflight.size(); ++tid) {
        if (perThreadInflight[tid] >= perThreadMaxInflight) {
            ++stats.perThreadInflightFullCycles[tid];
        }
    }

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
    unsigned queued_after_translation = 0;
    for (const auto &[serial, candidate] : candidates) {
        queued_after_translation +=
            candidate->state == State::LaunchQueued;
    }
    stats.translationWidthDeferred += queued_after_translation;

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
    unsigned examined = 0;
    for (auto *candidate : ready) {
        if (issued >= issueWidth) {
            break;
        }
        ++examined;
        if (sendCandidate(*candidate)) {
            ++issued;
        }
    }

    live_issued = 0;
    unsigned active_after = 0;
    for (const auto &[serial, candidate] : candidates) {
        const bool terminal = candidate->state == State::Reused ||
            candidate->state == State::FallbackNormal ||
            candidate->state == State::Discarded;
        active_after += !terminal;
        live_issued += candidate->everAdmitted && !terminal;
    }
    if (issued >= issueWidth) {
        stats.admissionWidthDeferred += ready.size() - examined;
    }
    stats.translationsPerCycle.sample(translations);
    stats.admissionAttemptsPerCycle.sample(admissionAttemptsThisCycle);
    stats.prefetchesIssuedPerCycle.sample(prefetchesIssuedThisCycle);
    stats.liveCandidates = active_after;
    stats.liveIssuedCandidates = live_issued;
    stats.inflightAtDump = inflight;

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
    const bool well_formed_data = pkt->hasData() &&
        pkt->getSize() == candidate->size &&
        pkt->getAddr() == candidate->translatedPa;
    if (well_formed_data) {
        stats.prefetchBytesReceived += candidate->size;
        if (candidate->producer->rfpLoadS0Observed) {
            ++stats.responseSlow;
            stats.latencyLoadS0ToResponse.sample(cyclesBetween(
                candidate->producer->rfpLoadS0Tick, curTick()));
        } else {
            ++stats.responseFast;
        }
    }

    const bool orphaned = candidate->orphaned ||
        candidate->state != State::Inflight ||
        candidate->producer->isSquashed() ||
        candidate->generation != generations[candidate->tid];
    if (pkt->isInvalidate()) {
        invalidateLine(pkt->getAddr(), candidate->serial,
                       FailureReason::ResponseInvalidate);
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
    } else if (!well_formed_data) {
        ++stats.responseMalformed;
        discardCandidate(*candidate, FailureReason::ResponseMalformed,
                         true);
    } else {
        std::copy_n(pkt->getConstPtr<uint8_t>(), candidate->size,
                    candidate->data.begin());
        candidate->responseTick = curTick();
        stats.latencyAdmissionToResponse.sample(
            cyclesBetween(candidate->admissionTick, curTick()));

        const Fault publish_fault =
            candidate->producer->publishRfpValue(pkt);
        if (publish_fault != NoFault) {
            ++stats.responsePublishFault;
            discardCandidate(*candidate, FailureReason::PublishFault, true);
        } else {
            candidate->responseHasData = true;
            candidate->state = State::ResponseReady;
            candidate->producer->rfpDataPublished = true;
            candidate->producer->rfpUseState =
                DynInst::RfpUseState::DataReady;
            ++stats.responseDataReady;
            candidate->wakeup.onDataReady();
            if (candidate->wakeup.woken()) {
                ++stats.responseDataReadyAfterProducerIssue;
                scheduler->rfpDataReady(candidate->producer);
            }
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

    ++stats.snoopInvalidateEvents;
    invalidateLine(pkt->getAddr(), 0, FailureReason::SnoopInvalidate);
}

void
RegisterPrefetcher::invalidateLine(
    Addr address, uint64_t excluded_serial, FailureReason reason)
{

    const Addr block_mask = ~(static_cast<Addr>(cpu->cacheLineSize()) - 1);
    const Addr invalidated = address & block_mask;
    for (auto &[serial, candidate] : candidates) {
        if (serial != excluded_serial && candidate->request &&
            candidate->request->hasPaddr() &&
            (candidate->translatedPa & block_mask) == invalidated &&
            candidate->state != State::Reused &&
            candidate->state != State::S0Validated &&
            candidate->state != State::Discarded &&
            candidate->state != State::FallbackNormal) {
            if (reason == FailureReason::SnoopInvalidate) {
                ++stats.snoopCandidatesInvalidated;
            }
            discardCandidate(*candidate, reason, true);
        }
    }
}

void
RegisterPrefetcher::observeLocalWrite(Addr address, unsigned size)
{
    if (!enable || size == 0) {
        return;
    }
    ++stats.localWriteEvents;

    const Addr block_mask = ~(static_cast<Addr>(cpu->cacheLineSize()) - 1);
    const Addr first_block = address & block_mask;
    const Addr max_addr = std::numeric_limits<Addr>::max();
    const Addr last_byte = address > max_addr - (size - 1) ?
        max_addr : address + size - 1;
    const Addr last_block = last_byte & block_mask;

    for (auto &[serial, candidate] : candidates) {
        const bool read_admitted = candidate->state == State::Inflight ||
            candidate->state == State::ResponseReady;
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
        discardCandidate(*candidate, FailureReason::LocalWrite, true);
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
        candidate.state != State::ResponseReady) {
        *failure = FailureReason::NoData;
        return false;
    }
    *failure = FailureReason::None;
    return true;
}

bool
RegisterPrefetcher::hasCandidateForS0(const DynInstPtr &inst) const
{
    return enable && inst && inst->rfpTokenSerial != 0 &&
        !inst->rfpS0Attempted;
}

RegisterPrefetcher::DeadlineState
RegisterPrefetcher::deadlineState(const Candidate *candidate) const
{
    if (!candidate) {
        return DeadlineState::NoCandidate;
    }
    switch (candidate->state) {
      case State::LaunchQueued:
        return DeadlineState::LaunchQueued;
      case State::Translating:
        return DeadlineState::Translating;
      case State::CacheQueued:
        return DeadlineState::CacheQueued;
      case State::Inflight:
        return DeadlineState::Inflight;
      case State::ResponseReady:
        return DeadlineState::ResponseReady;
      case State::S0Validated:
        return DeadlineState::S0Validated;
      case State::Reused:
      case State::FallbackNormal:
      case State::Discarded:
        return DeadlineState::TerminalUnavailable;
      case State::NumStates:
        break;
    }
    panic("Unknown RFP candidate state at the demand deadline");
}

void
RegisterPrefetcher::observeLoadS0(const DynInstPtr &inst)
{
    if (!enable || !inst || !inst->rfpEligibleAtRename ||
        inst->rfpLoadS0Observed) {
        return;
    }
    inst->rfpLoadS0Observed = true;
    inst->rfpLoadS0Tick = curTick();
    ++stats.eligibleLoadS0;

    if (!inst->rfpPredictionValid) {
        return;
    }

    const Candidate *candidate = inst->rfpTokenSerial == 0 ? nullptr :
        findCandidate(inst->rfpTokenSerial);
    if (candidate && candidate->producerIssueTick != 0) {
        stats.latencyProducerIssueToLoadS0.sample(cyclesBetween(
            candidate->producerIssueTick, curTick()));
    }
    const DeadlineState state = !candidate && inst->rfpPredictionSelected ?
        DeadlineState::TerminalUnavailable : deadlineState(candidate);
    ++stats.predictionDeadlineState[
        static_cast<unsigned>(state)];
    const bool usable_at_s0 = candidate && candidate->responseHasData &&
        candidate->state == State::ResponseReady;
    if (usable_at_s0) {
        ++stats.prefetchOnTimeAtS0;
        stats.latencyResponseToLoadS0.sample(cyclesBetween(
            candidate->responseTick, curTick()));
    } else if (candidate && candidate->state == State::Inflight) {
        ++stats.prefetchLateAtS0;
    } else if (inst->rfpPrefetchIssued) {
        ++stats.prefetchUnavailableAtS0;
    } else {
        ++stats.prefetchNotIssuedAtS0;
    }
    stats.latencyPredictionToLoadS0.sample(
        cyclesBetween(inst->rfpRenameTick, curTick()));
    if (inst->rfpPrefetchIssued) {
        stats.latencyAdmissionToLoadS0.sample(cyclesBetween(
            inst->rfpPrefetchAdmissionTick, curTick()));
    }

    if (!inst->effAddrValid()) {
        return;
    }
    const bool prediction_correct = inst->rfpPredictedVa == inst->effAddr;
    ++stats.predictionResolved;
    if (prediction_correct) {
        ++stats.predictionCorrect;
    } else {
        ++stats.predictionIncorrect;
    }
    const Addr block_mask =
        ~(static_cast<Addr>(cpu->cacheLineSize()) - 1);
    if ((inst->rfpPredictedVa & block_mask) ==
        (inst->effAddr & block_mask)) {
        ++stats.predictionLineCorrect;
    }
    if (inst->rfpPredictionSelected) {
        ++stats.selectedPredictionResolved;
        if (prediction_correct) {
            ++stats.selectedPredictionCorrect;
        } else {
            ++stats.selectedPredictionIncorrect;
        }
    }
    if (inst->rfpPrefetchIssued) {
        ++stats.issuedPredictionResolved;
        if (prediction_correct) {
            ++stats.issuedPredictionCorrect;
        } else {
            ++stats.issuedPredictionIncorrect;
        }
    }
}

void
RegisterPrefetcher::recordNormalDemandRead(
    const DynInstPtr &inst, unsigned size)
{
    if (!enable || !inst || !inst->rfpEligibleAtRename || size == 0) {
        return;
    }
    if (!inst->rfpNormalDemandReadSent) {
        inst->rfpNormalDemandReadSent = true;
        ++stats.normalEligibleDemandLoads;
    }
    ++stats.normalEligibleDemandPackets;
    stats.normalEligibleDemandBytes += size;
}

bool
RegisterPrefetcher::acceptAtS0(
    const DynInstPtr &inst, const RequestPtr &normal_req,
    bool rar_reserved, bool raw_reserved)
{
    if (!hasCandidateForS0(inst)) {
        return false;
    }

    inst->rfpS0Attempted = true;
    ++stats.s0ValidationAttempt;

    auto *candidate = findCandidate(inst->rfpTokenSerial);
    if (!candidate || !validateIdentity(*candidate, inst)) {
        ++stats.s0RejectIdentity;
        ++stats.cancelTokenMismatch;
        if (candidate) {
            discardCandidate(*candidate, FailureReason::TokenMismatch, true);
        } else {
            inst->rfpTokenSerial = 0;
            inst->rfpUseState = DynInst::RfpUseState::Invalid;
        }
        return false;
    }

    panic_if(!candidate->wakeup.producerIssued() ||
                 !candidate->wakeup.woken() || !inst->isIssued(),
             "RFP S0 validation preceded producer issue: sn=%llu",
             inst->seqNum);

    FailureReason failure = FailureReason::None;
    if (!validateAddressAndAttributes(
            *candidate, inst, normal_req, &failure)) {
        switch (failure) {
          case FailureReason::AddressMismatch:
            ++stats.s0RejectAddress;
            ++stats.validationFailVa;
            ++stats.cancelAddressMismatch;
            break;
          case FailureReason::PhysicalAddressMismatch:
            ++stats.s0RejectAddress;
            ++stats.validationFailPa;
            break;
          case FailureReason::FlagsMismatch:
            ++stats.s0RejectAttributes;
            ++stats.validationFailFlags;
            break;
          case FailureReason::Generation:
            ++stats.s0RejectIdentity;
            ++stats.cancelGenerationMismatch;
            break;
          case FailureReason::NoData:
            ++stats.s0RejectNoData;
            ++stats.cancelNoData;
            break;
          default:
            ++stats.s0RejectIdentity;
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
    candidate->state = State::S0Validated;
    inst->rfpReusePending = true;
    inst->rfpValidationPassed = true;
    inst->rfpS0Validated = true;
    inst->rfpS0ValidationTick = curTick();
    inst->rfpUseState = DynInst::RfpUseState::S0Validated;
    ++stats.validationPass;
    ++stats.s0ValidationPass;
    if (rar_reserved) {
        ++stats.s0RarReserved;
    }
    if (raw_reserved) {
        ++stats.s0RawReserved;
    }
    DPRINTF(RfpValidate,
            "[tid:%u] [sn:%llu] token:%llu final S0 validation passed "
            "rar:%d raw:%d\n",
            candidate->tid, candidate->seqNum, candidate->serial,
            rar_reserved, raw_reserved);
    return true;
}

void
RegisterPrefetcher::rejectAtS0(
    const DynInstPtr &inst, S0RejectReason reason)
{
    if (!hasCandidateForS0(inst)) {
        return;
    }

    inst->rfpS0Attempted = true;
    ++stats.s0ValidationAttempt;

    auto *candidate = findCandidate(inst->rfpTokenSerial);
    if (!candidate || !validateIdentity(*candidate, inst)) {
        ++stats.s0RejectIdentity;
        ++stats.cancelTokenMismatch;
        if (candidate) {
            discardCandidate(*candidate, FailureReason::TokenMismatch, true);
        } else {
            inst->rfpTokenSerial = 0;
            inst->rfpUseState = DynInst::RfpUseState::Invalid;
        }
        return;
    }

    FailureReason failure = FailureReason::NormalCompletion;
    switch (reason) {
      case S0RejectReason::TranslationPending:
        ++stats.s0RejectTranslationPending;
        failure = FailureReason::DemandTranslationPending;
        break;
      case S0RejectReason::Fault:
        ++stats.s0RejectFault;
        failure = FailureReason::DemandFault;
        break;
      case S0RejectReason::Unsupported:
        ++stats.s0RejectUnsupported;
        failure = FailureReason::Unsupported;
        break;
      case S0RejectReason::Forwarding:
        ++stats.s0RejectForwarding;
        ++stats.validationFailForwarding;
        failure = FailureReason::Forwarding;
        break;
      case S0RejectReason::Mdp:
        ++stats.s0RejectMdp;
        ++stats.validationFailMdp;
        ++stats.cancelOrderingConflict;
        failure = FailureReason::Mdp;
        break;
      case S0RejectReason::StoreAddressPending:
        ++stats.s0RejectStoreAddressPending;
        ++stats.cancelOrderingConflict;
        failure = FailureReason::StoreAddressPending;
        break;
      case S0RejectReason::Nuke:
        ++stats.s0RejectNuke;
        ++stats.validationFailNuke;
        ++stats.cancelOrderingConflict;
        failure = FailureReason::Nuke;
        break;
      case S0RejectReason::RarFull:
        ++stats.s0RejectRarFull;
        ++stats.validationFailRarRaw;
        ++stats.cancelOrderingConflict;
        failure = FailureReason::RarFull;
        break;
      case S0RejectReason::RawFull:
        ++stats.s0RejectRawFull;
        ++stats.validationFailRarRaw;
        ++stats.cancelOrderingConflict;
        failure = FailureReason::RawFull;
        break;
    }

    discardCandidate(*candidate, failure, true);
}

void
RegisterPrefetcher::completeReuse(const DynInstPtr &inst)
{
    auto *candidate = findCandidate(inst);
    panic_if(!candidate || !inst->rfpValidationPassed ||
             !inst->rfpS0Validated || !inst->rfpDataPublished ||
             candidate->state != State::S0Validated,
             "Completing an RFP reuse without a validated candidate");

    candidate->state = State::Reused;
    inst->rfpReusePending = false;
    inst->rfpReused = true;
    inst->rfpUseState = DynInst::RfpUseState::Reused;
    ++stats.reused;
    ++stats.duplicateDemandAvoided;
    stats.prefetchBytesUseful += candidate->size;
    stats.latencyCandidateLifetime.sample(
        cyclesBetween(candidate->lookupTick, curTick()));
    stats.retriesPerCandidate.sample(candidate->retryCycles);
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
    if (inst->rfpNormalRecoveryPending) {
        // A normal memory-order redirect owns recovery. Keep the RFP operand
        // live until the architectural squash removes producer and consumers.
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
    if (!candidate || !candidate->wakeup.woken() || candidate->tid != tid ||
        candidate->seqNum >= consumer_seq || candidate->orphaned) {
        return OperandStatus::Uncontrolled;
    }
    if (candidate->consumerGateSeen.insert(consumer_seq).second) {
        ++stats.consumerPairsSeenAtGate;
        if (!candidate->producer->rfpLoadS0Observed) {
            ++stats.consumerGateBeforeS0;
        }
    }

    if (producer) {
        *producer = candidate->producer;
    }
    if (candidate->producer->rfpDataPublished &&
        candidate->producer->rfpS0Validated) {
        return OperandStatus::Ready;
    }

    auto wait_it = candidate->consumerWaitStart.find(consumer_seq);
    const Tick wait_start = wait_it == candidate->consumerWaitStart.end() ?
        curTick() : wait_it->second;
    if (reuseMaxWaitCycles > 0 &&
        cyclesBetween(wait_start, curTick()) < reuseMaxWaitCycles) {
        if (wait_it == candidate->consumerWaitStart.end()) {
            candidate->consumerWaitStart.emplace(consumer_seq, curTick());
            ++stats.consumerPairsWaited;
        }
        ++stats.consumerWait;
        return OperandStatus::Waiting;
    }
    if (candidate->canceledConsumers.insert(consumer_seq).second) {
        ++stats.consumerPairsCanceled;
        if (wait_it != candidate->consumerWaitStart.end()) {
            ++stats.consumerPairsWaitedCanceled;
            stats.consumerWaitToCancel.sample(
                cyclesBetween(wait_it->second, curTick()));
        }
    }
    return OperandStatus::Cancel;
}

void
RegisterPrefetcher::recordConsumerUse(
    const DynInstPtr &producer, const DynInstPtr &consumer)
{
    panic_if(!consumer, "Recording an RFP use for a null consumer");
    auto *candidate = findCandidate(producer);
    panic_if(!candidate || !candidate->wakeup.woken() ||
                 !producer->rfpDataPublished ||
                 !producer->rfpS0Validated ||
                 !producer->rfpValidationPassed,
             "Recording an unvalidated RFP consumer");
    if (candidate->issuedConsumers.insert(consumer->seqNum).second) {
        producer->rfpConsumerUsed = true;
        ++stats.consumerIssuedWithData;
        auto wait = candidate->consumerWaitStart.find(consumer->seqNum);
        if (wait != candidate->consumerWaitStart.end()) {
            ++stats.consumerPairsWaitedToReady;
            stats.consumerWaitToReady.sample(
                cyclesBetween(wait->second, curTick()));
        }
        stats.latencyRenameToConsumerUse.sample(
            cyclesBetween(producer->rfpRenameTick, curTick()));
        const auto s0_to_issue = cyclesBetween(
            producer->rfpS0ValidationTick, curTick());
        stats.latencyS0ToConsumerIssue.sample(s0_to_issue);
        if (s0_to_issue == 0) {
            ++stats.consumerBackToBack;
        }
        if (candidate->responseTick != 0) {
            stats.latencyResponseToConsumerIssue.sample(cyclesBetween(
                candidate->responseTick, curTick()));
        }
        DPRINTF(RfpValidate,
                "producer [sn:%llu] S0 -> consumer [sn:%llu] issue gate in "
                "%llu cycles\n",
                producer->seqNum, consumer->seqNum, s0_to_issue);
        if (!consumer->rfpProducerUses) {
            consumer->rfpProducerUses =
                std::make_unique<std::vector<DynInst::RfpProducerUse>>();
        }
        consumer->rfpProducerUses->push_back(
            {producer->seqNum, producer->rfpS0ValidationTick});
    }
}

void
RegisterPrefetcher::recordConsumerAtFu(const DynInstPtr &consumer)
{
    if (!enable || !consumer || !consumer->rfpProducerUses) {
        return;
    }
    auto producer_uses = std::move(consumer->rfpProducerUses);
    if (consumer->isSquashed()) {
        return;
    }
    for (const auto &use : *producer_uses) {
        ++stats.consumerAtFu;
        const auto s0_to_fu = cyclesBetween(use.s0ValidationTick, curTick());
        panic_if(s0_to_fu == 0,
                 "RFP consumer reached its FU before the cycle after S0");
        stats.latencyS0ToConsumerFu.sample(s0_to_fu);
        if (s0_to_fu == 1) {
            ++stats.consumerAtFuBackToBack;
        }
        DPRINTF(RfpValidate,
                "producer [sn:%llu] S0 -> consumer [sn:%llu] FU in %llu "
                "cycles\n",
                use.producerSeqNum, consumer->seqNum, s0_to_fu);
    }
}

void
RegisterPrefetcher::recordNormalMemOrderSquash(
    const DynInstPtr &producer, NormalRecoveryReason reason)
{
    if (!enable || !producer ||
        (!producer->rfpS0Validated && !producer->rfpReused)) {
        return;
    }
    if (producer->rfpNormalRecoveryPending) {
        return;
    }

    ++stats.normalMemOrderSquashAfterRfp;
    ++stats.normalRecoveryReason[static_cast<unsigned>(reason)];
    // Do not clear the preg owner or the speculative scoreboards yet. The
    // redirect is not visible to every issue stage until the normal pipeline
    // squash runs, and younger consumers may still reach their final gate in
    // this cycle. They may execute transiently and are recovered exactly like
    // consumers of an ordinary misspeculated load.
    producer->rfpNormalRecoveryPending = true;
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
        inst->rfpS0Validated = false;
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

    if (candidate.state == State::S0Validated &&
        reason != FailureReason::Squashed &&
        reason != FailureReason::PregRecycle &&
        reason != FailureReason::ThreadTeardown) {
        ++stats.postS0RfpInvariantFailure;
        panic_if(enableDebugTrace,
                 "RFP-specific rejection after S0 acceptance: sn=%llu "
                 "reason=%u",
                 candidate.seqNum, static_cast<unsigned>(reason));
    }

    const State prior_state = candidate.state;
    candidate.failure = reason;
    ++stats.candidateDiscarded;
    ++stats.candidateDiscardReason[static_cast<unsigned>(reason)];
    ++stats.candidateDiscardState[static_cast<unsigned>(prior_state)];
    stats.latencyCandidateLifetime.sample(
        cyclesBetween(candidate.lookupTick, curTick()));
    stats.retriesPerCandidate.sample(candidate.retryCycles);
    if (candidate.everAdmitted) {
        ++stats.issuedTerminalWaste;
        stats.prefetchBytesWasted += candidate.size;
        if (reason == FailureReason::Squashed) {
            ++stats.wrongPathPrefetchIssued;
        }
    }
    if (candidate.responseHasData) {
        ++stats.responseReadyTerminalWaste;
    }
    for (const auto &[consumer_seq, wait_tick] :
         candidate.consumerWaitStart) {
        if (candidate.issuedConsumers.count(consumer_seq) == 0 &&
            candidate.canceledConsumers.insert(consumer_seq).second) {
            ++stats.consumerPairsCanceled;
            ++stats.consumerPairsWaitedCanceled;
            stats.consumerWaitToCancel.sample(
                cyclesBetween(wait_tick, curTick()));
        }
    }
    candidate.orphaned = candidate.packetInflight ||
                         candidate.translationOutstanding;
    const bool fallback = candidate.wakeup.woken() ||
        candidate.state == State::ResponseReady ||
        candidate.state == State::S0Validated;
    candidate.state = fallback ? State::FallbackNormal : State::Discarded;
    if (fallback) {
        ++stats.fallbackNormal;
    }

    if (candidate.wakeup.woken() && scheduler) {
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
                if (!candidate.fanoutSampled) {
                    stats.fanoutPerReusedLoad.sample(
                        candidate.issuedConsumers.size());
                    candidate.fanoutSampled = true;
                }
                for (const auto &[consumer_seq, wait_tick] :
                     candidate.consumerWaitStart) {
                    if (candidate.issuedConsumers.count(consumer_seq) == 0 &&
                        candidate.canceledConsumers.count(consumer_seq) == 0) {
                        ++stats.consumerPairsWaitExpired;
                        stats.consumerWaitToCancel.sample(
                            cyclesBetween(wait_tick, curTick()));
                    }
                }
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
        panic_if(candidate.responseHasData != candidate.wakeup.hasData(),
                 "RFP response state disagrees with wakeup data state");
        panic_if(candidate.wakeup.producerIssued() !=
                     candidate.wakeup.woken(),
                 "RFP producer issue and wake state disagree");
        panic_if(candidate.wakeup.woken() &&
                     candidate.producerIssueTick == 0,
                 "RFP issue wake has no producer issue timestamp");

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
            candidate.state == State::S0Validated ||
            candidate.state == State::Reused;
        panic_if(value_state &&
                     (!candidate.responseHasData ||
                      !candidate.producer->rfpDataPublished),
                 "RFP value state has no published response data");
        panic_if(candidate.producer->rfpValidationPassed !=
                     candidate.producer->rfpS0Validated,
                 "RFP legacy validation flag disagrees with S0 state");
        panic_if(candidate.state == State::S0Validated &&
                     (!candidate.wakeup.woken() ||
                      !candidate.producer->isIssued() ||
                      !candidate.producer->rfpReusePending ||
                      !candidate.producer->rfpS0Validated),
                 "RFP S0 validation state has no pending producer");
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
    for (const auto &tracker : streamTrackers) {
        tracker.checkInvariants();
    }
}

void
RegisterPrefetcher::squash(ThreadID tid, InstSeqNum squash_seq_num)
{
    if (!enable) {
        return;
    }
    stats.streamOccurrencesSquashed +=
        streamTrackers.at(tid).squash(squash_seq_num);
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
        if (candidate->tid == tid &&
            candidate->state != State::S0Validated &&
            candidate->state != State::Reused) {
            discardCandidate(*candidate, FailureReason::Generation, true);
        }
    }
}

void
RegisterPrefetcher::flushThreadForTeardown(ThreadID tid)
{
    if (!enable || tid >= generations.size()) {
        return;
    }

    ++generations[tid];
    if (generations[tid] == 0) {
        generations[tid] = 1;
    }

    for (auto &[serial, candidate] : candidates) {
        if (candidate->tid != tid || candidate->state == State::Reused) {
            continue;
        }
        // Thread teardown owns all younger consumers, so no selective recovery
        // may outlive removal of the pipeline state.
        discardCandidate(*candidate, FailureReason::ThreadTeardown, false);
    }

    launchQueue.erase(
        std::remove_if(
            launchQueue.begin(), launchQueue.end(),
            [this, tid](uint64_t serial) {
                const auto *candidate = findCandidate(serial);
                return candidate && candidate->tid == tid;
            }),
        launchQueue.end());

    // Reused candidates are already terminal. This also erases every other
    // terminal candidate which has no outstanding translation or cache packet.
    cleanupTerminalCandidates();
    stats.streamOccurrencesTeardown += streamTrackers[tid].size();
    streamTrackers[tid].reset();
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
        streamTrackers[tid].reset();
        ++generations[tid];
    }
}

bool
RegisterPrefetcher::isDrained() const
{
    return !enable ||
        (candidates.empty() && inflight == 0 &&
         std::all_of(streamTrackers.begin(), streamTrackers.end(),
                     [](const auto &tracker) { return tracker.empty(); }));
}

void
RegisterPrefetcher::drainSanityCheck() const
{
    panic_if(!isDrained(), "RFP still owns candidates while CPU is drained");
}

} // namespace o3
} // namespace gem5
