#include "mem/cache/prefetch/prefetch_filter.hh"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <iterator>
#include <string>

#include "base/stats/group.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5 {
namespace prefetch {

PrefetchFilter::Stats::Stats(statistics::Group *parent, const std::string &name)
    : statistics::Group(parent, name.c_str()),
      ADD_STAT(insertCount, statistics::units::Count::get(), "PrefetchFilter insert count"),
      ADD_STAT(queryHitCount, statistics::units::Count::get(), "PrefetchFilter query hit count"),
    ADD_STAT(prefetchIssued, statistics::units::Count::get(), "Prefetches issued by PrefetchFilter"),
    ADD_STAT(replacementCount, statistics::units::Count::get(), "PrefetchFilter replacement count"),
    ADD_STAT(l1Calls, statistics::units::Count::get(), "GetPFAddrL1 calls"),
    ADD_STAT(l1Issued, statistics::units::Count::get(), "GetPFAddrL1 issued"),
    ADD_STAT(l2Calls, statistics::units::Count::get(), "GetPFAddrL2 calls"),
    ADD_STAT(l2Issued, statistics::units::Count::get(), "GetPFAddrL2 issued"),
    ADD_STAT(l3Calls, statistics::units::Count::get(), "GetPFAddrL3 calls"),
    ADD_STAT(l3Issued, statistics::units::Count::get(), "GetPFAddrL3 issued"),
    ADD_STAT(hashcollisionCount, statistics::units::Count::get(), "PrefetchFilter hash collision count"),
    ADD_STAT(contextAliasCount, statistics::units::Count::get(),
             "same virtual regions retained for different ContextIDs"),
    ADD_STAT(emptyReclaims, statistics::units::Count::get(),
             "empty entries reclaimed after their last pending block"),
    ADD_STAT(pendingEvictedBlocks, statistics::units::Count::get(),
             "pending blocks discarded when a valid entry is replaced")
{

}

PrefetchFilter::PrefetchFilter(gem5::BaseIndexingPolicy *idx_policy, 
                               gem5::replacement_policy::Base *rpl_policy,
                   unsigned entries, unsigned region_size,
                   unsigned blk_size, statistics::Group *parent,
                   unsigned vaddr_hash_width,
                   PrefetchSourceType pf_source_type,
                   const std::string &name,
                   bool strict_region_match,
                   bool reclaim_empty_entries)
        : table(entries, entries, idx_policy,rpl_policy, Entry()),
        regionSize(region_size),
        blkSize(blk_size),
        regionBlks(region_size / blk_size),
        rrIndex(0),
        REGION_ADDR_RAW_WIDTH(6),//align with rtl
        vaddrHashWidth(vaddr_hash_width),
        strictRegionMatch(strict_region_match),
        reclaimEmptyEntries(reclaim_empty_entries),
        stats(parent, name),
        pfSourceType(pf_source_type),
        table_name(name)
{
}

PrefetchFilter::~PrefetchFilter() = default;

// PrefetchFilter::Entry*
// PrefetchFilter::findByVaddr(Addr vaddr, bool is_secure)
// {
//     Addr region = vaddr / regionSize;
//     Entry *e = table.findEntry(region, is_secure);
//     if (e) {
//         stats.queryHitCount++;
//     }
//     return e;
// }

// PrefetchFilter::Entry*
// PrefetchFilter::findByRegion(Addr region, bool is_secure)
// {
//     Entry *e = table.findEntry(region, is_secure);
//     if (e) {
//         stats.queryHitCount++;
//     }
//     return e;
// }

// PrefetchFilter::Entry*
// PrefetchFilter::allocateForVaddr(Addr vaddr, bool is_secure, Addr region_addr)
// {
//     Addr region = vaddr / regionSize;
//     Entry *victim = table.findVictim(region);

//     victim->region_addr = region_addr ? region_addr : region;
//     victim->region_bits = 0;
//     victim->filter_bits = 0;
//     victim->alias_bits = aliasFromVaddr(vaddr);
//     victim->paddr_valid = (region_addr != 0);
//     victim->decr_mode = false;
//     victim->_setSecure(is_secure);

//     table.insertEntry(region, is_secure, victim);
//     return victim;
// }

bool
PrefetchFilter::GetPFAddrL1(std::vector<AddrPriority> &addresses)
{
    return getPFAddr(1, addresses);
}

bool
PrefetchFilter::GetPFAddrL2(std::vector<AddrPriority> &addresses)
{
    return getPFAddr(2, addresses);
}

bool
PrefetchFilter::GetPFAddrL3(std::vector<AddrPriority> &addresses)
{
    return getPFAddr(3, addresses);
}

bool
PrefetchFilter::PeekPFAddrL1(PendingRequest &request)
{
    return selectNext(1, request);
}

bool
PrefetchFilter::PeekPFAddrL2(PendingRequest &request)
{
    return selectNext(2, request);
}

bool
PrefetchFilter::PeekPFAddrL3(PendingRequest &request)
{
    return selectNext(3, request);
}

bool
PrefetchFilter::selectNext(uint64_t level, PendingRequest &request)
{
    switch (level) {
      case 1:
        stats.l1Calls++;
        break;
      case 2:
        stats.l2Calls++;
        break;
      case 3:
        stats.l3Calls++;
        break;
      default:
        panic("invalid prefetch-buffer target level: %lu", level);
    }

    auto it_begin = table.begin();
    const auto n = std::distance(it_begin, table.end());
    if (n == 0) {
        return false;
    }

    const uint64_t mask = regionBlks >= 64 ? ~uint64_t(0) :
        ((uint64_t(1) << regionBlks) - 1);
    for (unsigned i = 0; i < static_cast<unsigned>(n); ++i) {
        const unsigned idx = (rrIndex + i) % static_cast<unsigned>(n);
        auto it = it_begin;
        std::advance(it, idx);
        Entry *entry = &(*it);
        if (!entry->isValid() || !entry->paddr_valid ||
            entry->PFlevel != level) {
            continue;
        }

        const uint64_t pending = entry->region_bits & ~entry->filter_bits &
            mask;
        if (pending == 0) {
            continue;
        }

        const unsigned offset = entry->decr_mode ?
            63 - __builtin_clzll(pending) : __builtin_ctzll(pending);
        request.address = entry->region_addr * regionSize + offset * blkSize;
        request.region = entry->region_addr;
        request.offset = offset;
        request.priority = static_cast<int>(regionBlks) -
            static_cast<int>(offset);
        request.level = level;
        request.contextId = entry->contextId;
        request.secure = entry->isSecure();
        request.tableIndex = idx;
        request.trigger = TriggerInfo();
        if (offset < entry->bitTriggers.size() &&
            entry->bitTriggers[offset]) {
            request.trigger = *entry->bitTriggers[offset];
        }
        return true;
    }

    return false;
}

PrefetchFilter::Entry *
PrefetchFilter::findExactEntry(Addr region, ContextID context_id, bool secure,
                               uint64_t level)
{
    const Addr tag = contextKey(regionHashTag(region), context_id);
    for (Entry *entry : table.getPossibleEntries(tag)) {
        if (entry->isValid() && entry->isSecure() == secure &&
            entry->region_addr == region && entry->contextId == context_id &&
            entry->PFlevel == level) {
            return entry;
        }
    }
    return nullptr;
}

bool
PrefetchFilter::commit(const PendingRequest &request, bool *entry_empty)
{
    return consume(request, true, entry_empty);
}

bool
PrefetchFilter::discard(const PendingRequest &request, bool *entry_empty)
{
    return consume(request, false, entry_empty);
}

bool
PrefetchFilter::consume(const PendingRequest &request, bool count_as_issued,
                        bool *entry_empty)
{
    if (entry_empty) {
        *entry_empty = false;
    }

    Entry *entry = findExactEntry(request.region, request.contextId,
                                  request.secure, request.level);
    const uint64_t bit = uint64_t(1) << request.offset;
    if (entry == nullptr || (pendingBlocks(entry) & bit) == 0) {
        return false;
    }

    if (entry_empty) {
        *entry_empty = pendingBlocks(entry) == bit;
    }
    if (request.offset < entry->bitTriggers.size()) {
        entry->bitTriggers[request.offset].reset();
    }
    markBlockSent(entry, request.offset);
    if (count_as_issued) {
        stats.prefetchIssued++;
        switch (request.level) {
          case 1:
            stats.l1Issued++;
            break;
          case 2:
            stats.l2Issued++;
            break;
          case 3:
            stats.l3Issued++;
            break;
          default:
            panic("invalid prefetch-buffer target level: %lu", request.level);
        }
    }

    const auto n = std::distance(table.begin(), table.end());
    if (n != 0) {
        rrIndex = (request.tableIndex + 1) % static_cast<unsigned>(n);
    }
    return true;
}

bool
PrefetchFilter::getPFAddr(uint64_t level,
                           std::vector<AddrPriority> &addresses)
{
    PendingRequest request;
    if (!selectNext(level, request)) {
        return false;
    }

    if (!commit(request)) {
        return false;
    }

    const bool has_trigger = request.trigger.pfi_old != nullptr;
    const PrefetchSourceType source = has_trigger &&
            request.trigger.pfSourceType != PrefetchSourceType::PF_NONE ?
        request.trigger.pfSourceType : pfSourceType;
    if (has_trigger) {
        addresses.emplace_back(request.address, request.priority, source,
                               request.trigger);
    } else {
        addresses.emplace_back(request.address, request.priority, source);
    }
    if (level > 1) {
        addresses.back().pfahead_host = level;
        addresses.back().pfahead = true;
    }
    DPRINTF(HWPrefetch, "GetPFAddrL%lu issued addr=%#lx prio=%d trigger=%d\n",
            level, request.address, request.priority, has_trigger);
    return true;
}

void
PrefetchFilter::markBlockSent(PrefetchFilter::Entry *e, unsigned blk_idx)
{
    if (!e || blk_idx >= regionBlks)
        return;
    e->filter_bits |= (uint64_t(1) << blk_idx);
    if (reclaimEmptyEntries && pendingBlocks(e) == 0) {
        for (auto &trigger : e->bitTriggers) {
            trigger.reset();
        }
        table.invalidate(e);
        stats.emptyReclaims++;
    } else {
        table.accessEntry(e);
    }
}

void
PrefetchFilter::addRegionBits(PrefetchFilter::Entry *e, uint64_t bits)
{
    if (!e)
        return;
    e->region_bits |= bits;
    table.accessEntry(e);
}

void
PrefetchFilter::ensureTriggerStorage(PrefetchFilter::Entry &e)
{
    if (e.bitTriggers.size() != regionBlks) {
        e.bitTriggers.clear();
        e.bitTriggers.resize(regionBlks);
    }
}

void
PrefetchFilter::ensureStagedStorage(PrefetchFilter::Entry &e)
{
    ensureTriggerStorage(e);
    if (e.candidateIds.size() != regionBlks) {
        e.candidateIds.assign(regionBlks, 0);
    }
    if (e.decisionIds.size() != regionBlks) {
        e.decisionIds.assign(regionBlks, 0);
    }
}

void
PrefetchFilter::storeTriggersForBits(PrefetchFilter::Entry &e, uint64_t bits,
                                     const TriggerInfo *trigger)
{
    if (!trigger || regionBlks == 0 || bits == 0)
        return;

    ensureTriggerStorage(e);

    uint64_t remaining = bits;
    unsigned limit = (regionBlks > 64) ? 64 : regionBlks;
    for (unsigned idx = 0; idx < limit && remaining; ++idx) {
        if (remaining & uint64_t(1)) {
            PacketPtr pkt = trigger->pkt;
            e.bitTriggers[idx] = std::make_unique<TriggerInfo>(*trigger);
        }
        remaining >>= 1;
    }
}

PrefetchFilter::Entry*
PrefetchFilter::Insert(Addr region_addr, uint64_t region_bits, uint8_t alias_bits,
                       bool paddr_valid, bool decr_mode, 
                       bool is_secure, uint64_t PFlevel,
                       const TriggerInfo *trigger)
{
    stats.insertCount++;
    ContextID context_id = InvalidContextID;
    if (trigger && trigger->pfi_old &&
        trigger->pfi_old->hasContextId()) {
        context_id = trigger->pfi_old->contextId();
    }

    for (const auto &entry : table) {
        if (entry.isValid() && entry.region_addr == region_addr &&
            entry.contextId != context_id) {
            stats.contextAliasCount++;
            break;
        }
    }

    Addr tag = contextKey(regionHashTag(region_addr), context_id);
    Entry *e = table.findEntry(tag, is_secure);
    DPRINTF(HWPrefetch,
            "Insert called: region=%#lx tag=%#lx ctx=%d bits=%#lx "
            "level=%lu,name=%s\n",
            region_addr, tag, context_id, region_bits, PFlevel,
            table_name.c_str());
    if (strictRegionMatch) {
        const bool hashed_collision = e &&
            (e->region_addr != region_addr || e->contextId != context_id);
        Entry *exact_match = nullptr;
        for (Entry *candidate : table.getPossibleEntries(tag)) {
            if (candidate->isValid() && candidate->isSecure() == is_secure &&
                candidate->region_addr == region_addr &&
                candidate->contextId == context_id) {
                exact_match = candidate;
                break;
            }
        }
        if (hashed_collision) {
            DPRINTF(HWPrefetch,
                    "PrefetchFilter hash collision: existing=(%#lx,ctx=%d) "
                    "new=(%#lx,ctx=%d)\n",
                    e->region_addr, e->contextId, region_addr, context_id);
            stats.hashcollisionCount++;
        }
        e = exact_match;
    } else if (e && (e->region_addr != region_addr ||
                     e->contextId != context_id)) {
        DPRINTF(HWPrefetch,
                "Warning: PrefetchFilter tag collision. "
                "existing=(%#lx,ctx=%d) new=(%#lx,ctx=%d)\n",
                e->region_addr, e->contextId, region_addr, context_id);
        stats.hashcollisionCount++;
        e = nullptr;
    }
    if (e) {
        storeTriggersForBits(*e, region_bits, trigger);
        e->region_bits |= region_bits;
        table.accessEntry(e);
        stats.queryHitCount++;
        DPRINTF(HWPrefetch, "Insert hit: region=%#lx tag=%#lx bits=%#lx level=%lu\n",
                region_addr, tag, region_bits, PFlevel);
        //print all entry status
        for (const auto &entry : table) {
            DPRINTF(HWPrefetch, "  Entry: region=%#lx tag=%#lx bits=%#lx filter=%#lx level=%lu valid=%d\n",
                    entry.region_addr, entry.getTag(), entry.region_bits,
                    entry.filter_bits, entry.PFlevel, entry.isValid());
        }
        return e;
    }
    stats.replacementCount++;
    bool victim_valid = false;
    Entry *victim = table.findVictim(tag, nullptr, &victim_valid);
    if (victim_valid) {
        stats.pendingEvictedBlocks += __builtin_popcountll(
            pendingBlocks(victim));
    }
    victim->region_addr = region_addr;
    victim->region_bits = region_bits;
    victim->filter_bits = 0;
    victim->alias_bits = alias_bits;
    victim->paddr_valid = true;
    victim->decr_mode = decr_mode;
    victim->_setSecure(is_secure);
    victim->PFlevel = PFlevel;
    victim->contextId = context_id;
    ensureTriggerStorage(*victim);
    for (auto &slot : victim->bitTriggers) {
        slot.reset();
    }
    storeTriggersForBits(*victim, region_bits, trigger);

    table.insertEntry(tag, is_secure, victim);
    DPRINTF(HWPrefetch, "Insert miss: region=%#lx tag=%#lx bits=%#lx level=%lu\n",
            region_addr, tag, region_bits, PFlevel);
    //print all entry status
    for (const auto &entry : table) {
        DPRINTF(HWPrefetch, "  Entry: region=%#lx tag=%#lx bits=%#lx filter=%#lx level=%lu valid=%d\n",
                entry.region_addr, entry.getTag(), entry.region_bits,
                entry.filter_bits, entry.PFlevel, entry.isValid());
        
    }
    return victim;
}

uint64_t
PrefetchFilter::pendingBlocks(PrefetchFilter::Entry *e) const
{
    if (!e) {
        return 0;
    }

    const uint64_t mask = regionBlks >= 64 ? ~uint64_t(0) :
        ((uint64_t(1) << regionBlks) - 1);
    return e->region_bits & ~e->filter_bits & mask;
}

uint64_t
PrefetchFilter::selectableBlocks(const Entry *entry) const
{
    if (entry == nullptr) {
        return 0;
    }

    const uint64_t mask = regionBlks >= 64 ? ~uint64_t(0) :
        ((uint64_t(1) << regionBlks) - 1);
    return entry->region_bits & ~entry->filter_bits &
        ~entry->inFlightBits & mask;
}

bool
PrefetchFilter::tokenMatches(const Entry &entry,
                             const StagedPrefetchToken &token) const
{
    if (token.offset >= regionBlks || token.entryGeneration == 0 ||
        token.candidateId == 0 || entry.entryGeneration !=
            token.entryGeneration || entry.candidateIds.size() != regionBlks) {
        return false;
    }

    const uint64_t bit = uint64_t(1) << token.offset;
    return (entry.inFlightBits & bit) != 0 &&
        entry.candidateIds[token.offset] == token.candidateId;
}

PrefetchFilter::StagedPrefetchToken
PrefetchFilter::makeToken(const Entry &entry, unsigned offset,
                          bool secure) const
{
    assert(offset < regionBlks);
    assert(offset < entry.candidateIds.size());
    assert(offset < entry.decisionIds.size());

    StagedPrefetchToken token;
    token.region = entry.region_addr;
    token.contextId = entry.contextId;
    token.entryGeneration = entry.entryGeneration;
    token.candidateId = entry.candidateIds[offset];
    token.decisionId = entry.decisionIds[offset];
    token.targetLevel = entry.PFlevel;
    token.offset = offset;
    token.secure = secure;
    return token;
}

void
PrefetchFilter::collectPendingTokens(const Entry &entry, bool secure,
                                     StagedInsertResult &result) const
{
    const uint64_t mask = regionBlks >= 64 ? ~uint64_t(0) :
        ((uint64_t(1) << regionBlks) - 1);
    uint64_t pending = entry.region_bits & ~entry.filter_bits & mask;
    while (pending != 0) {
        const unsigned offset = __builtin_ctzll(pending);
        if (offset < entry.candidateIds.size() &&
            entry.candidateIds[offset] != 0) {
            fatal_if(result.evictedTokenCount >= result.evictedTokens.size(),
                     "staged prefetch eviction exceeds region width");
            result.evictedTokens[result.evictedTokenCount++] =
                makeToken(entry, offset, secure);
        }
        pending &= pending - 1;
    }
}

PrefetchFilter::StagedInsertResult
PrefetchFilter::insertStaged(Addr region_addr, uint64_t region_bits,
                             uint8_t alias_bits, bool paddr_valid,
                             bool decr_mode, bool is_secure,
                             uint64_t pf_level,
                             const TriggerInfo &trigger,
                             uint64_t decision_id)
{
    StagedInsertResult result;
    if (region_bits == 0 || trigger.pfi_old == nullptr || decision_id == 0) {
        return result;
    }

    stats.insertCount++;
    const ContextID context_id = trigger.pfi_old->hasContextId() ?
        trigger.pfi_old->contextId() : InvalidContextID;
    const uint64_t mask = regionBlks >= 64 ? ~uint64_t(0) :
        ((uint64_t(1) << regionBlks) - 1);
    region_bits &= mask;
    if (region_bits == 0) {
        return result;
    }
    const Addr tag = contextKey(regionHashTag(region_addr), context_id);

    Entry *entry = findExactEntry(region_addr, context_id, is_secure,
                                  pf_level);
    if (entry != nullptr) {
        result.newBits = region_bits & ~entry->region_bits;
        if (result.newBits == 0) {
            return result;
        }

        ensureStagedStorage(*entry);
        entry->region_bits |= result.newBits;
        storeTriggersForBits(*entry, result.newBits, &trigger);
        uint64_t remaining = result.newBits;
        while (remaining != 0) {
            const unsigned offset = __builtin_ctzll(remaining);
            entry->candidateIds[offset] = ++nextCandidateId;
            entry->decisionIds[offset] = decision_id;
            remaining &= remaining - 1;
        }
        table.accessEntry(entry);
        stats.queryHitCount++;
        return result;
    }

    stats.replacementCount++;
    bool victim_secure = false;
    bool victim_valid = false;
    Entry *victim = table.findVictimEligible(
        tag, [](const Entry &candidate) {
            return !candidate.isValid() || candidate.inFlightBits == 0;
        }, &victim_secure, &victim_valid);
    if (victim == nullptr) {
        return result;
    }

    if (victim_valid) {
        collectPendingTokens(*victim, victim_secure, result);
        stats.pendingEvictedBlocks += result.evictedTokenCount;
    }

    victim->region_addr = region_addr;
    victim->region_bits = region_bits;
    victim->filter_bits = 0;
    victim->alias_bits = alias_bits;
    victim->paddr_valid = paddr_valid;
    victim->decr_mode = decr_mode;
    victim->PFlevel = pf_level;
    victim->contextId = context_id;
    victim->entryGeneration = ++nextEntryGeneration;
    victim->inFlightBits = 0;
    ensureStagedStorage(*victim);
    for (auto &slot : victim->bitTriggers) {
        slot.reset();
    }
    std::fill(victim->candidateIds.begin(), victim->candidateIds.end(), 0);
    std::fill(victim->decisionIds.begin(), victim->decisionIds.end(), 0);
    storeTriggersForBits(*victim, region_bits, &trigger);
    uint64_t remaining = region_bits;
    while (remaining != 0) {
        const unsigned offset = __builtin_ctzll(remaining);
        victim->candidateIds[offset] = ++nextCandidateId;
        victim->decisionIds[offset] = decision_id;
        remaining &= remaining - 1;
    }
    table.insertEntry(tag, is_secure, victim);
    result.newBits = region_bits;
    return result;
}

std::optional<PrefetchFilter::StagedRequest>
PrefetchFilter::reserveStaged(uint64_t target_level)
{
    switch (target_level) {
      case 1:
        stats.l1Calls++;
        break;
      case 2:
        stats.l2Calls++;
        break;
      case 3:
        stats.l3Calls++;
        break;
      default:
        panic("invalid staged prefetch target level: %lu", target_level);
    }

    auto begin = table.begin();
    const auto n = std::distance(begin, table.end());
    if (n == 0) {
        return std::nullopt;
    }

    for (unsigned i = 0; i < static_cast<unsigned>(n); ++i) {
        const unsigned index = (rrIndex + i) % static_cast<unsigned>(n);
        auto it = begin;
        std::advance(it, index);
        Entry *entry = &(*it);
        if (!entry->isValid() || !entry->paddr_valid ||
            entry->PFlevel != target_level) {
            continue;
        }

        const uint64_t selectable = selectableBlocks(entry);
        if (selectable == 0) {
            continue;
        }

        const unsigned offset = entry->decr_mode ?
            63 - __builtin_clzll(selectable) : __builtin_ctzll(selectable);
        fatal_if(offset >= entry->bitTriggers.size() ||
                     entry->bitTriggers[offset] == nullptr,
                 "staged prefetch buffer entry has no trigger state");
        ensureStagedStorage(*entry);
        const uint64_t bit = uint64_t(1) << offset;
        entry->inFlightBits |= bit;
        table.accessEntry(entry);

        StagedRequest request;
        request.address = entry->region_addr * regionSize + offset * blkSize;
        request.priority = static_cast<int>(regionBlks) -
            static_cast<int>(offset);
        request.trigger = *entry->bitTriggers[offset];
        request.token = makeToken(*entry, offset, entry->isSecure());
        return request;
    }

    return std::nullopt;
}

std::optional<PrefetchFilter::TriggerInfo>
PrefetchFilter::completeStaged(const StagedPrefetchToken &token,
                               bool accepted)
{
    Entry *entry = findExactEntry(token.region, token.contextId, token.secure,
                                  token.targetLevel);
    if (entry == nullptr || !tokenMatches(*entry, token) ||
        token.offset >= entry->bitTriggers.size() ||
        entry->bitTriggers[token.offset] == nullptr) {
        return std::nullopt;
    }

    TriggerInfo trigger = *entry->bitTriggers[token.offset];
    const uint64_t bit = uint64_t(1) << token.offset;
    entry->inFlightBits &= ~bit;
    entry->candidateIds[token.offset] = 0;
    entry->decisionIds[token.offset] = 0;
    entry->bitTriggers[token.offset].reset();
    markBlockSent(entry, token.offset);
    if (accepted) {
        stats.prefetchIssued++;
        switch (token.targetLevel) {
          case 1:
            stats.l1Issued++;
            break;
          case 2:
            stats.l2Issued++;
            break;
          case 3:
            stats.l3Issued++;
            break;
          default:
            panic("invalid staged prefetch target level: %lu",
                  token.targetLevel);
        }
    }

    const auto n = std::distance(table.begin(), table.end());
    if (n != 0) {
        const unsigned index = static_cast<unsigned>(
            entry - &(*table.begin()));
        rrIndex = (index + 1) % static_cast<unsigned>(n);
    }
    return trigger;
}

bool
PrefetchFilter::releaseStaged(const StagedPrefetchToken &token)
{
    Entry *entry = findExactEntry(token.region, token.contextId, token.secure,
                                  token.targetLevel);
    if (entry == nullptr || !tokenMatches(*entry, token)) {
        return false;
    }

    entry->inFlightBits &= ~(uint64_t(1) << token.offset);
    table.accessEntry(entry);
    return true;
}

bool
PrefetchFilter::hasStagedRequests() const
{
    for (const Entry &entry : table) {
        if (entry.isValid() && entry.paddr_valid &&
            selectableBlocks(&entry) != 0) {
            return true;
        }
    }
    return false;
}
    // region-hash-tag implementation per chisel spec
Addr
PrefetchFilter::regionHashTag(Addr vaddr) const
{
    Addr low_mask = ((Addr(1) << REGION_ADDR_RAW_WIDTH) - 1);
    Addr low = vaddr & low_mask;

    unsigned high_low = REGION_ADDR_RAW_WIDTH;
    unsigned high_bits = 3 * vaddrHashWidth;
    Addr high = (vaddr >> high_low) & ((Addr(1) << high_bits) - 1);

    Addr seg0 = high & ((Addr(1) << vaddrHashWidth) - 1);
    Addr seg1 = (high >> vaddrHashWidth) & ((Addr(1) << vaddrHashWidth) - 1);
    Addr seg2 = (high >> (2 * vaddrHashWidth)) & ((Addr(1) << vaddrHashWidth) - 1);
    Addr high_hash = seg0 ^ seg1 ^ seg2;

    Addr tag = (high_hash << REGION_ADDR_RAW_WIDTH) | low;
    return tag;
}
bool
PrefetchFilter::hasPFRequestsInBuffer()
{
    auto it_begin = table.begin();
    auto it_end = table.end();
    const auto n = std::distance(it_begin, it_end);
    if (n == 0)
        return false;
    DPRINTF(HWPrefetch, "hasPFRequestsInBuffer called. table size: %lu,name=%s\n", static_cast<unsigned long>(n), table_name.c_str());
    //print all entry status
    for (const auto &entry : table) {
        DPRINTF(HWPrefetch, "  Entry: region=%#lx tag=%#lx bits=%#lx filter=%#lx level=%lu valid=%d\n",
                entry.region_addr, entry.getTag(), entry.region_bits,
                entry.filter_bits, entry.PFlevel, entry.isValid());
    }
    uint64_t mask = (regionBlks >= 64) ? ~uint64_t(0) : ((uint64_t(1) << regionBlks) - 1);

    for (unsigned i = 0; i < static_cast<unsigned>(n); ++i) {
        unsigned idx = (rrIndex + i) % static_cast<unsigned>(n);
        auto it = it_begin;
        std::advance(it, idx);
        Entry *e = &(*it);

        if (!e->isValid())
            continue;
        if (!e->paddr_valid)
            continue;
        uint64_t pending = e->region_bits & (~e->filter_bits) & mask;
        if (pending)
            return true;
    }

    return false;
}
} // namespace prefetch
} // namespace gem5
