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
      badPreNum(0),
      enableAutoDepth(p.enable_auto_depth),
      enableL3StreamPre(p.enable_l3_stream_pre),
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
        badPreNum++;
    }
    STREAMEntry *entry = streamLookup(pfi, in_active_page, decr);
    if ((issuedPrefetches >= VALIDITYCHECKINTERVAL) && (enableAutoDepth)) {
        if ((double)late_num / issuedPrefetches >= LATECOVERAGE) {
            if (depth != DEPTHRIGHT)
                depth = depth << DEPTHSTEP;
        }
        if (badPreNum > LATEMISSTHRESHOLD) {
            badPreNum = 0;
            if (depth != DEPTHLEFT) {
                depth = depth >> DEPTHSTEP;
            }
        }
        issuedPrefetches = 0;
    }

    if (in_active_page) {
        Addr pf_stream_l1 = decr ? block_addr - depth * blkSize : block_addr + depth * blkSize;
        sendPFWithFilter(pfi, pf_stream_l1, addresses, 1, stream_type, L1BLKDEGREE, 1);
        Addr pf_stream_l2 =
            decr ? block_addr - (depth << l2Ratio) * blkSize : block_addr + (depth << l2Ratio) * blkSize;
        sendPFWithFilter(pfi, pf_stream_l2, addresses, 1, stream_type, L2BLKDEGREE, 2);
        if (enableL3StreamPre) {
            Addr pf_stream_l3 =
                decr ? block_addr - (depth << l3Ratio) * blkSize : block_addr + (depth << l3Ratio) * blkSize;
            sendPFWithFilter(pfi, pf_stream_l3, addresses, 1, stream_type, L3BLKDEGREE, 3);
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

    STREAMEntry *entry = stream_array.findEntry(regionHashTag(vaddr_tag_num), pfi.isSecure());
    STREAMEntry *entry_plus = stream_array.findEntry(regionHashTag(vaddr_tag_num + 1), pfi.isSecure());
    STREAMEntry *entry_min = stream_array.findEntry(regionHashTag(vaddr_tag_num - 1), pfi.isSecure());

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
    entry = stream_array.findVictim(0);

    in_active_page = (entry_plus_active || entry_min_active);
    decr = entry_plus != nullptr;
    entry->tag = regionHashTag(vaddr_tag_num);
    entry->decrMode = decr;
    entry->bitVec = 1UL << vaddr_offset;
    entry->cnt = 1;
    entry->active = in_active_page;
    stream_array.insertEntry(regionHashTag(vaddr_tag_num), secure, entry);
    return entry;
}

void
XsStreamPrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
                                     int prio, PrefetchSourceType src, int pf_degree, int ahead_level)
{
    for (int i = 0; i < pf_degree; i++) {
        Addr pf_addr = addr + i * blkSize;
        if (filter->contains(pf_addr)) {
            DPRINTF(XsStreamPrefetcher, "Skip recently prefetched: %lx\n", pf_addr);
        } else {
            DPRINTF(XsStreamPrefetcher, "Send pf: %lx\n", pf_addr);
            filter->insert(pf_addr, 0);
            addresses.push_back(AddrPriority(pf_addr, prio, src));
            streamBlkFilter.insert(pf_addr, 0);
            if (ahead_level > 1) {
                assert(ahead_level == 2 || ahead_level == 3);
                addresses.back().pfahead_host = ahead_level;
                addresses.back().pfahead = true;
            } else {
                addresses.back().pfahead = false;
            }
        }
    }
}


}
}