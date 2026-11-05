/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of
 * Sciences. All rights reserved.
 */

#ifndef __CPU_O3_DIP_HH__
#define __CPU_O3_DIP_HH__

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "base/statistics.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/dyn_inst_ptr.hh"

namespace gem5
{
struct BaseO3CPUParams;

namespace o3
{
class CPU;

/** Dispatch-time profiling (DIP), ASPLOS 2026, Figure 3.
 *
 * Observe single-thread in-order admission at IEW, including upstream rename
 * backpressure. Slots remain tentative until retirement establishes their path;
 * squashes transfer them to the redirecting instruction. Period one enumerates
 * all slots rather than approximating them with statistical sampling.
 */
class Dip
{
  public:
    enum class State : uint8_t { Base, Stall, FrontEnd, Misspeculation };
    enum Cause : uint16_t
    {
        ROB = 1 << 0, ISB = 1 << 1, LDQ = 1 << 2, STQ = 1 << 3,
        REN = 1 << 4, SER = 1 << 5,
        Bubble = 1 << 6, CacheTlb = 1 << 7,
        SoftwareException = 1 << 8, Flush = 1 << 9,
        UnknownFlush = 1 << 10,
        NumCauses = 11
    };

    enum IQGate : uint8_t
    {
        IQInports = 1, IQEntries = 2, IQReplay = 4, IQQuota = 8,
        IQReadyPeer = 16
    };

    Dip(CPU *cpu, const BaseO3CPUParams &params);
    void beginCycle();
    bool samplingDue() const { return due; }
    void dispatch(const DynInstPtr &inst);
    void block(const DynInstPtr &inst, uint16_t cause, int iq = -1,
               uint8_t iq_flags = 0);
    void setFrontendReasons(const std::vector<StallReason> &reasons);
    void decodeStranded(unsigned slots);
    void readyBranchWaiting();
    void upstreamStall(uint16_t cause) { upstreamCause |= cause; }
    void renameBlock(const DynInstPtr &inst, bool registers);
    void setFlushCause(const DynInstPtr &inst, uint16_t cause,
                       SquashCause reason = SquashCause::None);
    void squash(InstSeqNum boundary, const SquashVersion &version);
    void commit(const DynInstPtr &inst);
    void tick();

  private:
    using Signature =
        std::tuple<State, uint16_t, int, uint8_t, StallReason, SquashCause>;
    using Slots = std::map<Signature, uint64_t>;
    struct Target
    {
        DynInstPtr inst;
        Slots slots;
    };
    struct DipStats : statistics::Group
    {
        DipStats(CPU *cpu, Dip &owner);
        void preDumpStats() override;
        void resetStats() override;
        Dip &owner;
        statistics::Scalar samples;
        statistics::Scalar sampledSlots;
        statistics::Scalar attributedSlots;
        statistics::Scalar unresolvedSlots;
        statistics::Scalar pendingSlots;
        statistics::Scalar decodeStrandedSlots;
        statistics::Scalar readyBranchWaits;
        statistics::Vector slotsByState;
        statistics::Vector slotsByCause;
    } stats;

    const unsigned width;
    const uint64_t period;
    const std::string outputName;
    uint64_t countdown = 0;
    bool due = true;
    unsigned dispatched = 0;
    DynInstPtr blocker;
    uint16_t blockCause = 0;
    int blockedIQ = -1;
    uint8_t blockedIQFlags = 0;
    std::vector<StallReason> frontendReasons;
    DynInstPtr renameBlocker;
    uint16_t upstreamCause = 0;
    uint16_t renameCause = 0;
    std::map<InstSeqNum, Target> targets;
    std::map<StallReason, uint64_t> frontendSlots;
    std::map<std::tuple<Addr, State, uint16_t, int, uint8_t, StallReason,
                        SquashCause>,
             uint64_t> weights;
    DynInstPtr flushInst;
    uint16_t flushCause = UnknownFlush;
    SquashCause flushReason = SquashCause::None;
    bool recovering = false;
    bool awaitingSquash = false;
    SquashVersion recoveryVersion;
    InstSeqNum committedSeq = 0;

    void record(const DynInstPtr &inst, State state, uint16_t cause,
                uint64_t slots, int iq = -1, uint8_t iq_flags = 0,
                StallReason frontend = NoStall,
                SquashCause reason = SquashCause::None);
    void resolve(Addr pc, State state, uint16_t cause, uint64_t slots,
                 int iq = -1, uint8_t iq_flags = 0,
                 StallReason frontend = NoStall,
                SquashCause reason = SquashCause::None);
    void misspeculate(uint64_t slots);
    bool oldPath(const DynInstPtr &inst) const;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_DIP_HH__
