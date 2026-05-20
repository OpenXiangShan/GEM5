//Created on 24-01-03
//choose stride or berti in sms

#include "mem/cache/prefetch/xs_stride.hh"

#include <algorithm>
#include <cstdlib>

#include "base/logging.hh"
#include "base/stats/group.hh"
#include "debug/XSStridePrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

XSStridePrefetcher::XSStridePrefetcher(const XSStridePrefetcherParams &p)
    : Queued(p),useXsDepth(p.use_xs_depth),useRedundantTable(p.use_redundant_table),
      fuzzyStrideMatching(p.fuzzy_stride_matching),
      shortStrideThres(p.short_stride_thres),
      strideDynDepth(p.stride_dyn_depth),
      enableNonStrideFilter(p.enable_non_stride_filter),
      enableOracleSegmentedStride(p.enable_oracle_segmented_stride),
      oracleSegmentedStridePcs(p.oracle_segmented_stride_pcs),
      oracleMajorStrideBytes(p.oracle_major_stride_bytes),
      oracleMinorStrideBytes(p.oracle_minor_stride_bytes),
      oracleSegmentLengthLines(p.oracle_segment_length_lines),
      oracleStepOverrideOffsets(p.oracle_step_override_offsets),
      oracleStepOverrideBytes(p.oracle_step_override_bytes),
      oracleL1WindowLines(p.oracle_l1_window_lines),
      oracleEnableL1Prefetch(p.oracle_enable_l1_prefetch),
      oracleL2WindowLines(p.oracle_l2_window_lines),
      oracleObserveToleranceLines(p.oracle_observe_tolerance_lines),
      oracleDeactivateMisses(p.oracle_deactivate_misses),
      oracleRecentHistoryWindowTicks(p.oracle_recent_history_window_ticks),
      oracleOverrideRegularStride(p.oracle_override_regular_stride),
      regionSize(p.region_size),
      regionBlks(p.region_size / p.block_size),     
      strideUnique(p.stride_entries, p.stride_entries, p.stride_unique_indexing_policy,
             p.stride_unique_replacement_policy, StrideEntry()),
      strideRedundant(p.stride_entries, p.stride_entries, p.stride_redundant_indexing_policy,
             p.stride_redundant_replacement_policy, StrideEntry()),
      nonStridePCs(p.non_stride_assoc, p.non_stride_entries, p.non_stride_indexing_policy,
             p.non_stride_replacement_policy, NonStrideEntry()),
      stats(this)
{
    if (enableOracleSegmentedStride) {
        fatal_if(oracleSegmentedStridePcs.empty(),
                 "Oracle segmented-stride requires at least one PC\n");
        fatal_if(oracleMajorStrideBytes == 0 || oracleMinorStrideBytes == 0,
                 "Oracle segmented-stride requires non-zero major/minor strides\n");
        fatal_if(oracleMajorStrideBytes % blkSize != 0,
                 "Oracle major stride %llu must be aligned to blkSize %u\n",
                 static_cast<unsigned long long>(oracleMajorStrideBytes),
                 blkSize);
        fatal_if(oracleMinorStrideBytes % blkSize != 0,
                 "Oracle minor stride %llu must be aligned to blkSize %u\n",
                 static_cast<unsigned long long>(oracleMinorStrideBytes),
                 blkSize);
        fatal_if(oracleSegmentLengthLines == 0 || oracleL1WindowLines == 0,
                 "Oracle segmented-stride requires non-zero segment length and L1 window\n");
        fatal_if(oracleL1WindowLines > oracleSegmentLengthLines,
                 "Oracle L1 window %u must not exceed segment length %u\n",
                 oracleL1WindowLines, oracleSegmentLengthLines);
        fatal_if(oracleL2WindowLines <= oracleL1WindowLines,
                 "Oracle L2 window %u must be larger than Oracle L1 window %u\n",
                 oracleL2WindowLines, oracleL1WindowLines);
        fatal_if(oracleDeactivateMisses == 0,
                 "Oracle deactivate miss threshold must be non-zero\n");
        fatal_if(oracleStepOverrideOffsets.size() !=
                     oracleStepOverrideBytes.size(),
                 "Oracle step override offsets/bytes size mismatch: %zu vs %zu\n",
                 oracleStepOverrideOffsets.size(),
                 oracleStepOverrideBytes.size());

        for (Addr pc : oracleSegmentedStridePcs) {
            oracleSegmentedStridePcSet.insert(pc);
        }
        for (size_t i = 0; i < oracleStepOverrideOffsets.size(); ++i) {
            const unsigned offset = oracleStepOverrideOffsets[i];
            const Addr step_bytes = oracleStepOverrideBytes[i];
            fatal_if(offset + 1 >= oracleSegmentLengthLines,
                     "Oracle step override offset %u must be within [0, %u)\n",
                     offset, oracleSegmentLengthLines - 1);
            fatal_if(step_bytes == 0 || step_bytes % blkSize != 0,
                     "Oracle step override %zu bytes %llu must be non-zero and aligned to blkSize %u\n",
                     i, static_cast<unsigned long long>(step_bytes), blkSize);
            const auto inserted =
                oracleStepOverrideMap.emplace(offset, step_bytes);
            fatal_if(!inserted.second,
                     "Duplicate Oracle step override for offset %u\n",
                     offset);
        }
        oracleSegmentPrefixBytes.assign(oracleSegmentLengthLines, 0);
        for (unsigned offset = 0; offset + 1 < oracleSegmentLengthLines;
             ++offset) {
            oracleSegmentPrefixBytes[offset + 1] =
                oracleSegmentPrefixBytes[offset] +
                oracleStepBytesForOffset(offset);
        }
        oracleBoundaryDeltaBytes =
            static_cast<int64_t>(oracleMinorStrideBytes) -
            static_cast<int64_t>(oracleSegmentPrefixBytes.back());
    }
}


