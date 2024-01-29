#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "base/logging.hh"
#include "base/stats/group.hh"
#include "base/stats/info.hh"
#include "base/trace.hh"
#include "cpu/func_unit.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/fu_pool.hh"
#include "cpu/o3/iew_delay_calibrator.hh"
#include "cpu/o3/limits.hh"
#include "debug/Counters.hh"
#include "debug/IQ.hh"
#include "debug/Schedule.hh"
#include "enums/OpClass.hh"
#include "issue_queue.hh"
#include "params/BaseO3CPU.hh"
#include "sim/core.hh"
#include "sim/sim_object.hh"
#include "sim/syscall_return.hh"

namespace gem5
{

namespace o3
{

bool
IssueQue::compare_priority::operator()(const QueEntry* a, const QueEntry* b)
{
    return a->inst->seqNum > b->inst->seqNum;
}

IssueQue::QueEntry::QueEntry(const DynInstPtr& inst)
    : inst(inst),
      archReadySrcs(inst->getNumSrcRegReady())
{}

IssueQue::IssueCompletion::IssueCompletion(IssueQue* que)
    : Event(Stat_Event_Pri, 0),
      que(que)
{}

void
IssueQue::IssueCompletion::process()
{
    // check resources
    // if resources not ready, replay
    auto inst = entry->inst;
    bool successful = que->checkResource(inst);
    if (successful) {
        // add to instToExecuteQue
        DPRINTF(Schedule, "[sn %lu] issue successful\n", inst->seqNum);
        que->issueToFu(entry);
    }
    else {
        // replay
        DPRINTF(Schedule, "[sn %lu] issue failed, replay\n", inst->seqNum);
        que->replayNextCycle(entry);
    }
    // dealloc
    que->freeEventList.push_back(this);
    entry = nullptr;
}

const char*
IssueQue::IssueCompletion::description() const
{
    return "Issue to Fu completion";
}
IssueQue::IssueQueStats::IssueQueStats(statistics::Group* parent, IssueQue* que, std::string name)
    : Group(parent, name.c_str()),
      ADD_STAT(issueSuccess, statistics::units::Count::get(), ""),
      ADD_STAT(issueFailed, statistics::units::Count::get(), ""),
      ADD_STAT(issueRate, statistics::units::Count::get(), ""),
      ADD_STAT(retryMem, statistics::units::Count::get(), ""),
      ADD_STAT(insertDist, statistics::units::Count::get(), ""),
      ADD_STAT(issueDist, statistics::units::Count::get(), "")
{
    insertDist.init(que->inoutPorts + 1).flags(statistics::nozero);
    issueDist.init(que->inoutPorts + 1).flags(statistics::nozero);
    issueRate.flags(statistics::total);
    issueRate = issueSuccess / (issueSuccess + issueFailed);
}

void
IssueQue::replayNextCycle(QueEntry * entry)
{
    if (entry->issued) {
        return;
    }
    iqstats->issueFailed++;

    DPRINTF(Schedule, "[sn %lu] replay reset archReadySrcs\n", entry->inst->seqNum);
    entry->scheduled = false;
    entry->inst->resetNumSrcRegReady(entry->archReadySrcs);
    replayNum[0]++;
    instNeedReplay.push_back(entry);
}

void
IssueQue::issueToFu(QueEntry * entry)
{
    if (entry->issued) {
        return;
    }
    iqstats->issueSuccess++;
    entry->issued = true;
    scheduler->addToFU(entry->inst);
    assert(instNum != 0);
    instNum--;
    DPRINTF(Schedule, "[sn %lu] instNum--\n", entry->inst->seqNum);
}

bool
IssueQue::checkResource(DynInstPtr& inst)
{
    for (int i=0; i<inst->numSrcRegs(); i++) {
        auto src = inst->renamedSrcIdx(i);
        if (src->isFixedMapping()) {
            continue;
        }
        // check bypass data ready or not
        if (!(*bypassScoreboard)[src->flatIndex()]) {
            DPRINTF(Schedule, "[sn %lu] p%lu can't get data from bypassNetwork\n", inst->seqNum, src->flatIndex());
            return false;
        }
        // check others ready or not
        // TODO
        if (!scheduler->checkFuReady(inst)) {
            DPRINTF(Schedule, "[sn %lu] fu is not ready\n");
            return false;
        }
    }
    return true;
}

void
IssueQue::wakeup(PhysRegIdPtr dst, bool speculative)
{
    if (dst->isFixedMapping()) {
        return;
    }
    if (!speculative && dst->getNumPinnedWritesToComplete() != 0) {
        return;
    }

    DPRINTF(Schedule, "was %s woken by p%lu\n", speculative ? "spec" : "wb", dst->flatIndex());
    for (auto& it: subDepGraph[dst->flatIndex()]) {
        int srcIdx = it.first;
        auto entry = it.second;

        if (speculative) {
            if (entry->readyOnce) {
                continue;
            }
        }
        else {
            entry->archReadySrcs++;
        }

        if (!entry->inst->readySrcIdx(srcIdx)) {
            entry->inst->markSrcRegReady(srcIdx);
        }

        if (entry->issued) {
            continue;
        }

        DPRINTF(Schedule, "[sn %lu] was woken\n", entry->inst->seqNum);
        addIfReady(entry);
    }

    if (!speculative) {
        subDepGraph[dst->flatIndex()].clear();
    }
}

void
IssueQue::scheduleInst()
{
    for (int i=0; i<inoutPorts && !readyInsts.empty();) {
        auto entry = readyInsts.top();
        if (entry->scheduled || entry->inst->isSquashed()) {
            readyInsts.pop();
            continue;
        }
        i++;
        entry->scheduled = true;
        selectedInst.push_back(entry);
        readyInsts.pop();
    }
    iqstats->issueDist[selectedInst.size()]++;
    for (int i=0; i<selectedInst.size(); i++) {
        DPRINTF(Schedule, "[sn %ld] was scheduled, insert into wakeupQue\n", selectedInst[i]->inst->seqNum);
        // insert into wakeupQue
        wakeupQue->insert(selectedInst[i]->inst, this);

        DPRINTF(Schedule, "start issue inst, need %d cycles\n", scheduleToExecDelay);
        assert(!freeEventList.empty());
        auto issueEvent = freeEventList.back();
        freeEventList.pop_back();
        issueEvent->reset(selectedInst[i]);
        cpu->schedule(issueEvent, cpu->clockEdge(Cycles(scheduleToExecDelay)) + 1);
    }
    selectedInst.clear();
}

void
IssueQue::addIfReady(QueEntry* entry)
{
    auto inst = entry->inst;
    if (inst->readyToIssue()) {
        if (inst->readyTick == -1) {
            inst->readyTick = curTick();
            DPRINTF(Counters, "set readyTick at addIfReady\n");
        }

        //Add the instruction to the proper ready list.
        if (inst->isMemRef()) {
            if (entry->memDepResovled) {
                DPRINTF(Schedule, "memRef Dependency was resovled can issue\n");
            }
            else {
                DPRINTF(Schedule, "memRef Dependency was not resovled can't issue\n");
                return;
            }
        }

        DPRINTF(Schedule, "[sn %lu] add to readyInstsQue\n", inst->seqNum);
        readyInsts.push(entry);
        entry->readyOnce = true;
    }
}

IssueQue::QueEntry*
IssueQue::findEntry(const DynInstPtr& inst)
{
    for (auto& it :instList) {
        if (it.inst.get() == inst.get()) {
            return &it;
        }
    }
    assert(false);
    return nullptr;
}

IssueQue::IssueQue(const IssueQueParams &params)
    : SimObject(params),
      inoutPorts(params.inoutPorts),
      iqsize(params.size),
      scheduleToExecDelay(params.scheduleToExecDelay),
      iqname(params.name),
      fuDescs(params.fuType),
      replayNum(2,2)
{
    replayNum[0] = 0;
    // build event pool
    for (int i=0; i<scheduleToExecDelay*inoutPorts*2; i++) {
        freeEventList.push_back(new IssueCompletion(this));
    }
}

void
IssueQue::setCPU(CPU* cpu)
{
    this->cpu = cpu;
    _name = cpu->name() + ".scheduler." + getName();
    iqstats = new IssueQueStats(cpu, this, "scheduler." + this->getName());
}

void
IssueQue::tick()
{
    iqstats->insertDist[instNumInsert]++;
    instNumInsert = 0;

    replayNum.advance();

    // replay reset status
    int numNeedReplay = replayNum[-1];
    for (int i=0; i<numNeedReplay; i++) {
        QueEntry* entry = instNeedReplay.front();
        instNeedReplay.pop_front();
        addIfReady(entry);// try again
    }
    replayNum[-1] = 0;

    scheduleInst();
}

bool
IssueQue::full()
{
    bool full = instNumInsert + instNum >= iqsize;
    bool dispBottleneck = instNumInsert >= inoutPorts;
    if (full) {
        DPRINTF(Schedule, "has full!\n");
        DPRINTF(Schedule, "start dump insts\n");
        for (auto& it : instList) {
            DPRINTF(Schedule, "[sn %lu], %s ready: %d issued: %d squashed: %d\n",
                it.inst->seqNum,
                enums::OpClassStrings[it.inst->opClass()],
                it.inst->readyToIssue(),
                it.issued,
                it.inst->isSquashed());
        }
    }
    if (dispBottleneck) {
        DPRINTF(Schedule, "can't insert more due to inports exhausted\n");
    }
    return full || dispBottleneck;
}

void
IssueQue::insert(const DynInstPtr& inst)
{
    assert(!full());
    DPRINTF(Schedule, "[sn %lu] %s insert into %s\n",
        inst->seqNum, enums::OpClassStrings[inst->opClass()] ,iqname);
    instNumInsert++;
    instNum++;
    DPRINTF(Schedule, "[sn %lu] instNum++\n", inst->seqNum);
    inst->issueQue = this;
    instList.emplace_back(inst);
    auto entry = &(instList.back());
    bool addToDepGraph = false;
    for (int i=0; i<inst->numSrcRegs(); i++) {
        auto src = inst->renamedSrcIdx(i);
        if (!inst->readySrcIdx(i) && !src->isFixedMapping()) {
            if ((*noSpecScoreboard)[src->flatIndex()]) {
                inst->markSrcRegReady(i);
            } else {
                DPRINTF(Schedule, "[sn %lu] src p%d add to depGraph\n", inst->seqNum, src->flatIndex());
                subDepGraph[src->flatIndex()].push_back({i, entry});
                addToDepGraph = true;
            }
        }
    }
    if (!addToDepGraph) {
        assert(inst->readyToIssue());
    }
    entry->archReadySrcs = inst->getNumSrcRegReady();

    if (inst->isMemRef()) {
        // insert and check memDep
        scheduler->memDepUnit[inst->threadNumber].insert(inst);
    }
    else {
        addIfReady(entry);
    }
}

void
IssueQue::insertNonSpec(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn %lu] insertNonSpec into %s\n", inst->seqNum, iqname);
    DPRINTF(Schedule, "[sn %lu] instNum++\n", inst->seqNum);
    inst->issueQue = this;
    if (inst->isMemRef()) {
        scheduler->memDepUnit[inst->threadNumber].insertNonSpec(inst);
    }
}

void
IssueQue::markMemDepDone(const DynInstPtr& inst)
{
    assert(inst->isMemRef());
    DPRINTF(Schedule, "[sn %lu] has resloved memdependency\n", inst->seqNum);
    auto entry = findEntry(inst);
    entry->memDepResovled = true;
    addIfReady(entry);
}

void
IssueQue::retryMem(const DynInstPtr& inst)
{
    iqstats->retryMem++;
    DPRINTF(Schedule, "retry %s [sn %lu]\n", enums::OpClassStrings[inst->opClass()], inst->seqNum);
    scheduler->addToFU(inst);
}

void
IssueQue::specWakeup(PhysRegIdPtr dst)
{
    wakeup(dst, true);
}

void
IssueQue::writebackWakeup(PhysRegIdPtr dst)
{
    wakeup(dst, false);
}

void
IssueQue::doCommit(const InstSeqNum seqNum)
{
    while (!instList.empty() && instList.front().inst->seqNum <= seqNum) {
        instList.front().inst=nullptr;
        instList.pop_front();
    }
}

void
IssueQue::doSquash(const InstSeqNum seqNum)
{
    for (auto& it : instList) {
        if (it.inst->seqNum > seqNum) {
            if (!it.issued) {
                assert(instNum != 0);
                instNum--;
                DPRINTF(Schedule, "[sn %lu] instNum--\n", it.inst->seqNum);
            }
            it.inst->setSquashedInIQ();
            it.inst->setIssued();
            it.inst->setCanCommit();
            it.inst->clearInIQ();
            it.issued = true;
        }
    }
    for (auto& entrys : subDepGraph) {
        for (auto it = entrys.begin(); it != entrys.end();) {
            if ((*it).second->inst->isSquashed()) {
                it = entrys.erase(it);
            }
            else {
                it++;
            }
        }
    }
}

WakeupQue::SpecWakeupCompletion::SpecWakeupCompletion(WakeupQue* que)
    : Event(Stat_Event_Pri, 0),
      que(que)
{ }

void
WakeupQue::SpecWakeupCompletion::reset(const DynInstPtr& inst, IssueQue* to)
{
    this->inst = inst;
    this->to = to;
}

void
WakeupQue::SpecWakeupCompletion::process()
{
    DPRINTF(Schedule, "[sn %lu] %s specwakeup to %s\n",
        inst->seqNum, enums::OpClassStrings[inst->opClass()], to->getName());
    for (int i=0; i<inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        to->specWakeup(dst);
    }
    que->freeEventList.push_back(this);
    inst = NULL;
    to = nullptr;
}

const char*
WakeupQue::SpecWakeupCompletion::description() const
{
    return "Spec wakeup completion";
}

void
WakeupQue::init(std::vector<IssueQue*>* issueQues,
    std::vector<SpecWakeupChannel*> matrix, bool xbar)
{
    this->issueQues = issueQues;
    int totalOutports = 0;
    int maxExecCycle = 30;
    for (auto it : *issueQues) {
        totalOutports += it->inoutPorts;
    }
    for (int i=0;i<totalOutports * maxExecCycle;i++) {
        freeEventList.push_back(new SpecWakeupCompletion(this));
    }


    auto findIQbyname = [this](std::string name) -> IssueQue*{
        for (auto it : *this->issueQues) {
            if (it->getName().compare(name) == 0) {
                return it;
            }
        }
        return nullptr;
    };
    specWakeupMatrix.resize(issueQues->size());
    for (int i=0; i<specWakeupMatrix.size(); i++) {
        specWakeupMatrix[i].resize(issueQues->size(), -2);
    }

    if (xbar) {
        for (auto srcIQ : *issueQues) {
            for (auto dstIQ : *issueQues) {
                specWakeupMatrix[srcIQ->getId()][dstIQ->getId()] = -1;
            }
        }
        return;
    }

    for (auto it : matrix) {
        auto srcIQ = findIQbyname(it->srcIQ);
        if (!srcIQ) {
            continue;
        }
        for (auto dstIQname : it->dstIQ) {
            auto dstIQ = findIQbyname(dstIQname);
            if (!dstIQ) {
                continue;
            }
            specWakeupMatrix[srcIQ->getId()][dstIQ->getId()] = it->autoAdjust ? -1 : it->delay;
        }
    }
}

void
WakeupQue::insert(DynInstPtr& inst, IssueQue* from)
{
    if (inst->isMemRef()) {
        return;
    }
    for (auto to : *issueQues) {
        int wakeupDelay = specWakeupMatrix[from->getId()][to->getId()];
        if (wakeupDelay < -1) {
            // no channel
            continue;
        }
        else if (wakeupDelay == -1) {
            wakeupDelay = scheduler->getOpLatency(inst) - 1;
            assert(wakeupDelay >= 0);
            if (from->getIssueStages() > to->getIssueStages()) {
                wakeupDelay += from->getIssueStages() - to->getIssueStages();
            }
        }
        DPRINTF(Schedule, "%s create wakeupEvent to %s, delay %d cycles\n",
            from->getName(), to->getName(), wakeupDelay);
        assert(!freeEventList.empty());
        auto wakeupEvent = freeEventList.back();
        freeEventList.pop_back();
        wakeupEvent->reset(inst, to);
        cpu->schedule(wakeupEvent, cpu->clockEdge(Cycles(wakeupDelay)) + 1);
    }
}

Scheduler::Scheduler(const SchedulerParams& params)
    : SimObject(params),
      issueQues(params.IQs)
{
    dispTable.resize(enums::OpClass::Num_OpClass);

    opExecTimeTable.resize(enums::OpClass::Num_OpClass, 1);

    for (int i=0; i< issueQues.size(); i++) {
        issueQues[i]->setIQID(i);
        issueQues[i]->setWakeupQue(&wakeupQue);
        issueQues[i]->scheduler = this;
        combinedFus += issueQues[i]->inoutPorts;
        for (auto fu : issueQues[i]->fuDescs) {
            for (auto op : fu->opDescList) {
                opExecTimeTable[op->opClass] = op->opLat;
                dispTable[op->opClass].push_back(issueQues[i]);
            }
        }
    }
    for (auto& it : dispTable) {
        if (it.empty()) {
            it.push_back(issueQues[0]);
        }
    }

    wakeupQue.setScheduler(this);
    wakeupQue.init(&issueQues, params.specWakeupNetwork, params.xbarWakeup);
}

void
Scheduler::setCPU(CPU* cpu)
{
    for (auto it : issueQues) {
        it->setCPU(cpu);
    }
    wakeupQue.setCPU(cpu);
}

void
Scheduler::resetDepGraph(uint64_t numPhysRegs)
{
    noSpecScoreboard.resize(numPhysRegs, true);
    bypassScoreboard.resize(numPhysRegs, true);
    for (auto it : issueQues) {
        it->setBypassScoreboard(&bypassScoreboard);
        it->setnoSpecScoreboard(&noSpecScoreboard);
        it->resetDepGraph(numPhysRegs);
    }
}

void
Scheduler::addToFU(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn %lu] add to FUs\n", inst->seqNum);
    instsToFu.push_back(inst);
}

