#include "mem/cache/prefetch/xs_stream.hh"

#include <algorithm>
#include <cmath>

#include "base/logging.hh"
#include "debug/XsStreamPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "sim/core.hh"

namespace gem5
{
namespace prefetch
{

XsStreamPrefetcher::XsStreamPrefetcher(const XsStreamPrefetcherParams &p)
    : Queued(p),
      regionSize(p.region_size),
      regionBlks(p.region_size / p.block_size),    
      depth(p.enable_competitive_depth ? p.competitive_initial_depth :
            (p.enable_auto_depth ? p.initial_bdp_depth : p.xs_stream_depth)),
      bdpDepth(depth),
      enableAutoDepth(p.enable_auto_depth),
      enableReuseObserver(p.enable_reuse_observer),
      enableCompetitiveDepth(p.enable_competitive_depth),
      enableUsedMove(p.enable_used_move),
      enableUnusedMove(p.enable_unused_move),
      persistentUnusedMove(p.persistent_unused_move),
      enableL3StreamPre(p.enable_l3_stream_pre),
      l2Depth(p.xs_stream_l2_depth),
      depthLevels(p.depth_levels),
      competitiveDepthLevels(p.competitive_depth_levels),
      competitiveWindowRefills(p.competitive_window_refills),
      competitiveLateLow(p.competitive_late_low),
      competitiveLateHigh(p.competitive_late_high),
      competitiveUnusedLow(p.competitive_unused_low),
      competitiveUnusedHigh(p.competitive_unused_high),
      competitiveCreditThreshold(p.competitive_credit_threshold),
      competitiveSettleWindowCount(p.competitive_settle_windows),
      competitiveMaxLevelStep(p.competitive_max_level_step),
      usedMoveOnHits(p.used_move_on_hits),
      usedMoveOffHits(p.used_move_off_hits),
      usedMoveOnWindows(p.used_move_on_windows),
      usedMoveOffWindows(p.used_move_off_windows),
      unusedMoveOnWindows(p.unused_move_on_windows),
      unusedMoveOffWindows(p.unused_move_off_windows),
      unusedMoveMinPressure(p.unused_move_min_pressure),
      unusedMoveDepthPressureMode(p.unused_move_depth_pressure_mode),
      unusedMoveMinDepthUpCreditThreshold(
          p.unused_move_min_depth_up_credit_threshold),
      bdpWindowSent(p.bdp_window_sent),
      bdpMinMshrSamples(p.bdp_min_mshr_samples),
      bdpMinRefillSamples(p.bdp_min_refill_samples),
      bdpCalibrationFactor(p.bdp_calibration_factor),
      bdpUpRatio(p.bdp_up_ratio),
      bdpDownRatio(p.bdp_down_ratio),
      bdpUpConfirmWindows(p.bdp_up_confirm_windows),
      bdpDownConfirmWindows(p.bdp_down_confirm_windows),
      bdpStableWindowCount(p.bdp_stable_windows),
      bdpEwmaAlpha(p.bdp_ewma_alpha),
      bdpMaxLevelStep(p.bdp_max_level_step),
      deltaWindowSent(p.delta_window_sent),
      deltaMinLateSamples(p.delta_min_late_samples),
      deltaMinRefillToUseSamples(p.delta_min_refill_to_use_samples),
      deltaStep(p.delta_step),
      deltaMaxAbs(p.delta_max_abs),
      deltaUpConfirmWindows(p.delta_up_confirm_windows),
      deltaDownConfirmWindows(p.delta_down_confirm_windows),
      deltaHoldWindowCount(p.delta_hold_windows),
      lateTargetRate(p.late_target_rate),
      lateUpperThreshold(p.late_upper_threshold),
      lateLowerThreshold(p.late_lower_threshold),
      lateWeight(p.late_weight),
      refillToUseTargetCycles(p.refill_to_use_target_cycles),
      refillToUseTargetAlpha(p.refill_to_use_target_alpha),
      refillToUseEarlyRatio(p.refill_to_use_early_ratio),
      refillToUseLateRatio(p.refill_to_use_late_ratio),
      refillToUseWeight(p.refill_to_use_weight),
      deltaPressureThreshold(p.delta_pressure_threshold),
      accuracyMinSamples(p.accuracy_min_samples),
      accuracyConfirmWindows(p.accuracy_confirm_windows),
      usefulAccuracyThreshold(p.useful_accuracy_threshold),
      unusedReplacementThreshold(p.unused_replacement_threshold),
      disabledProbeIntervalCalls(p.disabled_probe_interval_calls),
      reenableUsefulThreshold(p.reenable_useful_threshold),
      reenableConfirmWindows(p.reenable_confirm_windows),
      feedbackStats(this),
      stream_array(p.xs_stream_entries, p.xs_stream_entries, p.xs_stream_indexing_policy,
                   p.xs_stream_replacement_policy, STREAMEntry()),
      streamBlkFilter(pfFilterSize)
{
    fatal_if(depthLevels.empty(), "depth_levels must not be empty");
    fatal_if(!std::is_sorted(depthLevels.begin(), depthLevels.end()),
             "depth_levels must be sorted");
    fatal_if(std::adjacent_find(depthLevels.begin(), depthLevels.end()) !=
                 depthLevels.end(),
             "depth_levels must not contain duplicates");
    fatal_if(competitiveDepthLevels.empty(),
             "competitive_depth_levels must not be empty");
    fatal_if(!std::is_sorted(competitiveDepthLevels.begin(),
                             competitiveDepthLevels.end()),
             "competitive_depth_levels must be sorted");
    fatal_if(std::adjacent_find(competitiveDepthLevels.begin(),
                                competitiveDepthLevels.end()) !=
                 competitiveDepthLevels.end(),
             "competitive_depth_levels must not contain duplicates");
    if (enableCompetitiveDepth) {
        fatal_if(std::find(competitiveDepthLevels.begin(),
                           competitiveDepthLevels.end(), depth) ==
                     competitiveDepthLevels.end(),
                 "initial competitive stream depth %d must be present in "
                 "competitive_depth_levels", depth);
    } else {
        fatal_if(std::find(depthLevels.begin(), depthLevels.end(), depth) ==
                     depthLevels.end(),
                 "initial stream depth %d must be present in depth_levels",
                 depth);
    }
    const auto initial_depth = std::find(
        competitiveDepthLevels.begin(), competitiveDepthLevels.end(), depth);
    if (initial_depth != competitiveDepthLevels.end()) {
        competitiveDepthIndex = std::distance(
            competitiveDepthLevels.begin(), initial_depth);
    }
    fatal_if(bdpWindowSent == 0 || deltaWindowSent == 0,
             "controller windows must be non-zero");
    fatal_if(competitiveWindowRefills == 0,
             "competitive_window_refills must be non-zero");
    fatal_if(competitiveLateLow >= competitiveLateHigh ||
                 competitiveUnusedLow >= competitiveUnusedHigh,
             "competitive pressure low thresholds must be below high thresholds");
    fatal_if(competitiveCreditThreshold <= 0.0,
             "competitive_credit_threshold must be positive");
    fatal_if(competitiveMaxLevelStep == 0,
             "competitive_max_level_step must be non-zero");
    fatal_if((enableCompetitiveDepth || enableUsedMove || enableUnusedMove) &&
                 !enableReuseObserver,
             "competitive depth and victim movement require the reuse observer");
    fatal_if(usedMoveOffHits >= usedMoveOnHits,
             "used_move_off_hits must be below used_move_on_hits");
    fatal_if(usedMoveOnWindows == 0 || usedMoveOffWindows == 0 ||
                 unusedMoveOnWindows == 0 || unusedMoveOffWindows == 0,
             "move-controller confirmation windows must be non-zero");
    fatal_if(unusedMoveMinPressure <= 0.0 || unusedMoveMinPressure > 1.0,
             "unused_move_min_pressure must be in (0, 1]");
    fatal_if(unusedMoveDepthPressureMode > 3,
             "unused_move_depth_pressure_mode must be 0, 1, 2, or 3");
    fatal_if(unusedMoveMinDepthUpCreditThreshold < competitiveCreditThreshold,
             "unused_move_min_depth_up_credit_threshold must be at least "
             "competitive_credit_threshold");
    fatal_if(disabledProbeIntervalCalls == 0,
             "disabled_probe_interval_calls must be non-zero");
    feedbackStats.currentDepth = depth;
    feedbackStats.currentDepthEpoch = depthEpoch;
}

void
XsStreamPrefetcher::resetStats()
{
    Queued::resetStats();
    feedbackStats.currentDepth = depth;
    feedbackStats.currentDepthEpoch = depthEpoch;
    feedbackStats.usedMoveActive = enableUsedMove && usedMoveState;
    feedbackStats.unusedMoveActive = enableUnusedMove && unusedMoveState;
}

XsStreamPrefetcher::FeedbackStats::FeedbackStats(XsStreamPrefetcher *parent)
    : statistics::Group(parent, "feedback"),
      ADD_STAT(windows, statistics::units::Count::get(),
               "Completed stream feedback windows"),
      ADD_STAT(cumulativeSent, statistics::units::Count::get(),
               "Stream prefetches dequeued since the last stats reset"),
      ADD_STAT(cumulativePdbLoadUses, statistics::units::Count::get(),
               "Stream PDB load uses since the last stats reset"),
      ADD_STAT(cumulativeLateEvents, statistics::units::Count::get(),
               "Stream late events since the last stats reset"),
      ADD_STAT(cumulativeRefillToUseSamples,
               statistics::units::Count::get(),
               "Stream PDB refill-to-first-use samples since stats reset"),
      ADD_STAT(cumulativeRefillToUseCycles,
               statistics::units::Cycle::get(),
               "Stream PDB refill-to-first-use cycles since stats reset"),
      ADD_STAT(cumulativeMshrResponseSamples,
               statistics::units::Count::get(),
               "Stream MSHR response samples since the last stats reset"),
      ADD_STAT(cumulativeMshrResponseCycles,
               statistics::units::Cycle::get(),
               "Stream MSHR response cycles since the last stats reset"),
      ADD_STAT(depthIncreases, statistics::units::Count::get(),
               "Automatic stream depth increases"),
      ADD_STAT(depthDecreases, statistics::units::Count::get(),
               "Automatic stream depth decreases"),
      ADD_STAT(highGainWindows, statistics::units::Count::get(),
               "BDP windows using at least 0.5 adaptive filter gain"),
      ADD_STAT(hysteresisHolds, statistics::units::Count::get(),
               "BDP upshift requests suppressed by hysteresis"),
      ADD_STAT(forecastClampedWindows, statistics::units::Count::get(),
               "BDP windows whose trend forecast reached its safety bound"),
      ADD_STAT(bdpWindows, statistics::units::Count::get(),
               "BDP control windows"),
      ADD_STAT(bdpIncreaseRequests, statistics::units::Count::get(),
               "BDP windows requesting an increase"),
      ADD_STAT(bdpDecreaseRequests, statistics::units::Count::get(),
               "BDP windows requesting a decrease"),
      ADD_STAT(bdpDepthChanges, statistics::units::Count::get(),
               "Committed BDP base-depth changes"),
      ADD_STAT(deltaWindows, statistics::units::Count::get(),
               "Delta control windows"),
      ADD_STAT(deltaIncreaseRequests, statistics::units::Count::get(),
               "Delta windows requesting an increase"),
      ADD_STAT(deltaDecreaseRequests, statistics::units::Count::get(),
               "Delta windows requesting a decrease"),
      ADD_STAT(deltaHolds, statistics::units::Count::get(),
               "Delta windows holding the current offset"),
      ADD_STAT(accuracyWindows, statistics::units::Count::get(),
               "Accuracy protection windows"),
      ADD_STAT(accuracyDisableEvents, statistics::units::Count::get(),
               "Accuracy protection disable events"),
      ADD_STAT(accuracyReenableEvents, statistics::units::Count::get(),
               "Accuracy protection re-enable events"),
      ADD_STAT(finalDepthUpdates, statistics::units::Count::get(),
               "Immediate final-depth recomputations"),
      ADD_STAT(windowsAtDepth, statistics::units::Count::get(),
               "Complete feedback windows by stream depth"),
      ADD_STAT(sent, statistics::units::Count::get(),
               "Stream prefetches dequeued in the last complete window"),
      ADD_STAT(tlbMisses, statistics::units::Count::get(),
               "Stream translation requests missing the L1 TLB in the last window"),
      ADD_STAT(dcacheHits, statistics::units::Count::get(),
               "Stream prefetches dropped by a dcache hit in the last window"),
      ADD_STAT(pdbHits, statistics::units::Count::get(),
               "Stream prefetches dropped by a PDB hit in the last window"),
      ADD_STAT(mshrHits, statistics::units::Count::get(),
               "Stream prefetches dropped by a demand MSHR hit "
               "in the last window"),
      ADD_STAT(demandMshrHits, statistics::units::Count::get(),
               "First demand hits on in-flight stream prefetch MSHRs "
               "in the last window"),
      ADD_STAT(pdbLoadUses, statistics::units::Count::get(),
               "Stream PDB lines first used by a load in the last window"),
      ADD_STAT(pdbUnusedReplacements, statistics::units::Count::get(),
               "Unused stream PDB lines capacity-replaced in the last window"),
      ADD_STAT(pdbRefills, statistics::units::Count::get(),
               "Stream prefetch lines inserted into PDB in the last window"),
      ADD_STAT(pdbRefillIntervalSamples, statistics::units::Count::get(),
               "Intervals between stream PDB refills in the last window"),
      ADD_STAT(pdbRefillIntervalAvgCycles, statistics::units::Cycle::get(),
               "Mean cycles between stream PDB refills in the last window"),
      ADD_STAT(mshrResponseSamples, statistics::units::Count::get(),
               "Stream MSHR downstream response samples in the last window"),
      ADD_STAT(mshrResponseAvgCycles, statistics::units::Cycle::get(),
               "Mean stream MSHR send-to-data-response cycles in the last window"),
      ADD_STAT(refillToUseSamples, statistics::units::Count::get(),
               "Stream PDB first-use latency samples in the last window"),
      ADD_STAT(refillToUseAvgCycles, statistics::units::Cycle::get(),
               "Mean stream PDB refill-to-first-use cycles in the last window"),
      ADD_STAT(refillToReplaceSamples, statistics::units::Count::get(),
               "Stream PDB replacement latency samples in the last window"),
      ADD_STAT(refillToReplaceAvgCycles, statistics::units::Cycle::get(),
               "Mean stream PDB refill-to-replacement cycles in the last window"),
      ADD_STAT(useToReplaceSamples, statistics::units::Count::get(),
               "Used stream PDB replacement latency samples in the last window"),
      ADD_STAT(useToReplaceAvgCycles, statistics::units::Cycle::get(),
               "Mean stream PDB first-use-to-replacement cycles in the last window"),
      ADD_STAT(controllerTargetDepth, statistics::units::Count::get(),
               "BDP controller target depth after the last complete window"),
      ADD_STAT(modelDesiredDepth, statistics::units::Count::get(),
               "Continuous BDP depth demand before legal-level quantization"),
      ADD_STAT(hysteresisActive, statistics::units::Count::get(),
               "Whether upshift hysteresis held the last BDP request"),
      ADD_STAT(bandwidthDelayProduct, statistics::units::Ratio::get(),
               "EWMA memory-response bandwidth-delay product in stream blocks"),
      ADD_STAT(bdpFilterGain, statistics::units::Ratio::get(),
               "Adaptive BDP filter gain in the last valid window"),
      ADD_STAT(bdpInnovation, statistics::units::Ratio::get(),
               "Normalized BDP prediction error in the last valid window"),
      ADD_STAT(bdpTrend, statistics::units::Ratio::get(),
               "Smoothed per-window change in the BDP estimate"),
      ADD_STAT(bdpForecast, statistics::units::Ratio::get(),
               "Bounded BDP forecast used by the depth controller"),
      ADD_STAT(usefulRateEwma, statistics::units::Ratio::get(),
               "EWMA stream PDB load-use rate used by the controller"),
      ADD_STAT(lateRateEwma, statistics::units::Ratio::get(),
               "EWMA stream late rate used by the controller"),
      ADD_STAT(depth, statistics::units::Count::get(),
               "Stream prefetch depth after the last complete window"),
      ADD_STAT(bdpDepth, statistics::units::Count::get(),
               "Current BDP base depth"),
      ADD_STAT(delta, statistics::units::Count::get(),
               "Current late/lead depth offset"),
      ADD_STAT(finalDepth, statistics::units::Count::get(),
               "Current BDP depth plus delta"),
      ADD_STAT(lateRate, statistics::units::Ratio::get(),
               "Late rate in the latest delta window"),
      ADD_STAT(refillToUseTarget, statistics::units::Cycle::get(),
               "Refill-to-use target used by delta control"),
      ADD_STAT(deltaPressure, statistics::units::Ratio::get(),
               "Latest normalized late/lead competition pressure"),
      ADD_STAT(accuracy, statistics::units::Ratio::get(),
               "PDB useful accuracy in the latest accuracy window"),
      ADD_STAT(unusedReplacementRate, statistics::units::Ratio::get(),
               "Unused PDB replacement rate in the latest window"),
      ADD_STAT(accuracyDisabled, statistics::units::Count::get(),
               "Whether accuracy protection disabled stream prefetching"),
      ADD_STAT(competitiveWindows, statistics::units::Count::get(),
               "Completed reuse-competitive refill windows"),
      ADD_STAT(activeStreamDemandMisses, statistics::units::Count::get(),
               "Demand misses observed after stream lookup reports active"),
      ADD_STAT(activeStreamLateDemandMisses, statistics::units::Count::get(),
               "Active-stream demand misses covered late by PFQ or MSHR"),
      ADD_STAT(activeStreamUncoveredDemandMisses,
               statistics::units::Count::get(),
               "Active-stream demand misses not covered by PFQ or MSHR"),
      ADD_STAT(activeStreamDemandMissesAtDepth,
               statistics::units::Count::get(),
               "Active-stream demand misses by current stream depth"),
      ADD_STAT(activeStreamUncoveredDemandMissesAtDepth,
               statistics::units::Count::get(),
               "Uncovered active-stream demand misses by current stream depth"),
      ADD_STAT(competitiveWindowRefills, statistics::units::Count::get(),
               "Successful stream PDB refills in the latest competitive window"),
      ADD_STAT(competitiveWindowLateEvents, statistics::units::Count::get(),
               "Current-epoch late events in the latest competitive window"),
      ADD_STAT(competitiveWindowPrefetchHitDemandMshrEvents,
               statistics::units::Count::get(),
               "Stream prefetches hitting demand MSHRs in the latest window"),
      ADD_STAT(competitiveWindowDemandHitPrefetchMshrEvents,
               statistics::units::Count::get(),
               "Demands hitting stream-prefetch MSHRs in the latest window"),
      ADD_STAT(competitiveWindowUncoveredDemandMisses,
               statistics::units::Count::get(),
               "Uncovered active-stream misses in the latest competitive window"),
      ADD_STAT(competitiveWindowUnusedVictimHits,
               statistics::units::Count::get(),
               "Current-epoch unused-victim hits in the latest window"),
      ADD_STAT(competitiveWindowUsedVictimHits,
               statistics::units::Count::get(),
               "Current-epoch used-victim hits in the latest window"),
      ADD_STAT(currentEpochLateEvents, statistics::units::Count::get(),
               "Late events attributed to the current depth epoch"),
      ADD_STAT(staleEpochLateEvents, statistics::units::Count::get(),
               "Late events attributed to an older depth epoch"),
      ADD_STAT(currentEpochPrefetchHitDemandMshrEvents,
               statistics::units::Count::get(),
               "Stream prefetches hitting demand MSHRs in the current epoch"),
      ADD_STAT(staleEpochPrefetchHitDemandMshrEvents,
               statistics::units::Count::get(),
               "Stream prefetches hitting demand MSHRs in older epochs"),
      ADD_STAT(currentEpochDemandHitPrefetchMshrEvents,
               statistics::units::Count::get(),
               "Demands hitting stream-prefetch MSHRs in the current epoch"),
      ADD_STAT(staleEpochDemandHitPrefetchMshrEvents,
               statistics::units::Count::get(),
               "Demands hitting stream-prefetch MSHRs in older epochs"),
      ADD_STAT(currentEpochUnusedVictimHits,
               statistics::units::Count::get(),
               "Unused-victim hits attributed to the current depth epoch"),
      ADD_STAT(currentEpochUsedVictimHits,
               statistics::units::Count::get(),
               "Used-victim hits attributed to the current depth epoch"),
      ADD_STAT(staleEpochUnusedVictimHits,
               statistics::units::Count::get(),
               "Unused-victim hits attributed to an older depth epoch"),
      ADD_STAT(staleEpochUsedVictimHits,
               statistics::units::Count::get(),
               "Used-victim hits attributed to an older depth epoch"),
      ADD_STAT(latePressure, statistics::units::Ratio::get(),
               "Quantized deepening pressure in the latest window"),
      ADD_STAT(unusedPressure, statistics::units::Ratio::get(),
               "Raw quantized unused-victim pressure in the latest window"),
      ADD_STAT(unusedPressureConsumedWindows,
               statistics::units::Count::get(),
               "Windows where active unused move consumed nonzero depth pressure"),
      ADD_STAT(netPressure, statistics::units::Ratio::get(),
               "Late pressure minus applied unused pressure in the latest window"),
      ADD_STAT(upCredit, statistics::units::Ratio::get(),
               "Accumulated competitive deepening credit"),
      ADD_STAT(downCredit, statistics::units::Ratio::get(),
               "Accumulated competitive shallowing credit"),
      ADD_STAT(competitiveDepthIncreases, statistics::units::Count::get(),
               "Depth increases committed by competitive control"),
      ADD_STAT(competitiveDepthDecreases, statistics::units::Count::get(),
               "Depth decreases committed by competitive control"),
      ADD_STAT(competitiveSettleWindows, statistics::units::Count::get(),
               "Competitive windows suppressed for post-change settling"),
      ADD_STAT(equalNonzeroPressureWindows,
               statistics::units::Count::get(),
               "Windows with equal non-zero late and unused pressure"),
      ADD_STAT(unusedMoveEligibleWindows,
               statistics::units::Count::get(),
               "Full minimum-depth windows eligible to enable unused move"),
      ADD_STAT(currentDepth, statistics::units::Count::get(),
               "Current reuse-competitive stream depth"),
      ADD_STAT(currentDepthEpoch, statistics::units::Count::get(),
               "Current reuse-competitive depth epoch"),
      ADD_STAT(competitiveWindowsAtDepth, statistics::units::Count::get(),
               "Completed competitive windows by starting depth"),
      ADD_STAT(usedMoveEnables, statistics::units::Count::get(),
               "Actual used-victim move enable transitions"),
      ADD_STAT(usedMoveDisables, statistics::units::Count::get(),
               "Actual used-victim move disable transitions"),
      ADD_STAT(usedMoveActive, statistics::units::Count::get(),
               "Whether actual used-victim movement is active"),
      ADD_STAT(usedMoveWouldEnable, statistics::units::Count::get(),
               "Shadow used-victim move enable transitions"),
      ADD_STAT(usedMoveWouldMove, statistics::units::Count::get(),
               "Used victims selected by the shadow move policy"),
      ADD_STAT(unusedMoveEnables, statistics::units::Count::get(),
               "Actual unused-victim move enable transitions"),
      ADD_STAT(unusedMoveDisables, statistics::units::Count::get(),
               "Actual unused-victim move disable transitions"),
      ADD_STAT(unusedMoveActive, statistics::units::Count::get(),
               "Whether actual unused-victim movement is active"),
      ADD_STAT(unusedMoveWouldEnable, statistics::units::Count::get(),
               "Shadow unused-victim move enable transitions"),
      ADD_STAT(unusedMoveWouldMove, statistics::units::Count::get(),
               "Unused victims selected by the shadow move policy"),
      ADD_STAT(usedMoveAttempts, statistics::units::Count::get(),
               "Actual used-victim DCache move attempts"),
      ADD_STAT(usedMoveSuccesses, statistics::units::Count::get(),
               "Successful used-victim DCache moves"),
      ADD_STAT(usedMoveFailures, statistics::units::Count::get(),
               "Failed used-victim DCache moves"),
      ADD_STAT(unusedMoveAttempts, statistics::units::Count::get(),
               "Actual unused-victim DCache move attempts"),
      ADD_STAT(unusedMoveSuccesses, statistics::units::Count::get(),
               "Successful unused-victim DCache moves"),
      ADD_STAT(unusedMoveFailures, statistics::units::Count::get(),
               "Failed unused-victim DCache moves")
    {
    windowsAtDepth.init(parent->depthLevels.size());
    for (size_t i = 0; i < parent->depthLevels.size(); ++i) {
        windowsAtDepth.subname(i, std::to_string(parent->depthLevels[i]));
    }
    competitiveWindowsAtDepth.init(parent->competitiveDepthLevels.size());
    activeStreamDemandMissesAtDepth.init(
        parent->competitiveDepthLevels.size());
    activeStreamUncoveredDemandMissesAtDepth.init(
        parent->competitiveDepthLevels.size());
    for (size_t i = 0; i < parent->competitiveDepthLevels.size(); ++i) {
        competitiveWindowsAtDepth.subname(
            i, std::to_string(parent->competitiveDepthLevels[i]));
        activeStreamDemandMissesAtDepth.subname(
            i, std::to_string(parent->competitiveDepthLevels[i]));
        activeStreamUncoveredDemandMissesAtDepth.subname(
            i, std::to_string(parent->competitiveDepthLevels[i]));
    }
}

void
XsStreamPrefetcher::recordStreamProbe(
    PrefetchSourceType source, Base::PrefetchProbeResult result,
    uint32_t depth_epoch)
{
    switch (result) {
      case Base::PrefetchProbeResult::DcacheHit:
        ++feedback.dcacheHits;
        break;
      case Base::PrefetchProbeResult::PdbHit:
        ++feedback.pdbHits;
        break;
      case Base::PrefetchProbeResult::DemandMshrHit:
        ++feedback.mshrHits;
        ++feedbackStats.cumulativeLateEvents;
        if (source == PrefetchSourceType::SStream)
            recordCompetitiveLate(depth_epoch, false);
        break;
      case Base::PrefetchProbeResult::PrefetchMshrHit:
        break;
      case Base::PrefetchProbeResult::WriteBufferHit:
      case Base::PrefetchProbeResult::Sent:
        break;
    }
    if (source == PrefetchSourceType::SStream &&
        result != Base::PrefetchProbeResult::Sent) {
        ++feedback.lateHits;
    }
}

void
XsStreamPrefetcher::recordStreamPdbRefill(
    PrefetchSourceType source, uint32_t refill_epoch)
{
    const Tick now = curTick();
    if (feedback.pdbRefills) {
        feedback.pdbRefillIntervalCycles +=
            ticksToCycles(now - feedback.lastPdbRefillTick);
        ++feedback.pdbRefillIntervalSamples;
    }
    feedback.lastPdbRefillTick = now;
    ++feedback.pdbRefills;
    if (enableReuseObserver && source == PrefetchSourceType::SStream) {
        ++competitiveWindow.refills;
        if (refill_epoch != depthEpoch) {
            DPRINTF(XsStreamPrefetcher,
                    "stale-epoch PDB refill: refill=%u current=%u\n",
                    refill_epoch, depthEpoch);
        }
        if (competitiveWindow.refills == competitiveWindowRefills) {
            completeCompetitiveWindow();
        }
    }
}

void
XsStreamPrefetcher::recordStreamMshrResponse(uint64_t latency_cycles)
{
    ++feedback.mshrResponseSamples;
    feedback.mshrResponseCycles += latency_cycles;
    ++feedbackStats.cumulativeMshrResponseSamples;
    feedbackStats.cumulativeMshrResponseCycles += latency_cycles;
}

void
XsStreamPrefetcher::recordStreamPdbFirstUse(bool load,
                                             uint64_t refill_to_use)
{
    if (load)
        ++feedback.pdbLoadUses;
    ++feedback.refillToUseSamples;
    feedback.refillToUseCycles += refill_to_use;
    if (load)
        ++feedbackStats.cumulativePdbLoadUses;
    ++feedbackStats.cumulativeRefillToUseSamples;
    feedbackStats.cumulativeRefillToUseCycles += refill_to_use;
}

void
XsStreamPrefetcher::recordStreamPdbReplacement(
    bool used, uint64_t refill_to_replace, uint64_t use_to_replace)
{
    if (!used)
        ++feedback.pdbUnusedReplacements;
    ++feedback.refillToReplaceSamples;
    feedback.refillToReplaceCycles += refill_to_replace;
    if (used) {
        ++feedback.useToReplaceSamples;
        feedback.useToReplaceCycles += use_to_replace;
    }
}

void
XsStreamPrefetcher::recordCompetitiveLate(
    uint32_t event_epoch, bool demand_hit_prefetch_mshr)
{
    if (!enableReuseObserver)
        return;

    if (event_epoch == depthEpoch) {
        ++competitiveWindow.lateEvents;
        ++feedbackStats.currentEpochLateEvents;
        if (demand_hit_prefetch_mshr) {
            ++competitiveWindow.demandHitPrefetchMshrEvents;
            ++feedbackStats.currentEpochDemandHitPrefetchMshrEvents;
        } else {
            ++competitiveWindow.prefetchHitDemandMshrEvents;
            ++feedbackStats.currentEpochPrefetchHitDemandMshrEvents;
        }
    } else {
        ++feedbackStats.staleEpochLateEvents;
        if (demand_hit_prefetch_mshr)
            ++feedbackStats.staleEpochDemandHitPrefetchMshrEvents;
        else
            ++feedbackStats.staleEpochPrefetchHitDemandMshrEvents;
    }
}

void
XsStreamPrefetcher::recordPdbVictimReuse(
    bool used, uint32_t refill_depth_epoch)
{
    if (!enableReuseObserver)
        return;

    const bool current_epoch = refill_depth_epoch == depthEpoch;
    if (used) {
        if (current_epoch) {
            ++competitiveWindow.usedVictimHits;
            ++feedbackStats.currentEpochUsedVictimHits;
        } else {
            ++feedbackStats.staleEpochUsedVictimHits;
        }
    } else {
        if (current_epoch) {
            ++competitiveWindow.unusedVictimHits;
            ++feedbackStats.currentEpochUnusedVictimHits;
        } else {
            ++feedbackStats.staleEpochUnusedVictimHits;
        }
    }
}

double
XsStreamPrefetcher::quantizeCompetitivePressure(
    uint64_t count, unsigned low, unsigned high) const
{
    if (count <= low)
        return 0.0;
    if (count < high)
        return 0.5;
    return 1.0;
}

void
XsStreamPrefetcher::updateMoveControllers(
    double late_pressure, double unused_pressure,
    bool unused_window_at_min_depth)
{
    if (!usedMoveState) {
        usedMoveOnStreak =
            competitiveWindow.usedVictimHits >= usedMoveOnHits ?
            usedMoveOnStreak + 1 : 0;
        if (usedMoveOnStreak >= usedMoveOnWindows) {
            usedMoveState = true;
            usedMoveOnStreak = 0;
            usedMoveOffStreak = 0;
            ++feedbackStats.usedMoveWouldEnable;
            if (enableUsedMove)
                ++feedbackStats.usedMoveEnables;
        }
    } else {
        usedMoveOffStreak =
            competitiveWindow.usedVictimHits <= usedMoveOffHits ?
            usedMoveOffStreak + 1 : 0;
        if (usedMoveOffStreak >= usedMoveOffWindows) {
            usedMoveState = false;
            usedMoveOnStreak = 0;
            usedMoveOffStreak = 0;
            if (enableUsedMove)
                ++feedbackStats.usedMoveDisables;
        }
    }

    const bool at_min_depth = depth == competitiveDepthLevels.front();
    const bool unused_move_activation_eligible =
        unused_window_at_min_depth && at_min_depth;
    const bool activation_pressure =
        unused_pressure >= unusedMoveMinPressure &&
        unused_pressure >= late_pressure;
    if (unused_move_activation_eligible && activation_pressure)
        ++feedbackStats.unusedMoveEligibleWindows;
    const bool unused_move_maintenance_eligible =
        persistentUnusedMove && unusedMoveState;
    if (!unused_move_activation_eligible &&
        !unused_move_maintenance_eligible) {
        unusedMoveOnStreak = 0;
        unusedMoveOffStreak = 0;
        if (unusedMoveState) {
            unusedMoveState = false;
            if (enableUnusedMove)
                ++feedbackStats.unusedMoveDisables;
        }
    } else if (!unusedMoveState) {
        unusedMoveOnStreak = activation_pressure ?
            unusedMoveOnStreak + 1 : 0;
        if (unusedMoveOnStreak >= unusedMoveOnWindows) {
            unusedMoveState = true;
            unusedMoveOnStreak = 0;
            unusedMoveOffStreak = 0;
            ++feedbackStats.unusedMoveWouldEnable;
            if (enableUnusedMove)
                ++feedbackStats.unusedMoveEnables;
        }
    } else {
        unusedMoveOffStreak =
            unused_pressure == 0.0 ?
            unusedMoveOffStreak + 1 : 0;
        if (unusedMoveOffStreak >= unusedMoveOffWindows) {
            unusedMoveState = false;
            unusedMoveOnStreak = 0;
            unusedMoveOffStreak = 0;
            if (enableUnusedMove)
                ++feedbackStats.unusedMoveDisables;
        }
    }

    feedbackStats.usedMoveActive = enableUsedMove && usedMoveState;
    feedbackStats.unusedMoveActive = enableUnusedMove && unusedMoveState;
}

void
XsStreamPrefetcher::completeCompetitiveWindow()
{
    assert(competitiveWindow.refills == competitiveWindowRefills);
    const int old_depth = depth;
    const auto starting_depth = std::find(
        competitiveDepthLevels.begin(), competitiveDepthLevels.end(),
        old_depth);
    const bool starting_at_min_depth =
        old_depth == competitiveDepthLevels.front();
    const uint32_t old_epoch = depthEpoch;
    const double late_pressure = quantizeCompetitivePressure(
        competitiveWindow.lateEvents,
        competitiveLateLow, competitiveLateHigh);
    const double raw_unused_pressure = quantizeCompetitivePressure(
        competitiveWindow.unusedVictimHits,
        competitiveUnusedLow, competitiveUnusedHigh);
    const bool unused_move_handles_pressure =
        unusedMoveDepthPressureMode != 0 &&
        enableUnusedMove && unusedMoveState;
    double depth_unused_pressure = raw_unused_pressure;
    if (unused_move_handles_pressure &&
        (unusedMoveDepthPressureMode == 2 ||
         (unusedMoveDepthPressureMode == 3 && late_pressure > 0.0) ||
         (raw_unused_pressure > 0.0 &&
          raw_unused_pressure == late_pressure))) {
        depth_unused_pressure = 0.0;
    }
    const double net_pressure = late_pressure - depth_unused_pressure;
    const double up_credit_threshold =
        enableUnusedMove && unusedMoveState && starting_at_min_depth ?
        unusedMoveMinDepthUpCreditThreshold : competitiveCreditThreshold;

    ++feedbackStats.competitiveWindows;
    if (starting_depth != competitiveDepthLevels.end()) {
        ++feedbackStats.competitiveWindowsAtDepth[std::distance(
            competitiveDepthLevels.begin(), starting_depth)];
    }
    feedbackStats.competitiveWindowRefills = competitiveWindow.refills;
    feedbackStats.competitiveWindowLateEvents =
        competitiveWindow.lateEvents;
    feedbackStats.competitiveWindowPrefetchHitDemandMshrEvents =
        competitiveWindow.prefetchHitDemandMshrEvents;
    feedbackStats.competitiveWindowDemandHitPrefetchMshrEvents =
        competitiveWindow.demandHitPrefetchMshrEvents;
    feedbackStats.competitiveWindowUncoveredDemandMisses =
        competitiveWindow.uncoveredDemandMisses;
    feedbackStats.competitiveWindowUnusedVictimHits =
        competitiveWindow.unusedVictimHits;
    feedbackStats.competitiveWindowUsedVictimHits =
        competitiveWindow.usedVictimHits;
    feedbackStats.latePressure = late_pressure;
    feedbackStats.unusedPressure = raw_unused_pressure;
    if (depth_unused_pressure < raw_unused_pressure)
        ++feedbackStats.unusedPressureConsumedWindows;
    feedbackStats.netPressure = net_pressure;
    if (late_pressure > 0.0 && late_pressure == raw_unused_pressure)
        ++feedbackStats.equalNonzeroPressureWindows;
    if (enableCompetitiveDepth) {
        if (settleRemaining > 0) {
            --settleRemaining;
            upCredit = downCredit = 0.0;
            ++feedbackStats.competitiveSettleWindows;
        } else if (net_pressure > 0.0) {
            upCredit += net_pressure;
            downCredit = 0.0;
        } else if (net_pressure < 0.0) {
            downCredit += -net_pressure;
            upCredit = 0.0;
        } else {
            upCredit = downCredit = 0.0;
        }

        if (upCredit >= up_credit_threshold &&
            competitiveDepthIndex + 1 < competitiveDepthLevels.size()) {
            competitiveDepthIndex = std::min(
                competitiveDepthLevels.size() - 1,
                competitiveDepthIndex + competitiveMaxLevelStep);
            depth = competitiveDepthLevels[competitiveDepthIndex];
            ++depthEpoch;
            ++feedbackStats.competitiveDepthIncreases;
            settleRemaining = competitiveSettleWindowCount;
            upCredit = downCredit = 0.0;
        } else if (downCredit >= competitiveCreditThreshold &&
                   competitiveDepthIndex > 0) {
            competitiveDepthIndex = competitiveDepthIndex >
                    competitiveMaxLevelStep ?
                competitiveDepthIndex - competitiveMaxLevelStep : 0;
            depth = competitiveDepthLevels[competitiveDepthIndex];
            ++depthEpoch;
            ++feedbackStats.competitiveDepthDecreases;
            settleRemaining = competitiveSettleWindowCount;
            upCredit = downCredit = 0.0;
        } else if ((upCredit >= up_credit_threshold &&
                    competitiveDepthIndex + 1 ==
                        competitiveDepthLevels.size()) ||
                   (downCredit >= competitiveCreditThreshold &&
                    competitiveDepthIndex == 0)) {
            upCredit = downCredit = 0.0;
        }
    } else {
        upCredit = downCredit = 0.0;
    }

    updateMoveControllers(late_pressure, raw_unused_pressure,
                          starting_at_min_depth);
    feedbackStats.upCredit = upCredit;
    feedbackStats.downCredit = downCredit;
    feedbackStats.currentDepth = depth;
    feedbackStats.currentDepthEpoch = depthEpoch;
    if (enableCompetitiveDepth) {
        feedbackStats.depth = depth;
        feedbackStats.finalDepth = depth;
    }

    DPRINTF(XsStreamPrefetcher,
            "competitive depth: depth=%d->%d epoch=%u->%u refill=%llu "
            "late=%llu prefetchDemandMshr=%llu demandPrefetchMshr=%llu "
            "uncovered=%llu unused=%llu used=%llu "
            "pressure=%.1f/%.1f depthUnused=%.1f net=%.1f "
            "credit=%.1f/%.1f settle=%u usedMove=%d unusedMove=%d\n",
            old_depth, depth, old_epoch, depthEpoch,
            competitiveWindow.refills, competitiveWindow.lateEvents,
            competitiveWindow.prefetchHitDemandMshrEvents,
            competitiveWindow.demandHitPrefetchMshrEvents,
            competitiveWindow.uncoveredDemandMisses,
            competitiveWindow.unusedVictimHits,
            competitiveWindow.usedVictimHits,
            late_pressure, raw_unused_pressure, depth_unused_pressure,
            net_pressure,
            upCredit, downCredit,
            settleRemaining, usedMoveState, unusedMoveState);

    competitiveWindow = {};
}

bool
XsStreamPrefetcher::shouldMovePdbVictim(bool used)
{
    const bool shadow_move = used ? usedMoveState : unusedMoveState;
    if (shadow_move) {
        if (used)
            ++feedbackStats.usedMoveWouldMove;
        else
            ++feedbackStats.unusedMoveWouldMove;
    }

    const bool actual_move = shadow_move &&
        (used ? enableUsedMove : enableUnusedMove);
    if (actual_move) {
        if (used)
            ++feedbackStats.usedMoveAttempts;
        else
            ++feedbackStats.unusedMoveAttempts;
    }
    return actual_move;
}

void
XsStreamPrefetcher::recordPdbVictimMoveResult(bool used, bool success)
{
    if (used) {
        if (success)
            ++feedbackStats.usedMoveSuccesses;
        else
            ++feedbackStats.usedMoveFailures;
    } else {
        if (success)
            ++feedbackStats.unusedMoveSuccesses;
        else
            ++feedbackStats.unusedMoveFailures;
    }
}

void
XsStreamPrefetcher::completeFeedbackWindow()
{
    const auto &window = feedback;
    const auto mean = [](uint64_t cycles, uint64_t samples) {
        return samples ? double(cycles) / samples : 0.0;
    };
    const double refill_to_use =
        mean(window.refillToUseCycles, window.refillToUseSamples);
    const double refill_to_replace =
        mean(window.refillToReplaceCycles, window.refillToReplaceSamples);
    const double use_to_replace =
        mean(window.useToReplaceCycles, window.useToReplaceSamples);
    const double pdb_refill_interval = mean(
        window.pdbRefillIntervalCycles, window.pdbRefillIntervalSamples);
    const double mshr_response_latency = mean(
        window.mshrResponseCycles, window.mshrResponseSamples);

    const double useful_rate = window.sent ?
        double(window.pdbLoadUses) / window.sent : 0.0;
    const uint64_t late_events =
        window.mshrHits + window.demandMshrHits;
    const double late_rate = window.sent ?
        double(late_events) / window.sent : 0.0;
    constexpr uint64_t MIN_CONTROLLER_SAMPLES = 64;
    constexpr double EWMA_ALPHA = 0.25;
    constexpr double MIN_BDP_FILTER_GAIN = 0.125;
    constexpr double MAX_BDP_FILTER_GAIN = 0.75;
    constexpr double BDP_INNOVATION_GAIN = 0.5;
    constexpr double HIGH_FILTER_GAIN = 0.5;
    constexpr double BDP_TREND_GAIN = 0.25;
    constexpr double BDP_FORECAST_WINDOWS = 2.0;
    constexpr double MAX_BDP_FORECAST_DELTA = 0.5;
    constexpr double BDP_HEADROOM = 8.0;
    constexpr double HIGH_LATE_RATE = 0.05;
    constexpr double VERY_HIGH_LATE_RATE = 0.10;
    constexpr double UPSHIFT_HYSTERESIS = 1.25;
    constexpr int MAX_FEEDFORWARD_DEPTH = 32;
    constexpr int CONFIRM_WINDOWS = 2;

    if (!feedbackEwmaValid) {
        usefulRateEwma = useful_rate;
        lateRateEwma = late_rate;
        feedbackEwmaValid = true;
    } else {
        usefulRateEwma += EWMA_ALPHA * (useful_rate - usefulRateEwma);
        lateRateEwma += EWMA_ALPHA * (late_rate - lateRateEwma);
    }

    const bool bdp_valid =
        window.pdbRefillIntervalSamples >= MIN_CONTROLLER_SAMPLES &&
        window.mshrResponseSamples >= MIN_CONTROLLER_SAMPLES &&
        pdb_refill_interval > 0.0;
    bool forecast_clamped = false;
    if (bdp_valid) {
        const double bdp = mshr_response_latency / pdb_refill_interval;
        if (!bdpEwmaValid) {
            bdpEwma = bdp;
            bdpTrend = 0.0;
            bdpForecast = bdp;
            bdpFilterGain = 1.0;
            bdpInnovation = 0.0;
            bdpEwmaValid = true;
        } else {
            bdpInnovation = std::abs(bdp - bdpEwma) /
                std::max(0.5, bdpEwma);
            bdpFilterGain = std::clamp(
                MIN_BDP_FILTER_GAIN +
                    BDP_INNOVATION_GAIN * bdpInnovation,
                MIN_BDP_FILTER_GAIN, MAX_BDP_FILTER_GAIN);
            const double previous_bdp = bdpEwma;
            bdpEwma += bdpFilterGain * (bdp - bdpEwma);
            bdpTrend += BDP_TREND_GAIN *
                ((bdpEwma - previous_bdp) - bdpTrend);
            const double raw_forecast =
                bdpEwma + BDP_FORECAST_WINDOWS * bdpTrend;
            bdpForecast = std::clamp(
                raw_forecast,
                (1.0 - MAX_BDP_FORECAST_DELTA) * bdpEwma,
                (1.0 + MAX_BDP_FORECAST_DELTA) * bdpEwma);
            forecast_clamped = bdpForecast != raw_forecast;
        }
    }

    const int old_depth = depth;
    const auto depth_level =
        std::find(DEPTH_LEVELS.begin(), DEPTH_LEVELS.end(), depth);
    assert(depth_level != DEPTH_LEVELS.end());
    const auto depth_level_index =
        std::distance(DEPTH_LEVELS.begin(), depth_level);
    auto target_level = depth_level;
    int model_desired_depth = depth;
    bool hysteresis_hold = false;
    if (bdpEwmaValid) {
        model_desired_depth = std::min(
            MAX_FEEDFORWARD_DEPTH,
            std::max(DEPTH_LEVELS.front(),
                     int(std::ceil(BDP_HEADROOM * bdpForecast))));
        target_level = std::lower_bound(
            DEPTH_LEVELS.begin(), DEPTH_LEVELS.end(), model_desired_depth);

        if (target_level > depth_level &&
            model_desired_depth < depth * UPSHIFT_HYSTERESIS) {
            target_level = depth_level;
            hysteresis_hold = true;
        }

        if (usefulRateEwma < 0.20) {
            target_level = DEPTH_LEVELS.begin();
        } else if (usefulRateEwma < 0.40) {
            target_level = std::min(target_level, DEPTH_LEVELS.begin() + 1);
        } else if (usefulRateEwma < 0.65) {
            target_level = std::min(target_level, DEPTH_LEVELS.begin() + 2);
        }

    }
    const size_t emergency_step = lateRateEwma >= VERY_HIGH_LATE_RATE ?
        2 : lateRateEwma >= HIGH_LATE_RATE ? 1 : 0;
    if (emergency_step) {
        const size_t emergency_index = std::min(
            DEPTH_LEVELS.size() - 1,
            size_t(depth_level_index) + emergency_step);
        target_level = std::max(
            target_level, DEPTH_LEVELS.begin() + emergency_index);
    }
    const int target_depth = *target_level;
    const char *depth_decision = "disabled";
    if (enableAutoDepth) {
        if (depthSettlingWindows) {
            --depthSettlingWindows;
            depthDecisionScore = 0;
            depth_decision = "settle";
        } else if (target_level > depth_level) {
            depthDecisionScore = depthDecisionScore < 0 ?
                1 : depthDecisionScore + 1;
            depth_decision = "confirm-increase";
            if (depthDecisionScore >= CONFIRM_WINDOWS) {
                if (depth_level + 1 != DEPTH_LEVELS.end()) {
                    depth = *(depth_level + 1);
                    depth_decision = "increase";
                } else {
                    depth_decision = "max-depth";
                }
                depthDecisionScore = 0;
            }
        } else if (target_level < depth_level) {
            depthDecisionScore = depthDecisionScore > 0 ?
                -1 : depthDecisionScore - 1;
            depth_decision = "confirm-decrease";
            if (depthDecisionScore <= -CONFIRM_WINDOWS) {
                if (depth_level != DEPTH_LEVELS.begin()) {
                    depth = *(depth_level - 1);
                    depth_decision = "decrease";
                } else {
                    depth_decision = "min-depth";
                }
                depthDecisionScore = 0;
            }
        } else {
            depthDecisionScore = 0;
            depth_decision = "hold";
        }
    }

    ++feedbackStats.windows;
    ++feedbackStats.windowsAtDepth[depth_level_index];
    if (depth > old_depth) {
        ++feedbackStats.depthIncreases;
    } else if (depth < old_depth) {
        ++feedbackStats.depthDecreases;
    }
    if (bdp_valid && bdpFilterGain >= HIGH_FILTER_GAIN)
        ++feedbackStats.highGainWindows;
    if (hysteresis_hold)
        ++feedbackStats.hysteresisHolds;
    if (forecast_clamped)
        ++feedbackStats.forecastClampedWindows;
    if (depth != old_depth)
        depthSettlingWindows = 4;
    recordLegacyDepthChange(old_depth);
    feedbackStats.sent = window.sent;
    feedbackStats.tlbMisses = window.tlbMisses;
    feedbackStats.dcacheHits = window.dcacheHits;
    feedbackStats.pdbHits = window.pdbHits;
    feedbackStats.mshrHits = window.mshrHits;
    feedbackStats.demandMshrHits = window.demandMshrHits;
    feedbackStats.pdbLoadUses = window.pdbLoadUses;
    feedbackStats.pdbUnusedReplacements = window.pdbUnusedReplacements;
    feedbackStats.pdbRefills = window.pdbRefills;
    feedbackStats.pdbRefillIntervalSamples = window.pdbRefillIntervalSamples;
    feedbackStats.pdbRefillIntervalAvgCycles = pdb_refill_interval;
    feedbackStats.mshrResponseSamples = window.mshrResponseSamples;
    feedbackStats.mshrResponseAvgCycles = mshr_response_latency;
    feedbackStats.refillToUseSamples = window.refillToUseSamples;
    feedbackStats.refillToUseAvgCycles = refill_to_use;
    feedbackStats.refillToReplaceSamples = window.refillToReplaceSamples;
    feedbackStats.refillToReplaceAvgCycles = refill_to_replace;
    feedbackStats.useToReplaceSamples = window.useToReplaceSamples;
    feedbackStats.useToReplaceAvgCycles = use_to_replace;
    feedbackStats.controllerTargetDepth = target_depth;
    feedbackStats.modelDesiredDepth = model_desired_depth;
    feedbackStats.hysteresisActive = hysteresis_hold;
    feedbackStats.bandwidthDelayProduct = bdpEwma;
    feedbackStats.bdpFilterGain = bdpFilterGain;
    feedbackStats.bdpInnovation = bdpInnovation;
    feedbackStats.bdpTrend = bdpTrend;
    feedbackStats.bdpForecast = bdpForecast;
    feedbackStats.usefulRateEwma = usefulRateEwma;
    feedbackStats.lateRateEwma = lateRateEwma;
    feedbackStats.depth = depth;

    DPRINTF(XsStreamPrefetcher,
            "auto depth: %d -> %d, sent=%llu late=%llu bad=%d "
            "tlbMiss=%llu dcache=%llu pdb=%llu mshr=%llu demandMshr=%llu "
            "loadUse=%llu "
            "unusedReplace=%llu pdbRefills=%llu pdbRefillInterval=%.3f/%llu "
            "mshrResponse=%.3f/%llu "
            "refillToUse=%.3f/%llu "
            "refillToReplace=%.3f/%llu useToReplace=%.3f/%llu "
            "policyUse=%.3f policyLate=%.3f bdp=%.3f "
            "bdpGain=%.3f innovation=%.3f trend=%.3f forecast=%.3f "
            "forecastClamped=%d desired=%d hysteresis=%d target=%d "
            "score=%d decision=%s\n",
            old_depth, depth, window.sent, window.lateHits, badPreNum,
            window.tlbMisses, window.dcacheHits, window.pdbHits,
            window.mshrHits, window.demandMshrHits, window.pdbLoadUses,
            window.pdbUnusedReplacements,
            window.pdbRefills, pdb_refill_interval,
            window.pdbRefillIntervalSamples,
            mshr_response_latency, window.mshrResponseSamples,
            refill_to_use, window.refillToUseSamples,
            refill_to_replace, window.refillToReplaceSamples,
            use_to_replace, window.useToReplaceSamples,
            usefulRateEwma, lateRateEwma, bdpEwma, bdpFilterGain,
            bdpInnovation, bdpTrend, bdpForecast, forecast_clamped,
            model_desired_depth, hysteresis_hold, target_depth,
            depthDecisionScore,
            depth_decision);

    feedback = {};
    badPreNum = 0;
    issuedPrefetches = 0;
}

void
XsStreamPrefetcher::recordLegacyDepthChange(int old_depth)
{
    if (enableCompetitiveDepth || depth == old_depth)
        return;

    ++depthEpoch;
    competitiveWindow = {};
    upCredit = downCredit = 0.0;
    settleRemaining = 0;
    unusedMoveOnStreak = 0;

    const auto level = std::find(
        competitiveDepthLevels.begin(), competitiveDepthLevels.end(), depth);
    if (level != competitiveDepthLevels.end()) {
        competitiveDepthIndex = std::distance(
            competitiveDepthLevels.begin(), level);
    }

    if (!persistentUnusedMove &&
        depth != competitiveDepthLevels.front() && unusedMoveState) {
        unusedMoveState = false;
        unusedMoveOffStreak = 0;
        if (enableUnusedMove)
            ++feedbackStats.unusedMoveDisables;
    }

    feedbackStats.currentDepth = depth;
    feedbackStats.currentDepthEpoch = depthEpoch;
    feedbackStats.unusedMoveActive = enableUnusedMove && unusedMoveState;
    DPRINTF(XsStreamPrefetcher,
            "legacy depth epoch: depth=%d->%d epoch=%u\n",
            old_depth, depth, depthEpoch);
}

void
XsStreamPrefetcher::recomputeDepth(const char *reason)
{
    const int old_depth = depth;
    const int min_depth = depthLevels.front();
    const int max_depth = depthLevels.back();
    depth = std::clamp(bdpDepth + delta, min_depth, max_depth);
    feedbackStats.bdpDepth = bdpDepth;
    feedbackStats.delta = delta;
    feedbackStats.finalDepth = depth;
    feedbackStats.depth = depth;
    if (depth != old_depth) {
        ++feedbackStats.finalDepthUpdates;
        recordLegacyDepthChange(old_depth);
        DPRINTF(XsStreamPrefetcher,
                "depth control: reason=%s bdpDepth=%d delta=%d depth=%d->%d\n",
                reason, bdpDepth, delta, old_depth, depth);
    }
}

void
XsStreamPrefetcher::updateBdpController()
{
    const auto diff = [](uint64_t now, uint64_t old) { return now - old; };
    const uint64_t sent = diff(feedback.sent, bdpSnapshot.sent);
    const uint64_t response_samples = diff(
        feedback.mshrResponseSamples, bdpSnapshot.mshrResponseSamples);
    const uint64_t response_cycles = diff(
        feedback.mshrResponseCycles, bdpSnapshot.mshrResponseCycles);
    const uint64_t refill_samples = diff(
        feedback.pdbRefillIntervalSamples, bdpSnapshot.pdbRefillIntervalSamples);
    const uint64_t refill_cycles = diff(
        feedback.pdbRefillIntervalCycles, bdpSnapshot.pdbRefillIntervalCycles);
    bdpSnapshot.sent = feedback.sent;
    bdpSnapshot.mshrResponseSamples = feedback.mshrResponseSamples;
    bdpSnapshot.mshrResponseCycles = feedback.mshrResponseCycles;
    bdpSnapshot.pdbRefillIntervalSamples = feedback.pdbRefillIntervalSamples;
    bdpSnapshot.pdbRefillIntervalCycles = feedback.pdbRefillIntervalCycles;
    ++feedbackStats.bdpWindows;
    if (response_samples < bdpMinMshrSamples ||
        refill_samples < bdpMinRefillSamples || refill_cycles == 0) {
        DPRINTF(XsStreamPrefetcher,
                "BDP window invalid: sent=%llu response=%llu refill=%llu\n",
                sent, response_samples, refill_samples);
        return;
    }

    const double raw_bdp = double(response_cycles) / refill_cycles;
    const double bdp_blocks = raw_bdp * bdpCalibrationFactor;
    if (!bdpEwmaValid) {
        bdpEwma = bdp_blocks;
        bdpEwmaValid = true;
    } else {
        bdpEwma += bdpEwmaAlpha * (bdp_blocks - bdpEwma);
    }
    feedbackStats.bandwidthDelayProduct = bdpEwma;

    const bool request_up = bdpEwma > bdpUpRatio * bdpDepth;
    const bool request_down = bdpEwma < bdpDownRatio * bdpDepth;
    if (request_up)
        ++feedbackStats.bdpIncreaseRequests;
    if (request_down)
        ++feedbackStats.bdpDecreaseRequests;
    if (bdpStableWindows) {
        --bdpStableWindows;
        bdpUpScore = bdpDownScore = 0;
        return;
    }

    if (request_up) {
        ++bdpUpScore;
        bdpDownScore = 0;
    } else if (request_down) {
        ++bdpDownScore;
        bdpUpScore = 0;
    } else {
        bdpUpScore = bdpDownScore = 0;
    }

    auto level = std::lower_bound(depthLevels.begin(), depthLevels.end(), bdpEwma);
    size_t target_index = level == depthLevels.end() ? depthLevels.size() - 1 :
        size_t(std::distance(depthLevels.begin(), level));
    if (target_index > 0 && level != depthLevels.end() &&
        (target_index == depthLevels.size() - 1 ||
         bdpEwma - depthLevels[target_index - 1] <
             depthLevels[target_index] - bdpEwma)) {
        --target_index;
    }
    const auto current = std::find(depthLevels.begin(), depthLevels.end(), bdpDepth);
    const size_t current_index = std::distance(depthLevels.begin(), current);
    if (request_up && bdpUpScore >= int(bdpUpConfirmWindows) &&
        target_index > current_index) {
        const size_t next = std::min(target_index,
            current_index + size_t(bdpMaxLevelStep));
        bdpDepth = depthLevels[next];
        ++feedbackStats.bdpDepthChanges;
        bdpStableWindows = bdpStableWindowCount;
        bdpUpScore = bdpDownScore = 0;
        recomputeDepth("bdp-up");
    } else if (request_down && bdpDownScore >= int(bdpDownConfirmWindows) &&
               target_index < current_index) {
        const size_t next = current_index > bdpMaxLevelStep ?
            std::max(target_index, current_index - size_t(bdpMaxLevelStep)) : 0;
        bdpDepth = depthLevels[next];
        ++feedbackStats.bdpDepthChanges;
        bdpStableWindows = bdpStableWindowCount;
        bdpUpScore = bdpDownScore = 0;
        recomputeDepth("bdp-down");
    }
    DPRINTF(XsStreamPrefetcher,
            "BDP control: sent=%llu bdp=%.3f base=%d request=%s upScore=%d downScore=%d\n",
            sent, bdpEwma, bdpDepth,
            request_up ? "up" : request_down ? "down" : "hold",
            bdpUpScore, bdpDownScore);
}

void
XsStreamPrefetcher::updateDeltaController()
{
    const auto diff = [](uint64_t now, uint64_t old) { return now - old; };
    const uint64_t sent = diff(feedback.sent, deltaSnapshot.sent);
    const uint64_t late = diff(feedback.mshrHits, deltaSnapshot.mshrHits) +
        diff(feedback.demandMshrHits, deltaSnapshot.demandMshrHits);
    const uint64_t use_samples = diff(
        feedback.refillToUseSamples, deltaSnapshot.refillToUseSamples);
    const uint64_t use_cycles = diff(
        feedback.refillToUseCycles, deltaSnapshot.refillToUseCycles);
    deltaSnapshot.sent = feedback.sent;
    deltaSnapshot.mshrHits = feedback.mshrHits;
    deltaSnapshot.demandMshrHits = feedback.demandMshrHits;
    deltaSnapshot.refillToUseSamples = feedback.refillToUseSamples;
    deltaSnapshot.refillToUseCycles = feedback.refillToUseCycles;
    ++feedbackStats.deltaWindows;
    if (sent < deltaMinLateSamples ||
        use_samples < deltaMinRefillToUseSamples) {
        ++feedbackStats.deltaHolds;
        return;
    }

    const double late_rate = double(late) / sent;
    const double refill_to_use = double(use_cycles) / use_samples;
    if (refillToUseTargetCycles > 0.0) {
        refillToUseTarget = refillToUseTargetCycles;
        refillToUseTargetValid = true;
    } else if (!refillToUseTargetValid) {
        refillToUseTarget = refill_to_use;
        refillToUseTargetValid = true;
    } else {
        refillToUseTarget += refillToUseTargetAlpha *
            (refill_to_use - refillToUseTarget);
    }
    const double late_pressure = late_rate >= lateUpperThreshold ? 1.0 :
        late_rate <= lateLowerThreshold ? -1.0 :
        (late_rate - lateTargetRate) /
            std::max(1e-9, lateUpperThreshold - lateLowerThreshold);
    const double lead_ratio = refill_to_use / refillToUseTarget;
    const double lead_pressure = lead_ratio >= refillToUseEarlyRatio ? -1.0 :
        lead_ratio <= refillToUseLateRatio ? 1.0 : 0.0;
    const double pressure = lateWeight * late_pressure +
        refillToUseWeight * lead_pressure;
    feedbackStats.lateRate = late_rate;
    feedbackStats.refillToUseTarget = refillToUseTarget;
    feedbackStats.deltaPressure = pressure;
    if (deltaHoldWindows) {
        --deltaHoldWindows;
        ++feedbackStats.deltaHolds;
        return;
    }

    const bool request_up = pressure > deltaPressureThreshold;
    const bool request_down = pressure < -deltaPressureThreshold;
    if (request_up) {
        ++feedbackStats.deltaIncreaseRequests;
        ++deltaUpScore;
        deltaDownScore = 0;
    } else if (request_down) {
        ++feedbackStats.deltaDecreaseRequests;
        ++deltaDownScore;
        deltaUpScore = 0;
    } else {
        ++feedbackStats.deltaHolds;
        deltaUpScore = deltaDownScore = 0;
    }
    if (request_up && deltaUpScore >= int(deltaUpConfirmWindows)) {
        delta = std::min<int>(deltaMaxAbs, delta + deltaStep);
        deltaUpScore = deltaDownScore = 0;
        deltaHoldWindows = deltaHoldWindowCount;
        recomputeDepth("delta-up");
    } else if (request_down && deltaDownScore >= int(deltaDownConfirmWindows)) {
        delta = std::max<int>(-int(deltaMaxAbs), delta - deltaStep);
        deltaUpScore = deltaDownScore = 0;
        deltaHoldWindows = deltaHoldWindowCount;
        recomputeDepth("delta-down");
    }
    DPRINTF(XsStreamPrefetcher,
            "delta control: sent=%llu late=%.4f refillToUse=%.3f target=%.3f pressure=%.3f delta=%d\n",
            sent, late_rate, refill_to_use, refillToUseTarget, pressure, delta);
}

void
XsStreamPrefetcher::updateAccuracyController()
{
    const auto diff = [](uint64_t now, uint64_t old) { return now - old; };
    const uint64_t sent = diff(feedback.sent, accuracySnapshot.sent);
    const uint64_t uses = diff(feedback.pdbLoadUses, accuracySnapshot.pdbLoadUses);
    const uint64_t unused = diff(feedback.pdbUnusedReplacements,
        accuracySnapshot.pdbUnusedReplacements);
    const uint64_t refills = diff(feedback.pdbRefills, accuracySnapshot.pdbRefills);
    accuracySnapshot.sent = feedback.sent;
    accuracySnapshot.pdbLoadUses = feedback.pdbLoadUses;
    accuracySnapshot.pdbUnusedReplacements = feedback.pdbUnusedReplacements;
    accuracySnapshot.pdbRefills = feedback.pdbRefills;
    ++feedbackStats.accuracyWindows;
    const uint64_t samples = uses + unused;
    if (sent < accuracyMinSamples || samples < accuracyMinSamples) {
        return;
    }
    const double accuracy = double(uses) / samples;
    const double unused_rate = refills ? double(unused) / refills : 0.0;
    feedbackStats.accuracy = accuracy;
    feedbackStats.unusedReplacementRate = unused_rate;
    const bool bad = accuracy < usefulAccuracyThreshold &&
        unused_rate > unusedReplacementThreshold;
    if (!accuracyDisabled) {
        accuracyBadWindows = bad ? accuracyBadWindows + 1 : 0;
        if (accuracyBadWindows >= accuracyConfirmWindows) {
            accuracyDisabled = true;
            accuracyBadWindows = 0;
            accuracyGoodWindows = 0;
            ++feedbackStats.accuracyDisableEvents;
            DPRINTF(XsStreamPrefetcher,
                    "accuracy protection: disable accuracy=%.4f unused=%.4f\n",
                    accuracy, unused_rate);
        }
    } else {
        accuracyGoodWindows = accuracy >= reenableUsefulThreshold ?
            accuracyGoodWindows + 1 : 0;
        if (accuracyGoodWindows >= reenableConfirmWindows) {
            accuracyDisabled = false;
            accuracyGoodWindows = 0;
            ++feedbackStats.accuracyReenableEvents;
            DPRINTF(XsStreamPrefetcher,
                    "accuracy protection: re-enable accuracy=%.4f\n", accuracy);
        }
    }
    feedbackStats.accuracyDisabled = accuracyDisabled;
}

void
XsStreamPrefetcher::maybeUpdateControllers()
{
    if (enableCompetitiveDepth)
        return;
    if (!enableAutoDepth)
        return;
    if (feedback.sent % deltaWindowSent == 0)
        updateDeltaController();
    if (feedback.sent % bdpWindowSent == 0)
        updateBdpController();
}

void
XsStreamPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                      std::vector<AddrPriority> &addresses)
{
    calculatePrefetchImpl(pfi, addresses, false, false);
}

void
XsStreamPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                      std::vector<AddrPriority> &addresses,
                                      bool late)
{
    calculatePrefetchImpl(pfi, addresses, true, late);
}

