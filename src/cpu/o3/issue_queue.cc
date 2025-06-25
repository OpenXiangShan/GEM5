#include "cpu/o3/issue_queue.hh"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <queue>
#include <stack>
#include <string>
#include <vector>

#include "base/logging.hh"
#include "base/stats/group.hh"
#include "base/stats/info.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/func_unit.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/reg_class.hh"
#include "debug/Counters.hh"
#include "debug/Dispatch.hh"
#include "debug/Schedule.hh"
#include "enums/OpClass.hh"
#include "params/BaseO3CPU.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

#define POPINST(x)                        \
    do {                                  \
        assert(instNum != 0);             \
        assert(opNum[x->opClass()] != 0); \
        opNum[x->opClass()]--;            \
        instNum--;                        \
    } while (0)

#define READYQ_PUSH(x)                                                                    \
    do {                                                                                  \
        (x)->setInReadyQ();                                                               \
        auto& readyQ = readyQclassify[(x)->opClass()];                                    \
        auto it = std::lower_bound(readyQ->begin(), readyQ->end(), (x), select_policy()); \
        readyQ->insert(it, (x));                                                          \
    } while (0)

// must be consistent with FUScheduler.py
// rfTypePortId = regfile typeid + portid
#define MAXVAL_TYPEPORTID (1 << (2 + 4)) // [5:4] is typeid, [3:0] is portid
#define RF_GET_PRIORITY(x) ((x) & 0b11)
#define RF_GET_PORTID(x) (((x) >> 2) & 0b1111)
#define RF_GET_TYPEID(x) ((x) >> 6)

#define RF_MAKE_TYPEPORTID(t, p) (((t) << 4) | (p))

#define RF_INTID 0
#define RF_FPID 1

namespace gem5
{

namespace o3
{

IssuePort::IssuePort(const IssuePortParams& params) : SimObject(params), rp(params.rp), fu(params.fu)
{
    for (auto it0 : params.fu) {
        for (auto it1 : it0->opDescList) {
            mask.set(it1->opClass);
        }
    }
}

bool
IssueQue::select_policy::operator()(const DynInstPtr& a, const DynInstPtr& b) const
{
    return a->seqNum < b->seqNum;
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
      ADD_STAT(issueOccupy, statistics::units::Count::get(), "count of replayQ blocked"),
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
    issueOccupy.flags(statistics::nozero);
}

IssueQue::IssueQue(const IssueQueParams& params)
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

    opNum.resize(enums::Num_OpClass, 0);
    portBusy.resize(outports, 0);

    intRfTypePortId.resize(outports);
    fpRfTypePortId.resize(outports);
    readyQs.resize(outports, nullptr);

    readyQclassify.resize(Num_OpClasses, nullptr);
    opPipelined.resize(Num_OpClasses, false);

