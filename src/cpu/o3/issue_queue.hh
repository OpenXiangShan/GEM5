#ifndef __CPU_O3_ISSUE_QUEUE_HH__
#define __CPU_O3_ISSUE_QUEUE_HH__

#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "base/stats/group.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/mem_dep_unit.hh"
#include "cpu/reg_class.hh"
#include "cpu/timebuf.hh"
#include "params/IssueQue.hh"
#include "params/Scheduler.hh"
#include "params/SpecWakeupChannel.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace memory
{
class MemInterface;
} // namespace memory

class FUDesc;

namespace o3
{

class FUPool;
class CPU;
class IEW;
class WakeupQue;
class Scheduler;

/**
 *          insert into queue
 *                 |
 *                 V
 *         speculative schedule <-------+
 *                 |                    |
 *                 V                    |
 *           delay n cycle      replay and cancle
 *                 |                    ^
 *                 V                    |
 *           bypass sources             |
 *       all resources ready? (no)------+
 *               (yes)
 *                 |
 *                 V
 *  insert into fu, free IssueQueEntry
*/

class IssueQue : public SimObject
{
    friend class IssueCompletion;
    friend class Scheduler;
    friend class WakeupQue;

    std::string _name;
    const int inoutPorts;
    const int iqsize;
    const int scheduleToExecDelay;
    const std::string iqname;
    const std::vector<FUDesc *> fuDescs;

    int IQID = -1;

    struct compare_priority
    {
        bool operator()(const DynInstPtr& a, const DynInstPtr& b);
    };

    struct IssueStream
    {
        int size;
        DynInstPtr insts[8];
        void push(const DynInstPtr& inst);
        DynInstPtr pop();
    };
    TimeBuffer<IssueStream> inflightIssues;
    TimeBuffer<IssueStream>::wire toIssue;
    TimeBuffer<IssueStream>::wire toFu;

    std::deque<DynInstPtr> instList;
    uint64_t instNumInsert = 0;
    uint64_t instNum = 0;

    // s0: wakeup inst, add ready inst to readyInstsQue
    std::priority_queue<DynInstPtr, std::vector<DynInstPtr>, compare_priority> readyInsts;
    // s1: schedule readyInsts
    std::vector<DynInstPtr> selectedInst;

    // srcIdx : inst
    std::vector<std::vector<std::pair<int, DynInstPtr>>> subDepGraph;
    // update at writeback, delay one cycle by execute
    std::vector<bool>* noSpecScoreboard;
    // update at execute
    std::vector<bool>* bypassScoreboard;

    CPU* cpu = nullptr;
    WakeupQue* wakeupQue = nullptr;
    Scheduler* scheduler = nullptr;

    struct IssueQueStats : public statistics::Group
    {
        IssueQueStats(statistics::Group* parent, IssueQue* que, std::string name);
        statistics::Scalar issueSuccess;
        statistics::Scalar issueFailed;
        statistics::Scalar full;
        statistics::Scalar bwfull;
        statistics::Formula issueRate;
        statistics::Scalar retryMem;
        statistics::Vector insertDist;
        statistics::Vector issueDist;
    } *iqstats = nullptr;

    void replay(const DynInstPtr& inst);
    void addToFu(const DynInstPtr& inst);
    bool checkResource(const DynInstPtr& inst);
    void issueToFu();
    void wakeup(PhysRegIdPtr dst, bool speculative);
    void scheduleInst();
    void addIfReady(const DynInstPtr& inst);

  public:
    IssueQue(const IssueQueParams &params);
    void setIQID(int id) { IQID = id; }
    void setCPU(CPU* cpu);
    void resetDepGraph(int numPhysRegs);
    void setnoSpecScoreboard(std::vector<bool>* scoreboard) { noSpecScoreboard = scoreboard; }
    void setBypassScoreboard(std::vector<bool>* scoreboard) { bypassScoreboard = scoreboard; }
    void setWakeupQue(WakeupQue* que) { this->wakeupQue = que; }

