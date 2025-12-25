/**
 * Copyright (c) 2024 XiangShan
 * All rights reserved.
 *
 * Unified implementation of XiangShan RRIP replacement policy series
 */

#include "mem/cache/replacement_policies/xs_drrip_rp.hh"

#include <cassert>
#include <memory>

#include "base/logging.hh"
#include "params/XSDRRIPRP.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

XSDRRIP::XSDRRIP(const Params &p)
  : Base(p),
    mode(static_cast<Mode>(p.mode)),
    numSets(p.num_sets),
    numWays(p.num_ways),
    pselCounter(PSEL_WIDTH, PSEL_THRESHOLD),
    entryCount(0),
    duelingStats(this)
{
    // DRRIP mode requires Set Dueling parameters
    if (mode == DRRIP_MODE) {
        fatal_if(numSets < 4, "DRRIP mode requires at least 4 sets for proper set dueling");

        setBits = floorLog2(numSets);
        halfSetBits = setBits - setBits / 2 - 1;
        setMask = (1 << (setBits - setBits / 2)) - 1;
    }
}

bool
XSDRRIP::useBRRIP(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    std::shared_ptr<XSDRRIPReplData> casted_data =
        std::static_pointer_cast<XSDRRIPReplData>(replacement_data);

    // SRRIP mode or SRRIP dedicated set → use SRRIP logic
    if (mode == SRRIP_MODE || casted_data->setType == 0) {
        return false;
    }

    // BRRIP mode or BRRIP dedicated set → use BRRIP logic
    if (mode == BRRIP_MODE || casted_data->setType == 1) {
        return true;
    }

    // Follower set in DRRIP mode → decide based on PSEL counter value
    // When PSEL >= threshold, BRRIP performs better; otherwise SRRIP performs better
    bool use_brrip = (pselCounter >= PSEL_THRESHOLD);
    if (use_brrip) {
        duelingStats.followerUseBrrip++;
    } else {
        duelingStats.followerUseSrrip++;
    }
    return use_brrip;
}

uint8_t
XSDRRIP::getRRPV(const PacketPtr pkt,
                 const std::shared_ptr<ReplacementData>& replacement_data,
                 bool use_brrip) const
{
    // Extract access characteristics from packet
    // These correspond to the 4-bit req_type encoding in XiangShan RTL
    bool is_reuse = false;
    bool is_release = false;
    bool is_prefetch = false;
    bool is_refill = false;

    if (pkt) {
        std::shared_ptr<XSDRRIPReplData> casted_replacement_data =
            std::static_pointer_cast<XSDRRIPReplData>(replacement_data);

        // Reuse: block has been accessed before (originBit=1)
        is_reuse = casted_replacement_data->originBit;

        // Release: write or eviction operation
        is_release = pkt->isWrite() || pkt->isEviction();

        // Prefetch: request from prefetcher
        is_prefetch = pkt->req->isPrefetch();

        // Refill: newly inserted block (originBit=0)
        // NOTE: originBit=0 indicates this is a refill operation
        is_refill = !casted_replacement_data->originBit;
    }

    bool is_non_prefetch = !is_prefetch;

    // Determine RRPV based on access pattern
    // Case 1: non-prefetch hit OR non-prefetch refill OR non-prefetch release reuse → RRPV=0
    // These are high-priority accesses, same for both SRRIP and BRRIP
    if ((is_non_prefetch && is_reuse) ||
        (is_non_prefetch && is_refill && !is_release) ||
        (is_non_prefetch && is_release && is_reuse)) {
        return 0;
    }

    // Case 2: prefetch refill → RRPV=1
    // Prefetched data gets medium-high priority, same for both SRRIP and BRRIP
    if (!is_non_prefetch && is_refill) {
        return 1;
    }

    // Case 3: non-prefetch release first-use OR prefetch release
    // The only difference between SRRIP and BRRIP:
    // - SRRIP returns 2 (medium priority, "give it a chance")
    // - BRRIP returns 3 (low priority, more pessimistic)
    if ((is_non_prefetch && is_release && !is_reuse) ||
        (!is_non_prefetch && is_release)) {
        return use_brrip ? 3 : 2;
    }

    // Default case: other access patterns
    // SRRIP returns 2, BRRIP returns 3
    return use_brrip ? 3 : 2;
}

void
XSDRRIP::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    std::shared_ptr<XSDRRIPReplData> casted_replacement_data =
        std::static_pointer_cast<XSDRRIPReplData>(replacement_data);

    casted_replacement_data->valid = false;
    casted_replacement_data->originBit = false;
    casted_replacement_data->rrpv.saturate();
}

void
XSDRRIP::touch(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt)
{
    std::shared_ptr<XSDRRIPReplData> casted_replacement_data =
        std::static_pointer_cast<XSDRRIPReplData>(replacement_data);

    // Decide which logic to use based on mode and pre-computed set type
    bool use_brrip = useBRRIP(replacement_data);

    // Determine RRPV based on access characteristics and mode
    casted_replacement_data->rrpv = SatCounter8(2, getRRPV(pkt, replacement_data, use_brrip));

    // Update originBit: mark this block as accessed
    casted_replacement_data->originBit = true;
}