void
XSStridePrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                                       PrefetchSourceType pf_source, bool miss_repeat, bool enter_new_region,
                                       bool is_first_shot, Addr &pf_addr, int64_t &learned_bop_offset)
{
    pf_addr = 0;
    learned_bop_offset = 0;
    oracleObserveFeedback(pfi);
    const bool oracle_matched = oracleGenerate(pfi, addresses);
    if (oracle_matched && oracleOverrideRegularStride) {
        return;
    }

    if (is_first_shot ||!useRedundantTable) {
        DPRINTF(XSStridePrefetcher, "Do stride lookup for first shot acc ...\n");
        strideLookup(strideUnique, pfi, addresses, late, pf_addr, pf_source, enter_new_region, miss_repeat,
                     learned_bop_offset, is_first_shot);
    } else {
        DPRINTF(XSStridePrefetcher, "Do stride lookup for repeat acc ...\n");
        strideLookup(strideRedundant, pfi, addresses, late, pf_addr, pf_source, enter_new_region, miss_repeat,
                     learned_bop_offset, is_first_shot);
    }
}
bool
XSStridePrefetcher::strideLookup(AssociativeSet<StrideEntry> &stride, const PrefetchInfo &pfi,
                                  std::vector<AddrPriority> &addresses, bool late, Addr &stride_pf,
                                  PrefetchSourceType last_pf_source, bool enter_new_region, bool miss_repeat,
                                  int64_t &learned_bop_offset, bool is_first_shot)
{
    if (is_first_shot) {
        stats.strideUniquequeryCount++;
    } else {
        stats.strideRedundantqueryCount++;
    }
    Addr lookupAddr = pfi.getAddr();
    Addr stride_hash_pc = strideHashPc(pfi.getPC());
    StrideEntry *entry = stride.findEntry(stride_hash_pc, pfi.isSecure());
    learned_bop_offset = 0;
    // TODO: add DPRINFT for stride
    DPRINTF(XSStridePrefetcher, "Stride lookup: pc:%x addr: %x, miss repeat: %i\n", pfi.getPC(), lookupAddr,
            miss_repeat);
    bool should_cover = false;
    if (entry) {
        if (archDBer){
            archDBer->strideTraceWrite(curTick(), lookupAddr, pfi.getPC(), stride_hash_pc,
                                       true, is_first_shot, pfi.isCacheMiss(), true);
        }
    }else{
        if (archDBer){
            archDBer->strideTraceWrite(curTick(), lookupAddr, pfi.getPC(), stride_hash_pc,
                                       false, is_first_shot, pfi.isCacheMiss(), true);
        }
    }
    if (entry) {
        if (is_first_shot) {
            stats.strideUniquehitCount++;
        } else {
            stats.strideRedundanthitCount++;
        }
        stride.accessEntry(entry);
        int64_t new_stride = lookupAddr - entry->lastAddr;
        if (new_stride == 0 || (labs(new_stride) < 64 && (miss_repeat || entry->longStride.calcSaturation() >= 0.5))) {
            DPRINTF(XSStridePrefetcher, "Stride touch in the same blk, ignore redundant req\n");
            return false;
        }
        bool stride_match = fuzzyStrideMatching ? (entry->stride > 64 && new_stride % entry->stride == 0) : false;
        stride_match |= new_stride == entry->stride;
        DPRINTF(XSStridePrefetcher, "Stride hit, with stride: %ld(%lx), old stride: %ld(%lx), long stride: %.2f\n",
                new_stride, new_stride, entry->stride, entry->stride, entry->longStride.calcSaturation());

        if (shortStrideThres) {
            if (labs(new_stride) > shortStrideThres) {
                entry->longStride.saturate();
            } else {
                entry->longStride--;
            }
        }

        if (shortStrideThres && entry->longStride.calcSaturation() > 0.5 && labs(new_stride) < shortStrideThres) {
            DPRINTF(XSStridePrefetcher, "Ignore short stride %li for long stride pattern\n", new_stride);
            return false;
        } else {
            DPRINTF(XSStridePrefetcher, "Stride long stride pattern: %.2f, short thres: %lu\n",
                    entry->longStride.calcSaturation(), shortStrideThres);
        }

        if (stride_match) {
            entry->conf++;
            if (strideDynDepth) {
                if (!pfi.isCacheMiss() && last_pf_source == PrefetchSourceType::SStride) {  // stride pref hit
                    entry->lateConf--;
                } else if (late) {  // stride pf late or other prefetcher late
                    entry->lateConf += 3;
                }
                if (entry->lateConf.isSaturated()) {
                    entry->depth++;
                    entry->lateConf.reset();
                } else if ((uint8_t)entry->lateConf == 0) {
                    entry->depth = std::max(1, entry->depth - 1);
                    entry->lateConf.reset();
                }
            }
            DPRINTF(XSStridePrefetcher, "Stride match, inc conf to %d, late: %i, late sat:%i, depth: %i\n",
                    (int)entry->conf, late, (uint8_t)entry->lateConf, entry->depth);
            entry->lastAddr = lookupAddr;
            entry->histStrides.clear();
            entry->matchedSinceAlloc = true;

        } else if (labs(entry->stride) > 64L && labs(new_stride) < 64L) {
            // different stride, but in the same cache line
            DPRINTF(XSStridePrefetcher, "Stride unmatch, but access goes to the same line, ignore\n");

        } else {
            entry->conf--;
            entry->lastAddr = lookupAddr;
            DPRINTF(XSStridePrefetcher, "Stride unmatch, dec conf to %d\n", (int)entry->conf);
            if ((int)entry->conf == 0) {
                DPRINTF(XSStridePrefetcher, "Stride conf = 0, reset stride to %ld\n", new_stride);

                bool found_in_hist = false;

                if (enableNonStrideFilter) {
                    if (entry->stride != 0) {
                        entry->histStrides.push_back(entry->stride);
                    }
                    for (auto it = entry->histStrides.begin(); it != entry->histStrides.end(); it++) {
                        DPRINTF(XSStridePrefetcher, "Stride hist: %ld, match: %i\n", *it, *it == new_stride);
                        if (*it == new_stride) {
                            found_in_hist = true;
                            entry->histStrides.erase(it);
                            break;
                        }
                    }
                    if (found_in_hist) {
                        entry->histStrides.clear();
                    }
                }

                if (enableNonStrideFilter && !found_in_hist && entry->histStrides.size() >= maxHistStrides) {
                    markNonStridePC(entry->pc);
                    entry->histStrides.clear();
                    entry->invalidate();
                } else {
                    entry->stride = new_stride;
                    entry->depth = 1;
                    entry->lateConf.reset();
                }
            }
        }
        if (entry->conf >= 2) {
            // if miss send 1*stride ~ depth*stride, else send depth*stride
            unsigned start_depth = pfi.isCacheMiss() ? std::max(1, (entry->depth - 4)) : entry->depth;
            Addr pf_addr = 0;
            if (useXsDepth) {
                sendPFWithFilter(pfi, blockAddress(lookupAddr + (entry->stride << 2)), addresses, 0,
                                 PrefetchSourceType::SStride, 1);
                sendPFWithFilter(pfi, blockAddress(lookupAddr + (entry->stride << 5)), addresses, 0,
                                 PrefetchSourceType::SStride, 2);
                if (is_first_shot) {
                    stats.strideUniquepfCount += 2;
                } else {
                    stats.strideRedundantpfCount += 2;
                }
                if (archDBer){
                    archDBer->strideTraceWrite(curTick(),  blockAddress(lookupAddr + (entry->stride << 2)), pfi.getPC(), stride_hash_pc,
                                            true, is_first_shot, pfi.isCacheMiss(), false);
                    archDBer->strideTraceWrite(curTick(),  blockAddress(lookupAddr + (entry->stride << 5)), pfi.getPC(), stride_hash_pc,
                                            true, is_first_shot, pfi.isCacheMiss(), false);
                }
            } else {
                for (unsigned i = start_depth; i <= entry->depth; i++) {
                    pf_addr = lookupAddr + entry->stride * i;
                    DPRINTF(XSStridePrefetcher, "Stride conf >= 2, send pf: %x with depth %i\n", pf_addr, i);
                    sendPFWithFilter(pfi, blockAddress(pf_addr), addresses, 0, PrefetchSourceType::SStride, 1);
                    if (is_first_shot) {
                        stats.strideUniquepfCount++;
                    } else {
                        stats.strideRedundantpfCount++;
                    }
                }
                stride_pf = pf_addr;  // the longest lookahead
            }

            should_cover = true;
        }
    } else {
        if (is_first_shot) {
            stats.strideUniquemissCount++;
        } else {
            stats.strideRedundantmissCount++;
        }
        DPRINTF(XSStridePrefetcher, "Stride miss, insert it\n");
        entry = stride.findVictim(0);
        DPRINTF(XSStridePrefetcher, "Stride found victim pc = %x, stride = %i\n", entry->pc, entry->stride);
        if (enableNonStrideFilter && (entry->histStrides.size() >= maxHistStrides - 1 || !entry->matchedSinceAlloc)) {
            DPRINTF(XSStridePrefetcher, "Stride hist %u >= %u, mark pc %x as non-stride\n", entry->histStrides.size(),
                    maxHistStrides - 1, entry->pc);
            markNonStridePC(entry->pc);
        }
        if (entry->conf >= 2){
            if (is_first_shot) {
                stats.strideUniquereplaceusefulCount++;
            } else {
                stats.strideRedundantreplaceusefulCount++;
            }
        }
        if (entry->conf >= 2 && entry->stride > 1024) {  // > 1k
            DPRINTF(XSStridePrefetcher, "Stride Evicting a useful stride, send it to BOP with offset %i\n",
                    entry->stride / 64);
            // learnedBOP->tryAddOffset(entry->stride / 64);
            learned_bop_offset = entry->stride / 64;
        }
        entry->conf.reset();
        entry->lastAddr = lookupAddr;
        entry->stride = 0;
        entry->depth = 1;
        entry->lateConf.reset();
        entry->pc = pfi.getPC();
        entry->histStrides.clear();
        entry->matchedSinceAlloc = false;
        DPRINTF(XSStridePrefetcher, "Stride miss, insert with stride 0\n");
        stride.insertEntry(stride_hash_pc, pfi.isSecure(), entry);
    }
    periodStrideDepthDown();
    return should_cover;
}

