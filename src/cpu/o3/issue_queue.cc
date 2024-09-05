#include "cpu/o3/issue_queue.hh"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <queue>
#include <stack>
#include <string>
#include <vector>

#include "arch/riscv/insts/vector.hh"
#include "base/logging.hh"
#include "base/stats/group.hh"
#include "base/stats/info.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/func_unit.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/fu_pool.hh"
#include "cpu/o3/iew_delay_calibrator.hh"
#include "cpu/reg_class.hh"
#include "debug/Counters.hh"
#include "debug/Dispatch.hh"
#include "debug/Schedule.hh"
#include "enums/OpClass.hh"
#include "params/BaseO3CPU.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

#define POPINST(x) \
    do {\
        if (x->opClass() != FMAMulOp) [[likely]] {\
            assert(instNum != 0);\
            instNum--;\
        }\
    } while (0)

#define CANTBEWAKE(x) (x->opClass() == IntDivOp || x->opClass() == FloatDivOp || x->opClass() == FloatSqrtOp)

namespace gem5
{

namespace o3
{

IssuePort::IssuePort(const IssuePortParams &params)
    : SimObject(params),
      fu(params.fu)
{
    mask.resize(Num_OpClasses, false);
    for (auto it0 : params.fu) {
        for (auto it1 : it0->opDescList) {
            mask.set(it1->opClass);
        }
    }
}

bool
IssueQue::select_ploy::operator()(const DynInstPtr& a, const DynInstPtr& b) const
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
      ADD_STAT(retryMem, statistics::units::Count::get(), "count of load/store retry"),
      ADD_STAT(canceledInst, statistics::units::Count::get(), "count of canceled insts"),
      ADD_STAT(loadmiss, statistics::units::Count::get(), "count of load miss"),
      ADD_STAT(arbFailed, statistics::units::Count::get(), "count of arbitration failed"),
      ADD_STAT(insertDist, statistics::units::Count::get(), "distruibution of insert"),
      ADD_STAT(issueDist, statistics::units::Count::get(), "distruibution of issue"),
      ADD_STAT(portissued, statistics::units::Count::get(), "count each port issues"),
      ADD_STAT(portBusy, statistics::units::Count::get(), "count each port busy cycles"),
      ADD_STAT(avgInsts, statistics::units::Count::get(), "average insts")
{
    insertDist.init(que->inports + 1).flags(statistics::nozero);
    issueDist.init(que->outports + 1).flags(statistics::nozero);
    portissued.init(que->outports).flags(statistics::nozero);
    portBusy.init(que->outports).flags(statistics::nozero);
    retryMem.flags(statistics::nozero);
    canceledInst.flags(statistics::nozero);
    loadmiss.flags(statistics::nozero);
    arbFailed.flags(statistics::nozero);
}

IssueQue::IssueQue(const IssueQueParams &params)
    : SimObject(params),
      inports(params.inports),
      outports(params.oports.size()),
      iqsize(params.size),
      scheduleToExecDelay(params.scheduleToExecDelay),
      iqname(params.name),
      inflightIssues(scheduleToExecDelay, 0)
{
    toIssue = inflightIssues.getWire(0);
    toFu = inflightIssues.getWire(-scheduleToExecDelay);
    if (outports > 8) {
        panic("%s: outports > 8 is not supported\n", iqname);
    }

    bool same_fu = true;
    for (int i = 0; i < outports; i++) {
        for (int j = i + 1; j < outports; j++) {
            if (params.oports[i]->mask != params.oports[j]->mask) {
                same_fu = false;
            }
            if (!same_fu && (params.oports[i]->mask & params.oports[j]->mask).any()) {
                panic("%s: Found the conflict opClass in different FU, portid: %d and %d\n", iqname, i, j);
            }
        }
    }
    if (same_fu) {
        warn("%s: Use one selector by multiple identical fus\n", iqname);
    }

    if (same_fu) {
        // we only allocate one ReadyQue
        auto t = new ReadyQue;
        readyQs.resize(outports, t);
        auto& port = params.oports[0];
        fuDescs.insert(fuDescs.begin(), port->fu.begin(), port->fu.end());
    }
    else {
        readyQs.resize(outports, nullptr);
        for (int i = 0; i < outports; i++) {
            readyQs[i] = new ReadyQue;
            auto& port = params.oports[i];
            fuDescs.insert(fuDescs.begin(), port->fu.begin(), port->fu.end());
        }
    }

    readyQclassify.resize(enums::OpClass::Num_OpClass, nullptr);
    opPipelined.resize(Num_OpClasses, false);
    for (int pi = 0; pi < (same_fu ? 1 : outports); pi++) {
        auto& port = params.oports[pi];
        for (auto ops : port->fu) {
            for (auto op : ops->opDescList) {
                if (readyQclassify[op->opClass]) {
                    panic("%s: Found the conflict opClass in different FU, opclass: %s\n", iqname,
                          enums::OpClassStrings[op->opClass]);
                }
                readyQclassify[op->opClass] = readyQs.at(pi);
                opPipelined[op->opClass] = op->pipelined;
            }
        }
    }

    portBusy.resize(outports, 0);
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
IssueQue::checkScoreboard(const DynInstPtr& inst)
{
    for (int i=0; i<inst->numSrcRegs(); i++) {
        auto src = inst->renamedSrcIdx(i);
        if (src->isFixedMapping()) [[unlikely]] {
            continue;
        }
        // check bypass data ready or not
        if (!scheduler->bypassScoreboard[src->flatIndex()]) [[unlikely]] {
            auto dst_inst = scheduler->getInstByDstReg(src->flatIndex());
            panic("[sn %lu] %s can't get data from bypassNetwork, dst inst: %s\n", inst->seqNum, inst->srcRegIdx(i),
                  dst_inst->genDisassembly());
        }
    }
    inst->checkOldVdElim();
}

void
IssueQue::addToFu(const DynInstPtr& inst)
{
    if (inst->isIssued()) [[unlikely]] {
        panic("%s [sn %lu] has alreayd been issued\n", enums::OpClassStrings[inst->opClass()], inst->seqNum);
    }
    inst->setIssued();
    scheduler->addToFU(inst);
    POPINST(inst);
}

void
IssueQue::issueToFu()
{
    int size = toFu->size;
    for (int i=0; i<size;i++) {
        auto inst = toFu->pop();
        if (!inst) {
            continue;
        }
        checkScoreboard(inst);
        addToFu(inst);
        if (scheduler->getCorrectedOpLat(inst) > 1) {
            scheduler->specWakeUpDependents(inst, this);
        }
    }
}

void
IssueQue::retryMem(const DynInstPtr& inst)
{
    assert(!inst->isNonSpeculative());
    iqstats->retryMem++;
    DPRINTF(Schedule, "retry %s [sn %lu]\n", enums::OpClassStrings[inst->opClass()], inst->seqNum);
    // scheduler->loadCancel(inst);
    scheduler->addToFU(inst);
}

void
IssueQue::markMemDepDone(const DynInstPtr& inst)
{
    assert(inst->isMemRef());
    DPRINTF(Schedule, "[sn %lu] has solved memdependency\n", inst->seqNum);
    inst->setMemDepDone();
    addIfReady(inst);
}

void
IssueQue::wakeUpDependents(const DynInstPtr& inst, bool speculative)
{
    if (speculative && inst->canceled()) [[unlikely]] {
        return;
    }
    for (int i = 0; i < inst->numDestRegs(); i++) {
        PhysRegIdPtr dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping() || dst->getNumPinnedWritesToComplete() != 1) [[unlikely]] {
            continue;;
        }

        DPRINTF(Schedule, "was %s woken by p%lu [sn %lu]\n",
            speculative ? "spec" : "wb", dst->flatIndex(), inst->seqNum);
        for (auto& it: subDepGraph[dst->flatIndex()]) {
            int srcIdx = it.first;
            auto& consumer = it.second;
            if (consumer->readySrcIdx(srcIdx)) {
                continue;
            }
            consumer->markSrcRegReady(srcIdx);

            if (!speculative && consumer->srcRegIdx(srcIdx) == RiscvISA::VecRenamedVLReg) [[unlikely]] {
                consumer->checkOldVdElim();
            }

            DPRINTF(Schedule, "[sn %lu] src%d was woken\n", consumer->seqNum, srcIdx);
            addIfReady(consumer);
        }

        if (!speculative) {
            subDepGraph[dst->flatIndex()].clear();
        }
    }
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
            } else {
                DPRINTF(Schedule, "memRef Dependency was not solved can't issue\n");
                return;
            }
        }