void
Scheduler::tick()
{
    for (auto it : issueQues) {
        it->tick();
    }
}

bool
Scheduler::full(const DynInstPtr& inst)
{
    auto iqs = dispTable[inst->opClass()];
    for (auto iq : iqs) {
        if (!iq->full()) {
            return false;
        }
    }
    return true;
}

void
Scheduler::insert(const DynInstPtr& inst)
{
    inst->setInIQ();
    auto iqs = dispTable[inst->opClass()];
    assert(!iqs.empty());
    for (auto iq : iqs) {
        if (!iq->full()) {
            iq->insert(inst);
            break;
        }
    }

    for (int i=0; i<inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        noSpecScoreboard[dst->flatIndex()] = false;
        bypassScoreboard[dst->flatIndex()] = false;
        DPRINTF(Schedule, "mark scoreboard p%lu not ready\n", dst->flatIndex());
    }
}

void
Scheduler::insertNonSpec(const DynInstPtr& inst)
{
    inst->setInIQ();
    auto iqs = dispTable[inst->opClass()];
    assert(!iqs.empty());
    for (auto iq : iqs) {
        if (!iq->full()) {
            iq->insertNonSpec(inst);
            break;
        }
    }

    for (int i=0; i<inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        noSpecScoreboard[dst->flatIndex()] = false;
        bypassScoreboard[dst->flatIndex()] = false;
    }
}

