/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "mem/cache/replacement_policies/pa_maip_rp.hh"

#include <algorithm>
#include <cassert>

#include "base/intmath.hh"
#include "base/logging.hh"

namespace gem5
{
namespace replacement_policy
{

namespace
{

bool
isPrefetch(const PacketPtr pkt)
{
    return pkt && (pkt->req->isPrefetch() || pkt->cmd.isPrefetch());
}

unsigned
checkedEntries(const AMPMHintTableParams &p)
{
    fatal_if(!p.assoc || !p.entries || p.entries % p.assoc ||
             !isPowerOf2(p.zone_size) || !isPowerOf2(p.block_size) ||
             p.zone_size < p.block_size,
             "AMPM needs nonempty maps, divisible associativity and "
             "power-of-two zone/line sizes");
    return p.entries;
}

} // anonymous namespace

AMPMHintTable::AMPMHintTable(const Params &p)
  : SimObject(p), maps(checkedEntries(p), p.assoc, p.zone_size, p.block_size),
    stats(this)
{
}

bool
AMPMHintTable::getHint(const PacketPtr pkt, pa_maip::State &state) const
{
    uint8_t hint;
    if (!pkt->req->getInsertionHint(this, hint)) {
        return false;
    }
    state = static_cast<pa_maip::State>(hint);
    return true;
}

void
AMPMHintTable::observe(const PacketPtr pkt)
{
    if (pkt->req->isUncacheable() || pkt->req->isCacheMaintenance() ||
        pkt->isEviction() || pkt->isWriteback()) {
        return;
    }
    const bool prefetch = isPrefetch(pkt);
    if (!prefetch && !pkt->isDemand() &&
        !(pkt->isResponse() && pkt->isRead())) {
        return;
    }
    pa_maip::State state;
    if (getHint(pkt, state)) {
        return;
    }
    state = maps.access(pkt->getAddr(), pkt->isSecure(), prefetch);
    pkt->req->setInsertionHint(this, state);
    (prefetch ? stats.prefetchStates : stats.demandStates)[state]++;
}

PAMAIP::PAMAIP(const Params &p)
  : Base(p), hints(p.hints), lowInsertPeriod(p.low_insert_period),
    suppressFirstHit(p.suppress_first_hit), stats(this)
{
    fatal_if(!hints || lowInsertPeriod == 0,
             "PA-MAIP requires access maps and a nonzero insertion period");
}

void
PAMAIP::notifyAccess(const PacketPtr pkt)
{
    hints->observe(pkt);
}

void
PAMAIP::notifyDataUse(const std::shared_ptr<ReplacementData>& replacement_data,
                     const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<PAMAIPReplData>(replacement_data);
    if (pkt->isDemand() && !isPrefetch(pkt) && data->consumeDemand()) {
        stats.fillConsumptions++;
    }
}

void
PAMAIP::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    auto data = std::static_pointer_cast<PAMAIPReplData>(replacement_data);
    data->valid = false;
    data->unusedPrefetch = false;
    data->rrpv = 3;
}

void
PAMAIP::touch(const std::shared_ptr<ReplacementData>& replacement_data,
              const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<PAMAIPReplData>(replacement_data);
    const bool prefetch = isPrefetch(pkt);
    // Cache maintenance and writebacks are not demand consumption events.
    if ((!prefetch && !pkt->isDemand()) || pkt->req->isCacheMaintenance()) {
        return;
    }
    if (prefetch) {
        stats.prefetchHits++;
    } else if (data->unusedPrefetch) {
        stats.firstUses++;
        if (suppressFirstHit) {
            stats.suppressedFirstUses++;
        }
    }
    if (data->touch(prefetch, suppressFirstHit)) {
        stats.promotions++;
    }
}

void
PAMAIP::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<PAMAIPReplData>(replacement_data);
    data->unusedPrefetch = false;
    data->rrpv = 0;
}

void
PAMAIP::reset(const std::shared_ptr<ReplacementData>& replacement_data,
              const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<PAMAIPReplData>(replacement_data);
    const bool prefetch = isPrefetch(pkt);
    pa_maip::State state;
    bool has_hint = hints->getHint(pkt, state);
    if (!has_hint) {
        hints->observe(pkt);
        has_hint = hints->getHint(pkt, state);
        if (!has_hint) {
            stats.missingHints++;
        }
    }
    // Writeback allocations have no demand instruction/map hint. Retain the
    // ordinary SRRIP insertion level rather than claiming inferred reuse.
    bool high = !has_hint || pa_maip::highPriority(state);
    if (!high && ++lowCounter == lowInsertPeriod) {
        high = true;
        lowCounter = 0;
    }
    data->rrpv = high ? 2 : 3;
    data->valid = true;
    data->unusedPrefetch = prefetch;
    stats.insertions[data->rrpv]++;
    if (prefetch) {
        stats.prefetchFills++;
    }
}

void
PAMAIP::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<PAMAIPReplData>(replacement_data);
    data->rrpv = 2;
    data->valid = true;
    data->unusedPrefetch = false;
}

ReplaceableEntry *
PAMAIP::getVictim(const ReplacementCandidates& candidates) const
{
    assert(!candidates.empty());
    ReplaceableEntry *victim = candidates.front();
    uint8_t maximum = 0;
    for (const auto candidate : candidates) {
        auto data = std::static_pointer_cast<PAMAIPReplData>(
            candidate->replacementData);
        if (!data->valid) {
            return candidate;
        }
        if (data->rrpv > maximum) {
            maximum = data->rrpv;
            victim = candidate;
        }
    }
    const unsigned increment = 3 - maximum;
    for (const auto candidate : candidates) {
        auto data = std::static_pointer_cast<PAMAIPReplData>(
            candidate->replacementData);
        data->rrpv += increment;
    }
    return victim;
}

std::shared_ptr<ReplacementData>
PAMAIP::instantiateEntry()
{
    return std::make_shared<PAMAIPReplData>();
}

AMPMHintTable::HintStats::HintStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(demandStates, statistics::units::Count::get(),
             "Demand requests by pre-access map state"),
    ADD_STAT(prefetchStates, statistics::units::Count::get(),
             "Prefetch requests by pre-access map state")
{
    demandStates.init(4);
    prefetchStates.init(4);
    for (const auto vector : {&demandStates, &prefetchStates}) {
        vector->subname(pa_maip::Init, "init");
        vector->subname(pa_maip::Prefetched, "prefetched");
        vector->subname(pa_maip::Accessed, "accessed");
        vector->subname(pa_maip::Reused, "reused");
    }
}

PAMAIP::PolicyStats::PolicyStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(insertions, statistics::units::Count::get(),
             "Fills by insertion RRPV"),
    ADD_STAT(prefetchFills, statistics::units::Count::get(), "Prefetch fills"),
    ADD_STAT(firstUses, statistics::units::Count::get(),
             "First demand tag hits on a prefetched line"),
    ADD_STAT(suppressedFirstUses, statistics::units::Count::get(),
             "First demand tag hits retaining their insertion priority"),
    ADD_STAT(prefetchHits, statistics::units::Count::get(),
             "Prefetch hits that do not promote the line"),
    ADD_STAT(promotions, statistics::units::Count::get(), "Demand promotions"),
    ADD_STAT(fillConsumptions, statistics::units::Count::get(),
             "Prefetch first uses satisfied directly by a fill"),
    ADD_STAT(missingHints, statistics::units::Count::get(),
             "Fills lacking demand/prefetch hints, including writebacks")
{
    insertions.init(4);
}

} // namespace replacement_policy
} // namespace gem5
