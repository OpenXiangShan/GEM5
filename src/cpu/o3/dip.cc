/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of
 * Sciences. All rights reserved.
 */

#include "cpu/o3/dip.hh"

#include <iomanip>

#include "base/logging.hh"
#include "base/output.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{
namespace o3
{

namespace
{
const char *stateNames[] = {"Base", "Stall", "FrontEnd", "Misspeculation"};
const char *causeNames[] = {"S_ROB", "S_ISB", "S_LDQ", "S_STQ", "S_REN",
    "S_SER", "F_BUB", "F_CTM", "M_SWE", "M_FL", "M_UNKNOWN"};
}

Dip::DipStats::DipStats(CPU *cpu, Dip &dip)
    : statistics::Group(cpu, "dip"), owner(dip),
      ADD_STAT(samples, statistics::units::Count::get(),
               "DIP cycle sampling boundaries"),
      ADD_STAT(sampledSlots, statistics::units::Count::get(),
               "DIP sampled dispatch slots"),
      ADD_STAT(attributedSlots, statistics::units::Count::get(),
               "Resolved DIP slots attributed to a static instruction"),
      ADD_STAT(unresolvedSlots, statistics::units::Count::get(),
               "Resolved DIP slots whose redirect cause PC is unknown"),
      ADD_STAT(pendingSlots, statistics::units::Count::get(),
               "DIP slots awaiting dispatch or architectural path resolution"),
      ADD_STAT(slotsByState, statistics::units::Count::get(),
               "Resolved DIP slots by mutually exclusive final state"),
      ADD_STAT(slotsByCause, statistics::units::Count::get(),
               "Resolved DIP slots containing each possibly overlapping cause")
{
    slotsByState.init(4);
    for (unsigned i = 0; i < 4; ++i) {
        slotsByState.subname(i, stateNames[i]);
    }
    slotsByCause.init(NumCauses);
    for (unsigned i = 0; i < NumCauses; ++i) {
        slotsByCause.subname(i, causeNames[i]);
    }
}

void
Dip::DipStats::preDumpStats()
{
    statistics::Group::preDumpStats();
    uint64_t pending = owner.frontendSlots;
    auto handle = simout.create(owner.outputName, false, true);
    auto &out = *handle->stream();
    out << "thread,pc,state,cause,slot_weight,cycle_weight,resolution\n"
        << std::setprecision(17);
    auto row = [&](Addr pc, State state, uint16_t cause, uint64_t slots,
                   const char *resolution) {
        if (!slots) {
            return;
        }
        out << "0,";
        if (pc) {
            out << "0x" << std::hex << pc << std::dec;
        }
        out << ',' << stateNames[static_cast<unsigned>(state)] << ','
            << cause << ',' << slots << ',' << double(slots) / owner.width
            << ',' << resolution << '\n';
    };
    for (const auto &[key, slots] : owner.weights) {
        const auto &[pc, state, cause] = key;
        row(pc, state, cause, slots, pc ? "resolved" : "unresolved");
    }
    for (const auto &[seq, target] : owner.targets) {
        for (const auto &[signature, slots] : target.slots) {
            pending += slots;
            row(target.inst->teaFirstMember()->pcState().instAddr(),
                signature.first, signature.second, slots, "pending");
        }
    }
    row(0, State::FrontEnd, 0, owner.frontendSlots, "pending");
    pendingSlots = pending;
    simout.close(handle);
}

void
Dip::DipStats::resetStats()
{
    statistics::Group::resetStats();
    owner.weights.clear();
    owner.targets.clear();
    owner.frontendSlots = 0;
    // The live redirect/version is pipeline state, not measurement history.
}

Dip::Dip(CPU *cpu, const BaseO3CPUParams &params)
    : stats(cpu, *this), width(params.renameWidth),
      period(params.dipSamplePeriod), outputName(cpu->name() + ".dip.csv")
{
    fatal_if(params.numThreads != 1 || params.smtNumPreDispatchThreads != 1,
             "DIP currently requires a single-thread dispatch window");
}

void
Dip::beginCycle()
{
    due = countdown == 0;
    countdown = due ? (period ? period - 1 : 0) : countdown - 1;
    dispatched = 0;
    blocker = nullptr;
    renameBlocker = nullptr;
    blockCause = upstreamCause = renameCause = 0;
}

bool
Dip::oldPath(const DynInstPtr &inst) const
{
    return recovering && (awaitingSquash ||
        recoveryVersion.largerThan(inst->getVersion()));
}

void
Dip::record(const DynInstPtr &inst, State state, uint16_t cause,
            uint64_t slots)
{
    if (!slots) {
        return;
    }
    if (!inst || inst->seqNum <= committedSeq) {
        resolve(inst ? inst->teaFirstMember()->pcState().instAddr() : 0,
                state, cause, slots);
        return;
    }
    auto &target = targets[inst->seqNum];
    target.inst = inst;
    target.slots[{state, cause}] += slots;
}

void
Dip::resolve(Addr pc, State state, uint16_t cause, uint64_t slots)
{
    weights[{pc, state, cause}] += slots;
    if (pc) {
        stats.attributedSlots += slots;
    } else {
        stats.unresolvedSlots += slots;
    }
    stats.slotsByState[static_cast<unsigned>(state)] += slots;
    for (unsigned i = 0; i < NumCauses; ++i) {
        if (cause & (1u << i)) {
            stats.slotsByCause[i] += slots;
        }
    }
}

void
Dip::misspeculate(uint64_t slots)
{
    record(flushInst, State::Misspeculation, flushCause, slots);
}

void
Dip::dispatch(const DynInstPtr &inst)
{
    ++dispatched;
    assert(dispatched <= width);
    if (oldPath(inst)) {
        if (due) {
            misspeculate(1);
        }
        return;
    }
    recovering = false;
    // Frontend slots belong to the next admitted uop, even if this cycle is
    // not sampled. Fetch carries the supplying cache-line events into it.
    const uint16_t events = inst->teaPSV();
    const uint16_t cause = events & (TeaEvent::bit(TeaEvent::DR_L1) |
        TeaEvent::bit(TeaEvent::DR_TLB)) ? CacheTlb : Bubble;
    record(inst, State::FrontEnd, cause, frontendSlots);
    frontendSlots = 0;
    if (due) {
        record(inst, State::Base, 0, 1);
    }
}

void
Dip::block(const DynInstPtr &inst, uint16_t cause)
{
    blocker = inst;
    blockCause = cause;
}

void
Dip::renameBlock(const DynInstPtr &inst, bool registers)
{
    renameBlocker = inst;
    renameCause = upstreamCause | (registers ? REN : 0);
}

void
Dip::setFlushCause(const DynInstPtr &inst, uint16_t cause)
{
    flushInst = inst;
    flushCause = cause;
    recovering = true;
    awaitingSquash = true;
}

void
Dip::squash(InstSeqNum boundary, const SquashVersion &version)
{
    recovering = true;
    awaitingSquash = false;
    recoveryVersion = version;
    uint64_t slots = frontendSlots;
    frontendSlots = 0;
    for (auto it = targets.upper_bound(boundary); it != targets.end();) {
        for (const auto &[signature, count] : it->second.slots) {
            slots += count;
        }
        it = targets.erase(it);
    }
    // A flush can include its cause (e.g., a replaying load). Keep that PC
    // tentative until retirement passes its sequence number, so an older
    // redirect can still reattribute a nested wrong-path recovery.
    misspeculate(slots);
}

void
Dip::commit(const DynInstPtr &inst)
{
    committedSeq = inst->seqNum;
    for (auto it = targets.begin(); it != targets.end() &&
         it->first <= committedSeq;) {
        const Addr pc = it->second.inst->teaFirstMember()->pcState().instAddr();
        for (const auto &[signature, slots] : it->second.slots) {
            resolve(pc, signature.first, signature.second, slots);
        }
        it = targets.erase(it);
    }
}

void
Dip::tick()
{
    // New-path input blocked at rename ends refill just as an IEW blocker
    // does. Advance this pipeline state even between sampling boundaries.
    if ((blocker && !oldPath(blocker)) ||
        (renameBlocker && renameCause && !oldPath(renameBlocker))) {
        recovering = false;
    }
    if (!due) {
        return;
    }
    ++stats.samples;
    stats.sampledSlots += width;
    const unsigned empty = width - dispatched;
    if (!empty) {
        return;
    }
    if (recovering) {
        misspeculate(empty);
    } else if (blocker) {
        record(blocker, State::Stall, blockCause, empty);
    } else if (renameBlocker && renameCause) {
        record(renameBlocker, State::Stall, renameCause, empty);
    } else {
        frontendSlots += empty;
    }
}

} // namespace o3
} // namespace gem5
