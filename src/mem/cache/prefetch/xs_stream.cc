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
      depth(p.xs_stream_depth),
      badPreNum(0),
      enableAutoDepth(p.enable_auto_depth),
      enableL3StreamPre(p.enable_l3_stream_pre),
      l2Depth(p.xs_stream_l2_depth),
      feedbackStats(this),
      stream_array(p.xs_stream_entries, p.xs_stream_entries, p.xs_stream_indexing_policy,
                   p.xs_stream_replacement_policy, STREAMEntry()),
      streamBlkFilter(pfFilterSize)
{
    fatal_if(std::find(DEPTH_LEVELS.begin(), DEPTH_LEVELS.end(), depth) ==
                 DEPTH_LEVELS.end(),
             "xs_stream_depth must be one of 4, 8, 16, 32, 64, 96, or 128; "
             "got %d", depth);
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
               "Stream prefetch depth after the last complete window")
{
    windowsAtDepth.init(DEPTH_LEVELS.size());
    for (size_t i = 0; i < DEPTH_LEVELS.size(); ++i) {
        windowsAtDepth.subname(i, std::to_string(DEPTH_LEVELS[i]));
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
    if (feedback.sent == VALIDITYCHECKINTERVAL)
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
XsStreamPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                      std::vector<AddrPriority> &addresses)
{
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
