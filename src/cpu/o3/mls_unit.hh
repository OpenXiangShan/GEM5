#ifndef __CPU_O3_MLS_UNIT_HH__
#define __CPU_O3_MLS_UNIT_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <optional>
#include <vector>

#include "arch/generic/mmu.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "matrix/MatrixTypes.hh"
#include "mem/request.hh"
#include "sim/cur_tick.hh"
#include "sim/faults.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;

class MlsVirtualQueue
{
  public:
    struct Entry
    {
        InstSeqNum robSeqNum = 0;
        bool allocated = false;
        bool finished = false;
        DynInstPtr waitingForReplay;
    };

    MlsVirtualQueue(unsigned num_threads, unsigned capacity = 8);

    bool canAllocate(ThreadID tid, unsigned count = 1) const;
    bool hasEntry(const DynInstPtr &inst) const;
    bool allocate(const DynInstPtr &inst);
    bool markFinished(const DynInstPtr &inst);
    void waitForReplaySlot(const DynInstPtr &inst);
    DynInstPtr takeReplayWaiter(ThreadID tid);
    unsigned retireCommitted(ThreadID tid, InstSeqNum committed_seq);
    unsigned squash(ThreadID tid, InstSeqNum squash_seq);

    unsigned freeEntries(ThreadID tid) const;
    unsigned size(ThreadID tid) const;

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

class MlsReplayQueue
{
  public:
    struct TranslationContext
    {
        RequestPtr request;
        Fault fault = NoFault;
        bool finished = false;
        bool squashed = false;
        std::vector<RequestPtr> pageRequests;
        // First accessed byte on each 64-byte backend beat, or 64 if unused.
        std::vector<std::array<uint8_t, 64>> pageLines;
        size_t nextPage = 0;
        std::shared_ptr<matrix::MatrixPageMappings> pageMappings =
            std::make_shared<matrix::MatrixPageMappings>();
    };

    struct ReplayState
    {
        Addr vaddr = 0;
        RegVal stride = 0;
        RegVal tile0 = 0;
        RegVal tile1 = 0;
        RequestPtr request;
        std::shared_ptr<TranslationContext> translationContext;
    };

    struct Entry
    {
        InstSeqNum robSeqNum = 0;
        bool allocated = false;
        bool scheduled = false;
        bool ready = false;
        Tick availableTick = 0;
        DynInstPtr inst;
        ReplayState state;
    };

    MlsReplayQueue(unsigned num_threads, unsigned capacity = 8,
                   Tick replay_select_latency = 10000);

    ReplayState *reserve(const DynInstPtr &inst);
    bool scheduleNext(ThreadID tid, DynInstPtr &inst_out);
    void complete(const DynInstPtr &inst);
    unsigned squash(ThreadID tid, InstSeqNum squash_seq);

  private:
    void freeEntry(Entry &entry);

    const Tick replaySelectLatency;
    std::vector<std::vector<Entry>> entries;
};

class MlsUnit
{
  public:
    MlsUnit(CPU *cpu, const BaseO3CPUParams &params);

    bool issue(const DynInstPtr &inst);
    bool canAllocate(ThreadID tid, unsigned count = 1) const;
    bool allocate(const DynInstPtr &inst);
    void scheduleReplay(const std::list<ThreadID> &active_threads);
    unsigned squash(ThreadID tid, InstSeqNum squash_seq);
    unsigned retireCommitted(ThreadID tid, InstSeqNum committed_seq);

  private:
    void retryWaiting(ThreadID tid);
    Fault matrixMemEarlyFault(
        const DynInstPtr &inst,
        const MlsReplayQueue::ReplayState &state) const;
    Fault prepareTranslation(const DynInstPtr &inst,
                             MlsReplayQueue::ReplayState &state,
                             Fault fault) const;

    CPU *cpu;
    MlsVirtualQueue virtualQueue;
    MlsReplayQueue replayQueue;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MLS_UNIT_HH__
