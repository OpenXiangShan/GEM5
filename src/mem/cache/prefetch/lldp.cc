#include "mem/cache/prefetch/lldp.hh"

#include <algorithm>
#include <limits>
#include <set>

#include "base/output.hh"
#include "cpu/base.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/LLDPrefetcher.hh"
#include "params/LLDPrefetcher.hh"

namespace gem5
{
namespace prefetch
{
LLDPrefetcher::LLDPStats::LLDPStats(statistics::Group *parent)
    : statistics::Group(parent, "lldp"),
      ADD_STAT(trainAccepted, statistics::units::Count::get(), "Accepted dependency trains"),
      ADD_STAT(trainDropped, statistics::units::Count::get(), "Training FIFO overflows"),
      ADD_STAT(trainSquashed, statistics::units::Count::get(), "Squashed training requests"),
      ADD_STAT(dualSourceTrainRejected, statistics::units::Count::get(),
               "Dependency trains excluded because their chains contain dual-register operations"),
      ADD_STAT(producerWrites, statistics::units::Count::get(), "LLDT producer allocations and updates"),
      ADD_STAT(producerReplacements, statistics::units::Count::get(), "LLDT producer evictions"),
      ADD_STAT(producerMatches, statistics::units::Count::get(), "Load PC matches in LLDT"),
      ADD_STAT(pipelineBypasses, statistics::units::Count::get(), "S1 revalidation after older S2 writes"),
      ADD_STAT(spatialLoadTrain, statistics::units::Count::get(), "Spatial-prefetch loadTrain inputs"),
      ADD_STAT(spatialHints, statistics::units::Count::get(), "Hints produced for spatial prefetches"),
      ADD_STAT(lengthChanges, statistics::units::Count::get(), "Consumer chain length changes"),
      ADD_STAT(opChanges, statistics::units::Count::get(), "Consumer operation changes"),
      ADD_STAT(immChanges, statistics::units::Count::get(), "Consumer arithmetic or load immediate changes"),
      ADD_STAT(exactImmStable, statistics::units::Count::get(), "Exact immediate repeats"),
      ADD_STAT(lineImmStable, statistics::units::Count::get(), "Cacheline immediate repeats"),
      ADD_STAT(offsetImmStable, statistics::units::Count::get(), "Byte-offset immediate repeats"),
      ADD_STAT(sameLineImmUpdates, statistics::units::Count::get(), "Immediate updates on the same line"),
      ADD_STAT(nextLineImmUpdates, statistics::units::Count::get(), "Immediate updates on the next line"),
      ADD_STAT(otherLineImmUpdates, statistics::units::Count::get(), "Immediate updates on another line"),
      ADD_STAT(immValueHist, statistics::units::Count::get(), "Immediate delta histogram"),
      ADD_STAT(lineDeltaHist, statistics::units::Count::get(), "Cacheline delta histogram"),
      ADD_STAT(offsetDeltaHist, statistics::units::Count::get(), "Byte-offset delta histogram"),
      ADD_STAT(byteOffsetHist, statistics::units::Count::get(), "Load-size and byte-offset histogram"),
      ADD_STAT(hints, statistics::units::Count::get(),
               "Hints retained for miss return or LLDP-prefetch hit data"),
      ADD_STAT(hitHintsDiscarded, statistics::units::Count::get(), "Hints discarded on cache hits"),
      ADD_STAT(hitHintsRetained, statistics::units::Count::get(),
               "LLDP-prefetch cache-hit hints retained for chaining"),
      ADD_STAT(returnedHints, statistics::units::Count::get(),
               "Requests supplying cacheline data for a retained hint"),
      ADD_STAT(staleHints, statistics::units::Count::get(),
               "Hints invalidated by an LLDT producer or consumer update"),
      ADD_STAT(staleHintConsumers, statistics::units::Count::get(),
               "Hint consumer chains invalidated by LLDT replacement"),
      ADD_STAT(candidates, statistics::units::Count::get(), "LLDP virtual address candidates"),
      ADD_STAT(filtered, statistics::units::Count::get(), "Candidates filtered by recent TLB-line history"),
      ADD_STAT(duplicates, statistics::units::Count::get(), "Duplicate candidate cache lines"),
      ADD_STAT(unsupported, statistics::units::Count::get(), "Chains or data unsafe to replay"),
      ADD_STAT(samplerOutputsToMeta, statistics::units::Count::get(),
               "Stable address pairs emitted from SamplerTable to MetaTable"),
      ADD_STAT(metaTableHits, statistics::units::Count::get(),
               "loadTrain addresses hitting MetaTable"),
      ADD_STAT(metaTablePrefetches, statistics::units::Count::get(),
               "Prefetches queued directly from MetaTable"),
      ADD_STAT(samplerValidEntries, statistics::units::Count::get(),
               "Current valid SamplerTable entries"),
      ADD_STAT(metaValidEntries, statistics::units::Count::get(),
               "Current valid MetaTable entries"),
      ADD_STAT(samplerTargetMismatch, statistics::units::Count::get(),
               "Sampler mappings that changed target address"),
      ADD_STAT(samplerRepromotions, statistics::units::Count::get(),
               "Sampler stable mappings refreshed into MetaTable"),
      ADD_STAT(metaInvalidations, statistics::units::Count::get(),
               "MetaTable entries invalidated or replaced by feedback"),
      ADD_STAT(metaTargetSwitches, statistics::units::Count::get(),
               "MetaTable entries installed with a new target address"),
      ADD_STAT(metaTokenStalls, statistics::units::Count::get(),
               "MetaTable lookups blocked by token or outstanding limits"),
      ADD_STAT(metaFallbacks, statistics::units::Count::get(),
               "LLDP hints retained for consumers not covered by MetaTable"),
      ADD_STAT(samplerReservoirAdmissions, statistics::units::Count::get(),
               "New exact keys admitted into the deterministic Sampler reservoir"),
      ADD_STAT(samplerReservoirBypasses, statistics::units::Count::get(),
               "New exact keys rejected by the deterministic Sampler reservoir"),
      ADD_STAT(pairTrustPromotions, statistics::units::Count::get(),
               "Promotions accelerated by stable PC-pair evidence"),
      ADD_STAT(pairTrustReplacements, statistics::units::Count::get(),
               "Compact PC-pair trust-table replacements"),
      ADD_STAT(pairTrustProbes, statistics::units::Count::get(),
               "Trusted PC pairs installing a one-token exact Meta probe"),
      ADD_STAT(spatialFeedbackSignals, statistics::units::Count::get(),
               "LLDPS feedback signals sent to stream/stride prefetchers"),
      ADD_STAT(spatialFeedbackValid, statistics::units::Count::get(),
               "LLDPS feedback signals accepted by the queue"),
      ADD_STAT(chainTriggerAccepted, statistics::units::Count::get(),
               "LLDP-family prefetch triggers admitted for one more chain hop"),
      ADD_STAT(chainStoppedBySource, statistics::units::Count::get(),
               "LLDP-family prefetch triggers rejected by source policy"),
      ADD_STAT(chainStoppedByDepth, statistics::units::Count::get(),
               "LLDP-family prefetch triggers rejected by chain-depth limit"),
      ADD_STAT(qualityRejected, statistics::units::Count::get(),
               "LLDP candidates rejected by bounded quality control"),
      ADD_STAT(qualityBoosts, statistics::units::Count::get(),
               "LLDPS quality feedback boosts"),
      ADD_STAT(qualityRevokes, statistics::units::Count::get(),
               "LLDPS quality feedback revocations"),
      ADD_STAT(qualityEvictionRevokes, statistics::units::Count::get(),
               "Degree boosts revoked before quality replacement"),
      ADD_STAT(qualityPressureRevokes, statistics::units::Count::get(),
               "Degree boosts revoked by PFQ pressure"),
      ADD_STAT(ownerFull, statistics::units::Count::get(), "Candidate owner capacity rejects"),
      ADD_STAT(ownerCurrent, statistics::units::Count::get(), "Live candidate owners at dump"),
      ADD_STAT(ownerPeak, statistics::units::Count::get(), "Peak live candidate owners"),
      ADD_STAT(ownerStaleFeedback, statistics::units::Count::get(), "Unowned or repeated terminal feedback"),
      ADD_STAT(ownerDuplicateIssue, statistics::units::Count::get(), "Repeated issue notifications"),
      ADD_STAT(terminalBySource, statistics::units::Count::get(), "Unique terminal candidates"),
      ADD_STAT(demandMergedBySource, statistics::units::Count::get(), "Candidates completed by real demand merge"),
      ADD_STAT(cacheCollisionBySource, statistics::units::Count::get(), "Cache duplicate candidates"),
      ADD_STAT(pfMshrCollisionBySource, statistics::units::Count::get(), "Prefetch-only MSHR duplicate candidates"),
      ADD_STAT(demandMshrCollisionBySource, statistics::units::Count::get(),
               "Candidates meeting an existing demand MSHR"),
      ADD_STAT(wbCollisionBySource, statistics::units::Count::get(), "Write-buffer duplicate candidates"),
      ADD_STAT(queueDroppedBySource, statistics::units::Count::get(), "Candidate drops before PFQ dequeue"),
      ADD_STAT(qualityDecisions, statistics::units::Count::get(), "LLDPC decisions by reason"),
      ADD_STAT(lldpShadowDecisions, statistics::units::Count::get(), "LLDP decisions by reason"),
      ADD_STAT(lldpAdmissionRejected, statistics::units::Count::get(), "LLDP quality admission rejects"),
      ADD_STAT(feedbackStaleGeneration, statistics::units::Count::get(), "Feedback for replaced LLDT generation"),
      ADD_STAT(demandPairObserved, statistics::units::Count::get(), "Exact committed producer-consumer address pairs"),
      ADD_STAT(demandPairMissing, statistics::units::Count::get(), "Committed consumer with missing producer history"),
      ADD_STAT(demandPairTrained, statistics::units::Count::get(),
               "Committed demand pairs entering temporal training"),
      ADD_STAT(replayPairObserved, statistics::units::Count::get(), "Predicted pairs from replay translation/filter"),
      ADD_STAT(metaRecoveryProbes, statistics::units::Count::get(),
               "Meta periodic probes with exhausted quality or tokens"),
      ADD_STAT(samplerStaleAdmissions, statistics::units::Count::get(),
               "Stale sampler slots reused across workload phases"),
      ADD_STAT(benefitVictimChanges, statistics::units::Count::get(), "LLDT replacement decisions changed by benefit"),
      ADD_STAT(pfqCurrent, statistics::units::Count::get(), "PFQ slots by source at dump"),
      ADD_STAT(pfqPeak, statistics::units::Count::get(), "Peak PFQ slots by source"),
      ADD_STAT(pfqOccupancySamples, statistics::units::Count::get(), "Sum of occupied slots at PFQ event samples"),
      ADD_STAT(pfqSamples, statistics::units::Count::get(), "PFQ occupancy event samples"),
      ADD_STAT(lldpDuplicateRejected, statistics::units::Count::get(),
               "LLDP cache/MSHR duplicates removed before admission"),
      ADD_STAT(pairObserved, statistics::units::Count::get(), "Address-pair observations by pair/context hash bucket"),
      ADD_STAT(pairTrustedCount, statistics::units::Count::get(), "Trusted pair observations by hash bucket"),
      ADD_STAT(sampleRetained, statistics::units::Count::get(), "Sampler retained observations by pair bucket"),
      ADD_STAT(metaAllocated, statistics::units::Count::get(), "Meta allocations by pair bucket"),
      ADD_STAT(metaHit, statistics::units::Count::get(), "Meta raw hits by pair bucket"),
      ADD_STAT(metaAdmissionReject, statistics::units::Count::get(), "Meta admission rejects by pair bucket"),
      ADD_STAT(consumerUnused, statistics::units::Count::get(), "Generation-owned unused LLDT candidates"),
      ADD_STAT(consumerCollision, statistics::units::Count::get(), "Generation-owned duplicate LLDT candidates"),
      ADD_STAT(samplerReplacementCnt, statistics::units::Count::get(),
               "SamplerTable victim count distribution"),
      ADD_STAT(candidateGenerated, statistics::units::Count::get(), "Candidate lifecycles generated"),
      ADD_STAT(candidateQueued, statistics::units::Count::get(), "Candidate lifecycles queued"),
      ADD_STAT(candidateIssued, statistics::units::Count::get(), "Candidate lifecycles issued"),
      ADD_STAT(candidateDropped, statistics::units::Count::get(), "Candidate lifecycles dropped before issue"),
      ADD_STAT(candidateMerged, statistics::units::Count::get(), "Candidate lifecycles merged with demand"),
      ADD_STAT(candidateUseful, statistics::units::Count::get(), "Candidate lifecycles useful"),
      ADD_STAT(candidateUnused, statistics::units::Count::get(), "Candidate lifecycles unused"),
      ADD_STAT(candidateLate, statistics::units::Count::get(), "Candidate lifecycles late"),
      ADD_STAT(candidateCacheHit, statistics::units::Count::get(), "Candidates hitting cache"),
      ADD_STAT(candidateMshrHit, statistics::units::Count::get(), "Candidates hitting MSHR"),
      ADD_STAT(candidateWbHit, statistics::units::Count::get(), "Candidates hitting write buffer"),
      ADD_STAT(childrenAtReplacement, statistics::units::Count::get(), "Valid consumers per evicted producer"),
      ADD_STAT(childrenAtDump, statistics::units::Count::get(), "Current producers by valid consumer count"),
      ADD_STAT(pcpActiveHistogram, statistics::units::Count::get(), "Current producers by active consumer count"),
      ADD_STAT(pcpActivePeak, statistics::units::Count::get(), "Peak active consumers by producer row"),
      ADD_STAT(pcpConsumerReplacement, statistics::units::Count::get(), "Consumer replacements by producer row"),
      ADD_STAT(pcpConsumerOverflow, statistics::units::Count::get(), "Consumer overflows by producer row"),
      ADD_STAT(pcpCandidate, statistics::units::Count::get(), "Candidates by producer row"),
      ADD_STAT(pcpUseful, statistics::units::Count::get(), "Useful candidates by producer row"),
      ADD_STAT(pcpLate, statistics::units::Count::get(), "Late candidates by producer row"),
      ADD_STAT(pcpProducerPC, statistics::units::Count::get(), "Producer PC by row"),
      ADD_STAT(pcpContext, statistics::units::Count::get(), "Producer context by row"),
      ADD_STAT(pcpValid, statistics::units::Count::get(), "Producer valid bit by row"),
      ADD_STAT(pcpConf, statistics::units::Count::get(), "Producer confidence by row"),
      ADD_STAT(pcpDemandPC, statistics::units::Count::get(), "Last demand PC matched to a candidate by row"),
      ADD_STAT(pcpDemandChainId, statistics::units::Count::get(), "Last matched LLDT generation by row"),
      ADD_STAT(pcpDemandSource, statistics::units::Count::get(), "Last matched prefetch source by row"),
      ADD_STAT(pcpDemandHits, statistics::units::Count::get(), "Candidate demand matches by row"),
      ADD_STAT(consumerValid, statistics::units::Count::get(), "Consumer valid bit by row and column"),
      ADD_STAT(consumerPC, statistics::units::Count::get(), "Consumer PC by row and column"),
      ADD_STAT(consumerLength, statistics::units::Count::get(), "Consumer chain length by row and column"),
      ADD_STAT(consumerOp1, statistics::units::Count::get(), "Consumer first operation by row and column"),
      ADD_STAT(consumerOp2, statistics::units::Count::get(), "Consumer second operation by row and column"),
      ADD_STAT(consumerImmLoad, statistics::units::Count::get(), "Consumer load immediate by row and column"),
      ADD_STAT(consumerImmConf, statistics::units::Count::get(), "Consumer exact-immediate confidence"),
      ADD_STAT(consumerLineConf, statistics::units::Count::get(), "Consumer immediate-line confidence"),
      ADD_STAT(consumerOffsetConf, statistics::units::Count::get(), "Consumer immediate-offset confidence"),
      ADD_STAT(consumerConf, statistics::units::Count::get(), "Consumer confidence by row and column"),
      ADD_STAT(consumerUpdates, statistics::units::Count::get(), "Consumer updates by row and column"),
      ADD_STAT(consumerReplacements, statistics::units::Count::get(), "Consumer replacements by row and column"),
      ADD_STAT(consumerExactImmStable, statistics::units::Count::get(), "Consumer exact-immediate stable updates"),
      ADD_STAT(consumerLineImmStable, statistics::units::Count::get(), "Consumer immediate-line stable updates"),
      ADD_STAT(consumerCandidates, statistics::units::Count::get(), "Consumer candidates by row and column"),
      ADD_STAT(consumerUseful, statistics::units::Count::get(), "Consumer useful candidates by row and column"),
      ADD_STAT(consumerLate, statistics::units::Count::get(), "Consumer late candidates by row and column"),
      ADD_STAT(consumerDemandHits, statistics::units::Count::get(), "Consumer candidate demand matches"),
      ADD_STAT(validProducers, statistics::units::Count::get(), "Current valid LLDT producers")
{
    samplerReplacementCnt.init(256);
    immValueHist.init(256);
    lineDeltaHist.init(129);
    offsetDeltaHist.init(127);
    byteOffsetHist.init(5 * 64);
    childrenAtReplacement.init(SubEntries + 1);
    childrenAtDump.init(SubEntries + 1);
    pcpActiveHistogram.init(SubEntries + 1);
    pcpActivePeak.init(TableEntries);
    pcpConsumerReplacement.init(TableEntries);
    pcpConsumerOverflow.init(TableEntries);
    pcpCandidate.init(TableEntries);
    pcpUseful.init(TableEntries);
    pcpLate.init(TableEntries);
    pcpProducerPC.init(TableEntries);
    pcpContext.init(TableEntries);
    pcpValid.init(TableEntries);
    pcpConf.init(TableEntries);
    pcpDemandPC.init(TableEntries);
    pcpDemandChainId.init(TableEntries);
    pcpDemandSource.init(TableEntries);
    for (auto *counter : {&terminalBySource, &demandMergedBySource,
                         &cacheCollisionBySource, &pfMshrCollisionBySource,
                         &demandMshrCollisionBySource, &wbCollisionBySource,
                         &queueDroppedBySource}) {
        counter->init(NUM_PF_SOURCES);
        for (unsigned source = 0; source < NUM_PF_SOURCES; ++source)
            counter->subname(source, prefetchSourceTypeName(source));
    }
    pcpDemandHits.init(TableEntries);
    const char *decision_names[] = {"healthy", "probe", "pressure", "outstanding",
                                    "cold", "unhealthy", "stale"};
    for (auto *counter : {&qualityDecisions, &lldpShadowDecisions}) {
        counter->init(7);
        for (unsigned i = 0; i < 7; ++i)
            counter->subname(i, decision_names[i]);
    }
    for (auto *counter : {&pairObserved, &pairTrustedCount, &sampleRetained,
                         &metaAllocated, &metaHit, &metaAdmissionReject})
        counter->init(64);
    for (auto *counter : {&pfqCurrent, &pfqPeak, &pfqOccupancySamples}) {
        counter->init(NUM_PF_SOURCES);
        for (unsigned source = 0; source < NUM_PF_SOURCES; ++source)
            counter->subname(source, prefetchSourceTypeName(source));
    }
    consumerUnused.init(TableEntries * SubEntries);
    consumerCollision.init(TableEntries * SubEntries);
    consumerValid.init(TableEntries * SubEntries);
    consumerPC.init(TableEntries * SubEntries);
    consumerLength.init(TableEntries * SubEntries);
    consumerOp1.init(TableEntries * SubEntries);
    consumerOp2.init(TableEntries * SubEntries);
    consumerImmLoad.init(TableEntries * SubEntries);
    consumerImmConf.init(TableEntries * SubEntries);
    consumerLineConf.init(TableEntries * SubEntries);
    consumerOffsetConf.init(TableEntries * SubEntries);
    consumerConf.init(TableEntries * SubEntries);
    consumerUpdates.init(TableEntries * SubEntries);
    consumerReplacements.init(TableEntries * SubEntries);
    consumerExactImmStable.init(TableEntries * SubEntries);
    consumerLineImmStable.init(TableEntries * SubEntries);
    consumerCandidates.init(TableEntries * SubEntries);
    consumerUseful.init(TableEntries * SubEntries);
    consumerLate.init(TableEntries * SubEntries);
    consumerDemandHits.init(TableEntries * SubEntries);
}

LLDPrefetcher::LLDPrefetcher(const LLDPrefetcherParams &p)
    : Queued(p), candidateOwners(p.candidate_owner_entries),
      learningEvent([this] { learningTick(); }, name() + ".learning"),
      trainingQueueSize(p.training_queue_size),
      maxConf((1U << std::min(p.confidence_bits, 8U)) - 1),
      initialConf(p.initial_confidence), producerInitialConf(p.producer_initial_confidence),
      producerThreshold(p.producer_threshold), consumerThreshold(p.consumer_threshold),
      immediateThreshold(p.immediate_threshold),
      enableSpatialFeedback(p.enable_spatial_feedback),
      maxLldpcChainDepth(p.max_lldpc_chain_depth),
      enableQualityControl(p.enable_quality_control),
      enableLldpcQuality(p.enable_lldpc_quality),
      enableLldpsFeedbackQuality(p.enable_lldps_feedback_quality),
      enableLldpShadow(p.enable_lldp_shadow),
      enableLldpAdmission(p.enable_lldp_admission),
      enableDemandPairTraining(p.enable_demand_pair_training),
      enableLldtBenefit(p.enable_lldt_benefit),
      enableTemporalRecovery(p.enable_temporal_recovery),
      temporalProbeInterval(p.temporal_probe_interval),
      pairTrustThreshold(p.pair_trust_threshold),
      samplerRetentionEpochs(p.sampler_retention_epochs),
      qualityPolicy{p.quality_min_samples, p.quality_min_accuracy_pct,
                    p.quality_max_late_pct, p.quality_probe_interval,
                    p.quality_max_outstanding,
                    p.quality_max_pressure_pct,
                    p.quality_max_collision_pct},
      lldpAdmissionPolicy{p.quality_min_samples,
                          p.lldp_admission_min_accuracy_pct,
                          p.lldp_admission_max_late_pct,
                          p.quality_probe_interval,
                          p.quality_max_outstanding,
                          p.quality_max_pressure_pct,
                          p.quality_max_collision_pct},
      lldpsFeedbackPolicy{p.lldps_feedback_min_samples,
                          p.lldps_feedback_min_accuracy_pct,
                          p.lldps_feedback_max_late_pct,
                          p.quality_probe_interval,
                          p.quality_max_outstanding,
                          p.quality_max_pressure_pct,
                          p.quality_max_collision_pct},
      enableCandidateTrace(p.enable_candidate_trace),
      traceSampleInterval(p.trace_sample_interval),
      traceProducerPC(p.trace_producer_pc),
      trainingCPU(p.training_cpu), stats(this)
{
    fatal_if(p.confidence_bits == 0 || p.confidence_bits > 8 || !trainingQueueSize ||
        initialConf > maxConf || producerInitialConf > maxConf ||
        producerThreshold > maxConf || consumerThreshold > maxConf ||
        immediateThreshold > maxConf, "Invalid LLDP confidence/FIFO parameters");
    fatal_if(!traceSampleInterval, "Candidate trace sample interval must be positive");
    if (enableCandidateTrace) {
        candidateTrace.open(simout.resolve(name() + ".candidates.csv"));
        fatal_if(!candidateTrace, "Cannot open LLDP candidate trace");
        candidateTrace << "tick,event,id,source,level,context,pcp,pcc,row_generation,"
                          "consumer_generation,meta_generation,quality_generation,"
                          "address,issued,terminal\n";
    }
    fatal_if(!useVirtualAddresses, "LLDP replay generates virtual addresses");
    fatal_if(pairTrustThreshold > 7, "Pair trust threshold must be at most 7");
    fatal_if(enableDemandPairTraining && !trainingCPU,
             "Committed demand pair training requires the L1 training CPU");
    fatal_if(qualityPolicy.minAccuracyPct > 100 ||
             qualityPolicy.maxLatePct > 100 ||
             qualityPolicy.maxPressurePct > 100 ||
             qualityPolicy.maxCollisionPct > 100 ||
             lldpsFeedbackPolicy.minAccuracyPct > 100 ||
             lldpsFeedbackPolicy.maxLatePct > 100,
             "Invalid LLDP quality percentages");
}

PrefetchQualityControl::Key
LLDPrefetcher::qualityKey(PrefetchSourceType source, Addr producer_pc,
                          Addr consumer_pc, ContextID context) const
{
    return {source, producer_pc, consumer_pc, context};
}

void
LLDPrefetcher::observeQuality(
    uint64_t candidate_id, PrefetchQualityControl::Outcome outcome)
{
    if (!enableQualityControl || !candidate_id)
        return;
    const auto it = candidateOwners.find(candidate_id);
    if (!it || !it->hasQualityHandle || !it->hasIssued)
        return;
    const auto owner = *it;
    auto &control = owner.source == PrefetchSourceType::LLDP ?
        lldpAdmissionControl : qualityControl;
    control.observe(owner.qualityHandle, outcome);
    if (owner.source == PrefetchSourceType::LLDPS &&
        enableLldpsFeedbackQuality && owner.feedbackSource !=
        PrefetchSourceType::PF_NONE) {
        const int change = qualityControl.updateBoost(
            owner.qualityHandle, lldpsFeedbackPolicy, qualityPressure());
        if (change > 0) {
            stats.qualityBoosts++;
            sendSpatialFeedback(owner.qualityHandle, true);
        } else if (change < 0) {
            stats.qualityRevokes++;
            sendSpatialFeedback(owner.qualityHandle, false);
        }
    }
}

bool
LLDPrefetcher::finishCandidate(uint64_t candidate_id,
    PrefetchQualityControl::Outcome outcome, int meta_result)
{
    auto *owner = candidateOwners.find(candidate_id);
    if (!owner || !owner->finish()) {
        stats.ownerStaleFeedback++;
        return false;
    }
    static const char *events[] = {"useful", "unused", "demand_merge", "collision", "drop"};
    traceCandidate(candidate_id, events[unsigned(outcome)], *owner);
    stats.terminalBySource[owner->source]++;
    if (outcome == PrefetchQualityControl::Outcome::DemandMerged)
        stats.demandMergedBySource[owner->source]++;
    if (outcome == PrefetchQualityControl::Outcome::Dropped)
        stats.queueDroppedBySource[owner->source]++;
    observeQuality(candidate_id, outcome);
    updateMetaOwner(candidate_id, meta_result);
    if (owner->hasDependency && !owner->meta && table[owner->row].generation == owner->generation &&
        table[owner->row].consumers[owner->col].generation == owner->consumerGeneration &&
        table[owner->row].consumers[owner->col].consumerPC == owner->consumerPC) {
        auto &row = table[owner->row];
        auto &sub = row.consumers[owner->col];
        sub.benefit.observe(unsigned(outcome));
        if (outcome == PrefetchQualityControl::Outcome::Useful) {
            ++row.usefulCount;
            ++sub.usefulCount;
        } else if (outcome == PrefetchQualityControl::Outcome::DemandMerged) {
            ++row.lateCount;
            ++sub.lateCount;
        } else if (outcome == PrefetchQualityControl::Outcome::Unused) {
            ++sub.unusedCount;
        } else if (outcome == PrefetchQualityControl::Outcome::Collision) {
            ++sub.collisionCount;
        }
    } else if (owner->hasDependency && !owner->meta) {
        stats.feedbackStaleGeneration++;
    }
    candidateOwners.erase(candidate_id);
    return true;
}

void
LLDPrefetcher::regProbeListeners()
{
    Queued::regProbeListeners();
    if (trainingCPU) {
        dependenceListener = trainingCPU->getProbeManager()->connect<DependenceListener>(
            *this, "dependenceTrain");
        commitListener = trainingCPU->getProbeManager()->connect<CommitListener>(
            *this, "Commit");
    }
    fatal_if(!tlb, "LLDP requires a registered data TLB");
    // Use the timing PTW retry path on a TLB miss, rather than functional lookup.
    functionalTLB = false;
}

void
LLDPrefetcher::observeCommittedLoad(const o3::DynInstPtr &inst)
{
    if (!inst || !inst->isLoad() || inst->isSquashed() ||
        inst->isAtomic() || inst->isVector() || !inst->effAddrValid())
        return;
    const auto &meta = *inst->xsMeta;
    if (inst->lldpChain.trainable() && meta.lldpProducerSeq) {
        const auto producer = demandPairHistory.lookup(meta.lldpProducerSeq,
            inst->lldpChain.producerPC, inst->contextId());
        if (producer) {
            stats.demandPairObserved++;
            if (enableDemandPairTraining) {
                trainAddressPair(*producer, blockAddress(inst->physEffAddr),
                    inst->lldpChain.producerPC, inst->pcState().instAddr(),
                    inst->contextId());
                stats.demandPairTrained++;
            }
        } else {
            stats.demandPairMissing++;
        }
    }
    demandPairHistory.record(inst->seqNum, inst->pcState().instAddr(),
                             inst->physEffAddr, inst->contextId());
}

int
LLDPrefetcher::findProducer(Addr pc, ContextID context) const
{
    for (unsigned i = 0; i < table.size(); ++i)
        if (table[i].valid && table[i].producerPC == pc && table[i].context == context)
            return i;
    return -1;
}

int
LLDPrefetcher::findConsumer(const Entry &entry, Addr pc) const
{
    for (unsigned i = 0; i < SubEntries; ++i)
        if (entry.consumers[i].valid && entry.consumers[i].consumerPC == pc)
            return i;
    return -1;
}

bool
LLDPrefetcher::isSpatialPrefetch(const PacketPtr &pkt) const
{
    if (!pkt || !pkt->req || !pkt->req->isPrefetch() ||
        !pkt->req->hasXsMetadata())
        return false;
    switch (pkt->req->getXsMetadata().prefetchSource) {
      case PrefetchSourceType::SStream:
      case PrefetchSourceType::SStride:
      case PrefetchSourceType::StoreStream:
      case PrefetchSourceType::SPht:
      case PrefetchSourceType::HWP_BOP:
      case PrefetchSourceType::SPP:
      case PrefetchSourceType::IPCP:
      case PrefetchSourceType::IPCP_CS:
      case PrefetchSourceType::IPCP_CPLX:
      case PrefetchSourceType::Berti:
      case PrefetchSourceType::SOpt:
      case PrefetchSourceType::DespacitoStream:
        return true;
      default:
        return false;
    }
}

bool
LLDPrefetcher::isLldpSource(PrefetchSourceType source)
{
    return source == PrefetchSourceType::LLDP ||
        source == PrefetchSourceType::LLDPS ||
        source == PrefetchSourceType::LLDPT ||
        source == PrefetchSourceType::LLDPC;
}

unsigned
LLDPrefetcher::samplerSet(Addr addr_p) const
{
    return (addr_p ^ (addr_p >> 6)) & (SamplerSets - 1);
}

unsigned
LLDPrefetcher::metaSet(Addr addr_p) const
{
    return (addr_p ^ (addr_p >> 6)) & (MetaSets - 1);
}

unsigned
LLDPrefetcher::pairHintSet(uint32_t key_hash) const
{
    return (key_hash ^ (key_hash >> 16)) & (PairHintSets - 1);
}

uint32_t
LLDPrefetcher::pairHintHash(Addr producer_pc, Addr consumer_pc,
                            ContextID context)
{
    uint64_t value = uint64_t(producer_pc) ^
        (uint64_t(consumer_pc) * UINT64_C(0x9e3779b97f4a7c15)) ^
        (uint64_t(context) * UINT64_C(0xbf58476d1ce4e5b9));
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return uint32_t(value) ^ uint32_t(value >> 32);
}

unsigned
LLDPrefetcher::pairHintVictim(unsigned set)
{
    auto &ways = pairHintTable[set];
    for (;;) {
        unsigned victim = 0;
        uint8_t best = 0;
        for (unsigned way = 0; way < AddressTableWays; ++way) {
            if (!ways[way].valid)
                return way;
            if (ways[way].rrpv > best) {
                best = ways[way].rrpv;
                victim = way;
            }
        }
        if (best >= 3)
            return victim;
        for (auto &entry : ways)
            entry.rrpv = std::min<uint8_t>(3, entry.rrpv + 1);
    }
}

void
LLDPrefetcher::recordPairEvidence(Addr producer_pc, Addr consumer_pc,
                                  ContextID context)
{
    const uint32_t key_hash = pairHintHash(producer_pc, consumer_pc, context);
    const unsigned set = pairHintSet(key_hash);
    auto &ways = pairHintTable[set];
    PairHintEntry *entry = nullptr;
    for (auto &candidate : ways) {
        if (candidate.valid && candidate.keyHash == key_hash) {
            entry = &candidate;
            break;
        }
    }
    if (!entry) {
        const unsigned victim = pairHintVictim(set);
        entry = &ways[victim];
        if (entry->valid)
            stats.pairTrustReplacements++;
        *entry = {};
        entry->valid = true;
        entry->keyHash = key_hash;
        entry->rrpv = 0;
        return;
    }
    entry->temporalConf = std::min<uint8_t>(7, entry->temporalConf + 1);
    entry->rrpv = 0;
}

bool
LLDPrefetcher::pairTrusted(Addr producer_pc, Addr consumer_pc,
                           ContextID context) const
{
    const uint32_t key_hash = pairHintHash(producer_pc, consumer_pc, context);
    const unsigned set = pairHintSet(key_hash);
    const auto &ways = pairHintTable[set];
    for (const auto &candidate : ways) {
        if (candidate.valid && candidate.keyHash == key_hash) {
            return candidate.temporalConf >= pairTrustThreshold;
        }
    }
    return false;
}

uint16_t
LLDPrefetcher::samplerRank(Addr addr_p, Addr producer_pc,
                           Addr consumer_pc, ContextID context)
{
    uint64_t value = uint64_t(addr_p) ^
        (uint64_t(producer_pc) * UINT64_C(0x9e3779b97f4a7c15)) ^
        (uint64_t(consumer_pc) * UINT64_C(0xbf58476d1ce4e5b9)) ^
        uint64_t(context);
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return uint16_t(value) ^ uint16_t(value >> 16) ^ uint16_t(value >> 32);
}

unsigned
LLDPrefetcher::samplerVictim(unsigned set)
{
    auto &ways = samplerTable[set];
    for (;;) {
        unsigned victim = 0;
        uint8_t best = 0;
        for (unsigned way = 0; way < AddressTableWays; ++way) {
            if (!ways[way].valid)
                return way;
            if (ways[way].rrpv >= best) {
                best = ways[way].rrpv;
                victim = way;
            }
        }
        if (best >= 3)
            return victim;
        for (auto &entry : ways)
            entry.rrpv = std::min<uint8_t>(3, entry.rrpv + 1);
    }
}

unsigned
LLDPrefetcher::metaVictim(unsigned set)
{
    auto &ways = metaTable[set];
    for (unsigned way = 0; way < AddressTableWays; ++way) {
        if (!ways[way].valid)
            return way;
    }

    unsigned victim = 0;
    for (unsigned way = 1; way < AddressTableWays; ++way) {
        const auto &candidate = ways[way];
        const auto &current = ways[victim];
        if (current.outstanding && !candidate.outstanding) {
            victim = way;
        } else if (candidate.outstanding == current.outstanding &&
                   (candidate.qualityConf < current.qualityConf ||
                    (candidate.qualityConf == current.qualityConf &&
                     candidate.trainConf < current.trainConf) ||
                    (candidate.qualityConf == current.qualityConf &&
                     candidate.trainConf == current.trainConf &&
                     candidate.timelyConf < current.timelyConf) ||
                    (candidate.qualityConf == current.qualityConf &&
                     candidate.trainConf == current.trainConf &&
                     candidate.timelyConf == current.timelyConf &&
                     candidate.lastUsefulEpoch < current.lastUsefulEpoch) ||
                    (candidate.qualityConf == current.qualityConf &&
                     candidate.trainConf == current.trainConf &&
                     candidate.timelyConf == current.timelyConf &&
                     candidate.lastUsefulEpoch == current.lastUsefulEpoch &&
                     candidate.rrpv > current.rrpv))) {
            victim = way;
        }
    }
    return victim;
}

void
LLDPrefetcher::updateMetaTable(const SamplerEntry &sample)
{
    const unsigned set = metaSet(sample.addrP ^ sample.producerPC ^
                                  sample.consumerPC ^ Addr(sample.context));
    auto &ways = metaTable[set];
    for (unsigned way = 0; way < AddressTableWays; ++way) {
        auto &entry = ways[way];
        if (entry.valid && entry.addrP == sample.addrP &&
            entry.producerPC == sample.producerPC &&
            entry.consumerPC == sample.consumerPC &&
            entry.context == sample.context) {
            entry.probation = false;
            if (entry.addrC != sample.addrC) {
                entry.trainConf = entry.trainConf > 1 ? entry.trainConf - 2 : 0;
                entry.tokens = 0;
                if (!entry.trainConf) {
                    entry.addrC = sample.addrC;
                    ++entry.generation;
                    entry.trainConf = 3;
                    entry.qualityConf = 4;
                    entry.timelyConf = 4;
                    entry.tokens = 2;
                    entry.outstanding = 0;
                    entry.lastTrainEpoch = tableEpoch;
                    entry.lastUsefulEpoch = 0;
                    entry.rrpv = 0;
                    stats.metaInvalidations++;
                    stats.metaTargetSwitches++;
                }
                return;
            }
            entry.trainConf = std::min<uint8_t>(7, entry.trainConf + 1);
            entry.lastTrainEpoch = tableEpoch;
            entry.rrpv = 0;
            return;
        }
    }

    stats.metaAllocated[pairHintHash(sample.producerPC, sample.consumerPC, sample.context) & 63]++;
    const unsigned victim = metaVictim(set);
    auto &entry = ways[victim];
    const uint32_t next_generation = entry.generation + 1;
    entry = {};
    entry.valid = true;
    entry.addrP = sample.addrP;
    entry.addrC = sample.addrC;
    entry.producerPC = sample.producerPC;
    entry.consumerPC = sample.consumerPC;
    entry.context = sample.context;
    entry.generation = next_generation;
    entry.trainConf = 3;
    entry.qualityConf = 4;
    entry.timelyConf = 4;
    entry.tokens = 2;
    entry.lastTrainEpoch = tableEpoch;
    entry.rrpv = 0;
}

void
LLDPrefetcher::installTrustedMeta(Addr addr_p, Addr addr_c,
                                  Addr producer_pc, Addr consumer_pc,
                                  ContextID context)
{
    const unsigned set = metaSet(addr_p ^ producer_pc ^ consumer_pc ^
                                 Addr(context));
    auto &ways = metaTable[set];
    for (auto &entry : ways) {
        if (entry.valid && entry.addrP == addr_p &&
            entry.producerPC == producer_pc &&
            entry.consumerPC == consumer_pc && entry.context == context)
            return;
    }
    stats.metaAllocated[pairHintHash(producer_pc, consumer_pc, context) & 63]++;
    const unsigned victim = metaVictim(set);
    auto &entry = ways[victim];
    const uint32_t next_generation = entry.generation + 1;
    entry = {};
    entry.valid = true;
    entry.addrP = addr_p;
    entry.addrC = addr_c;
    entry.producerPC = producer_pc;
    entry.consumerPC = consumer_pc;
    entry.context = context;
    entry.generation = next_generation;
    entry.trainConf = SamplerThreshold - 1;
    entry.qualityConf = 3;
    entry.timelyConf = 4;
    entry.tokens = 1;
    entry.probation = true;
    entry.lastTrainEpoch = tableEpoch;
    entry.rrpv = 0;
    stats.pairTrustProbes++;
}

void
LLDPrefetcher::trainAddressPair(Addr addr_p, Addr addr_c, Addr producer_pc,
                                Addr consumer_pc, ContextID context)
{
    if (archDBer) {
        archDBer->lldpTrainTraceWrite(
            curTick(), addr_p, addr_c, producer_pc, consumer_pc, context);
    }
    const unsigned bucket = pairHintHash(producer_pc, consumer_pc, context) & 63;
    stats.pairObserved[bucket]++;
    stats.pairTrustedCount[bucket] += pairTrusted(producer_pc, consumer_pc, context);
    const unsigned set = samplerSet(
        addr_p ^ producer_pc ^ consumer_pc ^ Addr(context));
    auto &ways = samplerTable[set];
    for (unsigned way = 0; way < AddressTableWays; ++way) {
        auto &entry = ways[way];
        if (!entry.valid || entry.addrP != addr_p ||
            entry.producerPC != producer_pc || entry.consumerPC != consumer_pc ||
            entry.context != context)
            continue;
        stats.sampleRetained[bucket]++;
        entry.lastSeenEpoch = tableEpoch;
        if (entry.addrC != addr_c) {
            stats.samplerTargetMismatch++;
            entry.mismatchCount = std::min<uint8_t>(7, entry.mismatchCount + 1);
            if (entry.stableCount)
                --entry.stableCount;
            if (!entry.stableCount) {
                entry.addrC = addr_c;
                entry.stableCount = 1;
                entry.mismatchCount = 0;
                entry.promotionVersion++;
            }
            entry.rrpv = 3;
            return;
        }
        entry.stableCount = std::min<uint8_t>(7, entry.stableCount + 1);
        recordPairEvidence(producer_pc, consumer_pc, context);
        if (entry.stableCount == 2 &&
            pairTrusted(producer_pc, consumer_pc, context)) {
            installTrustedMeta(addr_p, addr_c, producer_pc, consumer_pc,
                               context);
        }
        entry.mismatchCount = 0;
        entry.rrpv = 0;
        if (entry.stableCount == SamplerThreshold) {
            if (pairTrusted(producer_pc, consumer_pc, context))
                stats.pairTrustPromotions++;
            updateMetaTable(entry);
            stats.samplerOutputsToMeta++;
            entry.matchesSincePromotion = 0;
        } else if (entry.stableCount > SamplerThreshold &&
                   ++entry.matchesSincePromotion >= 8) {
            updateMetaTable(entry);
            stats.samplerRepromotions++;
            entry.matchesSincePromotion = 0;
        }
        return;
    }

    const uint16_t rank = samplerRank(
        addr_p, producer_pc, consumer_pc, context);
    unsigned victim = 0;
    bool found_invalid = false;
    for (unsigned way = 0; way < AddressTableWays; ++way) {
        if (!ways[way].valid) {
            victim = way;
            found_invalid = true;
            break;
        }
        if (ways[way].reservoirRank > ways[victim].reservoirRank)
            victim = way;
    }
    if (!found_invalid && rank >= ways[victim].reservoirRank) {
        if (samplerRetentionEpochs &&
            uint32_t(tableEpoch - ways[victim].lastSeenEpoch) >= samplerRetentionEpochs) {
            stats.samplerStaleAdmissions++;
        } else {
            stats.samplerReservoirBypasses++;
            return;
        }
    }
    if (ways[victim].valid)
        stats.samplerReplacementCnt[ways[victim].stableCount]++;
    ways[victim] = {};
    ways[victim].valid = true;
    ways[victim].addrP = addr_p;
    ways[victim].producerPC = producer_pc;
    ways[victim].consumerPC = consumer_pc;
    ways[victim].context = context;
    ways[victim].addrC = addr_c;
    const bool trusted = pairTrusted(producer_pc, consumer_pc, context);
    ways[victim].stableCount = trusted ? SamplerThreshold - 1 : 1;
    ways[victim].rrpv = 3;
    ways[victim].reservoirRank = rank;
    ways[victim].lastSeenEpoch = tableEpoch;
    stats.samplerReservoirAdmissions++;
    stats.sampleRetained[bucket]++;
    if (trusted)
        installTrustedMeta(addr_p, addr_c, producer_pc, consumer_pc, context);
}

std::optional<LLDPrefetcher::MetaHit>
LLDPrefetcher::lookupMetaTable(Addr addr_p, Addr producer_pc,
                               Addr consumer_pc, ContextID context)
{
    const unsigned set = metaSet(
        addr_p ^ producer_pc ^ consumer_pc ^ Addr(context));
    auto &ways = metaTable[set];
    for (unsigned way = 0; way < AddressTableWays; ++way) {
        auto &entry = ways[way];
        if (entry.valid && entry.addrP == addr_p &&
            entry.producerPC == producer_pc &&
            entry.consumerPC == consumer_pc && entry.context == context) {
            const unsigned bucket = pairHintHash(producer_pc, consumer_pc, context) & 63;
            stats.metaHit[bucket]++;
            stats.metaTableHits++;
            bool probe;
            if (!TemporalControl::admit(entry, enableTemporalRecovery,
                                        temporalProbeInterval, probe)) {
                stats.metaTokenStalls++;
                stats.metaAdmissionReject[bucket]++;
                return std::nullopt;
            }
            stats.metaRecoveryProbes += probe;
            return MetaHit{entry.addrC, set, way, entry.generation};
        }
    }
    return std::nullopt;
}

void
LLDPrefetcher::ageMetaTable()
{
    if (++tableEpoch % 4096)
        return;
    for (auto &ways : metaTable) {
        for (auto &entry : ways) {
            if (!entry.valid || tableEpoch - entry.lastTrainEpoch < 32768)
                continue;
            if (entry.trainConf)
                --entry.trainConf;
            entry.lastTrainEpoch = tableEpoch;
            if (!entry.trainConf) {
                entry.valid = false;
                stats.metaInvalidations++;
            }
        }
    }
}

void
LLDPrefetcher::updateMetaOwner(uint64_t candidate_id, int result)
{
    const auto it = candidateOwners.find(candidate_id);
    if (!it || !it->meta || !it->metaReserved)
        return;
    it->metaReserved = false;
    auto &entry = metaTable[it->metaSet][it->metaWay];
    if (!entry.valid || entry.generation != it->metaGeneration)
        return;
    entry.outstanding = entry.outstanding ? entry.outstanding - 1 : 0;
    switch (result) {
      case 0: // useful
        entry.unusedStreak = 0;
        entry.probation = false;
        entry.qualityConf = std::min<uint8_t>(7, entry.qualityConf + 2);
        entry.timelyConf = std::min<uint8_t>(7, entry.timelyConf + 1);
        entry.tokens = std::min<uint8_t>(15, entry.tokens + 4);
        entry.lastUsefulEpoch = tableEpoch;
        entry.rrpv = 0;
        break;
      case 1: // merged
        entry.unusedStreak = 0;
        entry.probation = false;
        entry.qualityConf = std::min<uint8_t>(7, entry.qualityConf + 1);
        entry.timelyConf = entry.timelyConf ? entry.timelyConf - 1 : 0;
        entry.tokens = std::min<uint8_t>(15, entry.tokens + 1);
        entry.rrpv = 0;
        break;
      case 2: // late
        entry.timelyConf = entry.timelyConf > 1 ? entry.timelyConf - 2 : 0;
        entry.tokens = entry.tokens ? entry.tokens - 1 : 0;
        break;
      case 3: // unused
        TemporalControl::unused(entry, enableTemporalRecovery);
        if (!entry.valid) {
            stats.metaInvalidations++;
        }
        break;
      case 4: // dropped before issue
      case 5: // duplicate address is not evidence of a wrong mapping
        if (it->metaSpentToken)
            entry.tokens = std::min<uint8_t>(15, entry.tokens + 1);
        break;
      default:
        break;
    }
}

bool
LLDPrefetcher::filterCandidate(Addr line)
{
    if (tlbFilterSet.count(line))
        return true;
    tlbFilter.push_back(line);
    tlbFilterSet.insert(line);
    if (tlbFilter.size() > TlbFilterEntries) {
        tlbFilterTranslations.erase(tlbFilter.front());
        tlbFilterSet.erase(tlbFilter.front());
        tlbFilter.pop_front();
    }
    return false;
}

bool
LLDPrefetcher::rejectTranslatedPrefetch(const DeferredPacket &dpp,
                                        Addr paddr)
{
    const auto metadata = dpp.pfInfo.getXsMetadata();
    if (!metadata.prefetchCandidateId)
        return false;
    const bool translated_source = metadata.prefetchLldpVirtual;
    if (translated_source) {
        stats.replayPairObserved++;
        if (!enableDemandPairTraining)
            trainAddressPair(metadata.prefetchLldpAddrP, blockAddress(paddr),
                         metadata.prefetchProducerPC,
                         metadata.prefetchConsumerPC,
                         dpp.pfInfo.contextId());
    }
    if (enableLldpAdmission && metadata.prefetchSource == PrefetchSourceType::LLDP &&
        (inCache(paddr, dpp.pfInfo.isSecure()) ||
         inMissQueue(paddr, dpp.pfInfo.isSecure()))) {
        stats.lldpDuplicateRejected++;
        return true;
    }
    if (metadata.prefetchSource == PrefetchSourceType::LLDP &&
        (enableLldpShadow || enableLldpAdmission)) {
        auto *owner = candidateOwners.find(metadata.prefetchCandidateId);
        if (owner) {
            owner->qualityHandle = lldpAdmissionControl.touch(qualityKey(
                PrefetchSourceType::LLDP, metadata.prefetchProducerPC,
                metadata.prefetchConsumerPC, dpp.pfInfo.contextId()));
            owner->hasQualityHandle = true;
            PrefetchQualityControl::Decision reason;
            const bool accepted = lldpAdmissionControl.admit(owner->qualityHandle,
                lldpAdmissionPolicy, qualityPressure(), &reason);
            stats.lldpShadowDecisions[unsigned(reason)]++;
            // Cold LLDP pairs retain periodic probes. Rejecting every cold
            // pair breaks address training and disproportionately harms
            // workloads with many producer/consumer keys (mcf). Admission
            // starts suppressing only after evidence is available.
            const bool retainCold = reason == PrefetchQualityControl::Decision::Cold;
            if (!accepted && !retainCold && enableLldpAdmission) {
                stats.lldpAdmissionRejected++;
                return true;
            }
        }
    }
    const Addr virtual_line = blockAddress(dpp.pfInfo.getAddr());
    const bool duplicate = filterCandidate(virtual_line);
    if (translated_source)
        tlbFilterTranslations[virtual_line] = blockAddress(paddr);
    if (!duplicate)
        return false;
    stats.filtered++;
    return true;
}

bool
LLDPrefetcher::rejectPrefetchCandidate(const PrefetchInfo &pfi,
                                       const AddrPriority &addr_prio)
{
    const bool translated_source = pfi.getXsMetadata().prefetchLldpVirtual;
    const Addr virtual_line = blockAddress(pfi.getAddr());
    const bool reject = translated_source &&
        pfi.getXsMetadata().prefetchCandidateId &&
        tlbFilterSet.count(virtual_line);
    if (reject) {
        const auto translation = tlbFilterTranslations.find(virtual_line);
        if (translation != tlbFilterTranslations.end()) {
            stats.replayPairObserved++;
            if (!enableDemandPairTraining)
                trainAddressPair(
                pfi.getXsMetadata().prefetchLldpAddrP,
                translation->second,
                pfi.getXsMetadata().prefetchProducerPC,
                pfi.getXsMetadata().prefetchConsumerPC,
                pfi.contextId());
        }
    }
    stats.filtered += reject;
    return reject;
}

void
LLDPrefetcher::prefetchDropped(const DeferredPacket &dpp)
{
    const auto metadata = dpp.pfInfo.getXsMetadata();
    if (metadata.prefetchCandidateId) {
        if (finishCandidate(metadata.prefetchCandidateId,
                            PrefetchQualityControl::Outcome::Dropped, 4))
            stats.candidateDropped++;
    }
}

void
LLDPrefetcher::dependenceTrain(const o3::XsDynInstMetaPtr &meta)
{
    if (!meta || !meta->lldpLoad || !meta->lldpChain.valid ||
        meta->lldpContext == InvalidContextID)
        return;
    if (meta->squashed) {
        stats.trainSquashed++;
        return;
    }
    if (meta->lldpChain.dualSrcRegisterOps != 0) {
        stats.dualSourceTrainRejected++;
        DPRINTF(LLDPrefetcher, "reject dual-source PCp=%#x PCc=%#x len=%u dual=%u imm=%u\n",
                meta->lldpChain.producerPC, meta->instAddr, meta->lldpChain.length,
                meta->lldpChain.dualSrcRegisterOps, meta->lldpChain.singleSrcImmediateOps);
        return;
    }
    if (!meta->lldpChain.trainable())
        return;
    if (input.size() >= trainingQueueSize) {
        stats.trainDropped++;
        return;
    }
    input.push_back({meta, meta->lldpChain, meta->instAddr,
                     meta->lldpContext, meta->lldpLoadImm, meta->lldpSize,
                     meta->lldpSigned,
                     int64_t(meta->lldpLoadLine), meta->lldpLoadOffset,
                     meta->lldpLoadAddressValid});
    stats.trainAccepted++;
    if (!learningEvent.scheduled())
        schedule(learningEvent, nextCycle());
}

LLDPrefetcher::Update
LLDPrefetcher::makeUpdate(Training train)
{
    if (train.owner->lldpLoadAddressValid) {
        train.loadLine = train.owner->lldpLoadLine;
        train.loadOffset = train.owner->lldpLoadOffset;
        train.loadAddressValid = true;
    }
    if (!train.loadAddressValid) {
        train.loadLine = train.loadImm >= 0 ?
            (train.loadImm / 64) * 64 :
            -(((-train.loadImm + 63) / 64) * 64);
        train.loadOffset = uint8_t(uint64_t(train.loadImm) & 63);
        train.loadAddressValid = true;
    }
    // An older S2 write may allocate/replace the row observed in S0. Revalidate
    // the lookup against that write before constructing the next row image.
    if (train.version != version) {
        stats.pipelineBypasses++;
        train.producer = findProducer(train.chain.producerPC, train.context);
        train.consumer = train.producer < 0 ? -1 :
            findConsumer(table[train.producer], train.consumerPC);
    }
    const bool allocate = train.producer < 0;
    unsigned row = allocate ? replacement.victim() : train.producer;
    if (allocate && enableLldtBenefit) {
        const auto score = [](const Entry &item) {
            int total = 0;
            for (const auto &sub : item.consumers)
                if (sub.valid)
                    total += sub.benefit.score();
            return total;
        };
        const unsigned original = row;
        for (unsigned i = 0; i < TableEntries; ++i)
            if (table[i].valid && score(table[i]) < score(table[row]))
                row = i;
        stats.benefitVictimChanges += original != row;
    }
    if (allocate) {
        for (unsigned i = 0; i < TableEntries; ++i)
            if (!table[i].valid) { row = i; break; }
    }
    Entry entry = allocate ? Entry() : table[row];
    if (allocate) {
        if (table[row].valid)
            entry.replacementCount = table[row].replacementCount + 1;
        entry.valid = true;
        entry.producerPC = train.chain.producerPC;
        entry.context = train.context;
        entry.generation = ++generation;
        entry.pConf = producerInitialConf;
    }
    const bool allocateSub = allocate || train.consumer < 0;
    unsigned col = allocateSub ? entry.replacement.victim() : train.consumer;
    if (allocateSub && enableLldtBenefit && !allocate) {
        const unsigned original = col;
        for (unsigned i = 0; i < SubEntries; ++i)
            if (entry.consumers[i].valid &&
                entry.consumers[i].benefit.score() < entry.consumers[col].benefit.score())
                col = i;
        stats.benefitVictimChanges += original != col;
    }
    if (allocateSub) {
        if (!allocate && validChildren(entry) == SubEntries)
            entry.consumerOverflow++;
        for (unsigned i = 0; i < SubEntries; ++i)
            if (!entry.consumers[i].valid) { col = i; break; }
        if (!allocate)
            entry.consumerReplacement++;
    }
    auto &sub = entry.consumers[col];
    if (allocateSub) {
        const uint64_t replacements = sub.replacementCount + sub.valid;
        sub = {};
        sub.replacementCount = replacements;
        sub.valid = true;
        sub.consumerPC = train.consumerPC;
        sub.generation = ++consumerGeneration;
        sub.immLine = train.loadImm >= 0 ? train.loadImm / 64 :
            -(((-train.loadImm + 63) / 64));
        sub.loadLine = train.loadLine;
        sub.immOffset = uint8_t(uint64_t(train.loadImm) & 63);
        sub.loadOffset = train.loadOffset;
        sub.loadSize = train.loadSize;
        sub.loadSigned = train.loadSigned;
        sub.loadAddressValid = train.loadAddressValid;
        sub.cConf = sub.immConf = sub.lineConf = sub.offsetConf = initialConf;
        if (!allocate)
            entry.pConf = std::min<unsigned>(maxConf, entry.pConf + 1);
    } else {
        // A hint may be waiting while the child is retrained.  Only a changed
        // replay recipe invalidates it; identical confidence refreshes do not.
        if (sub.chain.valid != train.chain.valid ||
            sub.chain.replayable != train.chain.replayable ||
            sub.chain.length != train.chain.length ||
            sub.chain.ops != train.chain.ops ||
            sub.loadImm != train.loadImm ||
            sub.loadSize != train.loadSize ||
            sub.loadSigned != train.loadSigned)
            sub.generation = ++consumerGeneration;
        const int64_t newLine = train.loadImm >= 0 ? train.loadImm / 64 :
            -(((-train.loadImm + 63) / 64));
        const uint8_t newOffset = uint8_t(uint64_t(train.loadImm) & 63);
        if (sub.loadImm == train.loadImm) {
            sub.immConf = std::min<unsigned>(maxConf, sub.immConf + 1);
            stats.exactImmStable++;
            sub.exactImmStableCount++;
        } else if (sub.immConf)
            --sub.immConf;
        if (sub.immLine == newLine) {
            stats.sameLineImmUpdates++;
            stats.lineImmStable++;
            sub.lineImmStableCount++;
            sub.lineConf = std::min<unsigned>(maxConf, sub.lineConf + 1);
        } else if (newLine == sub.immLine + 1) {
            stats.nextLineImmUpdates++;
            if (sub.lineConf)
                --sub.lineConf;
        } else {
            stats.otherLineImmUpdates++;
            if (sub.lineConf)
                --sub.lineConf;
        }
        const bool sameOffset = newOffset == sub.immOffset;
        if (sameOffset) {
            stats.offsetImmStable++;
            sub.offsetConf = std::min<unsigned>(maxConf, sub.offsetConf + 1);
        } else if (sub.offsetConf) {
            --sub.offsetConf;
        }
        const int64_t lineDelta = newLine - sub.immLine;
        stats.lineDeltaHist[std::clamp<int64_t>(lineDelta, -64, 64) + 64]++;
        const int offsetDelta = int(newOffset) - int(sub.immOffset);
        stats.offsetDeltaHist[std::clamp(offsetDelta, -63, 63) + 63]++;
        stats.immValueHist[uint8_t(uint64_t(train.loadImm - sub.loadImm) & 255)]++;
        const unsigned sizeBucket = train.loadSize == 1 ? 0 :
            train.loadSize == 2 ? 1 : train.loadSize == 4 ? 2 :
            train.loadSize == 8 ? 3 : 4;
        stats.byteOffsetHist[sizeBucket * 64 + newOffset]++;
        sub.cConf = std::min<unsigned>(maxConf, sub.cConf + 1);
    }
    sub.chain = train.chain;
    sub.loadImm = train.loadImm;
    sub.immLine = train.loadImm >= 0 ? train.loadImm / 64 :
        -(((-train.loadImm + 63) / 64));
    sub.loadLine = train.loadLine;
    sub.immOffset = uint8_t(uint64_t(train.loadImm) & 63);
    sub.loadOffset = train.loadOffset;
    sub.loadSize = train.loadSize;
    sub.loadSigned = train.loadSigned;
    sub.loadAddressValid = train.loadAddressValid;
    sub.updateCount++;
    entry.updateCount++;
    entry.activePeak = std::max<uint64_t>(entry.activePeak, validChildren(entry));
    entry.replacement.touch(col);
    return {train, row, entry};
}

unsigned
LLDPrefetcher::validChildren(const Entry &entry) const
{
    return std::count_if(entry.consumers.begin(), entry.consumers.end(),
                         [](const SubEntry &s) { return s.valid; });
}

void
LLDPrefetcher::commitUpdate(const Update &update)
{
    const auto &old = table[update.row];
    Entry committed = update.entry;
    const auto &next = committed;
    if (old.valid && old.generation != next.generation) {
        stats.producerReplacements++;
        stats.childrenAtReplacement[validChildren(old)]++;
    } else if (old.valid) {
        const int before = findConsumer(old, update.training.consumerPC);
        const int after = findConsumer(next, update.training.consumerPC);
        if (before >= 0 && after >= 0) {
            const auto &a = old.consumers[before];
            const auto &b = next.consumers[after];
            stats.lengthChanges += a.chain.length != b.chain.length;
            bool opChange = false, immChange = a.loadImm != b.loadImm;
            for (unsigned i = 0; i < 2; ++i) {
                opChange |= a.chain.ops[i].op != b.chain.ops[i].op ||
                    a.chain.ops[i].word != b.chain.ops[i].word ||
                    a.chain.ops[i].replayable != b.chain.ops[i].replayable;
                immChange |= a.chain.ops[i].imm != b.chain.ops[i].imm;
            }
            stats.opChanges += opChange;
            stats.immChanges += immChange;
        }
    }
    if (old.valid && old.generation == committed.generation) {
        committed.candidateCount = std::max(
            committed.candidateCount, old.candidateCount);
        committed.usefulCount = std::max(
            committed.usefulCount, old.usefulCount);
        committed.lateCount = std::max(committed.lateCount, old.lateCount);
        committed.demandHitCount = std::max(
            committed.demandHitCount, old.demandHitCount);
        if (old.demandHitCount > update.entry.demandHitCount) {
            committed.lastDemandPC = old.lastDemandPC;
            committed.lastDemandChainId = old.lastDemandChainId;
            committed.lastDemandSource = old.lastDemandSource;
        }
        for (const auto &oldSub : old.consumers) {
            if (!oldSub.valid)
                continue;
            const int col = findConsumer(committed, oldSub.consumerPC);
            if (col < 0 || committed.consumers[col].generation != oldSub.generation)
                continue;
            auto &sub = committed.consumers[col];
            sub.unusedCount = oldSub.unusedCount;
            sub.collisionCount = oldSub.collisionCount;
            sub.benefit = oldSub.benefit;
            sub.candidateCount = std::max(
                sub.candidateCount, oldSub.candidateCount);
            sub.usefulCount = std::max(sub.usefulCount, oldSub.usefulCount);
            sub.lateCount = std::max(sub.lateCount, oldSub.lateCount);
            sub.demandHitCount = std::max(
                sub.demandHitCount, oldSub.demandHitCount);
        }
    }
    table[update.row] = committed;
    replacement.touch(update.row);
    ++version;
    stats.producerWrites++;
    DPRINTF(LLDPrefetcher, "s2 PCp=%#x PCc=%#x len=%u row=%u\n",
            next.producerPC, update.training.consumerPC,
            update.training.chain.length, update.row);
}

void
LLDPrefetcher::learningTick()
{
    // s2 write, s1 construct, s0 lookup: one accepted train per cycle.
    if (s1) {
        if (!s1->training.owner->squashed)
            commitUpdate(*s1);
        else
            stats.trainSquashed++;
        s1.reset();
    }
    if (s0) {
        if (!s0->owner->squashed)
            s1 = makeUpdate(*s0);
        else
            stats.trainSquashed++;
        s0.reset();
    }
    if (!s0 && !input.empty()) {
        s0 = input.front();
        input.pop_front();
        s0->producer = findProducer(s0->chain.producerPC, s0->context);
        s0->consumer = s0->producer < 0 ? -1 :
            findConsumer(table[s0->producer], s0->consumerPC);
        s0->version = version;
    }
    if (!input.empty() || s0 || s1)
        schedule(learningEvent, nextCycle());
}

lldp::Hint
LLDPrefetcher::pfHint(const PacketPtr &pkt)
{
    lldp::Hint hint;
    const int row = findProducer(pkt->req->getPC(), pkt->req->contextId());
    if (row < 0)
        return hint;
    stats.producerMatches++;
    replacement.touch(row);
    const auto &entry = table[row];
    if (entry.pConf < producerThreshold)
        return hint;
    auto meta = pkt->req->getXsMetadata().instXsMetadata;
    hint.valid = true;
    hint.producerPC = entry.producerPC;
    hint.generation = entry.generation;
    for (unsigned col = 0; col < SubEntries; ++col)
        hint.consumerGenerations[col] = entry.consumers[col].generation;
    hint.offset = pkt->req->getPaddr() & (blkSize - 1);
    if (meta) {
        hint.size = meta->lldpSize;
        hint.signExtend = meta->lldpSigned;
    } else {
        const auto &pf_meta = pkt->req->getXsMetadata();
        hint.offset = pf_meta.prefetchDataOffset;
        hint.size = pf_meta.prefetchDataSize;
        hint.signExtend = pf_meta.prefetchDataSignExtend;
    }
    if (!hint.size || hint.offset + hint.size > blkSize ||
        (!pkt->req->isPrefetch() && pkt->req->getSize() != hint.size)) {
        hint.valid = false;
        stats.unsupported++;
    }
    return hint;
}

lldp::Hint
LLDPrefetcher::loadTrain(const PacketPtr &pkt, bool miss)
{
    const bool spatial_pf = isSpatialPrefetch(pkt);
    const bool lldp_pf = pkt && pkt->req && pkt->req->isPrefetch() &&
        pkt->req->hasXsMetadata() &&
        isLldpSource(pkt->req->getXsMetadata().prefetchSource);
    if (lldp_pf) {
        const auto &metadata = pkt->req->getXsMetadata();
        if (!allowLldpChainTrigger(metadata.prefetchSource,
                                   metadata.prefetchLldpChainDepth,
                                   maxLldpcChainDepth)) {
            if (metadata.prefetchSource == PrefetchSourceType::LLDPC)
                stats.chainStoppedBySource++;
            else
                stats.chainStoppedByDepth++;
            return {};
        }
        stats.chainTriggerAccepted++;
    }
    if (!pkt->isRead() || (!pkt->isDemand() && !spatial_pf && !lldp_pf) ||
        pkt->req->isInstFetch() || pkt->req->isUncacheable() ||
        pkt->req->isCacheMaintenance() ||
        (!pkt->req->hasVaddr() && !spatial_pf && !lldp_pf) ||
        !pkt->req->hasPC() || !pkt->req->hasContextId() ||
        !pkt->req->hasXsMetadata())
        return {};
    if (spatial_pf)
        stats.spatialLoadTrain++;
    const auto meta = pkt->req->getXsMetadata().instXsMetadata;
    if (!spatial_pf && !lldp_pf && (!meta || !meta->lldpLoad || meta->squashed))
        return {};
    // L1 learns at successful IQ issue. L2 sees only requests forwarded by
    // L1, and learns those at its own tag-result boundary (hit or miss).
    if (!spatial_pf && !lldp_pf && !trainingCPU)
        dependenceTrain(meta);
    auto hint = pfHint(pkt);
    if (hint.valid) {
        ageMetaTable();
        hint.spatial = spatial_pf;
        hint.chain = lldp_pf;
        const Addr addr_p = blockAddress(pkt->req->getPaddr()) | hint.offset;
        const int producer = findProducer(
            hint.producerPC, pkt->req->contextId());
        if (producer < 0)
            return hint;
        uint8_t covered = 0;
        unsigned eligible = 0;
        for (unsigned col = 0; col < SubEntries; ++col) {
            const auto &sub = table[producer].consumers[col];
            if (!sub.valid || sub.cConf < consumerThreshold ||
                sub.immConf < immediateThreshold)
                continue;
            ++eligible;
            const auto meta_hit = lookupMetaTable(
                addr_p, hint.producerPC, sub.consumerPC,
                pkt->req->contextId());
            const auto source = lldp_pf ? PrefetchSourceType::LLDPC :
                PrefetchSourceType::LLDPT;
            if (meta_hit && queueCandidate(pkt, hint, addr_p, meta_hit->addrC,
                                           source, col,
                                           meta_hit)) {
                covered |= uint8_t(1U << col);
                continue;
            }
        }
        hint.metaCoveredMask = covered;
        if (eligible > unsigned(__builtin_popcount(covered)))
            stats.metaFallbacks++;
        if (covered && unsigned(__builtin_popcount(covered)) >= eligible) {
            hint.valid = false;
            return hint;
        }
        if (!miss && covered && !lldp_pf) {
            hint.valid = false;
            return hint;
        }
        if (miss || lldp_pf) {
            stats.hints++;
            if (!miss)
                stats.hitHintsRetained++;
            if (spatial_pf)
                stats.spatialHints++;
        } else {
            stats.hitHintsDiscarded++;
            hint.valid = false;
        }
    }
    return hint;
}

bool
LLDPrefetcher::queueCandidate(const PacketPtr &demand,
                              const lldp::Hint &hint, Addr addr_p,
                              Addr addr_c, PrefetchSourceType source,
                              std::optional<unsigned> consumer,
                              std::optional<MetaHit> meta_hit,
                              std::optional<uint8_t> data_offset)
{
    stats.candidates++;
    stats.candidateGenerated++;
    CandidateOwner initial{};
    initial.source = source;
    initial.producerPC = hint.producerPC;
    initial.generation = hint.generation;
    initial.context = demand->req->contextId();
    initial.candidateAddress = addr_c;
    const int initial_row = findProducer(hint.producerPC, initial.context);
    if (initial_row >= 0 && consumer && *consumer < SubEntries)
        initial.consumerPC = table[initial_row].consumers[*consumer].consumerPC;
    const uint64_t id = candidateOwners.allocate(requestorId, initial);
    if (!id) {
        stats.ownerFull++;
        return false;
    }
    auto &owner = *candidateOwners.find(id);
    traceCandidate(id, "generated", owner);
    struct PendingOwner
    {
        LLDPrefetcher &parent;
        uint64_t id;
        bool queued{false};
        ~PendingOwner() {
            if (!queued && parent.candidateOwners.find(id))
                parent.finishCandidate(id, PrefetchQualityControl::Outcome::Dropped, 4);
        }
    } pending{*this, id};
    stats.ownerPeak = std::max<uint64_t>(stats.ownerPeak.value(), candidateOwners.size());
    revokePressureBoosts();
    if (!admitPfControlCandidate(source)) {
        if (source == PrefetchSourceType::LLDPS)
            emitSpatialFeedback(demand, false);
        return false;
    }
    PrefetchQualityControl::Handle quality_handle{};
    bool has_quality_handle = false;
    if ((enableQualityControl && source == PrefetchSourceType::LLDPC) ||
        source == PrefetchSourceType::LLDPS) {
        const Addr consumer_pc = source == PrefetchSourceType::LLDPC && consumer ?
            (findProducer(hint.producerPC, demand->req->contextId()) >= 0 ?
             table[findProducer(hint.producerPC, demand->req->contextId())]
                 .consumers[*consumer].consumerPC : 0) : 0;
        const Addr producer_pc = source == PrefetchSourceType::LLDPS &&
            demand->req->hasPC() ? demand->req->getPC() : hint.producerPC;
        auto key = qualityKey(source, producer_pc, consumer_pc,
                              demand->req->contextId());
        if (source == PrefetchSourceType::LLDPS)
            key.provider = demand->req->getXsMetadata().prefetchSource;
        quality_handle = touchQuality(key);
        const unsigned pressure = qualityPressure();
        if (source == PrefetchSourceType::LLDPC && enableLldpcQuality) {
            PrefetchQualityControl::Decision reason;
            const bool accepted = qualityControl.admit(
                quality_handle, qualityPolicy, pressure, &reason);
            stats.qualityDecisions[unsigned(reason)]++;
            if (!accepted) {
                stats.qualityRejected++;
                return false;
            }
        }
        has_quality_handle = true;
    }
    const Addr origin_addr = demand->req->hasVaddr() ?
        demand->req->getVaddr() : demand->req->getPaddr();
    PrefetchInfo origin(demand, origin_addr, true,
                        Request::XsMetadata(source));
    PrefetchInfo candidate(origin, addr_c);
    auto metadata = Request::XsMetadata(
        source, 0, hint.producerPC, hint.generation, id);
    if (source == PrefetchSourceType::LLDPC) {
        const unsigned prior = demand->req->getXsMetadata().prefetchLldpChainDepth;
        if (!allowLldpChainTrigger(
                demand->req->getXsMetadata().prefetchSource,
                prior, maxLldpcChainDepth)) {
            stats.chainStoppedByDepth++;
            return false;
        }
        metadata.prefetchLldpChainDepth = prior + 1;
    }
    metadata.prefetchLldpAddrP = addr_p;
    metadata.prefetchLldpVirtual = !meta_hit;
    if (consumer) {
        const int producer = findProducer(
            hint.producerPC, demand->req->contextId());
        if (producer >= 0) {
            const auto &sub = table[producer].consumers[*consumer];
            metadata.prefetchConsumerPC =
                sub.consumerPC;
            metadata.prefetchDataOffset = data_offset.value_or(sub.loadOffset);
            metadata.prefetchDataSize = sub.loadSize;
            metadata.prefetchDataSignExtend = sub.loadSigned;
        }
    } else if (meta_hit) {
        metadata.prefetchConsumerPC =
            metaTable[meta_hit->set][meta_hit->way].consumerPC;
    }
    candidate.setXsMetadata(metadata);

    AddrPriority command(addr_c, 0, source);
    command.isVA = metadata.prefetchLldpVirtual;
    command.forceTranslation = command.isVA;
    command.depth = demand->req->hasXsMetadata() ?
        demand->req->getXsMetadata().prefetchDepth + 1 : 1;
    statsQueued.pfIdentified++;

    const int row = findProducer(
        hint.producerPC, demand->req->contextId());
    if (row >= 0 && table[row].generation == hint.generation) {
        table[row].candidateCount++;
        if (consumer && *consumer < SubEntries)
            table[row].consumers[*consumer].candidateCount++;
    }
    owner.generation = hint.generation;
    owner.consumerPC = metadata.prefetchConsumerPC;
    owner.source = source;
    owner.feedbackPC = demand->req->hasPC() ? demand->req->getPC() : 0;
    owner.feedbackSource = demand->req->hasXsMetadata() ?
        demand->req->getXsMetadata().prefetchSource : PrefetchSourceType::PF_NONE;
    owner.qualityHandle = quality_handle;
    owner.hasQualityHandle = has_quality_handle;
    if (meta_hit) {
        auto &entry = metaTable[meta_hit->set][meta_hit->way];
        if (!entry.valid || entry.generation != meta_hit->generation)
            return false;
        owner.meta = true;
        owner.metaSet = meta_hit->set;
        owner.metaWay = meta_hit->way;
        owner.metaGeneration = meta_hit->generation;
        owner.metaReserved = true;
        owner.metaSpentToken = entry.tokens != 0;
    } else {
        if (row < 0 || !consumer || *consumer >= SubEntries ||
            table[row].generation != hint.generation)
            return false;
        owner.hasDependency = true;
        owner.row = unsigned(row);
        owner.col = *consumer;
        owner.consumerGeneration = table[row].consumers[*consumer].generation;
    }
    metadata.prefetchCandidateId = id;
    candidate.setXsMetadata(metadata);
    if (meta_hit) {
        auto &entry = metaTable[meta_hit->set][meta_hit->way];
        entry.tokens = entry.tokens ? entry.tokens - 1 : 0;
        ++entry.outstanding;
        entry.rrpv = 0;
    }
    if (!insert(demand, candidate, command)) {
        finishCandidate(id, PrefetchQualityControl::Outcome::Dropped, 4);
        if (source == PrefetchSourceType::LLDPS)
            emitSpatialFeedback(demand, false);
        return false;
    }
    pending.queued = true;
    traceCandidate(id, "queued", owner);
    stats.candidateQueued++;
    if (meta_hit)
        stats.metaTablePrefetches++;
    if (source == PrefetchSourceType::LLDPS &&
        (!enableQualityControl || !enableLldpsFeedbackQuality))
        emitSpatialFeedback(demand, true);
    return true;
}

void
LLDPrefetcher::traceCandidate(uint64_t id, const char *event, const CandidateOwner &owner)
{
    if (!enableCandidateTrace || ((id ^ (id >> 16)) % traceSampleInterval) ||
        (traceProducerPC && owner.producerPC != traceProducerPC))
        return;
    candidateTrace << curTick() << ',' << event << ',' << id << ','
        << unsigned(owner.source) << ',' << (cache ? cache->level() : 0) << ','
        << owner.context << ',' << owner.producerPC << ',' << owner.consumerPC << ','
        << owner.generation << ',' << owner.consumerGeneration << ',' << owner.metaGeneration << ','
        << owner.qualityHandle.generation << ',' << owner.candidateAddress << ','
        << owner.hasIssued << ',' << owner.terminal << '\n';
}

void
LLDPrefetcher::notifyCandidateEvent(const Request::XsMetadata &metadata,
                                   Addr address, unsigned event)
{
    if (!metadata.prefetchCandidateId || !isLldpSource(metadata.prefetchSource))
        return;
    auto *owner = candidateOwners.find(metadata.prefetchCandidateId);
    CandidateOwner archived{};
    if (!owner) {
        archived.source = metadata.prefetchSource;
        archived.producerPC = metadata.prefetchProducerPC;
        archived.consumerPC = metadata.prefetchConsumerPC;
        archived.generation = metadata.prefetchGeneration;
        owner = &archived;
    }
    CandidateOwner record = *owner;
    record.candidateAddress = address;
    traceCandidate(metadata.prefetchCandidateId, event == 0 ? "refill" : "evicted", record);
}

void
LLDPrefetcher::samplePfq()
{
    std::array<unsigned, NUM_PF_SOURCES> used{};
    for (const auto &packet : pfq)
        ++used[packet.pfInfo.getXsMetadata().prefetchSource];
    stats.pfqSamples++;
    for (unsigned source = 0; source < NUM_PF_SOURCES; ++source) {
        stats.pfqCurrent[source] = used[source];
        stats.pfqPeak[source] = std::max<uint64_t>(stats.pfqPeak[source].value(), used[source]);
        stats.pfqOccupancySamples[source] += used[source];
    }
}

unsigned
LLDPrefetcher::qualityPressure() const
{
    return queueSize ?
        unsigned(std::min<size_t>(100, (pfq.size() * 100) / queueSize)) : 100;
}

PrefetchQualityControl::Handle
LLDPrefetcher::touchQuality(const PrefetchQualityControl::Key &key)
{
    return qualityControl.touch(key,
        [this](const PrefetchQualityControl::Key &, PrefetchQualityControl::Handle old,
               bool boosted) {
            if (boosted) {
                // This callback runs before the quality slot is overwritten.
                sendSpatialFeedback(old, false);
                stats.qualityEvictionRevokes++;
                stats.qualityRevokes++;
            }
        });
}

void
LLDPrefetcher::revokePressureBoosts()
{
    if (qualityPressure() < lldpsFeedbackPolicy.maxPressurePct)
        return;
    // A bounded scan only on queue/candidate events, never a per-cycle walk.
    for (unsigned slot = 0; slot < feedbackSlots.size(); ++slot) {
        if (!feedbackSlots[slot].valid)
            continue;
        const PrefetchQualityControl::Handle handle{
            slot, feedbackQualityGenerations[slot]};
        if (qualityControl.setBoost(handle, false) < 0) {
            sendSpatialFeedback(handle, false);
            stats.qualityPressureRevokes++;
            stats.qualityRevokes++;
        }
    }
}

void
LLDPrefetcher::emitSpatialFeedback(const PacketPtr &demand, bool valid)
{
    // Quality-controlled feedback changes only with completed evidence or
    // explicit replacement/pressure. An unrelated queue reject cannot revoke it.
    if (enableQualityControl && enableLldpsFeedbackQuality)
        return;
    if (!enableSpatialFeedback || !spatialFeedbackHandler || !demand || !demand->req ||
        !demand->req->hasPC() || !demand->req->hasXsMetadata())
        return;
    const auto source = demand->req->getXsMetadata().prefetchSource;
    if (source != PrefetchSourceType::SStream &&
        source != PrefetchSourceType::StoreStream &&
        source != PrefetchSourceType::SStride)
        return;
    PrefetchQualityControl::Key key{PrefetchSourceType::LLDPS,
        demand->req->getPC(), 0, demand->req->contextId(), source};
    const auto handle = touchQuality(key);
    valid = valid && qualityPressure() < lldpsFeedbackPolicy.maxPressurePct;
    const int change = qualityControl.setBoost(handle, valid);
    if (change)
        sendSpatialFeedback(handle, valid);
}

void
LLDPrefetcher::sendSpatialFeedback(PrefetchQualityControl::Handle handle,
                                    bool valid)
{
    if (!enableSpatialFeedback || !spatialFeedbackHandler)
        return;
    const auto *key = qualityControl.key(handle);
    if (!key || key->source != PrefetchSourceType::LLDPS ||
        (key->provider != PrefetchSourceType::SStream &&
         key->provider != PrefetchSourceType::StoreStream &&
         key->provider != PrefetchSourceType::SStride))
        return;
    auto &feedback = feedbackSlots[handle.index];
    if (valid) {
        // Each boost, including a reboost in the same quality generation, owns
        // a new monotonic token. Old revokes cannot cancel the new boost.
        feedback = {key->provider, key->producerPC, key->context, handle.index,
                    ++feedbackGeneration, true};
        feedbackQualityGenerations[handle.index] = handle.generation;
    } else {
        if (!feedback.valid || feedbackQualityGenerations[handle.index] != handle.generation)
            return;
        feedback.valid = false;
    }
    stats.spatialFeedbackSignals++;
    stats.spatialFeedbackValid += valid;
    spatialFeedback(feedback);
}

void
LLDPrefetcher::spatialFeedback(const SpatialFeedback &feedback)
{
    if (enableSpatialFeedback && spatialFeedbackHandler)
        spatialFeedbackHandler(feedback);
}

void
LLDPrefetcher::hintData(const lldp::Hint &hint, const PacketPtr &demand,
                       Addr addr_p, const uint8_t *data, unsigned size)
{
    if (!hint.valid || !data || hint.offset + hint.size > size)
        return;
    if (hint.chain && demand->req->hasXsMetadata() &&
        !allowLldpChainTrigger(
            demand->req->getXsMetadata().prefetchSource,
            demand->req->getXsMetadata().prefetchLldpChainDepth,
            maxLldpcChainDepth)) {
        const auto source = demand->req->getXsMetadata().prefetchSource;
        if (source == PrefetchSourceType::LLDPC)
            stats.chainStoppedBySource++;
        else
            stats.chainStoppedByDepth++;
        return;
    }
    const auto meta = demand->req->hasXsMetadata() ?
        demand->req->getXsMetadata().instXsMetadata : nullptr;
    if (meta && meta->squashed)
        return;
    stats.returnedHints++;
    const int row = findProducer(hint.producerPC, demand->req->contextId());
    if (row < 0 || table[row].generation != hint.generation) {
        stats.staleHints++;
        return;
    }
    uint64_t value = 0;
    for (unsigned byte = 0; byte < hint.size; ++byte)
        value |= uint64_t(data[hint.offset + byte]) << (8 * byte);
    if (hint.signExtend && hint.size < 8 &&
        (value & (uint64_t(1) << (hint.size * 8 - 1))))
        value |= (~uint64_t(0)) << (hint.size * 8);
    const Addr hinted_addr_p = blockAddress(addr_p) | hint.offset;
    std::set<Addr> generated;
    bool stale_consumer = false;
    for (unsigned col = 0; col < SubEntries; ++col) {
        auto &sub = table[row].consumers[col];
        // A covered column already emitted its frozen MetaTable candidate at
        // loadTrain time; it does not read the current consumer recipe here.
        if (hint.metaCoveredMask & uint8_t(1U << col))
            continue;
        if (!hint.consumerMatches(col, sub.generation)) {
            if (hint.consumerGenerations[col]) {
                stats.staleHintConsumers++;
                stale_consumer = true;
            }
            continue;
        }
        if (!sub.valid || sub.cConf < consumerThreshold || sub.immConf < immediateThreshold)
            continue;
        bool valid = sub.chain.trainable();
        uint64_t address = value;
        for (unsigned i = 0; valid && i + 1 < sub.chain.length; ++i)
            valid &= lldp::apply(sub.chain.ops[i], address);
        if (!valid) {
            stats.unsupported++;
            continue;
        }
        const Addr load_addr = address + uint64_t(sub.loadImm);
        address = blockAddress(load_addr);
        if (!generated.insert(address).second) {
            stats.duplicates++;
            continue;
        }
        const auto source = hint.chain ?
            PrefetchSourceType::LLDPC :
            (hint.spatial ? PrefetchSourceType::LLDPS :
             PrefetchSourceType::LLDP);
        queueCandidate(demand, hint, hinted_addr_p, address, source, col,
                       std::nullopt, load_addr & (blkSize - 1));
        DPRINTF(LLDPrefetcher, "prefetch PCp=%#x PCc=%#x value=%#x va=%#x offset=%u\n",
                hint.producerPC, sub.consumerPC, value, address, hint.offset);
    }
    stats.staleHints += stale_consumer;
}

void
LLDPrefetcher::addToQueue(std::list<DeferredPacket> &queue, DeferredPacket &dpp)
{
    Queued::addToQueue(queue, dpp);
    samplePfq();
    revokePressureBoosts();
    if (&queue == &pfq && !pfq.empty() && packetReady)
        packetReady(pfq.front().tick);
}

void
LLDPrefetcher::rxHint(BaseMMU::Translation *translation)
{
    // Upstream prefetch-ahead packets use the existing downstream interface.
    // Copy the descriptor as WorkerPrefetcher does; its packet is transferred.
    auto *incoming = static_cast<DeferredPacket *>(translation);
    DeferredPacket dpp = *incoming;
    dpp.owner = this;
    if (dpp.pkt && dpp.pkt->req->hasXsMetadata() &&
        dpp.pkt->req->getXsMetadata().prefetchCandidateId &&
        tlbFilterSet.count(blockAddress(dpp.pfInfo.getAddr()))) {
        stats.filtered++;
        prefetchDropped(dpp);
        delete dpp.pkt;
        return;
    }
    if (admitPfControlDeferredPacket(dpp))
        addToQueue(pfq, dpp);
    else {
        prefetchDropped(dpp);
        delete dpp.pkt;
    }
}

void
LLDPrefetcher::preDumpStats()
{
    Queued::preDumpStats();
    samplePfq();
    if (enableCandidateTrace)
        candidateTrace.flush();
    stats.ownerCurrent = candidateOwners.size();
    stats.ownerPeak = std::max<uint64_t>(stats.ownerPeak.value(), candidateOwners.size());
    stats.validProducers = 0;
    stats.samplerValidEntries = 0;
    stats.metaValidEntries = 0;
    for (const auto &set : samplerTable)
        for (const auto &entry : set)
            stats.samplerValidEntries += entry.valid;
    for (const auto &set : metaTable)
        for (const auto &entry : set)
            stats.metaValidEntries += entry.valid;
    for (unsigned i = 0; i <= SubEntries; ++i)
        stats.childrenAtDump[i] = 0;
    for (unsigned i = 0; i <= SubEntries; ++i)
        stats.pcpActiveHistogram[i] = 0;
    for (unsigned row = 0; row < table.size(); ++row) {
        const auto &entry = table[row];
        stats.pcpProducerPC[row] = entry.producerPC;
        stats.pcpContext[row] = entry.context;
        stats.pcpValid[row] = entry.valid;
        stats.pcpConf[row] = entry.pConf;
        stats.pcpDemandPC[row] = entry.lastDemandPC;
        stats.pcpDemandChainId[row] = entry.lastDemandChainId;
        stats.pcpDemandSource[row] = unsigned(entry.lastDemandSource);
        stats.pcpDemandHits[row] = entry.demandHitCount;
        stats.pcpActivePeak[row] = entry.activePeak;
        stats.pcpConsumerReplacement[row] = entry.consumerReplacement;
        stats.pcpConsumerOverflow[row] = entry.consumerOverflow;
        stats.pcpCandidate[row] = entry.candidateCount;
        stats.pcpUseful[row] = entry.usefulCount;
        stats.pcpLate[row] = entry.lateCount;
        for (unsigned col = 0; col < SubEntries; ++col) {
            const auto &consumer = entry.consumers[col];
            const unsigned idx = row * SubEntries + col;
            stats.consumerValid[idx] = consumer.valid;
            stats.consumerPC[idx] = consumer.consumerPC;
            stats.consumerLength[idx] = consumer.chain.length;
            stats.consumerOp1[idx] = unsigned(consumer.chain.ops[0].op);
            stats.consumerOp2[idx] = unsigned(consumer.chain.ops[1].op);
            stats.consumerImmLoad[idx] = consumer.loadImm;
            stats.consumerImmConf[idx] = consumer.immConf;
            stats.consumerLineConf[idx] = consumer.lineConf;
            stats.consumerOffsetConf[idx] = consumer.offsetConf;
            stats.consumerConf[idx] = consumer.cConf;
            stats.consumerUpdates[idx] = consumer.updateCount;
            stats.consumerReplacements[idx] = consumer.replacementCount;
            stats.consumerExactImmStable[idx] =
                consumer.exactImmStableCount;
            stats.consumerLineImmStable[idx] =
                consumer.lineImmStableCount;
            stats.consumerCandidates[idx] = consumer.candidateCount;
            stats.consumerUseful[idx] = consumer.usefulCount;
            stats.consumerUnused[idx] = consumer.unusedCount;
            stats.consumerCollision[idx] = consumer.collisionCount;
            stats.consumerLate[idx] = consumer.lateCount;
            stats.consumerDemandHits[idx] = consumer.demandHitCount;
        }
        if (entry.valid) {
            stats.validProducers++;
            stats.childrenAtDump[validChildren(entry)]++;
            stats.pcpActiveHistogram[validChildren(entry)]++;
        }
    }
}

void
LLDPrefetcher::notifyPrefetchUseful(PrefetchSourceType source)
{
    Queued::notifyPrefetchUseful(source);
    if (isLldpSource(source))
        stats.candidateUseful++;
}

void
LLDPrefetcher::notifyPrefetchUseful(PrefetchSourceType source,
                                    uint64_t candidate_id)
{
    Queued::notifyPrefetchUseful(source);
    if (isLldpSource(source) && candidate_id) {
        stats.candidateUseful++;
        finishCandidate(candidate_id, PrefetchQualityControl::Outcome::Useful, 0);
    }
}

void
LLDPrefetcher::prefetchUnused(PrefetchSourceType source)
{
    Queued::prefetchUnused(source);
    if (isLldpSource(source))
        stats.candidateUnused++;
}

void
LLDPrefetcher::prefetchUnused(PrefetchSourceType source,
                              uint64_t candidate_id)
{
    Queued::prefetchUnused(source);
    if (isLldpSource(source) && candidate_id) {
        stats.candidateUnused++;
        finishCandidate(candidate_id, PrefetchQualityControl::Outcome::Unused, 3);
    }
}

void
LLDPrefetcher::prefetchUnused(Addr paddr, PrefetchSourceType source,
                              uint64_t candidate_id)
{
    prefetchUnused(source, candidate_id);
}

void
LLDPrefetcher::notifyPrefetchMerged(uint64_t candidate_id)
{
    if (!candidate_id)
        return;
    stats.candidateMerged++;
    finishCandidate(candidate_id, PrefetchQualityControl::Outcome::DemandMerged, 1);
}

void
LLDPrefetcher::notifyCandidateDemand(uint64_t candidate_id,
                                     const PacketPtr &demand)
{
    if (!candidate_id || !demand || !demand->req)
        return;
    const auto it = candidateOwners.find(candidate_id);
    if (!it || it->meta ||
        table[it->row].generation != it->generation ||
        table[it->row].consumers[it->col].generation !=
            it->consumerGeneration ||
        table[it->row].consumers[it->col].consumerPC !=
            it->consumerPC)
        return;
    auto &entry = table[it->row];
    entry.lastDemandPC = demand->req->hasPC() ? demand->req->getPC() : 0;
    entry.lastDemandChainId = it->generation;
    entry.lastDemandSource = it->source;
    entry.demandHitCount++;
    entry.consumers[it->col].demandHitCount++;
}

void
LLDPrefetcher::pfHitInCache(PrefetchSourceType source)
{
    Queued::pfHitInCache(source);
    if (isLldpSource(source)) {
        stats.candidateCacheHit++;
        stats.candidateLate++;
    }
}

void
LLDPrefetcher::pfHitInCache(PrefetchSourceType source,
                            uint64_t candidate_id)
{
    Queued::pfHitInCache(source);
    if (isLldpSource(source)) {
        stats.candidateCacheHit++;
        stats.candidateLate++;
    }
    if (isLldpSource(source) && candidate_id) {
        stats.cacheCollisionBySource[source]++;
        finishCandidate(candidate_id, PrefetchQualityControl::Outcome::Collision, 5);
    }
}

void
LLDPrefetcher::pfHitInMSHR(PrefetchSourceType source)
{
    Queued::pfHitInMSHR(source);
    if (isLldpSource(source)) {
        stats.candidateMshrHit++;
        stats.candidateLate++;
    }
}

void
LLDPrefetcher::pfHitInMSHR(PrefetchSourceType source,
                           uint64_t candidate_id, bool has_demand)
{
    Queued::pfHitInMSHR(source);
    if (isLldpSource(source)) {
        stats.candidateMshrHit++;
        stats.candidateLate++;
    }
    if (isLldpSource(source) && candidate_id) {
        if (has_demand)
            stats.demandMshrCollisionBySource[source]++;
        else
            stats.pfMshrCollisionBySource[source]++;
        finishCandidate(candidate_id,
            has_demand ? PrefetchQualityControl::Outcome::DemandMerged :
                         PrefetchQualityControl::Outcome::Collision,
            has_demand ? 1 : 5);
    }
}

void
LLDPrefetcher::pfHitInWB(PrefetchSourceType source)
{
    Queued::pfHitInWB(source);
    if (isLldpSource(source)) {
        stats.candidateWbHit++;
        stats.candidateLate++;
    }
}

void
LLDPrefetcher::pfHitInWB(PrefetchSourceType source,
                         uint64_t candidate_id)
{
    Queued::pfHitInWB(source);
    if (isLldpSource(source)) {
        stats.candidateWbHit++;
        stats.candidateLate++;
    }
    if (isLldpSource(source) && candidate_id) {
        stats.wbCollisionBySource[source]++;
        finishCandidate(candidate_id, PrefetchQualityControl::Outcome::Collision, 5);
    }
}

void
LLDPrefetcher::recordIssuedPrefetchStats(const PacketPtr &pkt)
{
    Base::recordIssuedPrefetchStats(pkt);
    if (pkt && pkt->req && pkt->req->hasXsMetadata() &&
        pkt->req->getXsMetadata().prefetchCandidateId) {
        stats.candidateIssued++;
        const auto it = candidateOwners.find(
            pkt->req->getXsMetadata().prefetchCandidateId);
        if (it && it->issue()) {
            it->candidateAddress = pkt->getAddr();
            traceCandidate(pkt->req->getXsMetadata().prefetchCandidateId, "issued", *it);
            samplePfq();
            if (it->hasQualityHandle) {
                auto &control = it->source == PrefetchSourceType::LLDP ?
                    lldpAdmissionControl : qualityControl;
                control.issued(it->qualityHandle);
            }
        } else if (it) {
            stats.ownerDuplicateIssue++;
        }
    }
}
} // namespace prefetch
} // namespace gem5
