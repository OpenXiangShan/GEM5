#ifndef __CPU_PRED_DECOUPLED_BPRED_HH__
#define __CPU_PRED_DECOUPLED_BPRED_HH__

#include <vector>
#include <queue>
#include <utility> 

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

class HistoryManager
{
  public:
    struct TakenEntry
    {
        TakenEntry(Addr _pc, Addr _target, bool _miss, uint64_t stream_id)
            : pc(_pc), target(_target), miss(_miss), streamId(stream_id)
        {
        }
      Addr pc;
      Addr target;
      bool miss;
      uint64_t streamId;
    };

  private:
    std::list<TakenEntry> speculativeHists;

    unsigned IdealHistLen{20};

  public:
    void addSpeculativeHist(const Addr addr, const Addr target, const uint64_t stream_id)
    {
        speculativeHists.emplace_back(addr, target, addr == 0, stream_id);

        const auto &it = speculativeHists.back();
        DPRINTF(DecoupleBP,
                "Add taken %lu, %#lx->%#lx\n",
                it.streamId, it.pc, it.target);
    }

    void commit(const uint64_t stream_id)
    {
        auto it = speculativeHists.begin();
        while (speculativeHists.size() > IdealHistLen &&
               it != speculativeHists.end()) {
            if (it->streamId < stream_id) {
                DPRINTF(DecoupleBP,
                        "Commit taken %lu, %#lx->%#lx\n",
                        it->streamId, it->pc, it->target);
                it = speculativeHists.erase(it);
            } else {
                ++it;
            }
        }
    }

    const std::list<TakenEntry> &getSpeculativeHist()
    {
        return speculativeHists;
    }

    void squash(const uint64_t stream_id, bool taken, const Addr taken_pc, const Addr target)
    {
        dump("before squash");
        auto it = speculativeHists.begin();
        while (it != speculativeHists.end()) {
            if (it->streamId == stream_id) {
                if (taken) {
                    it->miss = false;
                    it->pc = taken_pc;
                    it->target = target;
                } else {
                    it->miss = true;
                    it->pc = 0;
                    it->target = 0;
                }
            } if (it->streamId > stream_id) {
                DPRINTF(DecoupleBP,
                        "Squash taken %lu, %#lx->%#lx\n",
                        it->streamId, it->pc, it->target);
                it = speculativeHists.erase(it);
            } else {
                ++it;
            }
        }
        dump("after squash");
        checkSanity();
    }

    void checkSanity()
    {
        if (speculativeHists.size() < 2) {
            return;
        }
        auto last = speculativeHists.begin();
        if (last->miss) {
            return;
        }
        auto cur = speculativeHists.begin();
        cur++;
        while (cur != speculativeHists.end()) {
            if (cur->miss) {
                break;
            }
            if (last->target > cur->pc) {
                DPRINTF(DecoupleBP,
                        "Sanity check failed: %#lx->%#lx, %#lx->%#lx\n",
                        last->pc, last->target, cur->pc, cur->target);
            }
            assert(last->target <= cur->pc);
            if (cur->pc - last->target > 1024) {
                warn("Stream %#lx-%#lx is too long", last->target, cur->pc);
            }
            if (cur->pc - last->target > 32*1024) {
                dump("before panic");
                panic("Stream %#lx-%#lx is too long", last->target, cur->pc);
            }
            last = cur;
            cur++;
        }
    }

    void dump(const char* when)
    {
        DPRINTF(DecoupleBP, "Dump ideal history %s:\n", when);
        for (auto it = speculativeHists.begin(); it != speculativeHists.end();
             it++) {
            DPRINTFR(DecoupleBP,
                     "stream: %lu, %#lx -> %#lx, miss: %d\n",
                     it->streamId, it->pc, it->target, it->miss);
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

    HistoryManager historyManager;

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
    void trapSquash(unsigned ftq_id, unsigned fsq_id, Addr last_committed_pc,
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

    void checkHistory(const boost::dynamic_bitset<> &history);

    std::string buf1, buf2;
};

}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_DECOUPLED_BPRED_HH__