        DPRINTF(Schedule, "[sn %lu] add to readyInstsQue\n", inst->seqNum);
        inst->clearCancel();
        if (!inst->inReadyQ()) {
            inst->setInReadyQ();
            readyQclassify[inst->opClass()]->push(inst);
        }
    }
}

void
IssueQue::selectInst()
{
    selectQ.clear();
    for (int pi=0;pi<outports;pi++) {
        auto readyQ = readyQs[pi];
        while (!readyQ->empty()) {
            auto top = readyQ->top();
            if (!top->canceled()) {
                break;
            }
            top->clearInReadyQ();
            readyQ->pop();
        }

        if (!readyQ->empty()) {
            auto inst = readyQ->top();
            DPRINTF(Schedule, "[sn %ld] was selected\n", inst->seqNum);
            scheduler->insertSlot(inst);
            selectQ.push_back(std::make_pair(pi, inst));
            inst->clearInReadyQ();
            readyQ->pop();
        }
    }
}

void
IssueQue::scheduleInst()
{
    // here is issueStage 0
    for (auto& info : selectQ) {
        auto& pi = info.first; // port id
        auto& inst = info.second;
        if (inst->canceled()) {
            DPRINTF(Schedule, "[sn %ld] was canceled\n", inst->seqNum);
        } else if (inst->arbFailed() || portBusy[pi]) {
            if (inst->arbFailed()) {
                iqstats->arbFailed++;
                DPRINTF(Schedule, "[sn %ld] arbitration failed, retry\n", inst->seqNum);
            } else {
                iqstats->portBusy[pi]++;
                DPRINTF(Schedule, "[sn %ld] port busy, retry\n", inst->seqNum);
            }
            assert(inst->readyToIssue());
            inst->setInReadyQ();
            readyQclassify[inst->opClass()]->push(inst);// retry
        } else [[likely]] {
            iqstats->portissued[pi]++;
            DPRINTF(Schedule, "[sn %ld] no conflict, scheduled\n", inst->seqNum);
            inst->issueportid = pi;
            inst->clearInIQ();
            toIssue->push(inst);
            uint32_t lat = scheduler->getCorrectedOpLat(inst);
            if (lat <= 1) {
                scheduler->specWakeUpDependents(inst, this);
            } else if (!opPipelined[inst->opClass()]) [[unlikely]] {
                portBusy[pi] = lat - 1;
            }
        }
        inst->clearArbFailed();
    }
    if (toIssue->size > 0) {
        iqstats->issueDist[toIssue->size]++;
    }
}

