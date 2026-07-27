#include "mem/cache/prefetch/prefetch_filter.hh"
#include <cstdint>
#include <iterator>
#include <climits>
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
    ADD_STAT(hashcollisionCount, statistics::units::Count::get(), "PrefetchFilter hash collision count")
{

}

PrefetchFilter::PrefetchFilter(gem5::BaseIndexingPolicy *idx_policy, 
                               gem5::replacement_policy::Base *rpl_policy,
                               unsigned entries, unsigned region_size,
                               unsigned blk_size, statistics::Group *parent,
                   unsigned vaddr_hash_width,
                   PrefetchSourceType pf_source_type,
                   const std::string &name)
        : table(entries, entries, idx_policy,rpl_policy, Entry()),
        regionSize(region_size),
        blkSize(blk_size),
        regionBlks(region_size / blk_size),
        rrIndex(0),
        REGION_ADDR_RAW_WIDTH(6),//align with rtl
        vaddrHashWidth(vaddr_hash_width),
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
    stats.l1Calls++;
    auto it_begin = table.begin();
    auto it_end = table.end();
    const auto n = std::distance(it_begin, it_end);
    DPRINTF(HWPrefetch, "GetPFAddrL1 called. table size: %lu\n", static_cast<unsigned long>(n));
    if (n == 0)
        return false;

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
        if (e->PFlevel != 1)
            continue;
        uint64_t pending = e->region_bits & (~e->filter_bits) & mask;
        if (!pending)
            continue;

        unsigned region_offset = 0;
        if (e->decr_mode) {
            unsigned lz = __builtin_clzll(pending);
            unsigned msb = 63 - lz;
            region_offset = msb;
        } else {
            region_offset = __builtin_ctzll(pending);
        }

    Addr region_num = e->region_addr;
    // Use bit operations to compute: region_num * regionSize + region_offset * blkSize
    // Assume regionSize and blkSize are powers of two; compute shift amounts.
    unsigned rs_shift = __builtin_ctz(regionSize);
    unsigned bs_shift = __builtin_ctz(blkSize);
    Addr pf_addr = (region_num << rs_shift) + (Addr(region_offset) << bs_shift);

        TriggerInfo trigger_info;
        bool has_trigger = false;
        if (region_offset < e->bitTriggers.size()) {
            auto &slot = e->bitTriggers[region_offset];
            if (slot) {
                trigger_info = *slot;
                slot.reset();
                has_trigger = true;
            }
        }
        markBlockSent(e, region_offset);

        stats.prefetchIssued++;

        rrIndex = (idx + 1) % static_cast<unsigned>(n);

        // construct AddrPriority. Use a simple priority scheme: closer blocks get higher priority.
        int prio = static_cast<int>(regionBlks) - static_cast<int>(region_offset);
        if (has_trigger) {
            addresses.emplace_back(AddrPriority(pf_addr, prio,
                trigger_info.pfSourceType == PrefetchSourceType::PF_NONE ? pfSourceType : trigger_info.pfSourceType, trigger_info));
        } else {
            addresses.emplace_back(AddrPriority(pf_addr, prio, pfSourceType));
        }
        stats.l1Issued++;
        DPRINTF(HWPrefetch, "GetPFAddrL1 issued addr=%#lx prio=%d trigger=%d\n",
                pf_addr, prio, has_trigger);
        return true;
    }

    return false;
}
bool
PrefetchFilter::GetPFAddrL2(std::vector<AddrPriority> &addresses)
{
    stats.l2Calls++;
    auto it_begin = table.begin();
    auto it_end = table.end();
    const auto n = std::distance(it_begin, it_end);
    DPRINTF(HWPrefetch, "GetPFAddrL2 called. table size: %lu\n", static_cast<unsigned long>(n));
    if (n == 0)
        return false;

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
        if (e->PFlevel != 2)
            continue;
        uint64_t pending = e->region_bits & (~e->filter_bits) & mask;
        if (!pending)
            continue;

        unsigned region_offset = 0;
        if (e->decr_mode) {
            unsigned lz = __builtin_clzll(pending);
            unsigned msb = 63 - lz;
            region_offset = msb;
        } else {
            region_offset = __builtin_ctzll(pending);
        }

    Addr region_num = e->region_addr;
    // Use bit operations to compute: region_num * regionSize + region_offset * blkSize
    // Assume regionSize and blkSize are powers of two; compute shift amounts.
    unsigned rs_shift = __builtin_ctz(regionSize);
    unsigned bs_shift = __builtin_ctz(blkSize);
    Addr pf_addr = (region_num << rs_shift) + (Addr(region_offset) << bs_shift);

        TriggerInfo trigger_info;
        bool has_trigger = false;
        if (region_offset < e->bitTriggers.size()) {
            auto &slot = e->bitTriggers[region_offset];
            if (slot) {
                trigger_info = *slot;
                slot.reset();
                has_trigger = true;
            }
        }
        markBlockSent(e, region_offset);

        stats.prefetchIssued++;

        rrIndex = (idx + 1) % static_cast<unsigned>(n);

        // construct AddrPriority. Use a simple priority scheme: closer blocks get higher priority.
        int prio = static_cast<int>(regionBlks) - static_cast<int>(region_offset);
        if (has_trigger) {
            addresses.emplace_back(AddrPriority(pf_addr, prio,
                trigger_info.pfSourceType == PrefetchSourceType::PF_NONE ? pfSourceType : trigger_info.pfSourceType, trigger_info));
        } else {
            addresses.emplace_back(AddrPriority(pf_addr, prio, pfSourceType));
        }
        addresses.back().pfahead_host = 2;
        addresses.back().pfahead = true;
        stats.l2Issued++;
            DPRINTF(HWPrefetch, "GetPFAddrL2 issued addr=%#lx prio=%d trigger=%d\n",
                pf_addr, prio, has_trigger);
        return true;
    }

    return false;
}
bool
PrefetchFilter::GetPFAddrL3(std::vector<AddrPriority> &addresses)
{
    stats.l3Calls++;
    auto it_begin = table.begin();
    auto it_end = table.end();
    const auto n = std::distance(it_begin, it_end);
    DPRINTF(HWPrefetch, "GetPFAddrL3 called. table size: %lu\n", static_cast<unsigned long>(n));
    if (n == 0)
        return false;

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
        if (e->PFlevel != 3)
            continue;
        uint64_t pending = e->region_bits & (~e->filter_bits) & mask;
        if (!pending)
            continue;

        unsigned region_offset = 0;
        if (e->decr_mode) {
            unsigned lz = __builtin_clzll(pending);
            unsigned msb = 63 - lz;
            region_offset = msb;
        } else {
            region_offset = __builtin_ctzll(pending);
        }

    Addr region_num = e->region_addr;
    // Use bit operations to compute: region_num * regionSize + region_offset * blkSize
    // Assume regionSize and blkSize are powers of two; compute shift amounts.
    unsigned rs_shift = __builtin_ctz(regionSize);
    unsigned bs_shift = __builtin_ctz(blkSize);
    Addr pf_addr = (region_num << rs_shift) + (Addr(region_offset) << bs_shift);

        TriggerInfo trigger_info;
        bool has_trigger = false;
        if (region_offset < e->bitTriggers.size()) {
            auto &slot = e->bitTriggers[region_offset];
            if (slot) {
                trigger_info = *slot;
                slot.reset();
                has_trigger = true;
            }
        }
        markBlockSent(e, region_offset);

        stats.prefetchIssued++;

        rrIndex = (idx + 1) % static_cast<unsigned>(n);

        // construct AddrPriority. Use a simple priority scheme: closer blocks get higher priority.
        int prio = static_cast<int>(regionBlks) - static_cast<int>(region_offset);
        if (has_trigger) {
            addresses.emplace_back(AddrPriority(pf_addr, prio,
                trigger_info.pfSourceType == PrefetchSourceType::PF_NONE ? pfSourceType : trigger_info.pfSourceType, trigger_info));
        } else {
            addresses.emplace_back(AddrPriority(pf_addr, prio, pfSourceType));
        }
        addresses.back().pfahead_host = 3;
        addresses.back().pfahead = true;
        stats.l3Issued++;
            DPRINTF(HWPrefetch, "GetPFAddrL3 issued addr=%#lx prio=%d trigger=%d\n",
                pf_addr, prio, has_trigger);
        return true;
    }

    return false;
}

