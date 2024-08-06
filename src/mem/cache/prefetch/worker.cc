#include "mem/cache/prefetch/worker.hh"

#include "debug/WorkerPref.hh"
#include "mem/cache/base.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

WorkerPrefetcher::WorkerPrefetcher(const WorkerPrefetcherParams &p) : Queued(p), workerStats(this), pfLRUFilter(128)
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
    if ((ptr->pfahead ? (ptr->pfahead_host <= cache->level()) : true) &&
        (ptr->pfInfo.getXsMetadata().prefetchSource == PrefetchSourceType::SStream)) {
        if (pfLRUFilter.contains(ptr->pfInfo.getAddr())) {
            DPRINTF(WorkerPref, "Worker: offload: [%lx, %d] skip recently in localBuffer\n", ptr->pfInfo.getAddr(), ptr->pfahead_host);
            return;
        }
        pfLRUFilter.insert(ptr->pfInfo.getAddr(),0);
    }

    workerStats.hintsReceived++;

    DPRINTF(WorkerPref, "Worker: put [%lx, %d] into localBuffer(size:%lu)\n", ptr->pfInfo.getAddr(), ptr->pfahead_host,
            localBuffer.size());
    localBuffer.push_back(*ptr);
}

void
WorkerPrefetcher::transfer()
{
    // ignore information of pfi, grab the information from the local buffer  忽略pfi的信息，从本地缓冲区中获取信息
    unsigned count = 0;
    auto dpp_it = localBuffer.begin();
    while (count < depth && !localBuffer.empty()) {
        if (queueFilter) {
            if (alreadyInQueue(pfq, dpp_it->pfInfo.getAddr(), dpp_it->pfInfo.isSecure(), dpp_it->priority)) {
                DPRINTF(WorkerPref, "Worker: [%lx, %d] was already in pfq\n", dpp_it->pfInfo.getAddr(),
                        dpp_it->pfahead_host);
            } else if (alreadyInQueue(pfqMissingTranslation, dpp_it->pfInfo.getAddr(), dpp_it->pfInfo.isSecure(),
                                      dpp_it->priority)) {
                DPRINTF(WorkerPref, "Worker: [%lx, %d] was already in pfq\n", dpp_it->pfInfo.getAddr(),
                        dpp_it->pfahead_host);
            } else {
                addToQueue(pfq, *dpp_it);
                DPRINTF(WorkerPref, "Worker: put [%lx, %d] into local pfq\n", dpp_it->pfInfo.getAddr(),
                        dpp_it->pfahead_host);
            }
        } else {
            addToQueue(pfq, *dpp_it);   // 将dpp_it指向的DeferredPacket加入pfq
            DPRINTF(WorkerPref, "Worker: put [%lx, %d] into local pfq\n", dpp_it->pfInfo.getAddr(),
                    dpp_it->pfahead_host);
        }
        dpp_it = localBuffer.erase(dpp_it); // 删除dpp_it指向的DeferredPacket
        count++;
    }
    schedule(transferEvent, nextCycle());   // 重新调度transferEvent
}

}  // namespace prefetch
}  // namespace gem5