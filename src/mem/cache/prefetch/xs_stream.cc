#include "mem/cache/prefetch/xs_stream.hh"

#include "debug/XsStreamPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

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