void
IssueQue::tick()
{
    iqstats->avgInsts = instNum;

    if (instNumInsert > 0) {
        iqstats->insertDist[instNumInsert]++;
    }
    instNumInsert = 0;

    for (auto& t : portBusy) {
        t = t > 0 ? t - 1 : t;
    }

    scheduleInst();
    inflightIssues.advance();
}

bool
IssueQue::ready()
{
    bool bwFull = instNumInsert >= inports;
    if (bwFull) {
        DPRINTF(Schedule, "can't insert more due to inports exhausted\n");
    }
    return !full() && !bwFull;
}

bool
IssueQue::full()
{
    bool full = instNumInsert + instNum >= iqsize;
    if (full) {
        DPRINTF(Schedule, "has full!\n");
    }
    return full;
}

void
IssueQue::insert(const DynInstPtr& inst)
{
    if (inst->opClass() != FMAMulOp) [[likely]] {
        assert(instNum < iqsize);
        instNum++;
        instNumInsert++;
    }

    DPRINTF(Schedule, "[sn %lu] %s insert into %s\n",
        inst->seqNum, enums::OpClassStrings[inst->opClass()] ,iqname);
    DPRINTF(Schedule, "[sn %lu] instNum++\n", inst->seqNum);
    inst->issueQue = this;
    instList.emplace_back(inst);
    bool addToDepGraph = false;
    for (int i=0; i<inst->numSrcRegs(); i++) {
        auto src = inst->renamedSrcIdx(i);
        if (!inst->readySrcIdx(i) && !src->isFixedMapping()) {
            if (scheduler->scoreboard[src->flatIndex()]) {
                inst->markSrcRegReady(i);
            } else {
                DPRINTF(Schedule, "[sn %lu] src p%d add to depGraph\n", inst->seqNum, src->flatIndex());
                subDepGraph[src->flatIndex()].push_back({i, inst});
                addToDepGraph = true;
            }
        }
    }

    inst->checkOldVdElim();

    if (!addToDepGraph) {
        assert(inst->readyToIssue());
    }

    if (inst->isMemRef()) {
        // insert and check memDep
        scheduler->memDepUnit[inst->threadNumber].insert(inst);
    } else {
        addIfReady(inst);
    }
}

