#ifndef __CPU_O3_ISSUE_QUEUE_HH__
#define __CPU_O3_ISSUE_QUEUE_HH__

#include <cstdint>
#include <list>
#include <string>

#include <boost/heap/priority_queue.hpp>

#include "base/statistics.hh"
#include "base/stats/group.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/timebuf.hh"
#include "params/IssuePort.hh"
#include "params/IssueQue.hh"
#include "params/Scheduler.hh"
#include "params/SpecWakeupChannel.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class FUDesc;

namespace o3
{

class FUPool;
class CPU;
class IEW;
class Scheduler;
class MemDepUnit;

/**
 *          insert into queue
 *                 |
 *                 V
 *         speculative schedule <-------+
 *                 |                    |
 *                 V                    |
 *      schedule (issueStage 0)   ------+
 *                 |                    |
 *                 V                    |
 *       delay (issueStage n)     wake or cancel
 *                 |                    |
 *                 V                    |
 *      issue success bypass datas      |
 *                 |                    |
 *                 V                    |
 *              execute ----------------+
*/

class IssuePort : public SimObject
{
  public:
    std::vector<FUDesc*> fu;
    boost::dynamic_bitset<> mask;
    IssuePort(const IssuePortParams &params);
};

class IssueQue : public SimObject
{
    friend class Scheduler;

    std::string _name;
    const int inports;
    const int outports;
    const int iqsize;
    const int scheduleToExecDelay;
    const std::string iqname;
    std::vector<FUDesc *> fuDescs;
    std::vector<bool> opPipelined;
    int IQID = -1;

    struct select_ploy
    {
        bool operator()(const DynInstPtr& a, const DynInstPtr& b) const;
    };
    using ReadyQue = boost::heap::priority_queue<DynInstPtr, boost::heap::compare<select_ploy>>;
    using SelectQue = std::vector<std::pair<uint32_t, DynInstPtr>>;

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

    std::list<DynInstPtr> instList;
    uint64_t instNumInsert = 0;
    uint64_t instNum = 0;

    std::vector<int> portBusy;
    // opclass mapping to pipeid
    std::vector<ReadyQue*> readyQclassify;
    // s0: wakeup inst, add ready inst to readyInstsQue
    std::vector<ReadyQue*> readyQs;
    // s1: schedule readyInsts
    SelectQue selectQ;

    // srcIdx : inst
    std::vector<std::vector<std::pair<int, DynInstPtr>>> subDepGraph;

    CPU* cpu = nullptr;
    Scheduler* scheduler = nullptr;

    struct IssueQueStats : public statistics::Group
    {
        IssueQueStats(statistics::Group* parent, IssueQue* que, std::string name);
        statistics::Scalar retryMem;
        statistics::Scalar canceledInst;
        statistics::Scalar loadmiss;
        statistics::Scalar arbFailed;
        statistics::Vector insertDist;
        statistics::Vector issueDist;
        statistics::Vector portBusy;
    } *iqstats = nullptr;

    void replay(const DynInstPtr& inst);
    void addToFu(const DynInstPtr& inst);
    void checkScoreboard(const DynInstPtr& inst);
    void issueToFu();
    void wakeUpDependents(const DynInstPtr& inst, bool speculative);
    void selectInst();
    void scheduleInst();
    void addIfReady(const DynInstPtr& inst);

  public:
    IssueQue(const IssueQueParams &params);
    void setIQID(int id) { IQID = id; }
    void setCPU(CPU* cpu);
    void resetDepGraph(int numPhysRegs);

    void tick();
    bool full();
    bool ready();
    int emptyEntries() const { return instNum; }
    void insert(const DynInstPtr& inst);
    void insertNonSpec(const DynInstPtr& inst);

    void markMemDepDone(const DynInstPtr& inst);
    void retryMem(const DynInstPtr& inst);

    void doCommit(const InstSeqNum inst);
    void doSquash(const InstSeqNum seqNum);

    int getIssueStages() { return scheduleToExecDelay; }
    int getId() { return IQID; }
    // return IQ's name
    std::string getName() { return iqname; }
    // return gem5 simobject's name
    std::string name() const override { return _name; }
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

class Scheduler : public SimObject
{
    friend class IssueQue;
    class SpecWakeupCompletion : public Event
    {
        DynInstPtr inst;
        IssueQue* to_issue_queue = nullptr;
      public:
        SpecWakeupCompletion(const DynInstPtr& inst, IssueQue* to);
        void process() override;
        const char *description() const override;
    };

    CPU* cpu;
    MemDepUnit *memDepUnit;

    struct disp_ploy
    {
        bool operator()(IssueQue* a, IssueQue* b) const;
    };
    using DispPloy = std::vector<IssueQue*>;

    std::vector<int> opExecTimeTable;
    std::vector<DispPloy> dispTable;
    std::vector<IssueQue*> issueQues;
    std::vector<std::vector<IssueQue*>> wakeMatrix;
    uint32_t combinedFus;

    std::vector<DynInstPtr> instsToFu;

    std::vector<bool> bypassScoreboard;
    std::vector<bool> scoreboard;

    struct Slot
    {
        uint32_t priority;// smaller is lower priority
        uint32_t resourceDemand;
        DynInstPtr inst;
        Slot(uint32_t priority, uint32_t demand, const DynInstPtr& inst);
    };
    struct slot_ploy
    {
        bool operator()(const Slot& a, const Slot& b) const;
    };
    using SlotQue = boost::heap::priority_queue<Slot, boost::heap::compare<slot_ploy>>;

    const uint32_t intSlotNum;
    const uint32_t fpSlotNum;
    uint32_t intSlotOccupied = 0;
    uint32_t fpSlotOccupied = 0;
    // interger slot
    SlotQue intSlot;
    // floating point slot
    SlotQue fpSlot;

    // used for searching dependency chain
    std::stack<DynInstPtr> dfs;

    // should call at issue first/last cycle,
    void wakeUpDependents(const DynInstPtr& inst, IssueQue* from_issue_queue);

  public:
    Scheduler(const SchedulerParams& params);
    void setCPU(CPU* cpu);
    void resetDepGraph(uint64_t numPhysRegs);
    void setMemDepUnit(MemDepUnit *memDepUnit) { this->memDepUnit = memDepUnit; }

    void tick();
    void issueAndSelect();
    bool full(const DynInstPtr& inst);
    bool ready(const DynInstPtr& inst);
    DynInstPtr getInstByDstReg(RegIndex flatIdx);

    void addProducer(const DynInstPtr& inst);
    // return true if insert successful
    void insert(const DynInstPtr& inst);
    void insertNonSpec(const DynInstPtr& inst);
    void addToFU(const DynInstPtr& inst);
    DynInstPtr getInstToFU();

    void insertSlot(const DynInstPtr& inst);

    void loadCancel(const DynInstPtr& inst);

    void writebackWakeup(const DynInstPtr& inst);
    void bypassWriteback(const DynInstPtr& inst);

    uint32_t getArbPriority(const DynInstPtr& inst);
    uint32_t getOpLatency(const DynInstPtr& inst);
    uint32_t getCorrectedOpLat(const DynInstPtr& inst);
    bool hasReadyInsts();
    bool isDrained();
    void doCommit(const InstSeqNum seqNum);
    void doSquash(const InstSeqNum seqNum);
    uint32_t getIQInsts();
};


} // namespace o3
} // namespace gem5

#endif //__CPU_O3_INST_QUEUE_HH__