bool
XSStridePrefetcher::isOracleSegmentedStridePC(Addr pc) const
{
    return enableOracleSegmentedStride &&
           oracleSegmentedStridePcSet.find(pc) !=
               oracleSegmentedStridePcSet.end();
}

void
XSStridePrefetcher::triggerFromS1(const PrefetchInfo &pfi,
                                  std::vector<AddrPriority> &addresses)
{
    const bool oracle_matched = oracleGenerate(pfi, addresses);
    if (oracle_matched && oracleOverrideRegularStride) {
        return;
    }
}

void
XSStridePrefetcher::oracleResetStreamState(OracleStreamState &state)
{
    state = OracleStreamState();
}

void
XSStridePrefetcher::oracleActivateFromLine(OracleStreamState &state,
                                           Addr line_addr, bool aligned)
{
    oracleResetStreamState(state);
    state.active = true;
    state.aligned = aligned;
    state.baseHeadLine = line_addr;
    state.frontierIndex = 0;
    state.frontierLine = line_addr;
    state.lastTriggerValid = true;
    state.lastTriggerLine = line_addr;
    state.lastFeedbackValid = false;
    state.l1Armed = false;
    state.nextL2IssueDemandIndex = 0;
    state.nextL1IssueDemandIndex = 0;
}

void
XSStridePrefetcher::oracleDeactivate(OracleStreamState &state)
{
    oracleResetStreamState(state);
}

