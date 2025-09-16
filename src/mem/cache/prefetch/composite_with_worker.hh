#ifndef __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_HH__
#define __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_HH__

#include <vector>

#include "mem/cache/prefetch/cdp.hh"
#include "mem/cache/prefetch/worker.hh"
#include "params/CompositeWithWorkerPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class CompositeWithWorkerPrefetcher: public WorkerPrefetcher
{
  public:
    CompositeWithWorkerPrefetcher(const CompositeWithWorkerPrefetcherParams &p);

    void postNotifyInsert(const PacketPtr &trigger_pkt, std::vector<AddrPriority> &addresses);
    // TODO: This code is redundant with queued.cc, seperate it in queued

  protected:

    std::vector<AddrPriority> addressGenBuffer;

};

} // namespace prefetch
} // namespace gem5


#endif // __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_HH__