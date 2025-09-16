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
    bool isVaddrValid = ptr->pkt->req->hasVaddr();
    bool isPaddrValid = ptr->pkt->req->hasPaddr();
    Addr recvVAddr = isVaddrValid ? ptr->pkt->req->getVaddr() : 0;
    Addr recvPAddr = isPaddrValid ? ptr->pkt->req->getPaddr() : 0;

    // ignore if pfahead_host > itself level
    if ((ptr->isCrossLevel ? (ptr->targetLevel <= cache->level()) : true) &&
        (ptr->pfInfo.getXsMetadata().prefetchSource == PrefetchSourceType::SStream)) {
        if (isVaddrValid && pfLRUFilter.contains(recvVAddr, isVaddrValid)) {
            DPRINTF(WorkerPref, "Worker: offload vaddr: [%lx, %d] skip recently in localBuffer\n",
                recvVAddr, ptr->targetLevel);
            return;
        } else if (isPaddrValid && pfLRUFilter.contains(recvPAddr, isPaddrValid)) {
            DPRINTF(WorkerPref, "Worker: offload paddr: [%lx, %d] skip recently in localBuffer\n",
                isVaddrValid ? "vaddr":"paddr", recvPAddr, ptr->targetLevel);
            return;
        }
        if (isVaddrValid) {
            pfLRUFilter.insert(recvVAddr,isVaddrValid);
        } else if (isPaddrValid) {
            pfLRUFilter.insert(recvPAddr,isVaddrValid);
        }

    }

    workerStats.hintsReceived++;

    if (isVaddrValid) {
        DPRINTF(WorkerPref, "Worker: put [%lx, %d] into localBuffer(size:%lu)\n",
                recvVAddr, ptr->targetLevel, localBuffer.size());
    } else if (isPaddrValid) {
        DPRINTF(WorkerPref, "Worker: put [%lx, %d] into localBuffer(size:%lu)\n",
                recvPAddr, ptr->targetLevel, localBuffer.size());
    }
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