    std::unordered_map<std::bitset<Num_OpClasses>, ReadyQue*> readyQmap;
    for (int i = 0; i < outports; i++) {
        auto oport = params.oports[i];
        for (auto rfp : oport->rp) {
            int rf_type = RF_GET_TYPEID(rfp);
            int rf_portid = RF_GET_PORTID(rfp);
            int rf_portPri = RF_GET_PRIORITY(rfp);
            assert(rf_portPri < 4);
            assert(RF_MAKE_TYPEPORTID(rf_type, rf_portid) < 64);
            if (rf_type == RF_INTID) {
                intRfTypePortId[i].push_back(std::make_pair(RF_MAKE_TYPEPORTID(rf_type, rf_portid), rf_portPri));
            } else if (rf_type == RF_FPID) {
                fpRfTypePortId[i].push_back(std::make_pair(RF_MAKE_TYPEPORTID(rf_type, rf_portid), rf_portPri));
            } else {
                panic("%s: Unknown RF type %d\n", iqname, rf_type);
            }
        }

        // safety check for outports
        for (int j = i + 1; j < outports; j++) {
            if ((oport->mask != params.oports[j]->mask) && (oport->mask & params.oports[j]->mask).any()) {
                panic("%s: Found the same opClass in different FU, portid: %d and %d\n", iqname, i, j);
            }
        }
        fuDescs.insert(fuDescs.begin(), oport->fu.begin(), oport->fu.end());

        auto it = readyQmap.find(oport->mask);
        ReadyQue* t = nullptr;
        if (it == readyQmap.end()) {
            // create a new ReadyQue
            t = new ReadyQue;
            readyQmap[oport->mask] = t;
        } else {
            // use the existing one
            t = it->second;
        }
        readyQs[i] = t;

        bool storePipeAcc = false, loadPipeAcc = false;
        for (auto fu : oport->fu) {
            for (auto op : fu->opDescList) {
                readyQclassify[op->opClass] = t;
                opPipelined[op->opClass] = op->pipelined;

                if (op->opClass >= MemReadOp && op->opClass <= VectorWholeRegisterLoadOp) {
                    loadPipeAcc = true;
                }
                if (op->opClass >= MemWriteOp && op->opClass <= VectorWholeRegisterStoreOp) {
                    storePipeAcc = true;
                }
            }
        }

        if (loadPipeAcc) numLoadPipe++;
        if (storePipeAcc) numStorePipe++;
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
IssueQue::resetDepGraph(int numPhysRegs)
{
    subDepGraph.resize(numPhysRegs);
}

bool
IssueQue::checkScoreboard(const DynInstPtr& inst)
{
    for (int i = 0; i < inst->numSrcRegs(); i++) {
        auto src = inst->renamedSrcIdx(i);
        if (src->isFixedMapping()) [[unlikely]] {
            continue;
        }
        // check bypass data ready or not
        if (!scheduler->bypassScoreboard[src->flatIndex()]) [[unlikely]] {
            auto dst_inst = scheduler->getInstByDstReg(src->flatIndex());
            if (!dst_inst || !dst_inst->isLoad()) {
                panic("dst[sn:%llu] is not load", dst_inst->seqNum);
            }
            DPRINTF(Schedule, "[sn:%llu] %s can't get data from bypassNetwork, dst inst: %s\n", inst->seqNum,
                inst->srcRegIdx(i), dst_inst->genDisassembly());
            scheduler->loadCancel(dst_inst);
            return false;
        }
    }
    return true;
}

void
IssueQue::addToFu(const DynInstPtr& inst)
{
    if (inst->isIssued()) [[unlikely]] {
        panic("%s [sn:%llu] has alreayd been issued\n", enums::OpClassStrings[inst->opClass()], inst->seqNum);
    }
    inst->setIssued();
    POPINST(inst);
    scheduler->addToFU(inst);
}

void
IssueQue::issueToFu()
{
    int size = toFu->size;
    int replayed = 0;
    int issued = 0;

    int issuedLoad = 0;
    int issuedStore = 0;

    // replay first
    for (;!replayQ.empty() && replayed < outports; replayed++) {
        auto& inst = replayQ.front();

        if (inst->isLoad()) {
            if (issuedLoad >= numLoadPipe) {
                break;
            }
            issuedLoad++;
        }
        if (inst->isStore()) {
            if (issuedStore >= numStorePipe) {
                break;
            }
            issuedStore++;
        }

        scheduler->addToFU(inst);
        DPRINTF(Schedule, "[sn:%llu] replayed to FU\n", inst->seqNum);
        replayQ.pop();
        issued++;
    }

    for (int i = 0; i < size; i++) {
        auto inst = toFu->pop();
        if (!inst) {
            continue;
        }
        if ((i + replayed >= outports) ||
            (inst->isLoad() && (issuedLoad >= numLoadPipe)) ||
            (inst->isStore() && (issuedStore >= numStorePipe))) {
            inst->clearScheduled();
            // only for load/store
            READYQ_PUSH(inst);
            DPRINTF(Schedule, "[sn:%llu] issue failed due to being occupied\n", inst->seqNum);
            continue;
        }
        if (!checkScoreboard(inst)) {
            continue;
        }

        if (inst->isLoad()) {
            issuedLoad++;
        }
        if (inst->isStore()) {
            issuedStore++;
        }
        addToFu(inst);
        cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtIssueReadReg);
        issued++;
    }

    if (issued > 0) {
        iqstats->issueDist[issued]++;
    }
    if (replayed) {
        iqstats->issueOccupy += replayed;
    }
}

void
IssueQue::retryMem(const DynInstPtr& inst)
{
    assert(!inst->isNonSpeculative());
    iqstats->retryMem++;
    DPRINTF(Schedule, "retry %s [sn:%llu]\n", enums::OpClassStrings[inst->opClass()], inst->seqNum);
    replayQ.push(inst);
}

bool
IssueQue::idle()
{
    bool idle = false;
    for (auto it : readyQs) {
        if (it->size()) {
            idle = true;
        }
    }
    idle |= replayQ.size() > 0;
    return idle;
}

void
IssueQue::markMemDepDone(const DynInstPtr& inst)
{
    assert(inst->isMemRef());
    DPRINTF(Schedule, "[sn:%llu] has solved memdependency\n", inst->seqNum);
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
            continue;
        }
        scheduler->regCache.insert(dst->flatIndex(), {});
        DPRINTF(Schedule, "was %s woken by p%lu [sn:%llu]\n", speculative ? "spec" : "wb", dst->flatIndex(),
                inst->seqNum);
        auto& depgraph = subDepGraph[dst->flatIndex()];
        for (auto& it : depgraph) {
            int srcIdx = it.first;
            auto& consumer = it.second;
            if (consumer->readySrcIdx(srcIdx)) {
                continue;
            }
            consumer->markSrcRegReady(srcIdx);


            DPRINTF(Schedule, "[sn:%llu] src%d was woken\n", consumer->seqNum, srcIdx);
            addIfReady(consumer);
        }