void
IssueQue::insertNonSpec(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn %lu] insertNonSpec into %s\n", inst->seqNum, iqname);
    inst->issueQue = this;
    if (inst->isMemRef()) {
        scheduler->memDepUnit[inst->threadNumber].insertNonSpec(inst);
    }
}

void
IssueQue::doCommit(const InstSeqNum seqNum)
{
    while (!instList.empty() && instList.front()->seqNum <= seqNum) {
        assert(instList.front()->isIssued());
        instList.pop_front();
    }
}

void
IssueQue::doSquash(const InstSeqNum seqNum)
{
    for (auto it=instList.begin(); it!=instList.end();) {
        if ((*it)->seqNum > seqNum) {
            (*it)->setSquashedInIQ();
            (*it)->setCanCommit();
            (*it)->clearInIQ();
            (*it)->setCancel();
            if (!(*it)->isIssued()) {
                POPINST((*it));
                (*it)->setIssued();
            } else if ((*it)->issueportid >= 0) {
                portBusy[(*it)->issueportid] = 0;
            }
            it = instList.erase(it);
        } else {
            it++;
        }
    }

    for (int i = 0; i <= getIssueStages(); i++) {
        int size = inflightIssues[-i].size;
        for (int j = 0; j < size; j++) {
            auto& inst = inflightIssues[-i].insts[j];
            if (inst && inst->isSquashed()) {
                assert(inst->issueportid >= 0);
                portBusy[inst->issueportid] = 0;
                inst = nullptr;
            }
        }
    }

    // clear in depGraph
    for (auto& entrys : subDepGraph) {
        for (auto it = entrys.begin(); it != entrys.end();) {
            if ((*it).second->isSquashed()) {
                it = entrys.erase(it);
            } else {
                it++;
            }
        }
    }
}

Scheduler::Slot::Slot(uint32_t priority, uint32_t demand, const DynInstPtr& inst)
    : priority(priority), resourceDemand(demand), inst(inst)
{}

Scheduler::SpecWakeupCompletion::SpecWakeupCompletion(const DynInstPtr& inst, IssueQue* to)
    : Event(Stat_Event_Pri, AutoDelete),
      inst(inst),
      to_issue_queue(to)
{}

void
Scheduler::SpecWakeupCompletion::process()
{
    to_issue_queue->wakeUpDependents(inst, true);
}

const char *
Scheduler::SpecWakeupCompletion::description() const
{
    return "Spec wakeup completion";
}

bool
Scheduler::disp_ploy::operator()(IssueQue* a, IssueQue* b) const
{
    // bigger/ready first
    int p0 = a->ready() ? a->emptyEntries() : -1;
    int p1 = b->ready() ? b->emptyEntries() : -1;
    return p0 < p1;
}

bool
Scheduler::slot_ploy::operator()(const Slot& a, const Slot& b) const
{
    // smaller first
    return a.priority > b.priority;
}

