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
#include "debug/Dispatch.hh"
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
IssueQue::compare_priority::operator()(const DynInstPtr& a, const DynInstPtr& b)
{
    return a->seqNum > b->seqNum;
}

void
IssueQue::IssueStream::push(const DynInstPtr& inst)
{
    assert(size < 8);
    insts[size++] = inst;
}

DynInstPtr
IssueQue::IssueStream::pop()
{
    assert(size > 0);
    return insts[--size];
}

IssueQue::IssueQueStats::IssueQueStats(statistics::Group* parent, IssueQue* que, std::string name)
    : Group(parent, name.c_str()),
      ADD_STAT(issueSuccess, statistics::units::Count::get(), ""),
      ADD_STAT(issueFailed, statistics::units::Count::get(), ""),
      ADD_STAT(full, statistics::units::Count::get(), ""),
      ADD_STAT(bwfull, statistics::units::Count::get(), ""),
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
IssueQue::replay(const DynInstPtr& inst)
{
    assert (!inst->isIssued());
    iqstats->issueFailed++;
    DPRINTF(Schedule, "[sn %lu] replay reset archReadySrcs\n", inst->seqNum);
    inst->resetScheduled();
    inst->resetNumSrcRegReady(inst->archReadySrcs);
    // try again
    addIfReady(inst);
}

void
IssueQue::addToFu(const DynInstPtr& inst)
{
    assert (!inst->isIssued());
    iqstats->issueSuccess++;
    inst->setIssued();
    scheduler->addToFU(inst);
    DPRINTF(Schedule, "[sn %lu] instNum--\n", inst->seqNum);
    assert(instNum != 0);
    instNum--;
}

bool
IssueQue::checkResource(const DynInstPtr& inst)
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
IssueQue::issueToFu()
{
    int size = toFu->size;
    for (int i=0; i<size;i++) {
        auto inst = toFu->pop();
        if (inst->isSquashed()) {
            continue;
        }

        bool successful = checkResource(inst);
        if (successful) {
            // add to instToExecuteQue
            DPRINTF(Schedule, "[sn %lu] issue successful\n", inst->seqNum);
            addToFu(inst);
        }
        else {
            // replay
            DPRINTF(Schedule, "[sn %lu] issue failed, replay\n", inst->seqNum);
            replay(inst);
        }
    }
}

void
IssueQue::wakeup(PhysRegIdPtr dst, bool speculative)
{
    if (!speculative && dst->getNumPinnedWritesToComplete() != 0) {
        return;
    }

    DPRINTF(Schedule, "was %s woken by p%lu\n", speculative ? "spec" : "wb", dst->flatIndex());
    for (auto& it: subDepGraph[dst->flatIndex()]) {
        int srcIdx = it.first;
        auto inst = it.second;

        if (speculative) {
            if (inst->readyOnce()) {
                continue;
            }
        }
        else {
            inst->archReadySrcs++;
        }

        if (!inst->readySrcIdx(srcIdx)) {
            inst->markSrcRegReady(srcIdx);
        }

        if (inst->scheduled()) {
            continue;
        }

        DPRINTF(Schedule, "[sn %lu] was woken\n", inst->seqNum);
        addIfReady(inst);
    }

    if (!speculative) {
        subDepGraph[dst->flatIndex()].clear();
    }
}

void
IssueQue::scheduleInst()
{
    for (int i=0; i<inoutPorts && !readyInsts.empty();) {
        auto inst = readyInsts.top();
        if (inst->scheduled()) {
            readyInsts.pop();
            continue;
        }
        i++;
        inst->setScheduled();
        selectedInst.push_back(inst);
        readyInsts.pop();
    }
    iqstats->issueDist[selectedInst.size()]++;
    for (int i=0; i<selectedInst.size(); i++) {
        assert(i < inoutPorts);
        DPRINTF(Schedule, "[sn %ld] was scheduled, insert into wakeupQue\n", selectedInst[i]->seqNum);
        // insert into wakeupQue
        wakeupQue->insert(selectedInst[i], this);

        DPRINTF(Schedule, "start issue inst, need %d cycles\n", scheduleToExecDelay);
        toIssue->push(selectedInst[i]);
    }
    selectedInst.clear();
}

