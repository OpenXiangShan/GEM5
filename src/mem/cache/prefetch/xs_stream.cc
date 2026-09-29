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
      depth(p.enable_auto_depth ? p.initial_bdp_depth : p.xs_stream_depth),
      bdpDepth(depth),
      enableAutoDepth(p.enable_auto_depth),
      enableL3StreamPre(p.enable_l3_stream_pre),
      l2Depth(p.xs_stream_l2_depth),
      depthLevels(p.depth_levels),
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
      bdpMaxDownLevelStep(p.bdp_max_down_level_step),
      bdpFastUpMinDepth(p.bdp_fast_up_min_depth),
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
    fatal_if(std::adjacent_find(
                 depthLevels.begin(), depthLevels.end(),
                 [](int left, int right) { return left >= right; }) !=
                 depthLevels.end(),
             "depth_levels must be strictly increasing");
    fatal_if(std::find(depthLevels.begin(), depthLevels.end(), depth) ==
                 depthLevels.end(),
             "initial stream depth %d must be present in depth_levels", depth);
    fatal_if(bdpWindowSent == 0 || deltaWindowSent == 0,
             "controller windows must be non-zero");
    fatal_if(bdpMaxLevelStep == 0 || bdpMaxDownLevelStep == 0,
             "BDP level steps must be non-zero");
    fatal_if(bdpUpConfirmWindows == 0 || bdpDownConfirmWindows == 0,
             "BDP confirmation windows must be non-zero");
    fatal_if(disabledProbeIntervalCalls == 0,
             "disabled_probe_interval_calls must be non-zero");
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
               "Whether accuracy protection disabled stream prefetching")
    {
    windowsAtDepth.init(parent->depthLevels.size());
    for (size_t i = 0; i < parent->depthLevels.size(); ++i) {
        windowsAtDepth.subname(i, std::to_string(parent->depthLevels[i]));
    }
}

void
XsStreamPrefetcher::recordStreamProbe(
    PrefetchSourceType source, Base::PrefetchProbeResult result)
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
    if (feedback.sent == bdpWindowSent)
        completeFeedbackWindow();
}

