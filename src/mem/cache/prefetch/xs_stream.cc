#include "mem/cache/prefetch/xs_stream.hh"

#include "debug/XsStreamPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

XsStreamPrefetcher::XsStreamPrefetcher(const XsStreamPrefetcherParams &p)
    : Queued(p),
      depth(p.xs_stream_depth),
      bad_pre_num(0),
      stream_array(p.xs_stream_entries, p.xs_stream_entries, p.xs_stream_indexing_policy,
                   p.xs_stream_replacement_policy, STREAMEntry()),
      streamBlkFilter(pfFilterSize)
{
}
void
XsStreamPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, int late_num)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr block_addr = blockAddress(vaddr);
    PrefetchSourceType stream_type = PrefetchSourceType::SStream;
    bool in_active_page = false;
    bool decr = false;
    if (pfi.isStore()) {
        stream_type = PrefetchSourceType::StoreStream;
        DPRINTF(XsStreamPrefetcher, "prefetch trigger come from store unit\n");
    }
    if (pfi.isCacheMiss() && (streamBlkFilter.contains(block_addr))) {
        bad_pre_num++;
    }
    STREAMEntry *entry = streamLookup(pfi, in_active_page, decr);
    if (issuedPrefetches % VALIDITY_CHECK_INTERVAL == 0) {
        if (((double)late_num / issuedPrefetches >= 0.4)) {
            if (depth != (1 << 9))
                depth = depth << 1;
        }
        if (bad_pre_num > LATE_MISS_THRESHOLD) {
            bad_pre_num = 0;
            if (depth != 1) {
                depth = depth >> 1;
            }
        }
    }
    Addr pf_stream_l1 = decr ? block_addr - depth * blkSize : block_addr + depth * blkSize;
    sendPFWithFilter(pfi, pf_stream_l1, addresses, 1, stream_type);
    Addr pf_stream_l2 = decr ? block_addr - (depth << l2Ratio) * blkSize : block_addr + (depth << l2Ratio) * blkSize;
    sendPFWithFilter(pfi, pf_stream_l2, addresses, 1, stream_type);
    Addr pf_stream_l3 = decr ? block_addr - (depth << l3Ratio) * blkSize : block_addr + (depth << l3Ratio) * blkSize;
    sendPFWithFilter(pfi, pf_stream_l3, addresses, 1, stream_type);
}

XsStreamPrefetcher::STREAMEntry *
XsStreamPrefetcher::streamLookup(const PrefetchInfo &pfi, bool &in_active_page, bool &decr)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr vaddr_tag = tagAddress(vaddr);
    Addr vaddr_offset = tagOffset(vaddr);

    STREAMEntry *entry = stream_array.findEntry(vaddr_tag, pfi.isSecure());
    STREAMEntry *entry_plus = stream_array.findEntry(vaddr_tag + 1, pfi.isSecure());
    STREAMEntry *entry_min = stream_array.findEntry(vaddr_tag - 1, pfi.isSecure());

    if (entry) {
        stream_array.accessEntry(entry);
        uint64_t region_bit_accessed = 1UL << vaddr_offset;
        if (entry_plus)
            entry->decr_mode = true;
        if ((entry_plus || entry_min) || (entry->cnt > ACTIVE_THRESHOLD))
            entry->active = true;
        in_active_page = entry->active;
        decr = entry->decr_mode;
        if (!(entry->bitVec & region_bit_accessed)) {
            entry->cnt += 1;
        }
        return entry;
    }
    entry = stream_array.findVictim(0);

    in_active_page = (entry_plus || entry_min);
    decr = entry_plus != nullptr;
    entry->tag = vaddr_tag;
    entry->decr_mode = entry_plus != nullptr;
    entry->bitVec = 1UL << vaddr_offset;
    entry->decr_mode = entry_plus != nullptr;
    entry->active = (entry_plus != nullptr) || (entry_min != nullptr);
    return entry;
}
bool
XsStreamPrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
                                     int prio, PrefetchSourceType src, int ahead_level)
{
    if (filter->contains(addr)) {
        DPRINTF(XsStreamPrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        DPRINTF(XsStreamPrefetcher, "Send pf: %lx\n", addr);
        filter->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        streamBlkFilter.insert(addr, 0);
        if (ahead_level > 1) {
            assert(ahead_level == 2 || ahead_level == 3);
            addresses.back().pfahead_host = ahead_level;
            addresses.back().pfahead = true;
        } else {
            addresses.back().pfahead = false;
        }
        return true;
    }
}


}
}