        if (!speculative) {
            depgraph.clear();
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

        // Add the instruction to the proper ready list.
        if (inst->isMemRef()) {
            if (inst->memDepSolved()) {
                DPRINTF(Schedule, "memRef Dependency was solved can issue\n");
            } else {
                DPRINTF(Schedule, "memRef Dependency was not solved can't issue\n");
                return;
            }
        }

        DPRINTF(Schedule, "[sn:%llu] add to readyInstsQue\n", inst->seqNum);
        inst->clearCancel();
        if (!inst->inReadyQ()) {
            READYQ_PUSH(inst);
        }
    }
}

void
IssueQue::cancel(const DynInstPtr& inst)
{
    // before issued
    assert(!inst->isIssued());

    inst->setCancel();
    if (inst->isScheduled() && !opPipelined[inst->opClass()]) {
        inst->clearScheduled();
        portBusy[inst->issueportid] = 0;
    }

    iqstats->canceledInst++;
}

void
IssueQue::selectInst()
{
    selectQ.clear();
    for (int pi = 0; pi < outports; pi++) {
        auto readyQ = readyQs[pi];

        for (auto it = readyQ->begin(); it != readyQ->end();) {
            auto& inst = *it;

            // skipp canceled inst and wb conflict inst
            if (inst->canceled()) {
                inst->clearInReadyQ();
                it = readyQ->erase(it);
                continue;
            }

            if (!(portBusy[pi] & (1llu << scheduler->getCorrectedOpLat(inst)))) {
                DPRINTF(Schedule, "[sn %ld] was selected\n", inst->seqNum);

                // get regfile read port
                for (int i = 0; i < inst->numSrcRegs(); i++) {
                    auto src = inst->srcRegIdx(i);
                    PhysRegIdPtr psrc = inst->renamedSrcIdx(i);
                    if (psrc->isFixedMapping())
                        continue;
                    std::pair<int, int> rfTypePortId;
                    // read port is point to point with srcid
                    if (src.isIntReg() && intRfTypePortId[pi].size() > i) {
                        rfTypePortId = intRfTypePortId[pi][i];
                        scheduler->useRegfilePort(inst, psrc, rfTypePortId.first, rfTypePortId.second);
                    } else if (src.isFloatReg() && fpRfTypePortId[pi].size() > i) {
                        rfTypePortId = fpRfTypePortId[pi][i];
                        scheduler->useRegfilePort(inst, psrc, rfTypePortId.first, rfTypePortId.second);
                    }
                }

                selectQ.push_back(std::make_pair(pi, inst));
                inst->clearInReadyQ();
                readyQ->erase(it);
                break;
            } else {
                iqstats->portBusy[pi]++;
            }

            it++;
        }
    }
}

void
IssueQue::scheduleInst()
{
    // here is issueStage 0
    for (auto& info : selectQ) {
        auto& pi = info.first;  // issue port id
        auto& inst = info.second;
        if (inst->canceled()) {
            DPRINTF(Schedule, "[sn:%llu] was canceled\n", inst->seqNum);
        } else if (inst->arbFailed()) {
            DPRINTF(Schedule, "[sn:%llu] arbitration failed, retry\n", inst->seqNum);
            iqstats->arbFailed++;
            assert(inst->readyToIssue());

            READYQ_PUSH(inst);
        } else [[likely]] {
            DPRINTF(Schedule, "[sn:%llu] no conflict, scheduled\n", inst->seqNum);
            iqstats->portissued[pi]++;
            inst->setScheduled();
            toIssue->push(inst);
            inst->issueportid = pi;

            if (!opPipelined[inst->opClass()]) {
                portBusy[pi] = -1ll;
            } else if (scheduler->getCorrectedOpLat(inst) > 1) {
                portBusy[pi] |= 1ll << scheduler->getCorrectedOpLat(inst);
            }

            scheduler->specWakeUpDependents(inst, this);
            cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtIssueArb);
        }
        inst->clearArbFailed();
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

    scheduleInst();
    inflightIssues.advance();

    for (auto& t : portBusy) {
        t = t >> 1;
    }
}

bool
IssueQue::ready()
{
    bool bwFull = instNumInsert >= inports;
    bool full = (instNum >= iqsize) || (replayQ.size() > replayQsize);
    if (bwFull) {
        DPRINTF(Schedule, "can't insert more due to inports exhausted\n");
    }
    if (full) {
        DPRINTF(Schedule, "has full!\n");
    }
    return !full && !bwFull;
}

void
IssueQue::insert(const DynInstPtr& inst)
{
    assert(instNum < iqsize);
    opNum[inst->opClass()]++;
    instNum++;
    instNumInsert++;

    cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtIssueQue);