Scheduler::Scheduler(const SchedulerParams& params)
    : SimObject(params),
      issueQues(params.IQs),
      intSlotNum(params.intSlotNum),
      fpSlotNum(params.fpSlotNum)
{
    dispTable.resize(enums::OpClass::Num_OpClass);
    opExecTimeTable.resize(enums::OpClass::Num_OpClass, 1);

    boost::dynamic_bitset<> opChecker(enums::Num_OpClass, 0);
    for (int i=0; i< issueQues.size(); i++) {
        issueQues[i]->setIQID(i);
        issueQues[i]->scheduler = this;
        combinedFus += issueQues[i]->outports;
        panic_if(issueQues[i]->fuDescs.size() == 0, "Empty config IssueQue: " + issueQues[i]->getName());
        for (auto fu : issueQues[i]->fuDescs) {
            for (auto op : fu->opDescList) {
                opExecTimeTable[op->opClass] = op->opLat;
                dispTable[op->opClass].push_back(issueQues[i]);
                opChecker.set(op->opClass);
            }
        }
    }

    if (opChecker.count() != enums::Num_OpClass) {
        for (int i=0; i<enums::Num_OpClass; i++) {
            if (!opChecker[i]) {
                warn("No config for opClass: %s\n", enums::OpClassStrings[i]);
            }
        }
    }

    wakeMatrix.resize(issueQues.size());
    auto findIQbyname = [this](std::string name) -> IssueQue*{
        IssueQue* ret = nullptr;
        for (auto it : this->issueQues) {
            if (it->getName().compare(name) == 0) {
                if (ret) {
                    panic("has duplicate IQ name: %s\n", name);
                }
                ret = it;
            }
        }
        panic_if(!ret, "can't find IQ by name: %s\n", name);
        return ret;
    };
    if (params.xbarWakeup) {
        for (auto srcIQ : issueQues) {
            for (auto dstIQ : issueQues) {
                wakeMatrix[srcIQ->getId()].push_back(dstIQ);
                DPRINTF(Schedule, "build wakeup channel: %s -> %s\n", srcIQ->getName(), dstIQ->getName());
            }
        }
    } else {
        for (auto it : params.specWakeupNetwork) {
            auto srcIQ = findIQbyname(it->srcIQ);
            for (auto dstIQname : it->dstIQ) {
                auto dstIQ = findIQbyname(dstIQname);
                wakeMatrix[srcIQ->getId()].push_back(dstIQ);
                DPRINTF(Schedule, "build wakeup channel: %s -> %s\n", srcIQ->getName(), dstIQ->getName());
            }
        }
    }
}

void
Scheduler::setCPU(CPU* cpu)
{
    this->cpu = cpu;
    for (auto it : issueQues) {
        it->setCPU(cpu);
    }
}

void
Scheduler::resetDepGraph(uint64_t numPhysRegs)
{
    scoreboard.resize(numPhysRegs, true);
    bypassScoreboard.resize(numPhysRegs, true);
    for (auto it : issueQues) {
        it->resetDepGraph(numPhysRegs);
    }
}

void
Scheduler::addToFU(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "%s [sn %lu] add to FUs\n", enums::OpClassStrings[inst->opClass()], inst->seqNum);
    instsToFu.push_back(inst);
}

void
Scheduler::tick()
{
    for (auto it : issueQues) {
        it->tick();
    }
}

void
Scheduler::issueAndSelect(){
    for (auto it : issueQues) {
        it->issueToFu();
    }
    // must wait for all insts was issued
    for (auto it : issueQues) {
        it->selectInst();
    }
    // inst arbitration
    while (intSlotOccupied > intSlotNum) {
        auto& slot = intSlot.top();
        slot.inst->setArbFailed();
        intSlotOccupied -= slot.resourceDemand;
        DPRINTF(Schedule, "[sn %lu] remove from slot\n", slot.inst->seqNum);
        intSlot.pop();
    }
    while (fpSlotOccupied > fpSlotNum) {
        auto& slot = fpSlot.top();
        slot.inst->setArbFailed();
        fpSlotOccupied -= slot.resourceDemand;
        DPRINTF(Schedule, "[sn %lu] remove from slot\n", slot.inst->seqNum);
        fpSlot.pop();
    }

    // reset slot status
    intSlotOccupied = 0;
    fpSlotOccupied = 0;
    intSlot.clear();
    fpSlot.clear();
}