void
PrefetchFilter::markBlockSent(PrefetchFilter::Entry *e, unsigned blk_idx)
{
    if (!e || blk_idx >= regionBlks)
        return;
    e->filter_bits |= (uint64_t(1) << blk_idx);
    table.accessEntry(e);
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
    Addr tag = regionHashTag(region_addr);
    Entry *e = table.findEntry(tag, is_secure);
    DPRINTF(HWPrefetch, "Insert called: region=%#lx tag=%#lx bits=%#lx level=%lu,name=%s\n",
            region_addr, tag, region_bits, PFlevel, table_name.c_str());
    if (e) {
        if (e->region_addr != region_addr) {
            DPRINTF(HWPrefetch, "Warning: Insert called with existing entry but different region_addr. existing=%#lx new=%#lx\n",
                    e->region_addr, region_addr);
            stats.hashcollisionCount++;
        }
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
    Entry *victim = table.findVictim(tag);
    victim->region_addr = region_addr;
    victim->region_bits = region_bits;
    victim->filter_bits = 0;
    victim->alias_bits = alias_bits;
    victim->paddr_valid = true;
    victim->decr_mode = decr_mode;
    victim->_setSecure(is_secure);
    victim->PFlevel = PFlevel;
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

// uint64_t
// PrefetchFilter::pendingBlocks(PrefetchFilter::Entry *e) const
// {
//     if (!e)
//         return 0;
//     return e->region_bits & static_cast<uint64_t>(~e->filter_bits);
// }
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