void
IssueQue::addIfReady(const DynInstPtr& inst)
{
    if (inst->readyToIssue()) {
        if (inst->readyTick == -1) {
            inst->readyTick = curTick();
            DPRINTF(Counters, "set readyTick at addIfReady\n");
        }

        //Add the instruction to the proper ready list.
        if (inst->isMemRef()) {
            if (inst->memDepSolved()) {
                DPRINTF(Schedule, "memRef Dependency was solved can issue\n");
            }
            else {
                DPRINTF(Schedule, "memRef Dependency was not solved can't issue\n");
                return;
            }
        }

        DPRINTF(Schedule, "[sn %lu] add to readyInstsQue\n", inst->seqNum);
        readyInsts.push(inst);
        inst->setReadyOnce();
    }
}

IssueQue::IssueQue(const IssueQueParams &params)
    : SimObject(params),
      inoutPorts(params.inoutPorts),
      iqsize(params.size),
      scheduleToExecDelay(params.scheduleToExecDelay),
      iqname(params.name),
      fuDescs(params.fuType),
      inflightIssues(scheduleToExecDelay, 0)
{
    toIssue = inflightIssues.getWire(0);
    toFu = inflightIssues.getWire(-scheduleToExecDelay);
}

void
IssueQue::setCPU(CPU* cpu)
{
    this->cpu = cpu;
    _name = cpu->name() + ".scheduler." + getName();
    iqstats = new IssueQueStats(cpu, this, "scheduler." + this->getName());
}

void
IssueQue::resetDepGraph(int numPhysRegs)
{
    subDepGraph.resize(numPhysRegs);
}

void
IssueQue::tick()
{
    iqstats->insertDist[instNumInsert]++;
    instNumInsert = 0;

    inflightIssues.advance();

    scheduleInst();

    issueToFu();
}