Addr
XSStridePrefetcher::oracleStepBytesForOffset(unsigned segment_offset) const
{
    const auto override_it = oracleStepOverrideMap.find(segment_offset);
    if (override_it != oracleStepOverrideMap.end()) {
        return override_it->second;
    }
    return oracleMajorStrideBytes;
}

Addr
XSStridePrefetcher::oracleLineForOffset(Addr segment_base_line,
                                        unsigned segment_offset) const
{
    return segment_base_line + oracleSegmentPrefixBytes[segment_offset];
}

uint64_t
XSStridePrefetcher::oracleSegmentStartIndex(uint64_t stream_index) const
{
    return (stream_index / oracleSegmentLengthLines) *
           oracleSegmentLengthLines;
}

uint64_t
XSStridePrefetcher::oracleSegmentEndIndex(uint64_t stream_index) const
{
    return oracleSegmentStartIndex(stream_index) + oracleSegmentLengthLines - 1;
}

uint64_t
XSStridePrefetcher::oracleSegmentIdForIndex(uint64_t stream_index) const
{
    return stream_index / oracleSegmentLengthLines;
}

int
XSStridePrefetcher::oracleSegmentOffsetForIndex(uint64_t stream_index) const
{
    return stream_index % oracleSegmentLengthLines;
}

Addr
XSStridePrefetcher::oracleLineForStreamIndex(
    const OracleStreamState &state, uint64_t stream_index) const
{
    const uint64_t segment_id = oracleSegmentIdForIndex(stream_index);
    const unsigned segment_offset =
        static_cast<unsigned>(oracleSegmentOffsetForIndex(stream_index));
    const Addr segment_base_line =
        state.baseHeadLine + segment_id * oracleMinorStrideBytes;
    return oracleLineForOffset(segment_base_line, segment_offset);
}

bool
XSStridePrefetcher::oracleFindMatchedIndex(const OracleStreamState &state,
                                           Addr line_addr,
                                           uint64_t &matched_index) const
{
    if (!state.active || !state.aligned) {
        return false;
    }

    const uint64_t start_index =
        state.frontierIndex > oracleObserveToleranceLines
            ? state.frontierIndex - oracleObserveToleranceLines
            : 0;
    const uint64_t end_index =
        state.frontierIndex + oracleObserveToleranceLines;
    for (uint64_t candidate = start_index; candidate <= end_index;
         ++candidate) {
        if (oracleLineForStreamIndex(state, candidate) == line_addr) {
            matched_index = candidate;
            return true;
        }
    }

    return false;
}

void
XSStridePrefetcher::oracleTrackLaneIssue(Addr line_addr,
                                         uint64_t stream_index,
                                         int target_level)
{
    OracleTrackedLine &tracked = oracleOutstandingTargets[line_addr];
    tracked.streamIndex = stream_index;
    tracked.segmentId = oracleSegmentIdForIndex(stream_index);
    tracked.segmentOffset = oracleSegmentOffsetForIndex(stream_index);
    if (target_level > 1) {
        tracked.hadL2Issue = true;
        tracked.l2IssueTick = curTick();
    } else {
        tracked.hadL1Issue = true;
        tracked.l1IssueTick = curTick();
    }
    if (tracked.hadL1Issue) {
        oracleOutstandingL1Targets[line_addr] = tracked;
    }
    oracleRecentLines.erase(line_addr);
}

