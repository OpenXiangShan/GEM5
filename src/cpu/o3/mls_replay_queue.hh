#ifndef __CPU_O3_MLS_REPLAY_QUEUE_HH__
#define __CPU_O3_MLS_REPLAY_QUEUE_HH__

#include <functional>
#include <optional>
#include <vector>

#include "arch/generic/mmu.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"

namespace gem5
{

namespace o3
{

class MlsReplayQueue
{
  public:
    struct ReplayState
    {
        Addr vaddr = 0;
        Addr paddr = 0;
        RegVal stride = 0;
        RegVal tile0 = 0;
        RegVal tile1 = 0;
        BaseMMU::Mode mode = BaseMMU::Read;
        uint16_t asid = 0;
    };

    struct Entry
    {
        InstSeqNum robSeqNum = 0;
        ThreadID tid = InvalidThreadID;
        unsigned slot = 0;
        bool allocated = false;
        bool scheduled = false;
        bool ready = false;
        Tick availableTick = 0;
        DynInstPtr inst;
        ReplayState state;
    };

    MlsReplayQueue(unsigned num_threads, unsigned capacity = 8,
                   Tick replay_select_latency = 10000);

    bool hasEntry(const DynInstPtr &inst) const;
    const ReplayState *getState(const DynInstPtr &inst) const;
    bool allocateOrUpdate(
        const DynInstPtr &inst, const ReplayState &state, bool ready);
    void refreshReady(
        ThreadID tid,
        const std::function<bool(const ReplayState &)> &ready_fn);
    bool scheduleNext(ThreadID tid, DynInstPtr &inst_out);
    bool completeRetry(const DynInstPtr &inst);
    unsigned squash(ThreadID tid, InstSeqNum squash_seq);

    unsigned size(ThreadID tid) const;
    unsigned freeEntries(ThreadID tid) const;
    unsigned capacity() const { return queueCapacity; }

  private:
    Entry *findEntryBySlot(ThreadID tid, unsigned slot);
    const Entry *findEntryBySlot(ThreadID tid, unsigned slot) const;
    Entry *findEntryByInst(const DynInstPtr &inst);
    const Entry *findEntryByInst(const DynInstPtr &inst) const;
    std::optional<unsigned> allocateSlot(ThreadID tid);
    void freeEntry(Entry &entry);

    const unsigned queueCapacity;
    const Tick replaySelectLatency;
    std::vector<std::vector<Entry>> entries;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MLS_REPLAY_QUEUE_HH__
