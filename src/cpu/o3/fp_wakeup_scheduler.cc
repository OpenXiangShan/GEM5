/* Copyright (c) 2026. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <algorithm>
#include <unordered_set>

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/issue_queue.hh"
#include "debug/Schedule.hh"

namespace gem5
{
namespace o3
{

FpWakeIdentity
Scheduler::fpIdentity(const DynInstPtr& inst) const
{
    return {static_cast<uint32_t>(inst->threadNumber), inst->seqNum, inst->fpWakeAttempt};
}

bool
Scheduler::fpProducer(const DynInstPtr& inst) const
{
    return !inst->isVector() &&
        (inst->opClass() == FloatMultOp || inst->opClass() == FloatMultAccOp);
}

bool
Scheduler::fpConsumer(const DynInstPtr& inst, unsigned source) const
{
    if (inst->isVector() || inst->isMemRef() ||
        !inst->renamedSrcIdx(source)->isFloatReg())
        return false;
    // Split STA and STD share a sequence number; neither is a scalar FP
    // consumer. In particular, STD does not carry the memory-reference flag.
    switch (inst->opClass()) {
      case FloatAddOp:
      case FloatCmpOp:
      case FloatCvtOp:
      case FloatMvOp:
      case FloatMultOp:
      case FloatMultAccOp:
      case FloatDivOp:
      case FloatMiscOp:
      case FloatSqrtOp:
        return true;
      default:
        return false;
    }
}

bool
Scheduler::fpAddend(const DynInstPtr& inst, unsigned source) const
{
    if (inst->opClass() != FloatMultAccOp || !fpConsumer(inst, source))
        return false;
    unsigned floats = 0;
    for (unsigned i = 0; i <= source; ++i)
        floats += inst->renamedSrcIdx(i)->isFloatReg();
    return floats == 3;
}

Scheduler::FpRecord&
Scheduler::fpRecord(const DynInstPtr& inst)
{
    auto [it, inserted] = fpRecords.try_emplace(inst->seqNum);
    auto& record = it->second;
    if (inserted) {
        record.inst = inst;
        record.attempt = inst->fpWakeAttempt;
        record.sources.resize(inst->numSrcRegs());
        stats.fpStatePeak = std::max<uint64_t>(stats.fpStatePeak.value(), fpRecords.size());
    }
    return record;
}

bool
Scheduler::fpAttemptCurrent(const DynInstPtr& inst, uint64_t attempt) const
{
    return !enableFmaRtlAlignment ||
        (inst && inst->fpWakeAttempt == attempt && !inst->canceled());
}

uint32_t
Scheduler::getOutputDistance(const DynInstPtr& inst)
{
    if (enableFmaRtlAlignment && fpProducer(inst))
        return inst->opClass() == FloatMultOp ? 3 : 4;
    return getCorrectedOpLat(inst);
}

void
Scheduler::fpSelected(const DynInstPtr& inst, unsigned port)
{
    if (!enableFmaRtlAlignment)
        return;
    auto existing = fpRecords.find(inst->seqNum);
    if (existing != fpRecords.end())
        existing->second.canceled = false;
    auto it = fpRecords.find(inst->seqNum);
    if (it != fpRecords.end()) {
        for (auto& binding : it->second.sources) {
            if (binding) {
                fpWakeState.select(binding, cpu->clockEdge(Cycles(3)),
                                   2 * cpu->clockPeriod());
            }
        }
    }
    if (!fpProducer(inst))
        return;
    auto& record = fpRecord(inst);
    // Arbitration can fail without starting an attempt. Only published
    // promises become visible to consumers.
    if (record.output && !record.output->published) {
        fpWakeState.erase(record.output->producer);
        record.output.reset();
    }
    panic_if(record.output, "FP instruction selected twice without cancellation");
    const bool mul = inst->opClass() == FloatMultOp;
    const unsigned endpoint = inst->issueQue->getId() * 8 + port;
    record.output = fpWakeState.prepare(fpIdentity(inst),
        mul ? FpProducerKind::Fmul : FpProducerKind::Fma,
        endpoint, endpoint, cpu->clockEdge(Cycles(mul ? 3 : 4)),
        cpu->clockEdge(Cycles(mul ? 1 : 2)), cpu->clockEdge(Cycles(6)));
    for (const auto& binding : record.sources)
        if (binding)
            fpWakeState.addInput(fpIdentity(inst), binding);
    for (int i = 0; i < inst->numDestRegs(); ++i) {
        auto dst = inst->renamedDestIdx(i);
        if (!dst->isFixedMapping() && dst->isFloatReg()) {
            fpRegisters[dst->flatIndex()] = record.output;
            fpWakeState.setRegister(dst->flatIndex(), record.output);
        }
    }
    DPRINTF(Schedule, "FPA select tid=%u seq=%llu attempt=%llu port=%u\n",
            inst->threadNumber, inst->seqNum, inst->fpWakeAttempt, endpoint);
}

void
Scheduler::fpArbitrationFailed(const DynInstPtr& inst)
{
    if (!enableFmaRtlAlignment)
        return;
    auto it = fpRecords.find(inst->seqNum);
    if (it == fpRecords.end())
        return;
    for (auto& binding : it->second.sources) {
        if (!binding)
            continue;
        binding->selected = false;
        binding->deadline = 0;
        if (binding->token->m2Broadcast)
            binding->path = FpWakePath::M2;
    }
    if (it->second.output) {
        for (auto reg : it->second.output->registers) {
            auto owner = fpRegisters.find(reg);
            if (owner != fpRegisters.end() && owner->second == it->second.output)
                fpRegisters.erase(owner);
        }
        fpWakeState.erase(it->second.output->producer);
        it->second.output.reset();
    }
}

void
Scheduler::fpPublished(const DynInstPtr& inst)
{
    if (!enableFmaRtlAlignment || !fpProducer(inst))
        return;
    auto& record = fpRecord(inst);
    auto token = record.output;
    panic_if(!token || !fpWakeState.publish(token->producer), "Missing FP wake promise");
    fpWakeDue.emplace(token->m2Cycle, FpWakeEvent{inst, token, FpWakePath::M2});
    fpWakeDue.emplace(token->m4Cycle, FpWakeEvent{inst, token, FpWakePath::M4});
    DPRINTF(Schedule,
        "FPA publish tid=%u seq=%llu attempt=%llu m2=%llu m4=%llu data=%llu\n",
        inst->threadNumber, inst->seqNum, inst->fpWakeAttempt,
        token->m2Cycle, token->m4Cycle, token->dataCycle);
}

bool
Scheduler::fpBound(const DynInstPtr& inst, unsigned source) const
{
    if (!enableFmaRtlAlignment)
        return false;
    auto it = fpRecords.find(inst->seqNum);
    return it != fpRecords.end() && source < it->second.sources.size() &&
        it->second.sources[source] != nullptr;
}

void
Scheduler::fpInsert(const DynInstPtr& inst)
{
    if (!enableFmaRtlAlignment)
        return;
    for (unsigned i = 0; i < inst->numSrcRegs(); ++i) {
        if (!fpConsumer(inst, i) || inst->readySrcIdx(i))
            continue;
        auto token = fpWakeState.inheritM2(inst->renamedSrcIdx(i)->flatIndex());
        if (!token)
            continue;
        auto& record = fpRecord(inst);
        record.sources[i] = fpWakeState.bind(fpIdentity(inst), i, token, FpWakePath::M2);
        inst->markSrcRegReady(i);
        stats.fpM2Hits++;
        DPRINTF(Schedule, "FPA inherit tid=%u seq=%llu attempt=%llu src=%u from=%llu/%llu path=M2\n",
            inst->threadNumber, inst->seqNum, inst->fpWakeAttempt, i,
            token->producer.seq, token->producer.attempt);
    }
}

void
Scheduler::fpBroadcast(const FpWakeEvent& event)
{
    auto token = event.token;
    const auto& producer = event.inst;
    if (!fpWakeState.current(token) || token->faulted || producer->isSquashed())
        return;
    for (auto iq : issueQues) {
        for (int d = 0; d < producer->numDestRegs(); ++d) {
            auto dst = producer->renamedDestIdx(d);
            if (dst->isFixedMapping() || !dst->isFloatReg())
                continue;
            for (auto& edge : iq->subDepGraph[dst->flatIndex()]) {
                auto& consumer = edge.second;
                const unsigned src = edge.first;
                if (consumer->isSquashed() || consumer->threadNumber != producer->threadNumber ||
                    consumer->seqNum <= producer->seqNum || !fpConsumer(consumer, src))
                    continue;
                if (event.path == FpWakePath::M4 &&
                    (!fpAddend(consumer, src) || consumer->isIssued()))
                    continue;
                auto& record = fpRecord(consumer);
                auto& binding = record.sources[src];
                if (binding && binding->token == token && !binding->canceled) {
                    // M2 already converted unselected bindings in the state component.
                    continue;
                }
                if (consumer->readySrcIdx(src))
                    continue;
                binding = fpWakeState.bind(fpIdentity(consumer), src, token, event.path);
                if (!FpWakeupState::usable(binding))
                    continue;
                consumer->markSrcRegReady(src);
                if (event.path == FpWakePath::M2)
                    stats.fpM2Hits++;
                else
                    stats.fpM4Hits++;
                DPRINTF(Schedule,
                    "FPA wake tid=%u seq=%llu attempt=%llu src=%u from=%llu/%llu path=%s\n",
                    consumer->threadNumber, consumer->seqNum, consumer->fpWakeAttempt,
                    src, producer->seqNum, token->producer.attempt,
                    event.path == FpWakePath::M2 ? "M2" : "M4");
                iq->addIfReady(consumer);
            }
        }
    }
}

void
Scheduler::fpProcessWakes()
{
    if (!enableFmaRtlAlignment)
        return;
    std::vector<FpWakeEvent> due;
    while (!fpWakeDue.empty() && fpWakeDue.begin()->first <= curTick()) {
        due.push_back(std::move(fpWakeDue.begin()->second));
        fpWakeDue.erase(fpWakeDue.begin());
    }
    // Resolve upstream confirmation and cancellation before derived M4 wakes
    // and before any IQ slot can be released in this tick.
    std::stable_sort(due.begin(), due.end(), [](const auto& a, const auto& b) {
        if (a.path != b.path)
            return a.path == FpWakePath::M2;
        return a.inst->seqNum < b.inst->seqNum;
    });
    std::vector<FpWakeupState::Token> candidates;
    for (const auto& event : due) {
        if (!fpWakeState.current(event.token) || event.token->faulted || event.inst->isSquashed())
            continue;
        if (event.path == FpWakePath::M2) {
            if (fpWakeState.broadcastM2(event.token->producer, curTick())) {
                DPRINTF(Schedule, "FPA confirm tid=%u seq=%llu attempt=%llu\n",
                    event.token->producer.tid, event.token->producer.seq,
                    event.token->producer.attempt);
                fpBroadcast(event);
            } else {
                stats.fpConfirmFailed++;
                fpCancel(event.inst);
            }
        } else {
            candidates.push_back(event.token);
        }
    }
    const auto winners = FpWakeupState::arbitrateM4(candidates, curTick());
    stats.fpM4ArbFailed += candidates.size() - winners.size();
    for (const auto& event : due) {
        if (event.path == FpWakePath::M4 &&
            std::find(winners.begin(), winners.end(), event.token) != winners.end())
            fpBroadcast(event);
    }
}

bool
Scheduler::fpAdmission(const DynInstPtr& inst)
{
    if (!enableFmaRtlAlignment)
        return true;
    if (inst->isSquashed() || inst->canceled())
        return false;
    auto it = fpRecords.find(inst->seqNum);
    if (it == fpRecords.end())
        return true;
    auto& record = it->second;
    // Faulting older work is recovered by the existing precise-trap squash.
    // Keep younger work in its IQ until that recovery; no data was produced.
    if (record.output && record.output->faulted)
        return false;
    for (const auto& binding : record.sources)
        if (binding && binding->token->faulted)
            return false;
    bool valid = true;
    for (const auto& binding : record.sources)
        if (binding && (!FpWakeupState::usable(binding) || !binding->token->confirmed))
            valid = false;
    if (record.output)
        valid &= fpWakeState.admit(record.output->producer);
    if (!valid) {
        stats.fpConfirmFailed++;
        fpCancel(inst);
        inst->issueQue->addIfReady(inst);
        return false;
    }
    DPRINTF(Schedule, "FPA admit tid=%u seq=%llu attempt=%llu\n",
            inst->threadNumber, inst->seqNum, inst->fpWakeAttempt);
    return true;
}

void
Scheduler::fpAtFU(const DynInstPtr& inst)
{
    if (!enableFmaRtlAlignment)
        return;
    auto it = fpRecords.find(inst->seqNum);
    if (it == fpRecords.end())
        return;
    for (const auto& binding : it->second.sources) {
        if (!binding)
            continue;
        fpWakeState.select(binding, curTick(), 2 * cpu->clockPeriod());
        fpDataDue.emplace(binding->deadline, FpDataCheck{binding, binding->consumer});
        DPRINTF(Schedule,
            "FPA read tid=%u seq=%llu attempt=%llu src=%u from=%llu/%llu path=%s deadline=%llu\n",
            inst->threadNumber, inst->seqNum, inst->fpWakeAttempt, binding->source,
            binding->token->producer.seq, binding->token->producer.attempt,
            binding->path == FpWakePath::M2 ? "M2" : "M4", binding->deadline);
    }
    DPRINTF(Schedule, "FPA atfu tid=%u seq=%llu attempt=%llu\n",
            inst->threadNumber, inst->seqNum, inst->fpWakeAttempt);
}

bool
Scheduler::fpNeedsDeferredRead(const DynInstPtr& inst) const
{
    if (!enableFmaRtlAlignment || inst->opClass() != FloatDivOp)
        return false;
    auto it = fpRecords.find(inst->seqNum);
    if (it == fpRecords.end())
        return false;
    for (const auto& binding : it->second.sources) {
        if (binding && !binding->token->dataReady &&
            (binding->token->faulted || binding->token->dataCycle == curTick()))
            return true;
    }
    return false;
}

bool
Scheduler::fpExecutionReady(const DynInstPtr& inst)
{
    if (!enableFmaRtlAlignment)
        return true;
    if (inst->canceled())
        return false;
    auto it = fpRecords.find(inst->seqNum);
    if (it == fpRecords.end())
        return true;
    for (const auto& binding : it->second.sources) {
        if (!binding)
            continue;
        if (binding->canceled || binding->token->canceled || binding->token->faulted)
            return false;
        panic_if(!binding->token->dataReady,
            "FMA RTL alignment: missing real operand before native execution tid=%u seq=%llu attempt=%llu src=%u",
            inst->threadNumber, inst->seqNum, inst->fpWakeAttempt, binding->source);
    }
    return true;
}

void
Scheduler::fpCheckDeadlines()
{
    if (!enableFmaRtlAlignment)
        return;
    while (!fpDataDue.empty() && fpDataDue.begin()->first <= curTick()) {
        const auto check = fpDataDue.begin()->second;
        auto binding = check.binding;
        fpDataDue.erase(fpDataDue.begin());
        auto it = fpRecords.find(check.consumer.seq);
        if (it == fpRecords.end() || it->second.attempt != check.consumer.attempt ||
            it->second.inst->isSquashed() || binding->canceled)
            continue;
        const auto status = FpWakeupState::checkData(binding, curTick());
        if (status == FpDataStatus::FaultPending)
            continue;
        panic_if(status != FpDataStatus::Ready,
            "FMA RTL alignment: admitted operand missed deadline tid=%u seq=%llu attempt=%llu src=%u",
            binding->consumer.tid, binding->consumer.seq, binding->consumer.attempt, binding->source);
    }
}

void
Scheduler::fpComplete(const DynInstPtr& inst)
{
    if (!enableFmaRtlAlignment || inst->isSquashed() || inst->canceled())
        return;
    auto it = fpRecords.find(inst->seqNum);
    if (it == fpRecords.end() || !it->second.output)
        return;
    if (inst->fault != NoFault) {
        fpWakeState.fault(fpIdentity(inst));
        DPRINTF(Schedule, "FPA fault tid=%u seq=%llu attempt=%llu\n",
            inst->threadNumber, inst->seqNum, inst->fpWakeAttempt);
        return;
    }
    if (fpWakeState.complete(fpIdentity(inst)))
        DPRINTF(Schedule, "FPA complete tid=%u seq=%llu attempt=%llu\n",
                inst->threadNumber, inst->seqNum, inst->fpWakeAttempt);
}

bool
Scheduler::fpCancel(const DynInstPtr& inst, bool recovering)
{
    if (!enableFmaRtlAlignment)
        return false;
    auto first = fpRecords.find(inst->seqNum);
    if (first == fpRecords.end())
        return false;
    bool needsSquash = false;
    std::vector<DynInstPtr> pending{inst};
    std::unordered_set<const DynInst*> seen;
    while (!pending.empty()) {
        auto victim = pending.back();
        pending.pop_back();
        // Split store address/data uops share a sequence number. Ordinary
        // dependents must therefore be visited by instruction identity.
        if (!seen.insert(victim.get()).second)
            continue;
        auto it = fpRecords.find(victim->seqNum);
        FpRecord* record = it != fpRecords.end() && it->second.inst == victim ?
            &it->second : nullptr;
        if (record && record->canceled)
            continue;
        if (record)
            record->canceled = true;
        panic_if(victim->isIssued() && !recovering && !victim->isSquashed(),
            "FMA RTL alignment: cancellation after IQ release tid=%u seq=%llu attempt=%llu",
            victim->threadNumber, victim->seqNum, victim->fpWakeAttempt);
        if (record && record->output) {
            for (const auto& user : record->output->users) {
                auto binding = user.lock();
                if (!binding)
                    continue;
                auto downstream = fpRecords.find(binding->consumer.seq);
                if (downstream != fpRecords.end() &&
                    downstream->second.attempt == binding->consumer.attempt &&
                    binding->source < downstream->second.sources.size() &&
                    downstream->second.sources[binding->source] == binding)
                    pending.push_back(downstream->second.inst);
            }
            fpWakeState.cancel(record->output->producer);
        }
        // An ordinary speculative wake may already have reached dependents
        // outside the FP token graph (for example FADD -> store data). Retire
        // its pending events and revoke exactly those source-ready bits too.
        auto events = specWakeEvents.find(victim->seqNum);
        if (events != specWakeEvents.end()) {
            for (auto event : events->second)
                cpu->deschedule(event);
            specWakeEvents.erase(events);
        }
        for (int i = 0; i < victim->numDestRegs(); ++i) {
            auto dst = victim->renamedDestIdx(i);
            if (dst->isFixedMapping())
                continue;
            earlyScoreboard[dst->flatIndex()] = false;
            for (auto iq : issueQues) {
                for (auto& edge : iq->subDepGraph[dst->flatIndex()]) {
                    const auto source = edge.first;
                    const auto& consumer = edge.second;
                    if (consumer->threadNumber != victim->threadNumber ||
                        consumer->seqNum <= victim->seqNum ||
                        !consumer->readySrcIdx(source))
                        continue;
                    consumer->clearSrcRegReady(source);
                    if (!consumer->lldpInputs.empty()) {
                        consumer->lldpInputs[source] = {};
                        consumer->selectLldpChain();
                    }
                    pending.push_back(consumer);
                }
            }
        }
        // A baseline VP recovery may cancel released work; it must use its
        // existing squash path. Never insert such instructions back into IQ.
        const bool released = victim->isIssued();
        needsSquash |= released && !victim->isSquashed();
        if (record) {
            for (auto& binding : record->sources) {
                if (!binding)
                    continue;
                auto previous = binding;
                const bool usable = FpWakeupState::usable(previous);
                previous->canceled = true;
                binding.reset();
                if (!usable && victim->readySrcIdx(previous->source))
                    victim->clearSrcRegReady(previous->source);
                if (usable) {
                    auto next = fpIdentity(victim);
                    ++next.attempt;
                    binding = fpWakeState.bind(next, previous->source,
                        previous->token, previous->token->m2Broadcast ? FpWakePath::M2 : previous->path);
                }
            }
        }
        if (record && record->output) {
            for (auto reg : record->output->registers) {
                auto owner = fpRegisters.find(reg);
                if (owner != fpRegisters.end() && owner->second == record->output)
                    fpRegisters.erase(owner);
            }
            fpWakeState.erase(record->output->producer);
            record->output.reset();
        }
        ++victim->fpWakeAttempt;
        if (record)
            record->attempt = victim->fpWakeAttempt;
        victim->setCancel();
        // Nonpipelined reservations do not expire by shifting. Release them
        // before clearing Scheduled, including ordinary dependent attempts.
        if (!released && victim->isScheduled() && victim->issueportid >= 0 &&
            !opPipelined[victim->opClass()])
            victim->issueQue->clearBusy(victim->issueportid);
        if (!released && !victim->isSquashed()) {
            victim->clearScheduled();
            victim->clearArbFailed();
            stats.fpRetries++;
        }
        DPRINTF(Schedule, "FPA cancel tid=%u seq=%llu attempt=%llu released=%u\n",
                victim->threadNumber, victim->seqNum, victim->fpWakeAttempt, released);
    }
    return needsSquash;
}

void
Scheduler::fpForget(const DynInstPtr& inst)
{
    if (!enableFmaRtlAlignment)
        return;
    auto it = fpRecords.find(inst->seqNum);
    if (it == fpRecords.end())
        return;
    if (it->second.output) {
        auto token = it->second.output;
        for (auto reg : token->registers) {
            auto owner = fpRegisters.find(reg);
            if (owner != fpRegisters.end() && owner->second == token)
                fpRegisters.erase(owner);
        }
        fpWakeState.erase(token->producer);
    }
    fpRecords.erase(it);
}

} // namespace o3
} // namespace gem5