void
XSStridePrefetcher::oraclePruneRecentLines(Tick now)
{
    if (oracleRecentHistoryWindowTicks == 0) {
        oracleRecentLines.clear();
        oracleRecentLineOrder.clear();
        return;
    }

    while (!oracleRecentLineOrder.empty()) {
        const Addr line_addr = oracleRecentLineOrder.front().first;
        const Tick feedback_tick = oracleRecentLineOrder.front().second;
        if (feedback_tick + oracleRecentHistoryWindowTicks > now) {
            break;
        }

        const auto recent_it = oracleRecentLines.find(line_addr);
        if (recent_it != oracleRecentLines.end() &&
            recent_it->second.feedbackTick == feedback_tick) {
            oracleRecentLines.erase(recent_it);
        }
        oracleRecentLineOrder.pop_front();
    }
}

void
XSStridePrefetcher::oracleRememberRecentLine(Addr line_addr,
                                             const OracleTrackedLine &tracked,
                                             Tick feedback_tick)
{
    if (oracleRecentHistoryWindowTicks == 0) {
        return;
    }

    oracleRecentLines[line_addr] = OracleRecentLine{tracked, feedback_tick};
    oracleRecentLineOrder.emplace_back(line_addr, feedback_tick);
}

XSStridePrefetcher::OracleTrackedLine
XSStridePrefetcher::oracleFallbackTrackedLine(
    const OracleStreamState &state, Addr line_addr) const
{
    OracleTrackedLine tracked;
    uint64_t matched_index = 0;
    if (oracleFindMatchedIndex(state, line_addr, matched_index)) {
        tracked.streamIndex = matched_index;
        tracked.segmentId = oracleSegmentIdForIndex(matched_index);
        tracked.segmentOffset = oracleSegmentOffsetForIndex(matched_index);
        return tracked;
    }

    if (state.active && state.aligned) {
        tracked.streamIndex = state.frontierIndex;
        tracked.segmentId = oracleSegmentIdForIndex(state.frontierIndex);
        tracked.segmentOffset = oracleSegmentOffsetForIndex(state.frontierIndex);
    }
    return tracked;
}

void
XSStridePrefetcher::oracleClassifyDemandFeedback(
    const PrefetchInfo &pfi, const OracleTrackedLine &tracked,
    int observed_level)
{
    const Request::XsMetadata xs_metadata = pfi.getXsMetadata();
    const bool lower_covered =
        pfi.isCacheMiss() &&
        xs_metadata.prefetchSource == PrefetchSourceType::OracleStride;

    if (observed_level == 1) {
        if (tracked.hadL1Issue) {
            stats.oracleFirstTouchL1HitWithLeadCount++;
        } else {
            stats.oracleFirstTouchL1HitWithoutLeadCount++;
        }
    } else if (tracked.hadL1Issue) {
        if (lower_covered) {
            stats.oracleFirstTouchMissWithLeadLowerCoveredCount++;
        } else {
            stats.oracleFirstTouchMissWithLeadLowerUncoveredCount++;
        }
    } else if (lower_covered) {
        stats.oracleFirstTouchMissNoLeadLowerCoveredCount++;
    } else {
        stats.oracleFirstTouchMissNoLeadLowerUncoveredCount++;
    }
}

void
XSStridePrefetcher::oracleObserveFeedback(const PrefetchInfo &pfi)
{
    if (!enableOracleSegmentedStride || !pfi.hasPC() ||
        !isOracleSegmentedStridePC(pfi.getPC())) {
        return;
    }

    OracleStreamState &state = oracleStreamState;
    const Addr line_addr = blockAddress(pfi.getAddr());
    if (state.lastFeedbackValid && state.lastFeedbackLine == line_addr) {
        return;
    }

    state.lastFeedbackValid = true;
    state.lastFeedbackLine = line_addr;

    oraclePruneRecentLines(curTick());
    const auto tracked_it = oracleOutstandingTargets.find(line_addr);
    const auto recent_it = oracleRecentLines.find(line_addr);
    const bool has_outstanding = tracked_it != oracleOutstandingTargets.end();
    if (!has_outstanding && recent_it != oracleRecentLines.end()) {
        return;
    }

    const OracleTrackedLine tracked =
        has_outstanding ? tracked_it->second
                        : oracleFallbackTrackedLine(state, line_addr);
    int observed_level = 0;
    if (!pfi.isCacheMiss()) {
        observed_level = 1;
    } else if (pfi.getXsMetadata().prefetchSource !=
               PrefetchSourceType::PF_NONE) {
        observed_level = 2;
    }
    oracleClassifyDemandFeedback(pfi, tracked, observed_level);
    oracleRememberRecentLine(line_addr, tracked, curTick());

    if (has_outstanding) {
        oracleOutstandingTargets.erase(tracked_it);
    }
    oracleOutstandingL1Targets.erase(line_addr);
}

bool
XSStridePrefetcher::oracleIssuePrefetch(const PrefetchInfo &pfi,
                                        std::vector<AddrPriority> &addresses,
                                        const OracleStreamState &state,
                                        uint64_t stream_index,
                                        int target_level)
{
    const int ahead_level = target_level > 1 ? 2 : 1;
    const int depth_override =
        target_level > 1 ? static_cast<int>(oracleL2WindowLines)
                         : static_cast<int>(oracleL1WindowLines);
    Addr target_line_addr =
        blockAddress(oracleLineForStreamIndex(state, stream_index));
    const bool issued = sendPFWithFilter(
        pfi, target_line_addr, addresses, 0,
        PrefetchSourceType::OracleStride, ahead_level, depth_override);
    if (!issued) {
        return false;
    }

    oracleTrackLaneIssue(target_line_addr, stream_index, target_level);
    return true;
}