    void tick();
    bool full();
    void insert(const DynInstPtr& inst);
    void insertNonSpec(const DynInstPtr& inst);

    void markMemDepDone(const DynInstPtr& inst);
    void retryMem(const DynInstPtr& inst);

    void specWakeup(PhysRegIdPtr dst);
    void writebackWakeup(PhysRegIdPtr dst);
    void doCommit(const InstSeqNum inst);
    void doSquash(const InstSeqNum seqNum);

    int getIssueStages() { return scheduleToExecDelay; }
    int getId() { return IQID; }
    // return IQ's name
    std::string getName() { return iqname; }
    // return gem5 simobject's name
    std::string name() { return _name; }
};

class SpecWakeupChannel : public SimObject
{
  public:
    std::string srcIQ;
    std::vector<std::string> dstIQ;
    SpecWakeupChannel(const SpecWakeupChannelParams& params)
      : SimObject(params),
        srcIQ(params.srcIQ),
        dstIQ(params.dstIQ)
    { }
};

// global SpecWakeupNetwork
class WakeupQue
{
    class SpecWakeupCompletion : public Event
    {
        DynInstPtr inst;
        WakeupQue* que = nullptr;
        IssueQue* to = nullptr;
      public:
        SpecWakeupCompletion(WakeupQue* que);
        void reset(const DynInstPtr& inst, IssueQue* to);
        void process() override;
        const char *description() const override;
    };
    std::vector<SpecWakeupCompletion*> freeEventList;

    Scheduler* scheduler;
    CPU* cpu;
    // global issueQues
    std::vector<IssueQue*>* issueQues;
    // if enable, specWakeupMatrix[from][to] >= 0;
    // auto adjust, specWakeupMatrix[from][to] = -1;
    // if disable, specWakeupMatrix[from][to] < -1;
    std::vector<std::vector<bool>> specWakeupMatrix;

  public:
    void init(std::vector<IssueQue*>* issueQues,
        std::vector<SpecWakeupChannel*> matrix, bool xbar);
    void setCPU(CPU* cpu) { this->cpu = cpu; }
    void setScheduler(Scheduler* scheduler) { this->scheduler = scheduler; }
    // call by IssueQue sch
    void insert(const DynInstPtr& inst, IssueQue* from);
    void cancel();
    std::string name() { return "wakeupQue"; }
};

class Scheduler : public SimObject
{
    friend class IssueQue;
    friend class WakeupQue;

    MemDepUnit *memDepUnit;

    std::vector<int> opExecTimeTable;
    std::vector<std::vector<IssueQue*>> dispTable;
    WakeupQue wakeupQue;
    std::vector<IssueQue*> issueQues;
    uint32_t combinedFus;

    std::vector<DynInstPtr> instsToFu;

    std::vector<bool> bypassScoreboard;
    std::vector<bool> noSpecScoreboard;
    std::vector<bool> specScoreboard;

    bool forwardDisp = false;

  public:
    Scheduler(const SchedulerParams& params);
    void setCPU(CPU* cpu);
    void resetDepGraph(uint64_t numPhysRegs);
    void setMemDepUnit(MemDepUnit *memDepUnit) { this->memDepUnit = memDepUnit; }

    void tick();
    bool full(const DynInstPtr& inst);
    // return true if insert successful
    void insert(const DynInstPtr& inst);
    void insertNonSpec(const DynInstPtr& inst);
    void addToFU(const DynInstPtr& inst);
    DynInstPtr getInstToFU();
    uint32_t getOpLatency(const DynInstPtr& inst);
    bool checkFuReady(const DynInstPtr& inst);
    void allocFu(const DynInstPtr& inst);
    bool hasReadyInsts();
    bool isDrained();

    void writebackWakeup(const DynInstPtr& inst);
    void bypassWriteback(const DynInstPtr& inst);

    void doCommit(const InstSeqNum seqNum);
    void doSquash(const InstSeqNum seqNum);
};


} // namespace o3
} // namespace gem5

#endif //__CPU_O3_INST_QUEUE_HH__
