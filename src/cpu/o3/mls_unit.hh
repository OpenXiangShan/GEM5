#ifndef __CPU_O3_MLS_UNIT_HH__
#define __CPU_O3_MLS_UNIT_HH__

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include "arch/generic/mmu.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "sim/cur_tick.hh"
#include "sim/faults.hh"

namespace gem5
{

namespace o3
{

class CPU;

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

class MlsUnit
{
  public:
    struct IssueResult
    {
        bool needReplay = false;
    };

    explicit MlsUnit(CPU *cpu);

    void setVirtualQueue(MlsVirtualQueue *queue) { virtualQueue = queue; }
    void setReplayQueue(MlsReplayQueue *queue) { replayQueue = queue; }
    IssueResult issue(const DynInstPtr &inst);
    bool replayReady(const MlsReplayQueue::ReplayState &state) const;

  private:
    struct StageState;

    unsigned matrixMemAccessSizeBytes(const DynInstPtr &inst) const;
    Fault matrixMemEarlyFault(const DynInstPtr &inst,
                              const StageState &state) const;
    void probeTlbState(StageState &state) const;
    bool replayTlbReady(const MlsReplayQueue::ReplayState &state) const;
    bool ensureReplayReady(const MlsReplayQueue::ReplayState &state) const;
    void deriveStage0Shape(const DynInstPtr &inst, StageState &state) const;
    void captureStage0(const DynInstPtr &inst, StageState &state) const;
    void restoreStage0FromReplay(
        const DynInstPtr &inst,
        const MlsReplayQueue::ReplayState &replay_state,
        StageState &state) const;
    void runStage1(const DynInstPtr &inst, StageState &state) const;
    void runStage2(const DynInstPtr &inst, StageState &state) const;
    void runStage3(const DynInstPtr &inst, StageState &state) const;
    void runStage4(const DynInstPtr &inst, const StageState &state) const;
    MlsReplayQueue::ReplayState buildReplayState(
        const StageState &state) const;

    CPU *cpu;
    MlsVirtualQueue *virtualQueue = nullptr;
    MlsReplayQueue *replayQueue = nullptr;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MLS_UNIT_HH__
