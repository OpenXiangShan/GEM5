#include "mem/cache/prefetch/worker.hh"

#include "debug/WorkerPref.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

WorkerPrefetcher::WorkerPrefetcher(const WorkerPrefetcherParams &p) : Queued(p) {}

void
WorkerPrefetcher::rxHint(BaseMMU::Translation *dpp)
{
    auto ptr = reinterpret_cast<DeferredPacket *>(dpp);
    DPRINTF(WorkerPref, "Put %s into local buffer\n", ptr->pkt->print());
    localBuffer.push_back(*ptr);
}

void
WorkerPrefetcher::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    // ignore information of pfi, grab the information from the local buffer
    unsigned count = 0;
    auto dpp_it = localBuffer.begin();
    while (count < depth && !localBuffer.empty()) {
        DPRINTF(WorkerPref, "Prefetching %s\n", dpp_it->pkt->print());
        addToQueue(pfq, *dpp_it);
        dpp_it = localBuffer.erase(dpp_it);
    }
}

}
}