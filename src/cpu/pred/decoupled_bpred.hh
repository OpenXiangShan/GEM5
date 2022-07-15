#ifndef __CPU_PRED_DECOUPLED_BPRED_HH__
#define __CPU_PRED_DECOUPLED_BPRED_HH__

#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/stream_struct.hh"
#include "cpu/pred/ubtb.hh"
#include "debug/DecoupleBP.hh"
#include "params/DecoupledBPU.hh"

namespace gem5 {

namespace branch_prediction {

struct FetchTargetEnqState
{
    Addr pc;
    FetchStreamId streamId;
    FetchTargetId desireTargetId;
    FetchTargetEnqState() : pc(0), streamId(1), desireTargetId(0) {}
};

class FetchTargetQueue
{
    // todo: move fetch target buffer here
    // 1. enqueue from fetch stream buffer
    // 2. supply fetch with fetch target head
    // 3. redirect fetch target head after squash
    using FTQ = std::map<FetchTargetId, FtqEntry>;
    using FTQIt = FTQ::iterator;
    FTQ ftq;
    unsigned ftqSize;
    FetchTargetId ftqId{0};  // this is a queue ptr for ftq itself

    // The supply/responsing fetch target state
    struct FetchTargetReadState
    {
        bool valid;
        FetchTargetId targetId;
        FtqEntry entry;
    };
    FetchTargetReadState supplyFetchTargetState;
    // The demanded fetch target ID to send to fetch
    FetchTargetId fetchDemandTargetId{0};

    FetchTargetEnqState fetchTargetEnqState;

  public:
    FetchTargetQueue(unsigned size) : ftqSize(size)
    {
        fetchTargetEnqState.pc = 0x80000000;
        fetchDemandTargetId = 0;
        supplyFetchTargetState.valid = false;
    }

    void squash(FetchTargetId new_enq_target_id,
                FetchStreamId new_enq_stream_id, Addr new_enq_pc) {
        ftq.clear();

        // Because we squash the whole ftq, head and tail should be the same
        auto new_fetch_demand_target_id = new_enq_target_id;

        fetchTargetEnqState.desireTargetId = new_enq_target_id;
        fetchTargetEnqState.streamId = new_enq_stream_id;
        fetchTargetEnqState.pc = new_enq_pc;

        supplyFetchTargetState.valid = false;
        fetchDemandTargetId = new_fetch_demand_target_id;
        DPRINTF(DecoupleBP,
                "FTQ demand stream ID update to %lu, FTQ demand pc update to "
                "%#lx\n",
                new_enq_stream_id,
                new_enq_pc);
    }

    bool fetchTargetAvailable() const
    {
        return supplyFetchTargetState.valid &&
               supplyFetchTargetState.targetId == fetchDemandTargetId;
    }

    FtqEntry &getTarget()
    {
        assert(fetchTargetAvailable());
        return supplyFetchTargetState.entry;
    }

    FetchTargetEnqState &getEnqState() { return fetchTargetEnqState; }

    FetchTargetId getSupplyingTargetId()
    {
        return supplyFetchTargetState.targetId;
    }

    FetchStreamId getSupplyingStreamId()
    {
        return supplyFetchTargetState.entry.fsqID;
    }
    void finishCurrentFetchTarget()
    {
        ++fetchDemandTargetId;
        ftq.erase(supplyFetchTargetState.targetId);
        DPRINTF(DecoupleBP,
                "Finish current fetch target: %lu, inc demand to %lu\n",
                supplyFetchTargetState.targetId,
                fetchDemandTargetId);
    }

    bool trySupplyFetchWithTarget() {
        if (!supplyFetchTargetState.valid ||
            supplyFetchTargetState.targetId != fetchDemandTargetId) {
            auto it = ftq.find(fetchDemandTargetId);
            if (it != ftq.end()) {
                DPRINTF(DecoupleBP,
                        "Found ftq entry with id %lu, writing to "
                        "fetchReadFtqEntryBuffer\n",
                        fetchDemandTargetId);
                supplyFetchTargetState.valid = true;
                supplyFetchTargetState.targetId = fetchDemandTargetId;
                supplyFetchTargetState.entry = it->second;
                return true;
            } else {
                DPRINTF(DecoupleBP,
                        "Target id %lu not found\n",
                        fetchDemandTargetId);
                if (!ftq.empty()) {
                    // sanity check
                    --it;
                    DPRINTF(DecoupleBP,
                            "Last entry of target queue: %lu\n",
                            it->first);
                    assert(it->first < fetchDemandTargetId);
                }
                return false;
            }
        }
        DPRINTF(DecoupleBP,
                "FTQ supplying, valid: %u, supply id: %u, demand id: %u\n",
                supplyFetchTargetState.valid,
                supplyFetchTargetState.targetId,
                fetchDemandTargetId);
        return true;
    }

    bool empty() const { return ftq.empty(); }

    unsigned size() const { return ftq.size(); }

    bool full() const { return ftq.size() >= ftqSize; }

    std::pair<bool, FTQIt> getDemandTargetIt() {
        FTQIt it = ftq.find(fetchDemandTargetId);
        return std::make_pair(it != ftq.end(), it);
    }

    void enqueue(FtqEntry entry) {
        DPRINTF(DecoupleBP,
                "Enqueueing target %lu with pc %#x and stream %lu\n",
                fetchTargetEnqState.desireTargetId, entry.startPC, entry.fsqID);
        ftq[fetchTargetEnqState.desireTargetId] = entry;
    }

