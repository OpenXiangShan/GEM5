/**
 * @file
 * Describes a fetch directed instruction prefetcher.
 */

#include "mem/cache/prefetch/fdip.hh"

#include "debug/HWPrefetch.hh"
#include "params/FDIP.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

FDIP::FDIP(const FDIPParams &p)
    : Queued(p), degree(p.degree)
{

}

void
FDIP::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses)
{
    if (!streamToPrefetch.valid) {
        return;
    }
    streamToPrefetch.valid = false;

    int blk_num = 0;
    Addr new_addr;
    do {
        new_addr = blockAddress(streamToPrefetch.start) + blk_num*(blkSize);
        addresses.push_back(AddrPriority(new_addr, 0));
        blk_num++;
    }
    while (new_addr < streamToPrefetch.end);
}

void
FDIP::addStream(Addr stream_start_pc, Addr stream_end_pc)
{
    streamToPrefetch.valid = true;
    streamToPrefetch.start = stream_start_pc;
    streamToPrefetch.end = stream_end_pc;
}

bool
FDIP::prefetchFilter(const PrefetchInfo &pfi, Addr pf_addr)
{
    bool drop = cache->inCache(pf_addr, pfi.isSecure());
    if (drop){
        DPRINTF(HWPrefetch,"Filterate the addr in %#x,%s\n",
                pf_addr,pfi.isSecure() ? "s" : "ns");
    }
    return drop;
}

} // namespace prefetch
} // namespace gem5