void
XSDRRIP::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    std::shared_ptr<XSDRRIPReplData> casted_replacement_data =
        std::static_pointer_cast<XSDRRIPReplData>(replacement_data);

    // No packet info, set RRPV=0
    casted_replacement_data->rrpv.reset();
}

void
XSDRRIP::reset(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt)
{
    std::shared_ptr<XSDRRIPReplData> casted_replacement_data =
        std::static_pointer_cast<XSDRRIPReplData>(replacement_data);

    // DRRIP mode: update PSEL counter (only in leader sets)
    if (mode == DRRIP_MODE) {
        unsigned set_type = casted_replacement_data->setType;

        if (set_type == 0) {
            // SRRIP region miss → PSEL++ (favor BRRIP)
            pselCounter++;
            duelingStats.srripMisses++;
        } else if (set_type == 1) {
            // BRRIP region miss → PSEL-- (favor SRRIP)
            pselCounter--;
            duelingStats.brripMisses++;
        }

        // Update PSEL statistics value
        duelingStats.pselValue = pselCounter;
    }

    // Initialize originBit=0 (indicates newly inserted block, first-use)
    casted_replacement_data->originBit = false;

    // Decide which logic to use based on mode and pre-computed set type
    bool use_brrip = useBRRIP(replacement_data);

    // Determine RRPV based on access characteristics and mode
    casted_replacement_data->rrpv = SatCounter8(2, getRRPV(pkt, replacement_data, use_brrip));

    casted_replacement_data->valid = true;
}

void
XSDRRIP::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    std::shared_ptr<XSDRRIPReplData> casted_replacement_data =
        std::static_pointer_cast<XSDRRIPReplData>(replacement_data);

    // No packet info, use default RRPV
    // Depends on mode: SRRIP=2, BRRIP=3
    bool use_brrip = useBRRIP(replacement_data);

    casted_replacement_data->rrpv = SatCounter8(2, use_brrip ? 3 : 2);
    casted_replacement_data->valid = true;
}

ReplaceableEntry*
XSDRRIP::getVictim(const ReplacementCandidates& candidates) const
{
    assert(candidates.size() > 0);

    // Find entry with maximum RRPV
    ReplaceableEntry* victim = candidates[0];
    std::shared_ptr<XSDRRIPReplData> victim_repl_data =
        std::static_pointer_cast<XSDRRIPReplData>(
            victim->replacementData);

    for (const auto& candidate : candidates) {
        std::shared_ptr<XSDRRIPReplData> candidate_repl_data =
            std::static_pointer_cast<XSDRRIPReplData>(
                candidate->replacementData);

        // Prioritize selecting invalid entry
        if (!candidate_repl_data->valid) {
            return candidate;
        }

        // Update victim to entry with larger RRPV
        if (candidate_repl_data->rrpv > victim_repl_data->rrpv) {
            victim = candidate;
            victim_repl_data = candidate_repl_data;
        }
    }

    // Calculate aging increment
    int victim_rrpv = victim_repl_data->rrpv;
    int increcement = MAX_RRPV - victim_rrpv;

    // Perform aging
    if (increcement > 0) {
        for (const auto& candidate : candidates) {
            std::shared_ptr<XSDRRIPReplData> candidate_repl_data =
                std::static_pointer_cast<XSDRRIPReplData>(
                    candidate->replacementData);

            if (candidate_repl_data->valid) {
                candidate_repl_data->rrpv += increcement;
            }
        }
    }

    return victim;
}

std::shared_ptr<ReplacementData>
XSDRRIP::instantiateEntry()
{
    XSDRRIPReplData* repl_data = new XSDRRIPReplData();

    // Allocate set ID
    // entryCount / numWays = set index
    repl_data->setId = entryCount / numWays;
    entryCount++;

    // Pre-compute set type for DRRIP mode (optimization: compute once, use many times)
    // This avoids repeated bit-pattern matching in the hot path (touch/reset)
    if (mode == DRRIP_MODE) {
        unsigned set_id = repl_data->setId;

        bool match_srrip = (set_id >> halfSetBits) == (set_id & setMask);
        bool match_brrip = (set_id >> halfSetBits) == ((~set_id) & setMask);

        // Determine set type using bit-pattern matching
        if (match_srrip) {
            repl_data->setType = 0;  // SRRIP dedicated
        } else if (match_brrip) {
            repl_data->setType = 1;  // BRRIP dedicated
        } else {
            repl_data->setType = 2;  // Follower
        }
    }

    return std::shared_ptr<ReplacementData>(repl_data);
}

XSDRRIP::XSDRRIPStats::XSDRRIPStats(statistics::Group* parent)
  : statistics::Group(parent),
    ADD_STAT(srripMisses, "Number of misses in SRRIP leader sets"),
    ADD_STAT(brripMisses, "Number of misses in BRRIP leader sets"),
    ADD_STAT(followerUseSrrip, "Number of times follower sets used SRRIP"),
    ADD_STAT(followerUseBrrip, "Number of times follower sets used BRRIP"),
    ADD_STAT(pselValue, "Current PSEL counter value")
{
}

} // namespace replacement_policy
} // namespace gem5
