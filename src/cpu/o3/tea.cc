/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of
 * Sciences. All rights reserved.
 */

#include "cpu/o3/tea.hh"

#include <iomanip>

#include "base/output.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/rob.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

Tea::TeaStats::TeaStats(CPU *cpu, Tea &tea)
    : statistics::Group(cpu, "tea"), owner(tea),
      ADD_STAT(samples, statistics::units::Count::get(),
               "Number of TEA sampling boundaries"),
      ADD_STAT(samplesByState, statistics::units::Count::get(),
               "TEA sampling boundaries by state"),
      ADD_STAT(samplesByEvent, statistics::units::Count::get(),
               "Attributed sample weight containing each TEA event bit"),
      ADD_STAT(committedByThread, statistics::units::Count::get(),
               "Architectural instructions observed at TEA commit"),
      ADD_STAT(attributedSamples, statistics::units::Count::get(),
               "TEA sample weight attributed to an instruction"),
      ADD_STAT(cancelledTargets, statistics::units::Count::get(),
               "Stalled sample weight whose target was squashed"),
      ADD_STAT(unresolvedTargets, statistics::units::Count::get(),
               "Sample weight with no selectable target or known flush cause"),
      ADD_STAT(pendingSamples, statistics::units::Count::get(),
               "Sample weight awaiting target retirement or a drained successor")
{
    const char *states[] = {"Computing", "Stalled", "Flushed", "Drained"};
    const char *events[] = {"DR_L1", "DR_TLB", "DR_SQ", "FL_MB",
                            "FL_EX", "FL_MO", "ST_L1", "ST_TLB"};
    samplesByState.init(static_cast<unsigned>(TeaState::NumStates));
    for (unsigned i = 0; i < static_cast<unsigned>(TeaState::NumStates); ++i) {
        samplesByState.subname(i, states[i]);
    }
    samplesByEvent.init(static_cast<unsigned>(TeaEvent::NumEvents)).precision(12);
    for (unsigned i = 0; i < TeaEvent::NumEvents; ++i) {
        samplesByEvent.subname(i, events[i]);
    }
    committedByThread.init(cpu->numThreads);
    attributedSamples.precision(12);
}

void
Tea::TeaStats::preDumpStats()
{
    statistics::Group::preDumpStats();
    if (!owner.enabledFlag) {
        return;
    }

    auto snapshot = owner.weights;
    uint64_t pending = 0;
    for (ThreadID tid = 0; tid < owner.threads.size(); ++tid) {
        const auto &state = owner.threads[tid];
        pending += state.drainSamples;
        snapshot[{tid, 0, -2}] += state.drainSamples;
        for (const auto &[seq, target] : state.targets) {
            pending += target.samples;
            snapshot[{tid, target.inst->teaFirstMember()->pcState().instAddr(), -2}] +=
                target.samples;
        }
    }
    pendingSamples = pending;

    auto handle = simout.create(owner.outputName, false, true);
    auto &out = *handle->stream();
    out << "thread,pc,psv,sample_weight\n" << std::setprecision(17);
    for (const auto &[key, weight] : snapshot) {
        if (weight == 0) {
            continue;
        }
        const auto &[tid, pc, psv] = key;
        out << tid << ',';
        if (pc != 0) {
            out << "0x" << std::hex << pc << std::dec;
        }
        out << ',' << psv << ',' << weight << '\n';
    }
    simout.close(handle);
}

void
Tea::TeaStats::resetStats()
{
    statistics::Group::resetStats();
    owner.weights.clear();
    // Do not resolve samples from the previous measurement window into this
    // window. Live instruction events and recovery causes remain valid.
    for (auto &state : owner.threads) {
        state.targets.clear();
        state.drainSamples = 0;
        state.committed.clear();
    }
}

Tea::Tea(CPU *cpu, const BaseO3CPUParams &params)
    : stats(cpu, *this), threads(params.numThreads),
      enabledFlag(params.enableTea), samplePeriod(params.teaSamplePeriod),
      outputName(cpu->name() + ".tea.csv")
{
}

bool
Tea::active(ThreadID tid) const
{
    if (!activeThreads) {
        return true;
    }
    for (const auto active_tid : *activeThreads) {
        if (active_tid == tid) {
            return true;
        }
    }
    return false;
}