void
XsStreamPrefetcher::calculatePrefetchImpl(
    const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses,
    bool coverage_valid, bool late)
{
    if (accuracyDisabled) {
        ++disabledProbeCalls;
        if (disabledProbeCalls % disabledProbeIntervalCalls != 0)
            return;
    }
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr block_addr = blockAddress(vaddr);
    ContextID context_id = pfi.hasContextId() ?
        pfi.contextId() : InvalidContextID;
    PrefetchSourceType stream_type = PrefetchSourceType::SStream;
    bool in_active_page = false;
    bool decr = false;
    if (pfi.isStore()) {
        stream_type = PrefetchSourceType::StoreStream;
        DPRINTF(XsStreamPrefetcher, "prefetch trigger come from store unit\n");
    }
    if (pfi.isCacheMiss() &&
        streamBlkFilter.contains(contextKey(block_addr, context_id))) {
        badPreNum++;
    }
    STREAMEntry *entry = streamLookup(pfi, in_active_page, decr);
    if (in_active_page) {
        if (coverage_valid && pfi.isCacheMiss() &&
            stream_type == PrefetchSourceType::SStream) {
            ++feedbackStats.activeStreamDemandMisses;
            const auto current_depth = std::find(
                competitiveDepthLevels.begin(),
                competitiveDepthLevels.end(), depth);
            if (current_depth != competitiveDepthLevels.end()) {
                ++feedbackStats.activeStreamDemandMissesAtDepth[
                    std::distance(
                        competitiveDepthLevels.begin(), current_depth)];
            }
            if (late) {
                ++feedbackStats.activeStreamLateDemandMisses;
            } else {
                ++feedbackStats.activeStreamUncoveredDemandMisses;
                if (current_depth != competitiveDepthLevels.end()) {
                    ++feedbackStats
                        .activeStreamUncoveredDemandMissesAtDepth[
                            std::distance(competitiveDepthLevels.begin(),
                                          current_depth)];
                }
                if (enableReuseObserver)
                    ++competitiveWindow.uncoveredDemandMisses;
            }
        }
        Addr pf_stream_l1 = decr ? block_addr - depth * blkSize : block_addr + depth * blkSize;
        sendPFWithFilter(pfi, pf_stream_l1, addresses, 1, stream_type, L1BLKDEGREE, 1, entry);
        const auto l2_depth = l2Depth ? l2Depth : (depth << l2Ratio);
        Addr pf_stream_l2 = decr ? block_addr - l2_depth * blkSize :
                                   block_addr + l2_depth * blkSize;
        sendPFWithFilter(pfi, pf_stream_l2, addresses, 1, stream_type, L2BLKDEGREE, 2, entry);
        if (enableL3StreamPre) {
            Addr pf_stream_l3 =
                decr ? block_addr - (depth << l3Ratio) * blkSize : block_addr + (depth << l3Ratio) * blkSize;
            sendPFWithFilter(pfi, pf_stream_l3, addresses, 1, stream_type, L3BLKDEGREE, 3, entry);
        }
    }
}