DynInstPtr
Scheduler::getInstToFU()
{
    if (instsToFu.empty()) {
        return DynInstPtr(nullptr);
    }
    auto ret = instsToFu.back();
    instsToFu.pop_back();
    return ret;
}

uint32_t
Scheduler::getOpLatency(const DynInstPtr& inst)
{
    return opExecTimeTable[inst->opClass()];
}

bool
Scheduler::checkFuReady(const DynInstPtr& inst)
{
    return true;
}

void
Scheduler::allocFu(const DynInstPtr& inst)
{

}

void
Scheduler::writebackWakeup(DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn %lu] was writeback\n", inst->seqNum);
    inst->issueQue = nullptr;// clear in issueQue
    for (int i=0; i<inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        noSpecScoreboard[dst->flatIndex()] = true;
        for (auto it : issueQues) {
            it->writebackWakeup(dst);
        }
    }
}

void
Scheduler::bypassWriteback(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn %lu] bypass write\n", inst->seqNum);
    for (int i=0; i<inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        bypassScoreboard[dst->flatIndex()] = true;
         DPRINTF(Schedule, "p%lu in bypassNetwork ready\n", dst->flatIndex());
    }
}

void
Scheduler::doCommit(const InstSeqNum seqNum)
{
    for (auto it : issueQues) {
        it->doCommit(seqNum);
    }
}

void
Scheduler::doSquash(const InstSeqNum seqNum)
{
    DPRINTF(Schedule, "doSquash until seqNum %lu\n", seqNum);
    for (auto it : issueQues) {
        it->doSquash(seqNum);
    }
}

}
}