bool
Scheduler::ready(const DynInstPtr& inst)
{
    auto& iqs = dispTable[inst->opClass()];
    assert(!iqs.empty());
    // std::sort(iqs.begin(), iqs.end(), disp_ploy());
    // if (iqs.back()->ready()) {
    //     return true;
    // }

    for (auto iq : iqs) {
        if (iq->ready()) {
            return true;
        }
    }

    DPRINTF(Dispatch, "IQ not ready, opclass: %s\n", enums::OpClassStrings[inst->opClass()]);
    return false;
}

bool
Scheduler::full(const DynInstPtr& inst)
{
    auto& iqs = dispTable[inst->opClass()];
    // std::sort(iqs.begin(), iqs.end(), disp_ploy());
    // if (!iqs.back()->full()) {
    //     return false;
    // }

    for (auto iq : iqs) {
        if (!iq->full()) {
            return false;
        }
    }

    DPRINTF(Dispatch, "IQ full, opclass: %s\n", enums::OpClassStrings[inst->opClass()]);
    return true;
}

DynInstPtr
Scheduler::getInstByDstReg(RegIndex flatIdx)
{
    for (auto iq : issueQues)
    {
        for (auto& inst : iq->instList){
            if (inst->numDestRegs() > 0 && inst->renamedDestIdx(0)->flatIndex() == flatIdx) {
                return inst;
            }
        }
    }
    return nullptr;
}

void
Scheduler::addProducer(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn %lu] addProdecer\n", inst->seqNum);
    for (int i=0; i<inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        scoreboard[dst->flatIndex()] = false;
        bypassScoreboard[dst->flatIndex()] = false;
        DPRINTF(Schedule, "mark scoreboard p%lu not ready\n", dst->flatIndex());
    }
}

void
Scheduler::insert(const DynInstPtr& inst)
{
    inst->setInIQ();
    auto& iqs = dispTable[inst->opClass()];
    bool inserted = false;

    // std::sort(iqs.begin(), iqs.end(), disp_ploy());
    // auto iq = iqs.back();
    // if (iq->ready()) {
    //     iq->insert(inst);
    //     inserted = true;
    // }

    std::random_shuffle(iqs.begin(), iqs.end());
    for (auto iq : iqs) {
        if (iq->ready()) {
            iq->insert(inst);
            inserted = true;
            break;
        }
    }

    assert(inserted);
    DPRINTF(Dispatch, "[sn %lu] dispatch: %s\n", inst->seqNum, inst->staticInst->disassemble(0));
}

void
Scheduler::insertNonSpec(const DynInstPtr& inst)
{
    inst->setInIQ();
    auto& iqs = dispTable[inst->opClass()];

    // std::sort(iqs.begin(), iqs.end(), disp_ploy());
    // auto iq = iqs.back();
    // iq->insertNonSpec(inst);

    for (auto iq : iqs) {
        if (iq->ready()) {
            iq->insertNonSpec(inst);
            break;
        }
    }
}