    void dump(const char *when) {
        DPRINTF(DecoupleBP, "%s, dump FTQ\n", when);
        for (auto it = ftq.begin(); it != ftq.end(); ++it) {
            DPRINTFR(DecoupleBP,
                    "FTQ entry: %lu, pc: %#x, stream: %lu\n",
                    it->first, it->second.startPC, it->second.fsqID);
        }
    }
};

class DecoupledBPU : public BPredUnit
{
  public:
    typedef DecoupledBPUParams Params;

    DecoupledBPU(const Params &params);

  private:
    std::string _name;

    FetchTargetQueue fetchTargetQueue;

    std::map<FetchStreamId, FetchStream> fetchStreamQueue;  // this is a queue
                                                            // ptr for fsq
                                                            // itself
    unsigned fetchStreamQueueSize;
    FetchStreamId fsqId{1};

    unsigned cacheLineOffsetBits{6};  // TODO: parameterize this
    unsigned cacheLineSize{64};
    Addr alignToCacheLine(Addr addr) {
        return addr & ~((1 << cacheLineOffsetBits) - 1);
    }

    const unsigned historyBits{128};

    StreamUBTB *streamUBTB{};

    Addr s0StreamPC;
    boost::dynamic_bitset<> s0History;
    StreamPrediction s0UbtbPred;

    void tryEnqFetchStream();

    void tryEnqFetchTarget();

    void makeNewPredictionAndInsertFsq();

    void printStream(const FetchStream &e) {
        if (!e.resolved) {
            DPRINTFR(DecoupleBP,
                     "FSQ prediction:: %#lx-[%#lx, %#lx) --> %#lx, "
                     "hasEnteredFtq: %d\n",
                     e.streamStart,
                     e.predBranchAddr,
                     e.predStreamEnd,
                     e.predTarget,
                     e.hasEnteredFtq);
        } else {
            DPRINTFR(
                DecoupleBP,
                "Resolved: %i, resolved stream:: %#lx-[%#lx, %#lx) --> %#lx\n",
                e.resolved,
                e.streamStart,
                e.exeBranchAddr,
                e.exeStreamEnd,
                e.exeTarget);
        }
    }

    void printStreamFull(const FetchStream &e) {
        DPRINTFR(
            DecoupleBP,
            "FSQ prediction:: %#lx-[%#lx, %#lx) --> %#lx, hasEnteredFtq: %d\n",
            e.streamStart,
            e.predBranchAddr,
            e.predStreamEnd,
            e.predTarget,
            e.hasEnteredFtq);
        DPRINTFR(
            DecoupleBP,
            "Resolved: %i, resolved stream:: %#lx-[%#lx, %#lx) --> %#lx\n",
            e.exeEnded,
            e.streamStart,
            e.exeBranchAddr,
            e.exeStreamEnd,
            e.exeTarget);
    }

    void printFetchTarget(const FtqEntry &e, const char *when) {
        DPRINTFR(DecoupleBP,
                 "%s:: %#lx - [%#lx, %#lx) --> %#lx, taken: %d, fsqID: %lu\n",
                 when,
                 e.startPC,
                 e.takenPC,
                 e.endPC,
                 e.target,
                 e.taken,
                 e.fsqID);
    }

    void printFetchTargetFull(const FtqEntry &e) {
        DPRINTFR(DecoupleBP,
                 "Fetch Target:: %#lx-[%#lx, %#lx) --> %#lx\n",
                 e.startPC,
                 e.takenPC,
                 e.endPC,
                 e.target);
    }

  public:
    void tick();

    bool trySupplyFetchWithTarget();

    void squash(const InstSeqNum &squashed_sn, ThreadID tid) {
        panic("Squashing decoupled BP with tightly coupled API\n");
    }
    void squash(const InstSeqNum &squashed_sn, const PCStateBase &corr_target,
                bool actually_taken, ThreadID tid) {
        panic("Squashing decoupled BP with tightly coupled API\n");
    }


    std::pair<bool, bool> decoupledPredict(const StaticInstPtr &inst,
                                           const InstSeqNum &seqNum,
                                           PCStateBase &pc, ThreadID tid);

    void controlSquash(unsigned ftq_id, unsigned fsq_id,
                       const PCStateBase &control_pc,
                       const PCStateBase &target_pc,
                       const StaticInstPtr &static_inst, unsigned inst_bytes,
                       bool actually_taken, const InstSeqNum &squashed_sn,
                       ThreadID tid);

    void nonControlSquash(unsigned ftq_id, unsigned fsq_id,
                          const PCStateBase &inst_pc, const InstSeqNum seq,
                          ThreadID tid);

    void update(unsigned fsqID, ThreadID tid);

    unsigned getSupplyingTargetId()
    {
        return fetchTargetQueue.getSupplyingTargetId();
    }
    unsigned getSupplyingStreamId()
    {
        return fetchTargetQueue.getSupplyingStreamId();
    }

    void dumpFsq(const char *when);

    bool squashing{false};

    // Dummy overriding
    void uncondBranch(ThreadID tid, Addr pc, void *&bp_history) override {}

    void squash(ThreadID tid, void *bp_history) override {}

    void btbUpdate(ThreadID tid, Addr instPC, void *&bp_history) override {}

    void update(ThreadID tid, Addr instPC, bool taken, void *bp_history,
                bool squashed, const StaticInstPtr &inst,
                Addr corrTarget) override {}

    bool lookup(ThreadID tid, Addr instPC, void * &bp_history)
    {
        return false;
    }
};
}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_DECOUPLED_BPRED_HH__