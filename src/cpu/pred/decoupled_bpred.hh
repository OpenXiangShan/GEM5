#ifndef __CPU_PRED_DECOUPLED_BPRED_HH__
#define __CPU_PRED_DECOUPLED_BPRED_HH__

#include <vector>

#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/fetch_target_queue.hh"
#include "cpu/pred/stream_struct.hh"
#include "cpu/pred/modify_tage.hh"
#include "debug/DecoupleBP.hh"
#include "params/DecoupledBPU.hh"

namespace gem5
{

namespace branch_prediction
{

class DecoupledBPU : public BPredUnit
{
  public:
    typedef DecoupledBPUParams Params;

    DecoupledBPU(const Params &params);

  private:
    std::string _name;

    FetchTargetQueue fetchTargetQueue;

    std::map<FetchStreamId, FetchStream> fetchStreamQueue;
    unsigned fetchStreamQueueSize;
    FetchStreamId fsqId{1};

    unsigned cacheLineOffsetBits{6};  // TODO: parameterize this
    unsigned cacheLineSize{64};

    const unsigned historyTokenBits{8};

    constexpr Addr foldingTokenMask() { return (1 << historyTokenBits) - 1; }

    constexpr unsigned numFoldingTokens() { return 64/historyTokenBits; }

    const unsigned historyBits{128};

    StreamTAGE *streamTAGE{};

    Addr s0StreamPC;
    boost::dynamic_bitset<> s0History;
    StreamPrediction s0UbtbPred;

    boost::dynamic_bitset<> commitHistory;

    bool squashing{false};

    void tryEnqFetchStream();

    void tryEnqFetchTarget();

    void makeNewPredictionAndInsertFsq();

    Addr alignToCacheLine(Addr addr)
    {
        return addr & ~((1 << cacheLineOffsetBits) - 1);
    }

    Addr computePathHash(Addr br, Addr target);

    void histShiftIn(Addr hash, boost::dynamic_bitset<> &history);

    void printStream(const FetchStream &e)
    {
        if (!e.resolved) {
            DPRINTFR(DecoupleBP,
                     "FSQ prediction:: %#lx-[%#lx, %#lx) --> %#lx, "
                     "hasEnteredFtq: %d\n",
                     e.streamStart, e.predBranchAddr, e.predStreamEnd,
                     e.predTarget, e.hasEnteredFtq);
        } else {
            DPRINTFR(
                DecoupleBP,
                "Resolved: %i, resolved stream:: %#lx-[%#lx, %#lx) --> %#lx\n",
                e.resolved, e.streamStart, e.exeBranchAddr, e.exeStreamEnd,
                e.exeTarget);
        }
    }

    void printStreamFull(const FetchStream &e)
    {
        DPRINTFR(
            DecoupleBP,
            "FSQ prediction:: %#lx-[%#lx, %#lx) --> %#lx, hasEnteredFtq: %d\n",
            e.streamStart, e.predBranchAddr, e.predStreamEnd, e.predTarget,
            e.hasEnteredFtq);
        DPRINTFR(
            DecoupleBP,
            "Resolved: %i, resolved stream:: %#lx-[%#lx, %#lx) --> %#lx\n",
            e.exeEnded, e.streamStart, e.exeBranchAddr, e.exeStreamEnd,
            e.exeTarget);
    }

    void printFetchTarget(const FtqEntry &e, const char *when)
    {
        DPRINTFR(DecoupleBP,
                 "%s:: %#lx - [%#lx, %#lx) --> %#lx, taken: %d, fsqID: %lu\n",
                 when, e.startPC, e.takenPC, e.endPC, e.target, e.taken,
                 e.fsqID);
    }

    void printFetchTargetFull(const FtqEntry &e)
    {
        DPRINTFR(DecoupleBP, "Fetch Target:: %#lx-[%#lx, %#lx) --> %#lx\n",
                 e.startPC, e.takenPC, e.endPC, e.target);
    }

    bool streamQueueFull() const
    {
        return fetchStreamQueue.size() >= fetchStreamQueueSize;
    }

  public:
    void tick();

    bool trySupplyFetchWithTarget();

    void squash(const InstSeqNum &squashed_sn, ThreadID tid)
    {
        panic("Squashing decoupled BP with tightly coupled API\n");
    }
    void squash(const InstSeqNum &squashed_sn, const PCStateBase &corr_target,
                bool actually_taken, ThreadID tid)
    {
        panic("Squashing decoupled BP with tightly coupled API\n");
    }


    std::pair<bool, bool> decoupledPredict(const StaticInstPtr &inst,
                                           const InstSeqNum &seqNum,
                                           PCStateBase &pc, ThreadID tid);

    // redirect the stream
    void controlSquash(unsigned ftq_id, unsigned fsq_id,
                       const PCStateBase &control_pc,
                       const PCStateBase &target_pc,
                       const StaticInstPtr &static_inst, unsigned inst_bytes,
                       bool actually_taken, const InstSeqNum &squashed_sn,
                       ThreadID tid);

    // keep the stream: original prediction might be right
    // For memory violation, stream continues after squashing
    void nonControlSquash(unsigned ftq_id, unsigned fsq_id,
                          const PCStateBase &inst_pc, const InstSeqNum seq,
                          ThreadID tid);

    // Not a control. But stream is actually disturbed
    void trapSquash(unsigned ftq_id, unsigned fsq_id,
                    const PCStateBase &inst_pc, ThreadID tid);

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

    // Dummy overriding
    void uncondBranch(ThreadID tid, Addr pc, void *&bp_history) override {}

    void squash(ThreadID tid, void *bp_history) override {}

    void btbUpdate(ThreadID tid, Addr instPC, void *&bp_history) override {}

    void update(ThreadID tid, Addr instPC, bool taken, void *bp_history,
                bool squashed, const StaticInstPtr &inst,
                Addr corrTarget) override
    {
    }

    bool lookup(ThreadID tid, Addr instPC, void *&bp_history) { return false; }
};
}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_DECOUPLED_BPRED_HH__