void
XsStreamPrefetcher::recordStreamPdbRefill()
{
    const Tick now = curTick();
    if (feedback.pdbRefills) {
        feedback.pdbRefillIntervalCycles +=
            ticksToCycles(now - feedback.lastPdbRefillTick);
        ++feedback.pdbRefillIntervalSamples;
    }
    feedback.lastPdbRefillTick = now;
    ++feedback.pdbRefills;
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
    constexpr double MIN_BDP_FILTER_GAIN = 0.125;
    constexpr double MAX_BDP_FILTER_GAIN = 0.75;
    constexpr double BDP_INNOVATION_GAIN = 0.5;
    constexpr double HIGH_FILTER_GAIN = 0.5;
    constexpr double BDP_TREND_GAIN = 0.25;
    constexpr double BDP_FORECAST_WINDOWS = 2.0;
    constexpr double MAX_BDP_FORECAST_DELTA = 0.5;
    constexpr double HIGH_LATE_RATE = 0.05;
    constexpr double VERY_HIGH_LATE_RATE = 0.10;
    constexpr int MAX_FEEDFORWARD_DEPTH = 32;

    if (!feedbackEwmaValid) {
        usefulRateEwma = useful_rate;
        lateRateEwma = late_rate;
        feedbackEwmaValid = true;
    } else {
        usefulRateEwma += bdpEwmaAlpha * (useful_rate - usefulRateEwma);
        lateRateEwma += bdpEwmaAlpha * (late_rate - lateRateEwma);
    }

    const bool bdp_valid =
        window.pdbRefillIntervalSamples >= bdpMinRefillSamples &&
        window.mshrResponseSamples >= bdpMinMshrSamples &&
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
        std::find(depthLevels.begin(), depthLevels.end(), depth);
    assert(depth_level != depthLevels.end());
    const auto depth_level_index =
        std::distance(depthLevels.begin(), depth_level);
    auto target_level = depth_level;
    int model_desired_depth = depth;
    bool hysteresis_hold = false;
    if (bdpEwmaValid) {
        model_desired_depth = std::min(
            MAX_FEEDFORWARD_DEPTH,
            std::max(depthLevels.front(),
                     int(std::ceil(bdpCalibrationFactor * bdpForecast))));
        target_level = std::lower_bound(
            depthLevels.begin(), depthLevels.end(), model_desired_depth);

        if (target_level > depth_level &&
            model_desired_depth < depth * bdpUpRatio) {
            target_level = depth_level;
            hysteresis_hold = true;
        }

        if (usefulRateEwma < 0.20) {
            target_level = depthLevels.begin();
        } else if (usefulRateEwma < 0.40) {
            const auto cap = depthLevels.begin() +
                std::min<size_t>(1, depthLevels.size() - 1);
            target_level = std::min(target_level, cap);
        } else if (usefulRateEwma < 0.65) {
            const auto cap = depthLevels.begin() +
                std::min<size_t>(2, depthLevels.size() - 1);
            target_level = std::min(target_level, cap);
        }

    }
    const size_t emergency_step = lateRateEwma >= VERY_HIGH_LATE_RATE ?
        2 : lateRateEwma >= HIGH_LATE_RATE ? 1 : 0;
    if (emergency_step) {
        const size_t emergency_index = std::min(
            depthLevels.size() - 1,
            size_t(depth_level_index) + emergency_step);
        target_level = std::max(
            target_level, depthLevels.begin() + emergency_index);
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
            if (depthDecisionScore >= int(bdpUpConfirmWindows)) {
                if (depth_level + 1 != depthLevels.end()) {
                    const size_t distance = std::distance(
                        depth_level, target_level);
                    const size_t max_step =
                        target_depth >= int(bdpFastUpMinDepth) ?
                        bdpMaxLevelStep : 1;
                    const size_t step = std::min(
                        distance, max_step);
                    depth = *(depth_level + step);
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
            if (depthDecisionScore <= -int(bdpDownConfirmWindows)) {
                if (depth_level != depthLevels.begin()) {
                    const size_t distance = std::distance(
                        target_level, depth_level);
                    const size_t step = std::min(
                        distance, size_t(bdpMaxDownLevelStep));
                    depth = *(depth_level - step);
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
    ++feedbackStats.bdpWindows;
    ++feedbackStats.windowsAtDepth[depth_level_index];
    if (target_level > depth_level)
        ++feedbackStats.bdpIncreaseRequests;
    if (target_level < depth_level)
        ++feedbackStats.bdpDecreaseRequests;
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
    if (depth != old_depth) {
        ++feedbackStats.bdpDepthChanges;
        ++feedbackStats.finalDepthUpdates;
        depthSettlingWindows = bdpStableWindowCount;
    }
    bdpDepth = depth;
    delta = 0;
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
    feedbackStats.bdpDepth = bdpDepth;
    feedbackStats.delta = delta;
    feedbackStats.finalDepth = depth;

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
        const size_t max_step =
            depthLevels[target_index] >= int(bdpFastUpMinDepth) ?
            bdpMaxLevelStep : 1;
        const size_t next = std::min(target_index,
            current_index + max_step);
        bdpDepth = depthLevels[next];
        ++feedbackStats.bdpDepthChanges;
        bdpStableWindows = bdpStableWindowCount;
        bdpUpScore = bdpDownScore = 0;
        recomputeDepth("bdp-up");
    } else if (request_down && bdpDownScore >= int(bdpDownConfirmWindows) &&
               target_index < current_index) {
        const size_t next = current_index > bdpMaxDownLevelStep ?
            std::max(target_index,
                     current_index - size_t(bdpMaxDownLevelStep)) : 0;
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

    bool entry_plus_active = entry_plus && entry_plus->active;
    bool entry_min_active = entry_min && entry_min->active;

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
    if (ahead_level > 1) {
        stridestream_pfFilter_l2l3->Insert(regionAddress(addr), region_bit,0,true,entry->decrMode,pfi.isSecure(),ahead_level, &pfi.trigger_info);
    } else {
        stridestream_pfFilter_l1->Insert(regionAddress(addr), region_bit,0,true,entry->decrMode,pfi.isSecure(),ahead_level, &pfi.trigger_info);
    }
}


}
}