bool
IssueQue::full()
{
    bool full = instNumInsert + instNum >= iqsize;
    bool dispBottleneck = instNumInsert >= inoutPorts;
    if (full) {
        iqstats->full++;
        DPRINTF(Schedule, "has full!\n");
        DPRINTF(Schedule, "start dump insts\n");
        for (auto& it : instList) {
            DPRINTF(Schedule, "[sn %lu], %s ready: %d issued: %d squashed: %d\n",
                it->seqNum,
                enums::OpClassStrings[it->opClass()],
                it->readyToIssue(),
                it->isIssued(),
                it->isSquashed());
        }
    }
    if (dispBottleneck) {
        iqstats->bwfull++;
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
    bool addToDepGraph = false;
    for (int i=0; i<inst->numSrcRegs(); i++) {
        auto src = inst->renamedSrcIdx(i);
        if (!inst->readySrcIdx(i) && !src->isFixedMapping()) {
            if ((*noSpecScoreboard)[src->flatIndex()]) {
                inst->markSrcRegReady(i);
            } else {
                DPRINTF(Schedule, "[sn %lu] src p%d add to depGraph\n", inst->seqNum, src->flatIndex());
                subDepGraph[src->flatIndex()].push_back({i, inst});
                addToDepGraph = true;
            }
        }
    }
    if (!addToDepGraph) {
        assert(inst->readyToIssue());
    }
    inst->archReadySrcs = inst->getNumSrcRegReady();

    for (int i=0; i<inst->numSrcRegs(); i++) {
        auto src = inst->renamedSrcIdx(i);
        if (!inst->readySrcIdx(i) && !src->isFixedMapping()) {
            if (scheduler->specScoreboard[src->flatIndex()]) {
                inst->markSrcRegReady(i);
            }
        }
    }

    if (inst->isMemRef()) {
        // insert and check memDep
        scheduler->memDepUnit[inst->threadNumber].insert(inst);
    }
    else {
        addIfReady(inst);
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
    inst->setMemDepDone();
    addIfReady(inst);
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
    while (!instList.empty() && instList.front()->seqNum <= seqNum) {
        instList.pop_front();
    }
}

void
IssueQue::doSquash(const InstSeqNum seqNum)
{
    while (!instList.empty() && (instList.back()->seqNum > seqNum)) {
        auto it = instList.back();
        it->setSquashedInIQ();
        it->setCanCommit();
        it->clearInIQ();
        it->setScheduled();
        if (!it->isIssued()) {
            DPRINTF(Schedule, "[sn %lu] instNum--\n", it->seqNum);
            assert(instNum != 0);
            instNum--;
            it->setIssued();
        }
        instList.pop_back();
    }

    // clear in depGraph
    for (auto& entrys : subDepGraph) {
        for (auto it = entrys.begin(); it != entrys.end();) {
            if ((*it).second->isSquashed()) {
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
        if (dst->isFixedMapping()) {
            continue;
        }
        que->scheduler->specScoreboard[dst->flatIndex()] = true;
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
        specWakeupMatrix[i].resize(issueQues->size(), false);
    }

    if (xbar) {
        for (auto srcIQ : *issueQues) {
            for (auto dstIQ : *issueQues) {
                specWakeupMatrix[srcIQ->getId()][dstIQ->getId()] = true;
                DPRINTF(Schedule, "build wakeup/bypass channel: %s -> %s\n", srcIQ->getName(), dstIQ->getName());
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
            specWakeupMatrix[srcIQ->getId()][dstIQ->getId()] = true;
            DPRINTF(Schedule, "build wakeup/bypass channel: %s -> %s\n", srcIQ->getName(), dstIQ->getName());
        }
    }
}

void
WakeupQue::insert(const DynInstPtr& inst, IssueQue* from)
{
    if (inst->isMemRef()) {
        return;
    }
    for (auto to : *issueQues) {
        bool wakeup = specWakeupMatrix[from->getId()][to->getId()];
        int wakeupDelay = 0;
        if (wakeup) {
            wakeupDelay = scheduler->getOpLatency(inst) - 1;
            assert(wakeupDelay >= 0);
            if (from->getIssueStages() > to->getIssueStages()) {
                wakeupDelay += from->getIssueStages() - to->getIssueStages();
            }
        }
        else {
            continue;
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
    specScoreboard.resize(numPhysRegs, true);
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
    DPRINTF(Dispatch, "IQ full, opclass: %s\n", enums::OpClassStrings[inst->opClass()]);
    return true;
}

void
Scheduler::insert(const DynInstPtr& inst)
{
    inst->setInIQ();
    auto iqs = dispTable[inst->opClass()];
    assert(!iqs.empty());
    bool inserted = false;

    if (forwardDisp) {
        for (auto iq : iqs) {
            if (!iq->full()) {
                iq->insert(inst);
                inserted = true;
                break;
            }
        }
    }
    else {
        for (auto iq = iqs.rbegin(); iq != iqs.rend(); iq++) {
            if (!(*iq)->full()) {
                (*iq)->insert(inst);
                inserted = true;
                break;
            }
        }
    }
    assert(inserted);
    forwardDisp = !forwardDisp;
    DPRINTF(Dispatch, "[sn %lu] dispatch: %s\n", inst->seqNum, inst->staticInst->disassemble(0));

    for (int i=0; i<inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        specScoreboard[dst->flatIndex()] = false;
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
        specScoreboard[dst->flatIndex()] = false;
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
{ }

bool
Scheduler::hasReadyInsts()
{
    for (auto it : issueQues) {
        if (!it->readyInsts.empty()) {
            return true;
        }
    }
    return false;
}

bool
Scheduler::isDrained()
{
    for (auto it : issueQues) {
        if (!it->instList.empty()) {
            return false;
        }
    }
    return true;
}

void
Scheduler::writebackWakeup(const DynInstPtr& inst)
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
