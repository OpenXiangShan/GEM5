/**
 * @file
 * Describes a fetch directed instruction prefetcher.
 */

#include "mem/cache/prefetch/fdip.hh"

#include "params/FDIP.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

FDIP::FDIP(const FDIPParams &p)
    : Queued(p)
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
FDIP::getStreamId(FetchStreamId &stream_id)
{
    stream_id = streamToPrefetch.streamId;
}

void
FDIP::addStream(Addr stream_start_pc, Addr stream_end_pc,
                FetchStreamId stream_id)
{
    streamToPrefetch.valid = true;
    streamToPrefetch.start = stream_start_pc;
    streamToPrefetch.end = stream_end_pc;
    streamToPrefetch.streamId = stream_id;
}

void
FDIP::squash(FetchStreamId stream_id)
{
    auto it = pfq.begin();
    while (it != pfq.end())
    {
        if (it->streamId >= stream_id)
            pfq.erase(it++);
        else
            it++;
    }

    it = pfqMissingTranslation.begin();
    while (it != pfqMissingTranslation.end()){
        if (it->streamId >= stream_id){
            if (it->ongoingTranslation){
                auto splice_it = it;
                it++;
                DeferredPacket * old_ptr = &(*splice_it);
                pfqSquashed.splice(pfqSquashed.end(),
                                    pfqMissingTranslation,splice_it);
                auto check_it = pfqSquashed.end();
                check_it--;
                assert(&(*check_it) == old_ptr);
            } else {
                it = pfqMissingTranslation.erase(it);
            }
        } else {
            it++;
        }
    }
}

} // namespace prefetch
} // namespace gem5