XsStreamPrefetcher::STREAMEntry *
XsStreamPrefetcher::streamLookup(const PrefetchInfo &pfi, bool &in_active_page, bool &decr)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr vaddr_tag_num = tagAddress(vaddr);
    Addr vaddr_offset = tagOffset(vaddr);
    bool secure = pfi.isSecure();
    ContextID context_id = pfi.hasContextId() ?
        pfi.contextId() : InvalidContextID;

    STREAMEntry *entry = stream_array.findEntry(
        contextKey(regionHashTag(vaddr_tag_num), context_id), secure);
    STREAMEntry *entry_plus = stream_array.findEntry(
        contextKey(regionHashTag(vaddr_tag_num + 1), context_id), secure);
    STREAMEntry *entry_min = stream_array.findEntry(
        contextKey(regionHashTag(vaddr_tag_num - 1), context_id), secure);

    bool entry_plus_active = entry_plus && entry_plus->active &&
        entry_plus->cnt >= ACTIVETHRESHOLD;
    bool entry_min_active = entry_min && entry_min->active &&
        entry_min->cnt >= ACTIVETHRESHOLD;

    if (entry) {
        stream_array.accessEntry(entry);
        uint64_t region_bit_accessed = 1UL << vaddr_offset;
        if (entry_plus)
            entry->decrMode = true;
        if ((entry_plus_active || entry_min_active) || (entry->cnt > ACTIVETHRESHOLD))
            entry->active = true;
        in_active_page = entry->active;
        decr = entry->decrMode;
        if (!(entry->bitVec & region_bit_accessed)) {
            entry->bitVec |= region_bit_accessed;
            entry->cnt += 1;
        }
        return entry;
    }
    Addr stream_key =
        contextKey(regionHashTag(vaddr_tag_num), context_id);
    entry = stream_array.findVictim(stream_key);

    in_active_page = (entry_plus_active || entry_min_active);
    decr = entry_plus != nullptr;
    entry->tag = regionHashTag(vaddr_tag_num);
    entry->decrMode = decr;
    entry->bitVec = 1UL << vaddr_offset;
    entry->cnt = 1;
    entry->active = in_active_page;
    entry->contextId = context_id;
    stream_array.insertEntry(stream_key, secure, entry);
    return entry;
}

