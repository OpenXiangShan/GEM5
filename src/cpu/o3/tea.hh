/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of
 * Sciences. All rights reserved.
 */

#ifndef __CPU_O3_TEA_HH__
#define __CPU_O3_TEA_HH__

#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;
class ROB;

/** Event bits stored in an instruction's performance signature vector. */
namespace TeaEvent
{
enum Event : uint8_t
{
    DR_L1 = 0,
    DR_TLB,
    DR_SQ,
    FL_MB,
    FL_EX,
    FL_MO,
    ST_L1,
    ST_TLB,
    ST_LLC,
    NumEvents
};

constexpr uint16_t
bit(Event event)
{
    return static_cast<uint16_t>(1u << event);
}
} // namespace TeaEvent

enum class TeaState : uint8_t
{
    Computing = 0,
    Stalled,
    Flushed,
    Drained,
    NumStates
};

struct TeaMember
{
    Addr pc;
    uint16_t psv;
};

/**
 * Timing Event Attribution tracker for the O3 CPU.
 *
 * TEA is deliberately a companion to the functional ROB.  It observes
 * allocation, commit, and squash callbacks and samples the state of each
 * thread at the CPU cycle boundary.  The tracker is inactive unless
 * enableTea is set.
 */
class Tea
{
  private:
    struct PendingTarget
    {
        DynInstPtr inst;
        uint64_t samples = 0;
    };

    struct ThreadState
    {
        std::vector<TeaMember> committed;
        DynInstPtr stalled;
        std::unordered_map<InstSeqNum, PendingTarget> targets;
        uint64_t drainSamples = 0;
        uint16_t macroPSV = 0;
        bool recovering = false;
        bool flushValid = false;
        Addr flushPC = 0;
        uint16_t flushPSV = 0;
    };

    struct TeaStats : public statistics::Group
    {
        TeaStats(CPU *cpu, Tea &owner);
        void preDumpStats() override;
        void resetStats() override;

        Tea &owner;

        statistics::Scalar samples;
        statistics::Vector samplesByState;
        statistics::Vector samplesByEvent;
        statistics::Vector committedByThread;
        statistics::Scalar attributedSamples;
        statistics::Scalar cancelledTargets;
        statistics::Scalar unresolvedTargets;
        statistics::Scalar pendingSamples;
    } stats;

    ROB *rob = nullptr;
    std::list<ThreadID> *activeThreads = nullptr;
    std::vector<ThreadState> threads;
    uint64_t countdown = 0;
    const bool enabledFlag;
    const uint64_t samplePeriod;
    const unsigned lastLevelCache;
    const std::string outputName;
    // Negative PSV values identify cancelled, pending and unknown targets.
    std::map<std::tuple<ThreadID, Addr, int>, double> weights;

    bool active(ThreadID tid) const;
    void attribute(ThreadID tid, TeaMember member, double weight = 1.0);
    void recordSample(TeaState state);
    void finishTarget(ThreadID tid, const PendingTarget &target,
                      bool cancelled);

  public:
    Tea(CPU *cpu, const BaseO3CPUParams &params);

    bool enabled() const { return enabledFlag; }

    void setROB(ROB *rob_ptr) { rob = rob_ptr; }
    void setActiveThreads(std::list<ThreadID> *threads_ptr)
    {
        activeThreads = threads_ptr;
    }

    /** Called when an instruction is accepted by the ROB. */
    void allocate(const DynInstPtr &inst);
    /** Record a commit blocker, or select the first unready group member. */
    void stall(ThreadID tid, const DynInstPtr &inst = nullptr);
    /** Record the deepest cache miss reported by a cacheable data response. */
    void cacheMiss(const DynInstPtr &inst, unsigned depth);
    /** Called after an instruction has passed the architectural commit point. */
    void commit(const DynInstPtr &inst);
    /** Called when an in-flight instruction is discarded by a ROB squash. */
    void squash(const DynInstPtr &inst);

    /** Set or clear the cause visible during a recovery/flush interval. */
    void setFlush(ThreadID tid, Addr pc, uint16_t psv, bool valid = true);

    /** Retain walk activity even when this cycle finishes the ROB walk. */
    void markRecovery(ThreadID tid)
    {
        if (enabledFlag) {
            threads[tid].recovering = true;
        }
    }

    /** Take one cycle sample after all O3 stages have advanced. */
    void tick();
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TEA_HH__
