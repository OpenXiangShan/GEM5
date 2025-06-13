#include "mem/cache/prefetch/worker.hh"

#include "debug/WorkerPref.hh"
#include "mem/cache/base.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

WorkerPrefetcher::WorkerPrefetcher(const WorkerPrefetcherParams &p)
    : Queued(p),
      workerStats(this),
      pfLRUFilter(256)
{
    //Event *event = new EventFunctionWrapper([this]{ enableFunctionTrace(); }, name(), true);
    transferEvent = new EventFunctionWrapper([this](){
        transfer();
    },name(),false);
}

WorkerPrefetcher::WorkerStats::WorkerStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(hintsReceived, statistics::units::Count::get(),
               "Number of hints received"),
      ADD_STAT(hintsOffloaded, statistics::units::Count::get(),
               "Number of hints offloaded")
{
}

void
WorkerPrefetcher::rxHint(BaseMMU::Translation *dpp)
{
    auto ptr = reinterpret_cast<DeferredPacket *>(dpp);

    // ignore if pfahead_host > itself level
    if ((ptr->isCrossLevel ? (ptr->targetLevel <= cache->level()) : true) &&
        (ptr->pfInfo.getXsMetadata().prefetchSource == PrefetchSourceType::SStream)) {
        if (pfLRUFilter.contains(ptr->pfInfo.getVAddr())) {
            DPRINTF(WorkerPref, "Worker: offload: [%lx, %d] skip recently in localBuffer\n",
                ptr->pfInfo.getVAddr(), ptr->targetLevel);
            return;
        }
        pfLRUFilter.insert(ptr->pfInfo.getVAddr(),0);
    }

    workerStats.hintsReceived++;

    DPRINTF(WorkerPref, "Worker: put [%lx, %d] into localBuffer(size:%lu)\n",
        ptr->pfInfo.getVAddr(), ptr->targetLevel, localBuffer.size());
    localBuffer.push_back(*ptr);

    if (!transferEvent->scheduled()){
        schedule(transferEvent, nextCycle());
    }
}

void
WorkerPrefetcher::transfer()
{
    // ignore information of pfi, grab the information from the local buffer
    unsigned count = 0;
    auto dpp_it = localBuffer.begin();
    while (count < transferDepth && !localBuffer.empty()) {
        if (queueFilter) {
            if (alreadyInQueue(pfq, dpp_it->pfInfo, dpp_it->priority)) {
                DPRINTF(WorkerPref, "Worker: [%lx, %d] was already in pfq\n", dpp_it->pfInfo.getVAddr(),
                        dpp_it->targetLevel);
            } else if (alreadyInQueue(pfqMissingTranslation, dpp_it->pfInfo,
                                      dpp_it->priority)) {
                DPRINTF(WorkerPref, "Worker: [%lx, %d] was already in pfq\n", dpp_it->pfInfo.getVAddr(),
                        dpp_it->targetLevel);
            } else {
                addToQueue(pfq, *dpp_it);
                DPRINTF(WorkerPref, "Worker: put [%lx, %d] into local pfq\n", dpp_it->pfInfo.getVAddr(),
                        dpp_it->targetLevel);
            }
        } else {
            addToQueue(pfq, *dpp_it);
            DPRINTF(WorkerPref, "Worker: put [%lx, %d] into local pfq\n", dpp_it->pfInfo.getVAddr(),
                    dpp_it->targetLevel);
        }
        dpp_it = localBuffer.erase(dpp_it);
        count++;
    }

    if (!localBuffer.empty()) {
        schedule(transferEvent, nextCycle());
    }

}

}  // namespace prefetch
}  // namespace gem5