void
XSStridePrefetcher::oracleMaybeArmL1Lead(OracleStreamState &state)
{
    if (!oracleEnableL1Prefetch || !state.active || !state.aligned ||
        state.l1Armed) {
        return;
    }

    const uint64_t tail_start_offset =
        oracleSegmentLengthLines > oracleL1WindowLines
            ? oracleSegmentLengthLines - oracleL1WindowLines
            : 0;
    if (static_cast<uint64_t>(oracleSegmentOffsetForIndex(state.frontierIndex)) <
        tail_start_offset) {
        return;
    }

    state.l1Armed = true;
    state.nextL1IssueDemandIndex =
        oracleSegmentStartIndex(state.frontierIndex) + tail_start_offset;
    stats.oracleL1ArmCount++;
}

void
XSStridePrefetcher::oracleDrainL2Lane(
    const PrefetchInfo &pfi, OracleStreamState &state,
    std::vector<AddrPriority> &addresses)
{
    if (!state.active || !state.aligned) {
        return;
    }

    while (state.nextL2IssueDemandIndex <= state.frontierIndex) {
        const uint64_t demand_index = state.nextL2IssueDemandIndex++;
        const uint64_t stream_index = demand_index + oracleSegmentLengthLines;
        stats.oracleL2IssueAttemptCount++;
        if (oracleIssuePrefetch(pfi, addresses, state, stream_index, 2)) {
            stats.oracleL2IssueSentCount++;
        } else {
            stats.oracleL2IssueSuppressedCount++;
        }
    }
}

void
XSStridePrefetcher::oracleDrainL1Lane(
    const PrefetchInfo &pfi, OracleStreamState &state,
    std::vector<AddrPriority> &addresses)
{
    if (!oracleEnableL1Prefetch || !state.active || !state.aligned ||
        !state.l1Armed) {
        return;
    }

    while (state.nextL1IssueDemandIndex <= state.frontierIndex) {
        const uint64_t demand_index = state.nextL1IssueDemandIndex++;
        const uint64_t stream_index = demand_index + oracleL1WindowLines;
        const Addr target_line_addr =
            blockAddress(oracleLineForStreamIndex(state, stream_index));
        const auto target_it = oracleOutstandingTargets.find(target_line_addr);
        const bool had_prior_l2 =
            target_it != oracleOutstandingTargets.end() &&
            target_it->second.hadL2Issue;

        stats.oracleL1IssueAttemptCount++;
        if (oracleIssuePrefetch(pfi, addresses, state, stream_index, 1)) {
            stats.oracleL1IssueSentCount++;
            if (had_prior_l2) {
                stats.oracleL1IssuePriorL2Count++;
            } else {
                stats.oracleL1IssueNoPriorL2Count++;
            }
        } else {
            stats.oracleL1IssueSuppressedCount++;
        }
    }
}

void
XSStridePrefetcher::oracleDrainReadyLanes(
    const PrefetchInfo &pfi, OracleStreamState &state,
    std::vector<AddrPriority> &addresses)
{
    if (!state.active || !state.aligned) {
        return;
    }

    oracleDrainL2Lane(pfi, state, addresses);
    oracleMaybeArmL1Lead(state);
    oracleDrainL1Lane(pfi, state, addresses);
}

void
XSStridePrefetcher::oracleAdvanceFrontier(
    const PrefetchInfo &pfi, OracleStreamState &state,
    std::vector<AddrPriority> &addresses)
{
    bool advanced = false;
    while (state.seenFuture.erase(state.frontierIndex + 1) > 0) {
        state.frontierIndex++;
        state.frontierLine =
            oracleLineForStreamIndex(state, state.frontierIndex);
        stats.oracleAdvanceFrontierCount++;
        advanced = true;
    }

    if (!advanced && !state.seenFuture.empty()) {
        const uint64_t furthest_seen =
            *std::max_element(state.seenFuture.begin(), state.seenFuture.end());
        if (furthest_seen > state.frontierIndex + oracleObserveToleranceLines) {
            const uint64_t relaxed_frontier =
                furthest_seen - oracleObserveToleranceLines;
            for (auto it = state.seenFuture.begin();
                 it != state.seenFuture.end();) {
                if (*it <= relaxed_frontier) {
                    it = state.seenFuture.erase(it);
                } else {
                    ++it;
                }
            }
            state.frontierIndex = relaxed_frontier;
            state.frontierLine =
                oracleLineForStreamIndex(state, state.frontierIndex);
            stats.oracleAdvanceFrontierCount++;
            advanced = true;
        }
    }

    if (advanced) {
        state.unmatchedStreak = 0;
    }

    oracleDrainReadyLanes(pfi, state, addresses);
}

