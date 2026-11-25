//Created on 24-01-03
//choose stride or berti in sms

#include "mem/cache/prefetch/xs_stride.hh"

#include "base/stats/group.hh"
#include "debug/XSStridePrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

XSStridePrefetcher::XSStridePrefetcher(const XSStridePrefetcherParams &p)
    : Queued(p),
      strideHistoryBits(p.stride_history_bits),
      regionSize(p.region_size),
      regionBlks(p.region_size / p.block_size),
      strideUnique(p.stride_entries, p.stride_entries,
                   p.stride_unique_indexing_policy,
                   p.stride_unique_replacement_policy, StrideEntry()),
      stats(this)
{
    fatal_if(strideHistoryBits == 0 || strideHistoryBits > 63,
             "Stride history width must be between 1 and 63 bits");
    fatal_if(blkSize != 64, "XS stride requires 64-byte cache lines");
}

void
XSStridePrefetcher::calculatePrefetch(
    const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
    PrefetchSourceType pf_source, bool miss_repeat, bool enter_new_region,
    bool is_first_shot, Addr &pf_addr, int64_t &learned_bop_offset)
{
    learned_bop_offset = 0;
    strideLookup(pfi, addresses, is_first_shot);
}

bool
XSStridePrefetcher::strideLookup(
    const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses,
    bool is_first_shot)
{
    if (is_first_shot)
        stats.strideUniquequeryCount++;
    else
        stats.strideRedundantqueryCount++;

    const Addr lookupAddr = pfi.getAddr();
    const ContextID context_id = pfi.hasContextId() ?
        pfi.contextId() : InvalidContextID;
    const Addr stride_hash_pc =
        contextKey(strideHashPc(pfi.getPC()), context_id);
    StrideEntry *entry =
        strideUnique.findEntry(stride_hash_pc, pfi.isSecure());
    if (archDBer) {
        archDBer->strideTraceWrite(curTick(), lookupAddr, pfi.getPC(),
                                  stride_hash_pc, entry != nullptr,
                                  is_first_shot, pfi.isCacheMiss(), true);
    }

    if (!entry) {
        if (is_first_shot)
            stats.strideUniquemissCount++;
        else
            stats.strideRedundantmissCount++;

        entry = strideUnique.findVictim(stride_hash_pc);
        if (entry->state.confidence >= 3) {
            if (is_first_shot)
                stats.strideUniquereplaceusefulCount++;
            else
                stats.strideRedundantreplaceusefulCount++;
        }
        entry->state.allocate(lookupAddr, strideHistoryBits);
        strideUnique.insertEntry(stride_hash_pc, pfi.isSecure(), entry);
        return false;
    }

    if (is_first_shot)
        stats.strideUniquehitCount++;
    else
        stats.strideRedundanthitCount++;
    strideUnique.accessEntry(entry);

    auto &state = entry->state;
    const unsigned old_conf = state.confidence;
    const int64_t old_stride = state.stride;
    const Addr old_address = state.lastAddr;
    const auto decision = state.observe(lookupAddr, strideHistoryBits);
    DPRINTF(XSStridePrefetcher,
            "Stride decision: pc=%#x va=%#x prev=%#x observed=%ld "
            "old_stride=%ld new_stride=%ld old_conf=%u new_conf=%u "
            "valid=%d match=%d send=%d\n",
            pfi.getPC(), lookupAddr, old_address, decision.observed,
            old_stride, state.stride, old_conf, unsigned(state.confidence),
            decision.valid, decision.match, decision.send);
    if (!decision.send)
        return false;

    // Sending requires an exact match; old and new stride are identical.
    // Use unsigned address arithmetic for decreasing strides as well.
    const Addr offset = static_cast<Addr>(state.stride);
    const Addr targets[] = {blockAddress(lookupAddr + offset * 4),
                            blockAddress(lookupAddr + offset * 32)};
    for (unsigned level = 1; level <= 2; ++level) {
        const Addr target = targets[level - 1];
        sendPFWithFilter(pfi, target, addresses, 0,
                         PrefetchSourceType::SStride, level);
        if (archDBer) {
            archDBer->strideTraceWrite(curTick(), target, pfi.getPC(),
                                      stride_hash_pc, true, is_first_shot,
                                      pfi.isCacheMiss(), false);
        }
    }
    if (is_first_shot)
        stats.strideUniquepfCount += 2;
    else
        stats.strideRedundantpfCount += 2;
    return true;
}

void
XSStridePrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
                                      int prio, PrefetchSourceType src, int ahead_level)
{
    // Count generated prefetch
    prefetchStats.pfGenerated++;
    Addr filter_key = sharedFilterKey(pfi, addr);
    pfi.setTriggerInfo_PFsrc(src);
    if (ahead_level > 1){
        stridestream_pfFilter_l2l3->Insert(regionAddress(addr), uint64_t(1) << regionOffset(addr),0,true,false,pfi.isSecure(),ahead_level, &pfi.trigger_info);
        if (filterL2->contains(filter_key)) {
            DPRINTF(XSStridePrefetcher, "Skip recently prefetched: %lx\n", addr);
            // Count filtered prefetch
            prefetchStats.pfFiltered++;
        } else {
            DPRINTF(XSStridePrefetcher, "Send pf: %lx\n", addr);
            filterL2->insert(filter_key, 0);
            addresses.push_back(AddrPriority(addr, prio, src));
            assert(ahead_level == 2 || ahead_level == 3);
            addresses.back().pfahead_host = ahead_level;
            addresses.back().pfahead = true;
        }
    } else {
        stridestream_pfFilter_l1->Insert(regionAddress(addr), uint64_t(1) << regionOffset(addr),0,true,false,pfi.isSecure(),ahead_level, &pfi.trigger_info);
        if (filter->contains(filter_key)) {
            DPRINTF(XSStridePrefetcher, "Skip recently prefetched: %lx\n", addr);
            // Count filtered prefetch
            prefetchStats.pfFiltered++;
        } else {
            DPRINTF(XSStridePrefetcher, "Send pf: %lx\n", addr);
            filter->insert(filter_key, 0);
            addresses.push_back(AddrPriority(addr, prio, src));
            addresses.back().pfahead = false;
        }
    }
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
      ADD_STAT(strideRedundantreplaceusefulCount, statistics::units::Count::get(), "stride table replace num")
{
}

}

}
