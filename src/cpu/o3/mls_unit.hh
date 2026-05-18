#ifndef __CPU_O3_MLS_UNIT_HH__
#define __CPU_O3_MLS_UNIT_HH__

#include "arch/generic/mmu.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/mls_replay_queue.hh"
#include "sim/faults.hh"

namespace gem5
{

namespace o3
{

class CPU;
class MlsVirtualQueue;

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