bool
XSStridePrefetcher::oracleGenerate(const PrefetchInfo &pfi,
                                   std::vector<AddrPriority> &addresses)
{
    if (!enableOracleSegmentedStride || !pfi.hasPC() ||
        !isOracleSegmentedStridePC(pfi.getPC())) {
        return false;
    }

    OracleStreamState &state = oracleStreamState;
    const Addr line_addr = blockAddress(pfi.getAddr());
    if (!state.active) {
        stats.oracleActivateCount++;
        oracleActivateFromLine(state, line_addr, false);
        return true;
    }

    if (state.lastTriggerValid && state.lastTriggerLine == line_addr) {
        stats.oracleReplayCount++;
        return true;
    }

    if (!state.aligned && state.lastTriggerValid &&
        static_cast<int64_t>(line_addr) -
                static_cast<int64_t>(state.lastTriggerLine) ==
            oracleBoundaryDeltaBytes) {
        stats.oracleBoundaryResyncCount++;
        oracleActivateFromLine(state, line_addr, true);
        oracleDrainReadyLanes(pfi, state, addresses);
        return true;
    }

    state.lastTriggerValid = true;
    state.lastTriggerLine = line_addr;
    if (!state.aligned) {
        return true;
    }

    uint64_t matched_index = 0;
    if (oracleFindMatchedIndex(state, line_addr, matched_index)) {
        if (matched_index <= state.frontierIndex) {
            stats.oracleReplayCount++;
            return true;
        }

        const auto inserted = state.seenFuture.insert(matched_index);
        if (!inserted.second) {
            stats.oracleReplayCount++;
            return true;
        }

        stats.oracleMatchCount++;
        state.unmatchedStreak = 0;
        oracleAdvanceFrontier(pfi, state, addresses);
        return true;
    }

    state.unmatchedStreak++;
    stats.oracleWindowMissCount++;

    if (state.unmatchedStreak >= oracleDeactivateMisses) {
        stats.oracleDeactivateCount++;
        oracleDeactivate(state);
        stats.oracleActivateCount++;
        oracleActivateFromLine(state, line_addr, false);
    }

    return true;
}

void
XSStridePrefetcher::periodStrideDepthDown()
{
    if (depthDownCounter < depthDownPeriod) {
        depthDownCounter++;
    } else {
        for (auto stride : {&strideUnique, &strideRedundant}) {
            for (StrideEntry &entry : *stride) {
                if (entry.conf >= 2) {
                    entry.depth = std::max(entry.depth - 1, 1);
                }
            }
        }
        depthDownCounter = 0;
    }
}

void
XSStridePrefetcher::markNonStridePC(Addr pc)
{
    DPRINTF(XSStridePrefetcher, "Mark non-stride pc %x\n", pc);
    auto *entry = nonStridePCs.findEntry(nonStrideHash(pc), false);
    if (entry) {
        nonStridePCs.accessEntry(entry);
    } else {
        entry = nonStridePCs.findVictim(nonStrideHash(pc));
        assert(entry);
        entry->pc = pc;
        nonStridePCs.insertEntry(nonStrideHash(pc), false, entry);
    }
}

bool
XSStridePrefetcher::isNonStridePC(Addr pc)
{
    auto *entry = nonStridePCs.findEntry(nonStrideHash(pc), false);
    return entry != nullptr;
}

bool
XSStridePrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
                                      int prio, PrefetchSourceType src, int ahead_level,
                                      int depth_override)
{
    // Count generated prefetch
    prefetchStats.pfGenerated++;
    pfi.setTriggerInfo_PFsrc(src);
    if (ahead_level > 1){
        stridestream_pfFilter_l2l3->Insert(regionAddress(addr), uint64_t(1) << regionOffset(addr),0,true,false,pfi.isSecure(),ahead_level, &pfi.trigger_info);
        if (filterL2->contains(addr)) {
            DPRINTF(XSStridePrefetcher, "Skip recently prefetched: %lx\n", addr);
            // Count filtered prefetch
            prefetchStats.pfFiltered++;
            return false;
        } else {
            DPRINTF(XSStridePrefetcher, "Send pf: %lx\n", addr);
            filterL2->insert(addr, 0);
            addresses.push_back(AddrPriority(addr, prio, src));
            assert(ahead_level == 2 || ahead_level == 3);
            addresses.back().pfahead_host = ahead_level;
            addresses.back().pfahead = true;
            addresses.back().depth =
                depth_override > 0 ? depth_override : ahead_level;
            return true;
        }
    } else {
        stridestream_pfFilter_l1->Insert(regionAddress(addr), uint64_t(1) << regionOffset(addr),0,true,false,pfi.isSecure(),ahead_level, &pfi.trigger_info);
        if (filter->contains(addr)) {
            DPRINTF(XSStridePrefetcher, "Skip recently prefetched: %lx\n", addr);
            // Count filtered prefetch
            prefetchStats.pfFiltered++;
            return false;
        } else {
            DPRINTF(XSStridePrefetcher, "Send pf: %lx\n", addr);
            filter->insert(addr, 0);
            addresses.push_back(AddrPriority(addr, prio, src));
            addresses.back().pfahead_host = ahead_level;
            addresses.back().pfahead = false;
            addresses.back().depth =
                depth_override > 0 ? depth_override : ahead_level;
            return true;
        }
    }
    return false;
}

void
XSStridePrefetcher::prefetchUnused(Addr paddr, PrefetchSourceType pfSource)
{
    Base::prefetchUnused(paddr, pfSource);

    if (!enableOracleSegmentedStride ||
        pfSource != PrefetchSourceType::OracleStride) {
        return;
    }

    const Addr line_addr = blockAddress(paddr);
    const auto tracked_it = oracleOutstandingL1Targets.find(line_addr);
    if (tracked_it == oracleOutstandingL1Targets.end()) {
        return;
    }

    stats.oracleEvictBeforeUseL1Count++;
    oracleOutstandingL1Targets.erase(tracked_it);
}