void
Tea::attribute(ThreadID tid, TeaMember member, double weight)
{
    stats.attributedSamples += weight;
    weights[{tid, member.pc, member.psv}] += weight;
    for (unsigned event = 0; event < TeaEvent::NumEvents; ++event) {
        if (member.psv & (1u << event)) {
            stats.samplesByEvent[event] += weight;
        }
    }
}

void
Tea::recordSample(TeaState state)
{
    stats.samples++;
    stats.samplesByState[static_cast<unsigned>(state)]++;
}

void
Tea::finishTarget(ThreadID tid, const PendingTarget &target, bool cancelled)
{
    const auto *member = target.inst->teaFirstMember();
    if (cancelled) {
        stats.cancelledTargets += target.samples;
        weights[{tid, member->pcState().instAddr(), -1}] += target.samples;
    } else {
        attribute(tid, {member->pcState().instAddr(), member->teaPSV()}, target.samples);
    }
}

void
Tea::allocate(const DynInstPtr &inst)
{
    if (enabledFlag) {
        threads[inst->threadNumber].flushValid = false;
    }
}

void
Tea::commit(const DynInstPtr &inst)
{
    if (!enabledFlag) {
        return;
    }
    const auto tid = inst->threadNumber;
    auto &state = threads[tid];
    auto target = state.targets.find(inst->seqNum);
    if (target != state.targets.end()) {
        finishTarget(tid, target->second, false);
        state.targets.erase(target);
    }

    state.macroPSV |= inst->teaPSV();
    if (inst->isMicroop() && !inst->isLastMicroop()) {
        return;
    }
    const auto first = state.committed.size();
    if (inst->isFusion()) {
        for (const auto &member : inst->teaFusionMembers) {
            state.committed.push_back({member->pcState().instAddr(), member->teaPSV()});
        }
    } else {
        state.committed.push_back({inst->getPC(), state.macroPSV});
    }
    state.macroPSV = 0;
    stats.committedByThread[tid] += state.committed.size() - first;

    if (state.drainSamples) {
        attribute(tid, state.committed[first], state.drainSamples);
        state.drainSamples = 0;
    }
}

void
Tea::squash(const DynInstPtr &inst)
{
    if (!enabledFlag) {
        return;
    }
    const auto tid = inst->threadNumber;
    auto &state = threads[tid];
    auto target = state.targets.find(inst->seqNum);
    if (target != state.targets.end()) {
        finishTarget(tid, target->second, true);
        state.targets.erase(target);
    }
}

void
Tea::setFlush(ThreadID tid, Addr pc, uint8_t psv, bool valid)
{
    if (!enabledFlag) {
        return;
    }
    auto &state = threads[tid];
    state.recovering = true;
    state.flushValid = valid;
    state.flushPC = pc;
    state.flushPSV = psv;
}

void
Tea::tick()
{
    if (!enabledFlag) {
        return;
    }

    const bool due = countdown == 0;
    if (due) {
        countdown = samplePeriod == 0 ? 0 : samplePeriod - 1;
    } else {
        --countdown;
    }

    for (ThreadID tid = 0; tid < threads.size(); ++tid) {
        auto &state = threads[tid];
        if (active(tid) && due) {
            const bool empty = rob->isEmpty(tid);
            const bool recovering = state.recovering ||
                !rob->isDoneSquashing(tid) || (empty && state.flushValid);
            const TeaState sample_state =
                !state.committed.empty() ? TeaState::Computing :
                recovering ? TeaState::Flushed :
                empty ? TeaState::Drained : TeaState::Stalled;
            recordSample(sample_state);
            switch (sample_state) {
              case TeaState::Computing: {
                const double weight = 1.0 / state.committed.size();
                for (const auto &member : state.committed) {
                    attribute(tid, member, weight);
                }
                break;
              }
              case TeaState::Stalled: {
                const auto inst = rob->headStallInst(tid);
                if (inst) {
                    auto &target = state.targets[inst->seqNum];
                    target.inst = inst;
                    ++target.samples;
                } else {
                    ++stats.unresolvedTargets;
                    ++weights[{tid, 0, -3}];
                }
                break;
              }
              case TeaState::Flushed:
                if (state.flushValid) {
                    attribute(tid, {state.flushPC, state.flushPSV});
                } else {
                    ++stats.unresolvedTargets;
                    ++weights[{tid, 0, -3}];
                }
                break;
              case TeaState::Drained:
                ++state.drainSamples;
                break;
              default:
                break;
            }
        }
        state.committed.clear();
        state.recovering = false;
    }
}

} // namespace o3
} // namespace gem5
