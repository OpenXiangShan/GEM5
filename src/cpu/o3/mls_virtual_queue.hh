#ifndef __CPU_O3_MLS_VIRTUAL_QUEUE_HH__
#define __CPU_O3_MLS_VIRTUAL_QUEUE_HH__

#include <deque>
#include <optional>
#include <vector>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"

namespace gem5
{

namespace o3
{

class MlsVirtualQueue
{
  public:
    struct Entry
    {
        InstSeqNum robSeqNum = 0;
        ThreadID tid = InvalidThreadID;
        unsigned slot = 0;
        bool allocated = false;
        bool finished = false;
    };

    MlsVirtualQueue(unsigned num_threads, unsigned capacity = 8);

    bool canAllocate(ThreadID tid, unsigned count = 1) const;
    bool hasEntry(const DynInstPtr &inst) const;
    bool allocate(const DynInstPtr &inst);
    bool markFinished(const DynInstPtr &inst);
    unsigned retireCommitted(ThreadID tid, InstSeqNum committed_seq);
    unsigned squash(ThreadID tid, InstSeqNum squash_seq);

    unsigned freeEntries(ThreadID tid) const;
    unsigned size(ThreadID tid) const;
    unsigned capacity() const { return queueCapacity; }

  private:
    using Queue = std::deque<unsigned>;

    Entry *findEntryByInst(const DynInstPtr &inst);
    const Entry *findEntryByInst(const DynInstPtr &inst) const;
    Entry *findEntryBySlot(ThreadID tid, unsigned slot);
    const Entry *findEntryBySlot(ThreadID tid, unsigned slot) const;

    const unsigned queueCapacity;
    std::vector<Queue> queues;
    std::vector<std::vector<Entry>> entries;
    std::vector<unsigned> nextSlots;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MLS_VIRTUAL_QUEUE_HH__