void
Scheduler::specWakeUpDependents(const DynInstPtr& inst, IssueQue* from_issue_queue)
{
    if (inst->numDestRegs() == 0 || (inst->isVector() && inst->isLoad())) {
        // ignore if vector load
        return;
    }
    if (CANTBEWAKE(inst)) {
        return;
    }

    for (auto to : wakeMatrix[from_issue_queue->getId()]) {
        int wakeDelay = 0;
        int oplat = getCorrectedOpLat(inst);
        assert(oplat < 64);
        if (oplat == 1 && (from_issue_queue->getIssueStages() > to->getIssueStages())) {
            wakeDelay = from_issue_queue->getIssueStages() - to->getIssueStages();
        } else if (oplat > to->getIssueStages()) {
            wakeDelay = oplat - to->getIssueStages();
        }

        DPRINTF(Schedule, "[sn %lu] %s create wakeupEvent to %s, delay %d cycles\n",
            inst->seqNum, from_issue_queue->getName(), to->getName(), wakeDelay);
        if (wakeDelay == 0) {
            to->wakeUpDependents(inst, true);
        } else {
            auto wakeEvent = new SpecWakeupCompletion(inst, to);
            cpu->schedule(wakeEvent, cpu->clockEdge(Cycles(wakeDelay)) - 1);
        }
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

void
Scheduler::insertSlot(const DynInstPtr& inst)
{
    if (inst->isVector()) {
        // floating point and vector insts are not participate in arbitration
        return;
    }
    uint32_t priority = getArbPriority(inst);
    uint32_t needed = inst->numSrcRegs();

    if (inst->isFloating()) {
        fpSlotOccupied += needed;
        fpSlot.push(Slot(priority, needed, inst));
    }
    else if (inst->isInteger()) {
        intSlotOccupied += needed;
        intSlot.push(Slot(priority, needed, inst));
    }
    DPRINTF(Schedule, "[sn %lu] insert slot, priority: %u, needed: %u\n", inst->seqNum, priority, needed);
}

void Scheduler::loadCancel(const DynInstPtr& inst)
{
    if (inst->canceled()) {
        return;
    }
    DPRINTF(Schedule, "[sn %lu] %s cache miss, cancel consumers\n", inst->seqNum,
            enums::OpClassStrings[inst->opClass()]);
    inst->setCancel();
    if (inst->issueQue) {
        inst->issueQue->iqstats->loadmiss++;
    }

    dfs.push(inst);
    while (!dfs.empty()) {
        auto top = dfs.top();
        dfs.pop();
        for (int i=0; i<top->numDestRegs(); i++) {
            auto dst = top->renamedDestIdx(i);
            if (dst->isFixedMapping()) {
                continue;
            }
            for (auto iq : issueQues) {
                for (auto& it : iq->subDepGraph[dst->flatIndex()]) {
                    int srcIdx = it.first;
                    auto& depInst = it.second;
                    if (depInst->readySrcIdx(srcIdx) && depInst->renamedSrcIdx(srcIdx) != cpu->vecOnesPhysRegId) {
                        assert(!depInst->isIssued());
                        DPRINTF(Schedule, "cancel [sn %lu], clear src p%d ready\n",
                            depInst->seqNum, depInst->renamedSrcIdx(srcIdx)->flatIndex());
                        depInst->setCancel();
                        iq->iqstats->canceledInst++;
                        depInst->clearSrcRegReady(srcIdx);
                        dfs.push(depInst);
                    }
                }
            }
        }
    }

    for (auto iq : issueQues) {
        for (int i=0; i<=iq->getIssueStages(); i++) {
            int size = iq->inflightIssues[-i].size;
            for (int j=0; j<size; j++) {
                auto& inst = iq->inflightIssues[-i].insts[j];
                if (inst && inst->canceled()) {
                    inst = nullptr;
                }
            }
        }
    }
}

void
Scheduler::writebackWakeup(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn %lu] was writeback\n", inst->seqNum);
    inst->setWriteback();// clear in issueQue
    for (int i = 0; i < inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        scoreboard[dst->flatIndex()] = true;
    }
    for (auto it : issueQues) {
        it->wakeUpDependents(inst, false);
    }
}

void
Scheduler::bypassWriteback(const DynInstPtr& inst)
{
    if (inst->issueportid >= 0) {
        inst->issueQue->portBusy[inst->issueportid] = 0;
    }
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

uint32_t
Scheduler::getArbPriority(const DynInstPtr& inst)
{
    return (uint32_t)rand() % 10;
}

uint32_t
Scheduler::getOpLatency(const DynInstPtr& inst)
{
    return opExecTimeTable[inst->opClass()];
}

uint32_t
Scheduler::getCorrectedOpLat(const DynInstPtr& inst)
{
    uint32_t oplat = opExecTimeTable[inst->opClass()];
    oplat += inst->isLoad() ? 2 : 0;
    return oplat;
}

bool
Scheduler::hasReadyInsts()
{
    for (auto it : issueQues) {
        for (auto readyQ : it->readyQs) {
            if (!readyQ->empty()) {
                return true;
            }
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

uint32_t
Scheduler::getIQInsts()
{
    uint32_t total=0;
    for (auto iq : issueQues) {
        total += iq->instNum;
    }
    return total;
}

}
}