    DPRINTF(Schedule, "[sn:%llu] %s insert into %s\n", inst->seqNum, enums::OpClassStrings[inst->opClass()], iqname);
    inst->issueQue = this;
    instList.emplace_back(inst);
    bool addToDepGraph = false;
    for (int i = 0; i < inst->numSrcRegs(); i++) {
        auto src = inst->renamedSrcIdx(i);
        if (!inst->readySrcIdx(i) && !src->isFixedMapping()) {
            if (scheduler->scoreboard[src->flatIndex()]) {
                inst->markSrcRegReady(i);
            } else {
                if (scheduler->earlyScoreboard[src->flatIndex()]) {
                    inst->markSrcRegReady(i);
                }
                DPRINTF(Schedule, "[sn:%llu] src p%d add to depGraph\n", inst->seqNum, src->flatIndex());
                subDepGraph[src->flatIndex()].push_back({i, inst});
                addToDepGraph = true;
            }
        }
    }
    if (!addToDepGraph) {
        assert(inst->readyToIssue());
    }


    /** For memory-related instructions, memory dependency prediction is
     * used to determine whether they can be out of order execution.
     * -- pass the dependency check: instruction can be schedule.
     * -- failed in dependency check: schedule in the store address be computered.
     */
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
    DPRINTF(Schedule, "[sn:%llu] insertNonSpec into %s\n", inst->seqNum, iqname);
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
    for (auto it = instList.begin(); it != instList.end();) {
        if ((*it)->seqNum > seqNum) {
            if (!(*it)->isIssued()) {
                POPINST((*it));
                (*it)->setIssued();
            }
            if ((*it)->isScheduled() && (*it)->issueportid >= 0 && !opPipelined[(*it)->opClass()]) {
                portBusy.at((*it)->issueportid) = 0;
            }

            (*it)->setSquashedInIQ();
            (*it)->setCanCommit();
            (*it)->clearScheduled();
            (*it)->setCancel();
            it = instList.erase(it);
            assert(instList.size() >= instNum);
        } else {
            it++;
        }
    }