void
XsStreamPrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
                                     int prio, PrefetchSourceType src, int pf_degree, int ahead_level, STREAMEntry *entry)
{
    uint64_t region_bit = 0;
    for (int i = 0; i < pf_degree; i++) {
        Addr pf_addr = addr + i * blkSize;
        region_bit |= (uint64_t(1) << regionOffset(pf_addr));

        // Count generated prefetch
        prefetchStats.pfGenerated++;

        Addr filter_key = sharedFilterKey(pfi, pf_addr);
        if (filter->contains(filter_key)) {
            DPRINTF(XsStreamPrefetcher, "Skip recently prefetched: %lx\n", pf_addr);
            // Count filtered prefetch
            prefetchStats.pfFiltered++;
        } else {
            DPRINTF(XsStreamPrefetcher, "Send pf: %lx\n", pf_addr);
            filter->insert(filter_key, 0);
            addresses.push_back(AddrPriority(pf_addr, prio, src));
            addresses.back().depthEpoch = depthEpoch;
            streamBlkFilter.insert(filter_key, 0);
            if (ahead_level > 1) {
                assert(ahead_level == 2 || ahead_level == 3);
                addresses.back().pfahead_host = ahead_level;
                addresses.back().pfahead = true;
            } else {
                addresses.back().pfahead = false;
            }
        }
    }
    pfi.setTriggerInfo_PFsrc(src);
    pfi.setTriggerInfoDepthEpoch(depthEpoch);
    if (ahead_level > 1) {
        stridestream_pfFilter_l2l3->Insert(regionAddress(addr), region_bit,0,true,entry->decrMode,pfi.isSecure(),ahead_level, &pfi.trigger_info);
    } else {
        stridestream_pfFilter_l1->Insert(regionAddress(addr), region_bit,0,true,entry->decrMode,pfi.isSecure(),ahead_level, &pfi.trigger_info);
    }
}


}
}
