/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <set>

#include "base/logging.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/issue_queue.hh"
#include "debug/FpWakeup.hh"

namespace gem5
{
namespace o3
{

void
Scheduler::preDumpStats()
{
    statistics::Group::preDumpStats();
    unsigned ownedSlots = 0;
    for (auto iq : issueQues) {
        for (const auto &inst : iq->instList)
            ownedSlots += inst->fpWake.ownsIqSlot;
    }
    stats.fpOwnedIqSlots = ownedSlots;
    stats.fpPendingWakeups = fpWakeups.size();
    stats.fpPendingValidations = fpValidations.size();
    stats.fpWritebackReservations = fpWritebacks.size() + fpOutputSlots.size();
    stats.fpDividerOwners = fpExecution.size();
}

fpw::Cycle
Scheduler::fpCycle() const
{
    return static_cast<uint64_t>(cpu->curCycle());
}

bool
Scheduler::managesFp(const DynInstPtr& inst) const
{
    return fpWakeEnabled && !inst->isVector() && !inst->isMemRef() &&
        (inst->isFloating() || inst->opClass() == Int2FpOp);
}

bool
Scheduler::fpDestination(const DynInstPtr& inst) const
{
    if (inst->isVector())
        return false;
    for (int i = 0; i < inst->numDestRegs(); ++i) {
        auto reg = inst->renamedDestIdx(i);
        if (!reg->isFixedMapping() && reg->isFloatReg())
            return true;
    }
    return false;
}

int
Scheduler::fpAddendSource(const DynInstPtr& inst) const
{
    if (inst->isVector() || inst->opClass() != FloatMultAccOp)
        return -1;
    unsigned ordinal = 0;
    for (int i = 0; i < inst->numSrcRegs(); ++i) {
        if (inst->srcRegIdx(i).isFloatReg() && ordinal++ == 2)
            return i;
    }
    return -1;
}

bool
Scheduler::fpDivider(const DynInstPtr& inst) const
{
    return !inst->isVector() &&
        (inst->opClass() == FloatDivOp || inst->opClass() == FloatSqrtOp);
}

unsigned
Scheduler::fpEndpoint(const DynInstPtr& inst) const
{
    auto iq = inst->issueQue;
    return iq->fpWbPort >= 0 ? iq->fpWbPort : iq->getId();
}

bool
Scheduler::fpAttemptValid(const DynInstPtr& inst, uint64_t attempt) const
{
    return inst && (!fpWakeEnabled ||
        (inst->fpWake.attempt == attempt && !inst->isSquashed() &&
         !inst->canceled()));
}

void
Scheduler::invalidateFp(const DynInstPtr& inst)
{
    if (!fpWakeEnabled)
        return;
    auto &state = inst->fpWake;
    // Invalidation is exceptional; scan the bounded live event sets here,
    // rather than retaining squashed instructions until a future deadline.
    auto purge = [&](auto &queue) {
        for (auto it = queue.begin(); it != queue.end();) {
            if (it->second.inst == inst && it->second.attempt == state.attempt)
                it = queue.erase(it);
            else
                ++it;
        }
    };
    purge(fpWakeups);
    purge(fpValidations);
    auto pending = specWakeEvents.find(inst->seqNum);
    if (pending != specWakeEvents.end()) {
        auto &events = pending->second;
        for (auto it = events.begin(); it != events.end();) {
            auto event = *it;
            if (event->attempt != state.attempt) {
                ++it;
                continue;
            }
            it = events.erase(it);
            if (event->scheduled()) {
                // AutoDelete events are released by deschedule itself.
                cpu->deschedule(event);
            }
        }
        if (events.empty())
            specWakeEvents.erase(pending);
    }
    if (state.output) {
        auto token = state.output;
        token->cancel();
        fpWritebacks.release(token->producer);
        fpOutputSlots.release(token->producer);
        fpExecution.release(token->producer);
        auto preg = token->producer.preg;
        if (preg < fpProducers.size() && fpProducers[preg] == token) {
            fpProducers[preg].reset();
            earlyScoreboard[preg] = false;
            bypassScoreboard[preg] = false;
        }
        state.output.reset();
    }
    ++state.attempt;
    inst->setFdivLatency(-1);
    state.selected = fpw::Never;
    state.atFu = fpw::Never;
    state.validationDone = false;
    state.divResource = false;
}

bool
Scheduler::prepareFp(const DynInstPtr& inst, bool replay, unsigned port)
{
    // Value prediction owns these loads' readiness and squash recovery. Their
    // PRF value is intentionally usable before the real load returns; attaching
    // an actual-result token would revoke that independent native promise.
    if (inst->isLoad() && inst->vpApplied)
        return true;
    if (!managesFp(inst) && !fpDestination(inst))
        return true;
    auto &state = inst->fpWake;
    if (state.selected == fpCycle())
        return true;
    if (state.output) {
        cancelFpDependents(inst);
        invalidateFp(inst);
    }
    state.managed = managesFp(inst);
    const auto selected = fpCycle();
    const auto depth = inst->issueQue->getIssueStages();
    // Native fast replay enters instsToFu directly at this cycle's exit.
    // Its functional/S1 timing skips the ordinary IQ delay registers.
    const auto now = replay ? selected - depth : selected;
    int dest = -1;
    for (int i = 0; i < inst->numDestRegs(); ++i) {
        auto reg = inst->renamedDestIdx(i);
        if (!reg->isFixedMapping() && (reg->isFloatReg() || reg->isIntReg())) {
            dest = reg->flatIndex();
            break;
        }
    }
    const bool publishes = dest >= 0;
    // An x0 conversion still occupies the FEX result mux even though it
    // publishes no register value. Fixed register zero has no IQ dependents.
    if (dest < 0 && state.managed)
        dest = 0;
    if (dest < 0)
        return true;
    auto token = std::make_shared<fpw::WakeToken>();
    token->producer = {static_cast<uint32_t>(inst->threadNumber), inst->seqNum, state.attempt,
                       static_cast<uint32_t>(dest)};
    token->wbPort = fpEndpoint(inst);
    token->outputLane = inst->issueQue->getId() * 8 + port;
    token->load = inst->isLoad();
    unsigned m2Distance = 4;
    if (inst->opClass() == FloatAddOp ||
        (inst->opClass() == FloatMiscOp && fpDestination(inst)) ||
        ((inst->opClass() == FloatCmpOp) && fpDestination(inst))) {
        m2Distance = 2;
    } else if (inst->opClass() == FloatMultOp) {
        m2Distance = 3;
    }
    const auto latency = getOpLatency(inst);
    token->readyCycle = now + depth + (token->load ? 3 : latency);
    token->cancelSafeCycle = token->readyCycle;
    // WB arbitration follows the RTL-facing M2 horizon. The native
    // functional result may materialize later (notably scalar MUL).
    token->wbCycle = token->load ? now + depth + 4 : now + m2Distance + 2;
    if (fpDivider(inst)) {
        if (!fpExecution.acquire(inst->issueQue->getId(), token->producer)) {
            ++stats.fpDividerStalls;
            return false;
        }
        state.divResource = true;
    }
    // A FEX has one result lane even when instructions write different
    // register domains. Its virtual slot must not use native MUL opLat.
    if (!token->load && !fpDivider(inst) &&
        !fpOutputSlots.reserve(token->outputLane, token->wbCycle,
                               token->producer)) {
        ++stats.fpWbConflicts;
        return false;
    }
    // The load hit path predicts S2; replay revokes that reservation.
    // Dynamic divider results reserve at their completion-ahead notification.
    if (!fpDivider(inst) && fpDestination(inst) &&
        !fpWritebacks.reserve(token->wbPort, token->wbCycle,
                              token->producer)) {
        fpExecution.release(token->producer);
        fpOutputSlots.release(token->producer);
        state.divResource = false;
        ++stats.fpWbConflicts;
        return false;
    }
    state.selected = selected;
    state.output = token;
    if (publishes)
        fpProducers[dest] = token;
    if (token->load) {
        // Native load S0 admission is selection + depth + 1. M4 is
        // broadcast one cycle earlier; the S1 callback confirms it as M2.
        if (fpLoadM4Enabled && !replay)
            token->m4Cycle = now + depth;
        token->m2Cycle = now + depth + 2;
        token->cancelSafeCycle = token->readyCycle + 1;
    } else if (!fpDivider(inst)) {
        token->m2Cycle = now + m2Distance;
        fpWakeups.emplace(token->m2Cycle,
                          FpWork{inst, state.attempt, fpw::Phase::M2});
        bool unresolvedLoad = false;
        for (const auto &source : state.sources) {
            unresolvedLoad |= source.token && source.token->load &&
                source.phase == fpw::Phase::M4 &&
                now <= source.token->cancelSafeCycle;
        }
        if (!unresolvedLoad && (inst->opClass() == FloatMultOp ||
                                inst->opClass() == FloatMultAccOp)) {
            token->m4Cycle = now +
                (inst->opClass() == FloatMultOp ? 1 : 2);
        }
    }
    if (token->m4Cycle != fpw::Never) {
        fpWakeups.emplace(token->m4Cycle,
                          FpWork{inst, state.attempt, fpw::Phase::M4});
    }
    DPRINTF(FpWakeup,
            "event=select cycle=%llu tid=%u seq=%llu attempt=%llu pc=%#lx iq=%s\n",
            selected, inst->threadNumber, inst->seqNum, state.attempt,
            inst->pcState().instAddr(), inst->issueQue->getName());
    return true;
}

void
Scheduler::broadcastFp(const DynInstPtr& inst, fpw::Phase phase, IssueQue* target)
{
    auto token = inst->fpWake.output;
    if (!token || token->cancelled)
        return;
    const auto preg = token->producer.preg;
    const auto &destinations = fpWakeMatrix[inst->issueQue->getId()];
    const size_t count = target ? 1 : destinations.size();
    for (size_t i = 0; i < count; ++i) {
        auto iq = target ? target : destinations[i];
        if (phase == fpw::Phase::M2 &&
            !iq->portFuDescs.front().test(FloatAddOp) &&
            !iq->portFuDescs.front().test(Int2FpOp) &&
            fpCycle() + iq->getIssueStages() < token->readyCycle) {
            // Unmanaged paths still read/check at the native issue exit.
            // Delay their visibility rather than bypass that safety check.
            fpWakeups.emplace(token->readyCycle - iq->getIssueStages(),
                FpWork{inst, inst->fpWake.attempt, phase, iq});
            continue;
        }
        for (auto &edge : iq->subDepGraph[preg]) {
            const unsigned src = edge.first;
            auto consumer = edge.second;
            if (consumer->threadNumber != inst->threadNumber ||
                consumer->isSquashed() || consumer->isExecuted())
                continue;
            if (phase == fpw::Phase::M4 &&
                (fpAddendSource(consumer) != static_cast<int>(src) ||
                 consumer->readySrcIdx(src)))
                continue;
            auto &bindings = consumer->fpWake.sources;
            if (bindings.size() != consumer->numSrcRegs())
                bindings.resize(consumer->numSrcRegs());
            // An in-flight M4 consumer retains its late-source deadline
            // when its token is confirmed by M2.
            if (!(bindings[src].token == token &&
                  bindings[src].phase == fpw::Phase::M4)) {
                bindings[src] = {token, phase, fpCycle(), phase == fpw::Phase::M4};
            } else {
                // M4 grants two early selection opportunities. Once M2
                // arrives, an unselected entry uses ordinary operand timing;
                // its load-M4 provenance still survives independently.
                bindings[src].observeM2(consumer->isScheduled() ||
                    consumer->isIssued() || consumer->fpWake.selected != fpw::Never);
            }
            if (phase == fpw::Phase::M4) {
                consumer->markSrcRegReady(src);
                iq->addIfReady(consumer);
                fpValidations.emplace(token->m2Cycle,
                    FpWork{consumer, consumer->fpWake.attempt});
                ++stats.fpM4Wakeups;
            } else {
                ++stats.fpM2Wakeups;
            }
            DPRINTF(FpWakeup,
                "event=wake cycle=%llu tid=%u producer=%llu consumer=%llu "
                "src=%u phase=%s producer_select=%llu consumer_enq=%llu "
                "producer_attempt=%llu consumer_attempt=%llu\n",
                fpCycle(), inst->threadNumber, inst->seqNum, consumer->seqNum,
                src, phase == fpw::Phase::M4 ? "M4" : "M2",
                inst->fpWake.selected, consumer->fpWake.enqueued,
                inst->fpWake.attempt, consumer->fpWake.attempt);
        }
        if (phase == fpw::Phase::M2)
            iq->wakeUpDependents(inst, true);
    }
}

void
Scheduler::processFpWakeups()
{
    const auto now = fpCycle();
    // One M4 per physical write port. Older FMA candidates win over MUL;
    // ordinary M2 retains its independent bandwidth.
    std::map<unsigned, FpWork> m4;
    while (!fpWakeups.empty() && fpWakeups.begin()->first <= now) {
        auto work = fpWakeups.begin()->second;
        fpWakeups.erase(fpWakeups.begin());
        if (!fpAttemptValid(work.inst, work.attempt) ||
            !work.inst->fpWake.output)
            continue;
        auto token = work.inst->fpWake.output;
        if (token->cancelled)
            continue;
        if (work.phase == fpw::Phase::M2) {
            if (fpDivider(work.inst) &&
                (!fpOutputSlots.reserve(token->outputLane, token->readyCycle,
                                        token->producer) ||
                 !fpWritebacks.reserve(token->wbPort, token->readyCycle,
                                       token->producer))) {
                fpOutputSlots.release(token->producer);
                fpWritebacks.release(token->producer);
                ++stats.fpWbConflicts;
                ++token->readyCycle;
                token->wbCycle = token->readyCycle;
                token->m2Cycle = now + 1;
                token->cancelSafeCycle = token->readyCycle;
                fpWakeups.emplace(now + 1, work);
                DPRINTF(FpWakeup,
                    "event=retime cycle=%llu tid=%u seq=%llu attempt=%llu ready_cycle=%llu\n",
                    now, work.inst->threadNumber, work.inst->seqNum, work.attempt,
                    token->readyCycle);
                continue;
            }
            token->confirm();
            DPRINTF(FpWakeup,
                    "event=confirm cycle=%llu tid=%u seq=%llu attempt=%llu\n",
                    now, work.inst->threadNumber, work.inst->seqNum, work.attempt);
            broadcastFp(work.inst, work.phase, work.target);
        } else {
            auto found = m4.find(token->wbPort);
            if (found == m4.end() ||
                (work.inst->opClass() == FloatMultAccOp &&
                 found->second.inst->opClass() != FloatMultAccOp) ||
                (work.inst->opClass() == found->second.inst->opClass() &&
                 work.inst->seqNum < found->second.inst->seqNum)) {
                m4[token->wbPort] = work;
            }
        }
    }
    for (auto &candidate : m4)
        broadcastFp(candidate.second.inst, fpw::Phase::M4);
}

void
Scheduler::inheritFpWakeups(const DynInstPtr& inst)
{
    // Direct M2/M2D1 forwarding is finite, but dispatch busy-table readiness
    // persists after a confirmed M2 until allocation/cancel. M4 itself is
    // never inherited by an instruction absent from the broadcast IQ.
    for (int src = 0; src < inst->numSrcRegs(); ++src) {
        auto reg = inst->renamedSrcIdx(src);
        if (reg->isFixedMapping())
            continue;
        auto token = fpProducers[reg->flatIndex()];
        const bool pendingLoad = token && token->load && token->dataReady &&
            fpCycle() <= token->cancelSafeCycle;
        if (token && token->producer.tid == inst->threadNumber &&
            token->producer.seq < inst->seqNum &&
            !token->cancelled &&
            (pendingLoad || (!scoreboard[reg->flatIndex()] &&
                            token->canInherit(fpCycle())))) {
            if (!inst->fpWake.managed &&
                !token->canUse(fpCycle() + inst->issueQue->getIssueStages()))
                continue;
            inst->fpWake.sources[src] = {token, fpw::Phase::M2, fpCycle()};
            DPRINTF(FpWakeup,
                "event=inherit cycle=%llu tid=%u producer=%llu consumer=%llu "
                "src=%u phase=M2 producer_m2=%llu producer_attempt=%llu "
                "consumer_attempt=%llu data_ready=%u\n",
                fpCycle(), inst->threadNumber, token->producer.seq, inst->seqNum,
                src, token->m2Cycle, token->producer.attempt, inst->fpWake.attempt,
                token->dataReady);
            if (!inst->readySrcIdx(src))
                inst->markSrcRegReady(src);
            auto &edges = inst->issueQue->subDepGraph[reg->flatIndex()];
            if (pendingLoad && std::none_of(edges.begin(), edges.end(),
                [&](const auto &edge) { return edge.first == src && edge.second == inst; }))
                edges.emplace_back(src, inst);
        }
    }
}

bool
Scheduler::checkFpAdmission(const DynInstPtr& inst)
{
    const auto atFu = fpCycle() + 1;
    for (int src = 0; src < inst->numSrcRegs(); ++src) {
        auto reg = inst->renamedSrcIdx(src);
        if (reg->isFixedMapping() || bypassScoreboard[reg->flatIndex()])
            continue;
        const auto &binding = inst->fpWake.sources[src];
        const auto use = atFu + ((binding.lateUse && fpAddendSource(inst) == src) ? 2 : 0);
        if (binding.token && binding.token->canUse(use))
            continue;
        if (binding.token && binding.token->cancelled) {
            cancelFp(inst, binding.token->load ? fpw::ReplayReason::LoadCancel :
                                                fpw::ReplayReason::MissingConfirmation);
            return false;
        }
        auto producer = getInstByDstReg(reg->flatIndex(),
                                        inst->threadNumber, inst->seqNum);
        if (producer && producer->isLoad()) {
            loadCancel(producer);
            return false;
        }
        panic("FP source misses its use deadline: consumer %llu src %d\n",
              inst->seqNum, src);
    }
    return true;
}

void
Scheduler::fpFuAccepted(const DynInstPtr& inst, uint32_t& opLatency)
{
    if (!fpWakeEnabled)
        return;
    auto &state = inst->fpWake;
    state.atFu = fpCycle();
    auto token = state.output;
    if (token && !token->load) {
        auto actual = fpCycle() + opLatency - 1;
        if (fpDivider(inst)) {
            token->readyCycle = actual;
            token->wbCycle = actual;
            token->cancelSafeCycle = actual;
        }
        panic_if(actual != token->readyCycle,
                 "FP completion prediction changed for %llu (%llu != %llu)\n",
                 inst->seqNum, actual, token->readyCycle);
        if (fpDivider(inst)) {
            token->m2Cycle = actual >= fpCycle() + 2 ? actual - 2 : fpCycle();
            fpWakeups.emplace(token->m2Cycle,
                FpWork{inst, state.attempt, fpw::Phase::M2});
        }
    }
    if (state.managed) {
        fpValidations.emplace(fpCycle(), FpWork{inst, state.attempt});
        fpValidations.emplace(fpCycle() + 2, FpWork{inst, state.attempt});
    }
    DPRINTF(FpWakeup,
            "event=fu cycle=%llu tid=%u seq=%llu attempt=%llu managed=%u latency=%u\n",
            fpCycle(), inst->threadNumber, inst->seqNum, state.attempt,
            state.managed, opLatency);
}

void
Scheduler::fpLoadWakeup(const DynInstPtr& inst)
{
    auto token = inst->fpWake.output;
    if (!token || token->cancelled)
        return;
    const auto ready = fpCycle() + 1;
    const auto wb = fpCycle() + 2;
    if (wb != token->wbCycle) {
        fpWritebacks.release(token->producer);
        if (!fpWritebacks.reserve(token->wbPort, wb, token->producer)) {
            ++stats.fpWbConflicts;
            cancelFpDependents(inst);
            invalidateFp(inst);
            return;
        }
    }
    token->m2Cycle = fpCycle();
    token->readyCycle = ready;
    token->wbCycle = wb;
    token->cancelSafeCycle = fpCycle() + 2;
    // The LSQ's S1 notification remains speculative through S3.
    token->confirm();
    broadcastFp(inst, fpw::Phase::M2);
}

void
Scheduler::cancelFpDependents(const DynInstPtr& producer)
{
    std::vector<DynInstPtr> pending{producer};
    std::set<InstSeqNum> visited;
    while (!pending.empty()) {
        auto parent = pending.back();
        pending.pop_back();
        if (!visited.insert(parent->seqNum).second)
            continue;
        auto token = parent->fpWake.output;
        if (token)
            token->cancel();
        for (int dst = 0; dst < parent->numDestRegs(); ++dst) {
            auto reg = parent->renamedDestIdx(dst);
            if (reg->isFixedMapping())
                continue;
            for (auto iq : issueQues) {
                for (auto &edge : iq->subDepGraph[reg->flatIndex()]) {
                    auto consumer = edge.second;
                    const auto src = edge.first;
                    if (consumer->threadNumber != parent->threadNumber ||
                        consumer->isSquashed() || consumer->canceled())
                        continue;
                    if (token && (consumer->fpWake.sources.size() <= src ||
                        consumer->fpWake.sources[src].token != token))
                        continue;
                    if (!consumer->readySrcIdx(src))
                        continue;
                    if (consumer->fpWake.managed) {
                        cancelFp(consumer, token && token->load ?
                            fpw::ReplayReason::LoadCancel :
                            fpw::ReplayReason::MissingConfirmation);
                    } else if (!consumer->isIssued()) {
                        // Preserve the native pre-issue cancellation path for
                        // integer/store-data descendants of FP producers.
                        iq->cancel(consumer);
                        consumer->clearSrcRegReady(src);
                        pending.push_back(consumer);
                        invalidateFp(consumer);
                    }
                }
            }
        }
    }
}

void
Scheduler::cancelFp(const DynInstPtr& inst, fpw::ReplayReason reason)
{
    if (inst->isSquashed() || inst->canceled())
        return;
    panic_if(inst->isExecuted(),
             "FP cancellation after functional execution for %llu\n", inst->seqNum);
    const auto cancelledAttempt = inst->fpWake.attempt;
    cancelFpDependents(inst);
    invalidateFp(inst);
    inst->setCancel();
    inst->clearScheduled();
    inst->clearIssued();
    for (unsigned src = 0; src < inst->fpWake.sources.size(); ++src) {
        auto &binding = inst->fpWake.sources[src];
        if (binding.token && (binding.token->cancelled ||
            (binding.phase == fpw::Phase::M4 && !binding.token->confirmed &&
             fpCycle() >= binding.token->m2Cycle))) {
            if (inst->readySrcIdx(src))
                inst->clearSrcRegReady(src);
            binding = {};
        }
    }
    ++stats.fpReplays;
    DPRINTF(FpWakeup,
            "event=cancel cycle=%llu tid=%u seq=%llu attempt=%llu reason=%u next_attempt=%llu\n",
            fpCycle(), inst->threadNumber, inst->seqNum, cancelledAttempt,
            static_cast<unsigned>(reason), inst->fpWake.attempt);
    inst->issueQue->addIfReady(inst);
}

void
Scheduler::validateFpSources()
{
    if (!fpWakeEnabled)
        return;
    processFpWakeups();
    const auto now = fpCycle();
    while (!fpValidations.empty() && fpValidations.begin()->first <= now) {
        auto work = fpValidations.begin()->second;
        fpValidations.erase(fpValidations.begin());
        auto inst = work.inst;
        if (!fpAttemptValid(inst, work.attempt))
            continue;
        auto &state = inst->fpWake;
        if (work.cleanup) {
            auto token = state.output;
            if (token && !token->cancelled) {
                for (auto iq : issueQues) {
                    auto &edges = iq->subDepGraph[token->producer.preg];
                    if (fpProducers[token->producer.preg] == token) {
                        // No physical-register reuse: all of this producer's
                        // WB dependencies have passed the cancellation window.
                        edges.clear();
                        continue;
                    }
                    edges.erase(std::remove_if(edges.begin(), edges.end(),
                        [&](const auto &edge) {
                            const auto &sources = edge.second->fpWake.sources;
                            return edge.second->isCommitted() || edge.second->isSquashed() ||
                                (sources.size() > edge.first && sources[edge.first].token == token);
                        }), edges.end());
                }
            }
            continue;
        }
        bool replay = false;
        auto safe = state.atFu;
        for (unsigned src = 0; src < state.sources.size(); ++src) {
            const auto &binding = state.sources[src];
            auto token = binding.token;
            if (!token)
                continue;
            if (token->cancelled || (binding.phase == fpw::Phase::M4 &&
                now >= token->m2Cycle && !token->confirmed)) {
                cancelFp(inst, token->cancelled && token->load ?
                    fpw::ReplayReason::LoadCancel : fpw::ReplayReason::MissingConfirmation);
                replay = true;
                break;
            }
            if (state.atFu == fpw::Never)
                continue;
            const auto use = state.atFu +
                ((binding.lateUse &&
                  fpAddendSource(inst) == static_cast<int>(src)) ? 2 : 0);
            safe = std::max(safe, use);
            if (now == use) {
                DPRINTF(FpWakeup,
                    "event=use cycle=%llu tid=%u seq=%llu attempt=%llu "
                    "src=%u phase=%s ready=%u producer=%llu producer_attempt=%llu\n",
                    now, inst->threadNumber, inst->seqNum, state.attempt, src,
                    binding.lateUse ? "M4" : "M2",
                    token->dataReady, token->producer.seq, token->producer.attempt);
            }
            if (token->load)
                safe = std::max(safe, token->cancelSafeCycle);
            if (now >= use && !token->dataReady &&
                !bypassScoreboard[inst->renamedSrcIdx(src)->flatIndex()]) {
                if (token->load) {
                    token->cancel();
                    cancelFp(inst, fpw::ReplayReason::LoadCancel);
                    replay = true;
                    break;
                }
                panic("FP data unavailable at source use: consumer %llu src %u\n",
                      inst->seqNum, src);
            }
        }
        if (replay || state.atFu == fpw::Never || state.validationDone)
            continue;
        if (now < safe) {
            fpValidations.emplace(safe, work);
        } else {
            state.validationDone = true;
            inst->issueQue->releaseFpSlot(inst);
        }
    }
}

bool
Scheduler::fpCanExecute(const DynInstPtr& inst, uint64_t attempt) const
{
    if (!fpAttemptValid(inst, attempt))
        return false;
    if (!fpWakeEnabled || !inst->fpWake.managed)
        return true;
    for (const auto &binding : inst->fpWake.sources) {
        panic_if(binding.token &&
                     (binding.token->cancelled || !binding.token->dataReady),
                 "FP executed before validating source data for %llu\n", inst->seqNum);
    }
    return true;
}

} // namespace o3
} // namespace gem5