    for (int i = 0; i <= getIssueStages(); i++) {
        int size = inflightIssues[-i].size;
        for (int j = 0; j < size; j++) {
            auto& inst = inflightIssues[-i].insts[j];
            if (inst && inst->isSquashed()) {
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

Scheduler::SpecWakeupCompletion::SpecWakeupCompletion(const DynInstPtr& inst,
    IssueQue* to, PendingWakeEventsType* owner)
    : Event(Stat_Event_Pri, AutoDelete),
      inst(inst), owner(owner), to_issue_queue(to)
{
}

void
Scheduler::SpecWakeupCompletion::process()
{
    to_issue_queue->wakeUpDependents(inst, true);
    (*owner)[inst->seqNum].erase(this);
}

const char*
Scheduler::SpecWakeupCompletion::description() const
{
    return "Spec wakeup completion";
}

Scheduler::SchedulerStats::SchedulerStats(statistics::Group* parent)
  : statistics::Group(parent),
    ADD_STAT(exec_stall_cycle, "SUM(OpsExecuted[= FEW])"),
    ADD_STAT(memstall_any_load,
        "Cycles with no uops executed and at least X in-flight load that is not completed yet"),
    ADD_STAT(memstall_any_store,
        "Cycles with few uops executed and no more stores can be issued"),
    ADD_STAT(memstall_l1miss,
        "Cycles with no uops executed and at least X in-flight load that has missed the L1-cache"),
    ADD_STAT(memstall_l2miss,
        "Cycles with no uops executed and at least X in-flight load that has missed the L2-cache"),
    ADD_STAT(memstall_l3miss,
        "Cycles with no uops executed and at least X in-flight load that has missed the L3-cache")
{
}

bool
Scheduler::disp_policy::operator()(IssueQue* a, IssueQue* b) const
{
    // initNum smaller first
    int p0 = a->opNum[disp_op];
    int p1 = b->opNum[disp_op];
    return p0 < p1;
}

Scheduler::Scheduler(const SchedulerParams& params)
    : SimObject(params), old_disp(params.useOldDisp), stats(this), issueQues(params.IQs)
{
    dispTable.resize(enums::OpClass::Num_OpClass);
    opExecTimeTable.resize(enums::OpClass::Num_OpClass, 1);
    opPipelined.resize(enums::OpClass::Num_OpClass, false);

    boost::dynamic_bitset<> opChecker(enums::Num_OpClass, 0);
    std::vector<int> rfportChecker(MAXVAL_TYPEPORTID, 0);
    int maxTypePortId = 0;
    for (int i = 0; i < issueQues.size(); i++) {
        issueQues[i]->setIQID(i);
        issueQues[i]->scheduler = this;
        combinedFus += issueQues[i]->outports;
        panic_if(issueQues[i]->fuDescs.size() == 0, "Empty config IssueQue: " + issueQues[i]->getName());
        for (auto fu : issueQues[i]->fuDescs) {
            for (auto op : fu->opDescList) {
                opExecTimeTable[op->opClass] = op->opLat;
                opPipelined[op->opClass] = op->pipelined;
                dispTable[op->opClass].push_back(issueQues[i]);
                opChecker.set(op->opClass);
            }
        }

        for (auto rfTypePortId : issueQues[i]->intRfTypePortId) {
            for (auto &typePortId : rfTypePortId) {
                maxTypePortId = std::max(maxTypePortId, typePortId.first);
                rfportChecker[typePortId.first] += 1;
            }
        }
        for (auto rfTypePortId : issueQues[i]->fpRfTypePortId) {
            for (auto typePortId : rfTypePortId) {
                maxTypePortId = std::max(maxTypePortId, typePortId.first);
                rfportChecker[typePortId.first] += 1;
            }
        }
    }
    maxTypePortId += 1;
    assert(maxTypePortId <= MAXVAL_TYPEPORTID);
    rfMaxTypePortId = maxTypePortId;
    rfPortOccupancy.resize(maxTypePortId, {nullptr, 0});

    if (opChecker.count() != enums::Num_OpClass) {
        for (int i = 0; i < enums::Num_OpClass; i++) {
            if (!opChecker[i]) {
                warn("No config for opClass: %s\n", enums::OpClassStrings[i]);
            }
        }
    }

    wakeMatrix.resize(issueQues.size());
    auto findIQbyname = [this](std::string name) -> IssueQue* {
        IssueQue* ret = nullptr;
        for (auto it : this->issueQues) {
            if (it->getName().compare(name) == 0) {
                if (ret) {
                    panic("has duplicate IQ name: %s\n", name);
                }
                ret = it;
            }
        }
        warn_if(!ret, "can't find IQ by name: %s\n", name);
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
            for (auto src : it->srcIQs) {
                auto srcIQ = findIQbyname(src);
                if (srcIQ) {
                    for (auto dstIQname : it->dstIQs) {
                        auto dstIQ = findIQbyname(dstIQname);
                        if (dstIQ) {
                            wakeMatrix[srcIQ->getId()].push_back(dstIQ);
                            DPRINTF(Schedule, "build wakeup channel: %s -> %s\n", srcIQ->getName(), dstIQ->getName());
                        }
                    }
                }
            }
        }
    }

    assert(dispTable[MemWriteOp].size() == dispTable[StoreDataOp].size());

    dispSeqVec.resize(64);
}

void
Scheduler::setCPU(CPU* cpu, LSQ* lsq)
{
    this->cpu = cpu;
    this->lsq = lsq;
    for (auto it : issueQues) {
        it->setCPU(cpu);
    }
}

void
Scheduler::resetDepGraph(uint64_t numPhysRegs)
{
    scoreboard.resize(numPhysRegs, true);
    bypassScoreboard.resize(numPhysRegs, true);
    earlyScoreboard.resize(numPhysRegs, true);
    for (auto it : issueQues) {
        it->resetDepGraph(numPhysRegs);
    }
}

void
Scheduler::addToFU(const DynInstPtr& inst)
{
#if TRACING_ON
    inst->issueTick = curTick() - inst->fetchTick;
#endif
    inst->clearCancel();
    DPRINTF(Schedule, "%s [sn:%llu] add to FUs\n", enums::OpClassStrings[inst->opClass()], inst->seqNum);
    instsToFu.push_back(inst);
}

void
Scheduler::tick()
{
    // we need to update portBusy counter each cycle
    cpu->activateStage(CPU::IEWIdx);
    for (auto it : issueQues) {
        it->tick();
    }
}

void
Scheduler::issueAndSelect()
{
    // must wait for all insts was issued
    for (auto it : issueQues) {
        it->selectInst();
    }
    // inst arbitration
    for (auto inst : arbFailedInsts) {
        inst->setArbFailed();
    }
    arbFailedInsts.clear();
    std::fill(rfPortOccupancy.begin(), rfPortOccupancy.end(), std::make_pair(nullptr, 0));


    for (auto it : issueQues) {
        it->issueToFu();
    }
    if (instsToFu.size() < intel_fewops) {
        stats.exec_stall_cycle++;
        if (lsq->anyStoreNotExecute()) stats.memstall_any_store++;
    }
    if (instsToFu.size() == 0) {
        int misslevel = lsq->anyInflightLoadsNotComplete();
        if (misslevel != 0) stats.memstall_any_load++;
        if ((misslevel & ((1<<1) - 1)) == ((1<<1) - 1)) stats.memstall_l1miss++;
        if ((misslevel & ((1<<2) - 1)) == ((1<<2) - 1)) stats.memstall_l2miss++;
        if ((misslevel & ((1<<3) - 1)) == ((1<<3) - 1)) stats.memstall_l3miss++;
    }
}

void
Scheduler::lookahead(std::deque<DynInstPtr>& insts)
{
    if (old_disp) {
        // donothing
    } else {
        uint8_t disp_op_num[Num_OpClasses];
        std::memset(disp_op_num, 0, Num_OpClasses);
        int i = 0;
        for (auto& it : insts) {
            auto& iqs = dispTable[it->opClass()];
            std::sort(iqs.begin(), iqs.end(), disp_policy(it->opClass()));
            if (it->isSplitStoreAddr()) {
                auto& iqs = dispTable[StoreDataOp];
                std::sort(iqs.begin(), iqs.end(), disp_policy(StoreDataOp));
            }

            dispSeqVec[i] = disp_op_num[it->opClass()] % dispTable[it->opClass()].size();
            disp_op_num[it->opClass()]++;
            i++;
        }
    }
}

bool
Scheduler::ready(const DynInstPtr& inst, int disp_seq)
{
    if (inst->staticInst->isSplitStoreAddr() && !ready(StoreDataOp, disp_seq)) {
        return false;
    }

    auto& iqs = dispTable[inst->opClass()];
    assert(!iqs.empty());

    if (old_disp) [[unlikely]] {
        for (auto iq : iqs) {
            if (iq->ready()) {
                return true;
            }
        }
    } else {
        if (iqs[dispSeqVec.at(disp_seq)]->ready()) {
            return true;
        }
    }

    DPRINTF(Schedule, "IQ not ready, opclass: %s\n", enums::OpClassStrings[inst->opClass()]);
    return false;
}

bool
Scheduler::ready(OpClass op, int disp_seq)
{
    auto& iqs = dispTable[op];
    assert(!iqs.empty());

    if (old_disp) {
        for (auto iq : iqs) {
            if (iq->ready()) {
                return true;
            }
        }
    } else {
        if (iqs[dispSeqVec.at(disp_seq)]->ready()) {
            return true;
        }
    }

    DPRINTF(Schedule, "IQ not ready, opclass: %s\n", enums::OpClassStrings[op]);
    return false;
}

DynInstPtr
Scheduler::getInstByDstReg(RegIndex flatIdx)
{
    for (auto iq : issueQues) {
        for (auto& inst : iq->instList) {
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
    DPRINTF(Schedule, "[sn:%llu] addProdecer\n", inst->seqNum);
    for (int i = 0; i < inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        scoreboard[dst->flatIndex()] = false;
        bypassScoreboard[dst->flatIndex()] = false;
        earlyScoreboard[dst->flatIndex()] = false;
        DPRINTF(Schedule, "mark scoreboard p%lu not ready\n", dst->flatIndex());
    }
}

void
Scheduler::insert(const DynInstPtr& inst, int disp_seq)
{
    if (inst->isSplitStoreAddr()) {
        auto stduop = inst->createStoreDataUop();
        this->insert(stduop, disp_seq);
        // transform self to storeAddruop
        inst->buildStoreAddrUop();
    }

    auto& iqs = dispTable[inst->opClass()];

    if (old_disp) {
        bool insert = false;
        std::sort(iqs.begin(), iqs.end(), disp_policy(inst->opClass()));
        for (auto iq : iqs) {
            if (iq->ready()) {
                insert = true;
                iq->insert(inst);
                break;
            }
        }
        panic_if(!insert, "can't find ready IQ to insert");
    } else {
        assert(iqs[dispSeqVec.at(disp_seq)]->ready());
        iqs[dispSeqVec.at(disp_seq)]->insert(inst);
    }

    DPRINTF(Schedule, "[sn:%llu] dispatch: %s\n", inst->seqNum, inst->staticInst->disassemble(0));
}

void
Scheduler::insertNonSpec(const DynInstPtr& inst)
{
    auto& iqs = dispTable[inst->opClass()];

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
    if (!opPipelined[inst->opClass()] || inst->numDestRegs() == 0 || inst->isLoad()) {
        return;
    }

    for (auto to : wakeMatrix[from_issue_queue->getId()]) {
        int oplat = getCorrectedOpLat(inst);
        int wakeDelay = oplat - 1;
        assert(oplat < 64);
        int diff = std::abs(from_issue_queue->getIssueStages() - to->getIssueStages());
        if (from_issue_queue->getIssueStages() > to->getIssueStages()) {
            wakeDelay += diff;
        } else if (wakeDelay >= diff) {
            wakeDelay -= diff;
        }

        DPRINTF(Schedule, "[sn:%llu] %s create wakeupEvent to %s, delay %d cycles\n", inst->seqNum,
                from_issue_queue->getName(), to->getName(), wakeDelay);
        if (wakeDelay == 0) {
            to->wakeUpDependents(inst, true);
            if (!(inst->isFloating() || inst->isVector())) {
                for (int i = 0; i < inst->numDestRegs(); i++) {
                    PhysRegIdPtr dst = inst->renamedDestIdx(i);
                    if (dst->isFixedMapping()) [[unlikely]] {
                        continue;
                    }
                    earlyScoreboard[dst->flatIndex()] = true;
                }
            }
        } else {
            auto wakeEvent = new SpecWakeupCompletion(inst, to, &specWakeEvents);
            // track these pending events
            specWakeEvents[inst->seqNum].insert(wakeEvent);
            cpu->schedule(wakeEvent, cpu->clockEdge(Cycles(wakeDelay)) - 1);
        }
    }
}

void
Scheduler::specWakeUpFromLoadPipe(const DynInstPtr& inst)
{
    assert(inst->isLoad());
    auto from_issue_queue = inst->issueQue;
    for (auto to : wakeMatrix[from_issue_queue->getId()]) {

        DPRINTF(Schedule, "[sn:%llu] %s create wakeupEvent to %s at loadpipe, no delay\n", inst->seqNum,
                from_issue_queue->getName(), to->getName());
        to->wakeUpDependents(inst, true);

        for (int i = 0; i < inst->numDestRegs(); i++) {
            PhysRegIdPtr dst = inst->renamedDestIdx(i);
            if (dst->isFixedMapping()) [[unlikely]] {
                continue;
            }
            earlyScoreboard[dst->flatIndex()] = true;
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

bool
Scheduler::checkRfPortBusy(int typePortId, int pri)
{
    if (rfPortOccupancy[typePortId].first && rfPortOccupancy[typePortId].second > pri) {
        return false;
    }
    return true;
}

void
Scheduler::useRegfilePort(const DynInstPtr& inst, const PhysRegIdPtr& regid, int typePortId, int pri)
{
    if (regid->is(IntRegClass)) {
        if (regCache.contains(regid->flatIndex())) {
            regCache.get(regid->flatIndex());
            return;
        }
    }
    assert(typePortId < rfPortOccupancy.size());
    if (rfPortOccupancy[typePortId].first) {
        if (rfPortOccupancy[typePortId].second < pri) { // smaller is higher priority
            // inst arbitration failure
            arbFailedInsts.push_back(inst);
            DPRINTF(Schedule, "[sn:%llu] arbitration failure, typePortId %d occupied by [sn:%llu]\n", inst->seqNum, typePortId,
                    rfPortOccupancy[typePortId].first->seqNum);
            return;
        } else {
            // rfPortOccupancy[typePortId].first arbitration failure
            arbFailedInsts.push_back(rfPortOccupancy[typePortId].first);
            DPRINTF(Schedule, "[sn:%llu] arbitration failure, typePortId %d occupied by [sn:%llu]\n",
                    rfPortOccupancy[typePortId].first->seqNum, typePortId, inst->seqNum);
        }
    }
    rfPortOccupancy[typePortId] = std::make_pair(inst, pri);
}

void
Scheduler::loadCancel(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn:%llu] %s cache miss, cancel consumers\n", inst->seqNum,
            enums::OpClassStrings[inst->opClass()]);
    if (inst->issueQue) {
        inst->issueQue->iqstats->loadmiss++;
    }

    dfs.push(inst);
    while (!dfs.empty()) {
        auto top = dfs.top();
        dfs.pop();
        // clear pending wake events scheduled by top
        auto& pendingEvents = specWakeEvents[top->seqNum];
        for (auto it = pendingEvents.begin(); it != pendingEvents.end(); it++) {
            cpu->deschedule(*it);
        }
        specWakeEvents.erase(top->seqNum);
        for (int i = 0; i < top->numDestRegs(); i++) {
            auto dst = top->renamedDestIdx(i);
            if (dst->isFixedMapping()) {
                continue;
            }
            earlyScoreboard[dst->flatIndex()] = false;
            for (auto iq : issueQues) {
                for (auto& it : iq->subDepGraph[dst->flatIndex()]) {
                    int srcIdx = it.first;
                    auto& depInst = it.second;
                    if (depInst->readySrcIdx(srcIdx)) {
                        DPRINTF(Schedule, "cancel [sn:%llu], clear src p%d ready\n", depInst->seqNum,
                                depInst->renamedSrcIdx(srcIdx)->flatIndex());
                        depInst->issueQue->cancel(depInst);
                        depInst->clearSrcRegReady(srcIdx);
                        dfs.push(depInst);
                    }
                }
            }
        }
    }

    for (auto iq : issueQues) {
        for (int i = 0; i <= iq->getIssueStages(); i++) {
            int size = iq->inflightIssues[-i].size;
            for (int j = 0; j < size; j++) {
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
    DPRINTF(Schedule, "[sn:%llu] was writeback\n", inst->seqNum);
    inst->setWriteback();  // clear in issueQue
    cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtWriteVal);
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
    if (!opPipelined[inst->opClass()] && inst->issueportid >= 0) {
        inst->issueQue->portBusy[inst->issueportid] = 0;
    }
    cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtBypassVal);
    DPRINTF(Schedule, "[sn:%llu] bypass write\n", inst->seqNum);
    for (int i = 0; i < inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        bypassScoreboard[dst->flatIndex()] = true;
        DPRINTF(Schedule, "p%lu in bypassNetwork ready\n", dst->flatIndex());
    }
}

uint32_t
Scheduler::getOpLatency(const DynInstPtr& inst)
{
    if (inst->opClass() == FloatCvtOp) [[unlikely]] {
        if (inst->destRegIdx(0).isFloatReg()) {
            return 2 + opExecTimeTable[inst->opClass()];
        }
    }
    return opExecTimeTable[inst->opClass()];
}

uint32_t
Scheduler::getCorrectedOpLat(const DynInstPtr& inst)
{
    uint32_t oplat = getOpLatency(inst);
    return oplat;
}

bool
Scheduler::hasReadyInsts()
{
    for (auto it : issueQues) {
        if (!it->idle()) {
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
    uint32_t total = 0;
    for (auto iq : issueQues) {
        total += iq->instNum;
    }
    return total;
}

}
}