Addr
XSStridePrefetcher::strideHashPc(Addr pc)
{
    Addr pc_high_1 = (pc >> 20) & (0x1f);
    Addr pc_high_2 = (pc >> 15) & (0x1f);
    Addr pc_high_3 = (pc >> 10) & (0x1f);
    Addr pc_high = pc_high_1 ^ pc_high_2 ^ pc_high_3;
    Addr pc_low = pc & (0x1ff);
    return (pc_high << 10) | pc_low;
}

XSStridePrefetcher::XSstrideStats::XSstrideStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(strideUniquequeryCount, statistics::units::Count::get(), "stride table query num"),
      ADD_STAT(strideUniquehitCount, statistics::units::Count::get(), "stride table hit num"),
      ADD_STAT(strideUniquemissCount, statistics::units::Count::get(), "stride table miss num"),
      ADD_STAT(strideUniquepfCount, statistics::units::Count::get(), "stride prefetch num"),
      ADD_STAT(strideUniquereplaceusefulCount, statistics::units::Count::get(), "stride table replace num"),
      ADD_STAT(strideRedundantqueryCount, statistics::units::Count::get(), "stride table query num"),
      ADD_STAT(strideRedundanthitCount, statistics::units::Count::get(), "stride table hit num"),
      ADD_STAT(strideRedundantmissCount, statistics::units::Count::get(), "stride table miss num"),
      ADD_STAT(strideRedundantpfCount, statistics::units::Count::get(), "stride prefetch num"),
      ADD_STAT(strideRedundantreplaceusefulCount, statistics::units::Count::get(), "stride table replace num"),
      ADD_STAT(oracleActivateCount, statistics::units::Count::get(),
               "number of provisional Oracle stream activations"),
      ADD_STAT(oracleBoundaryResyncCount, statistics::units::Count::get(),
               "number of Oracle boundary re-sync events"),
      ADD_STAT(oracleMatchCount, statistics::units::Count::get(),
               "number of Oracle future matches"),
      ADD_STAT(oracleReplayCount, statistics::units::Count::get(),
               "number of Oracle replay or duplicate observations"),
      ADD_STAT(oracleAdvanceFrontierCount, statistics::units::Count::get(),
               "number of Oracle frontier advances"),
      ADD_STAT(oracleWindowMissCount, statistics::units::Count::get(),
               "number of Oracle window misses"),
      ADD_STAT(oracleDeactivateCount, statistics::units::Count::get(),
               "number of Oracle stream deactivations"),
      ADD_STAT(oracleL2IssueAttemptCount, statistics::units::Count::get(),
               "number of Oracle L2-lane issue attempts"),
      ADD_STAT(oracleL2IssueSentCount, statistics::units::Count::get(),
               "number of Oracle L2-lane requests sent"),
      ADD_STAT(oracleL2IssueSuppressedCount, statistics::units::Count::get(),
               "number of Oracle L2-lane duplicate suppressions"),
      ADD_STAT(oracleL1ArmCount, statistics::units::Count::get(),
               "number of times the Oracle L1 lead lane is armed"),
      ADD_STAT(oracleL1IssueAttemptCount, statistics::units::Count::get(),
               "number of Oracle L1-lane issue attempts"),
      ADD_STAT(oracleL1IssueSentCount, statistics::units::Count::get(),
               "number of Oracle L1-lane requests sent"),
      ADD_STAT(oracleL1IssueSuppressedCount, statistics::units::Count::get(),
               "number of Oracle L1-lane duplicate suppressions"),
      ADD_STAT(oracleL1IssuePriorL2Count, statistics::units::Count::get(),
               "number of Oracle L1 requests whose target already had L2 coverage"),
      ADD_STAT(oracleL1IssueNoPriorL2Count, statistics::units::Count::get(),
               "number of Oracle L1 requests issued without prior tracked L2 coverage"),
      ADD_STAT(oracleFirstTouchL1HitWithLeadCount, statistics::units::Count::get(),
               "number of first-touch Oracle-demand L1 hits with tracked L1 lead"),
      ADD_STAT(oracleFirstTouchL1HitWithoutLeadCount, statistics::units::Count::get(),
               "number of first-touch Oracle-demand L1 hits without tracked L1 lead"),
      ADD_STAT(oracleFirstTouchMissWithLeadLowerCoveredCount,
               statistics::units::Count::get(),
               "number of first-touch Oracle-demand misses with L1 lead and Oracle lower-level coverage"),
      ADD_STAT(oracleFirstTouchMissWithLeadLowerUncoveredCount,
               statistics::units::Count::get(),
               "number of first-touch Oracle-demand misses with L1 lead but no Oracle lower-level coverage"),
      ADD_STAT(oracleFirstTouchMissNoLeadLowerCoveredCount,
               statistics::units::Count::get(),
               "number of first-touch Oracle-demand misses without L1 lead but with Oracle lower-level coverage"),
      ADD_STAT(oracleFirstTouchMissNoLeadLowerUncoveredCount,
               statistics::units::Count::get(),
               "number of first-touch Oracle-demand misses without Oracle L1 or lower-level coverage"),
      ADD_STAT(oracleEvictBeforeUseL1Count, statistics::units::Count::get(),
               "number of Oracle L1-issued lines evicted before first use")
{
}

}

}
