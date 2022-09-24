#ifndef __CPU_PRED_DECOUPLED_BPRED_HH__
#define __CPU_PRED_DECOUPLED_BPRED_HH__

#include <vector>
#include <queue>
#include <utility> 
#include <stack>

#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/fetch_target_queue.hh"
#include "cpu/pred/stream_struct.hh"
#include "cpu/pred/modify_tage.hh"
#include "cpu/pred/stream_loop_pred.hh"
#include "debug/DecoupleBP.hh"
#include "debug/DecoupleBPHist.hh"
#include "debug/DecoupleBPProbe.hh"
#include "debug/DecoupleBPRAS.hh"
#include "debug/DecoupleBPVerbose.hh"
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

    unsigned IdealHistLen{246};

  public:
    void addSpeculativeHist(const Addr addr, const Addr target,
                            const uint64_t stream_id, bool is_miss)
    {
        speculativeHists.emplace_back(addr, target, is_miss, stream_id);

        const auto &it = speculativeHists.back();
        DPRINTF(DecoupleBP, "Add taken: %i, stream %lu, %#lx->%#lx\n",
                !is_miss, it.streamId, it.pc, it.target);
    }
    void updateSpeculativeHist(const Addr addr, const Addr target, const uint64_t stream_id)
    {
        auto &it = speculativeHists.back();
        assert(it.streamId == stream_id);
        assert(it.miss);
        it.miss = false;
        it.pc = addr;
        it.target = target;

        DPRINTF(DecoupleBP,
                "Update taken %lu, %#lx->%#lx\n",
                it.streamId, it.pc, it.target);
    }


    void commit(const uint64_t stream_id)
    {
        auto it = speculativeHists.begin();
        while (speculativeHists.size() > IdealHistLen &&
               it != speculativeHists.end()) {
            if (it->streamId < stream_id) {
                DPRINTF(DecoupleBPVerbose,
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
            // why is it empty in logs?
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
                DPRINTF(DecoupleBPVerbose,
                        "Squash taken %lu, %#lx->%#lx\n",
                        it->streamId, it->pc, it->target);
                it = speculativeHists.erase(it);
            } else {
                DPRINTF(DecoupleBPVerbose,
                        "Skip stream %i when squashing stream %i\n",
                        it->streamId, stream_id);
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
        DPRINTF(DecoupleBPVerbose, "Dump ideal history %s:\n", when);
        for (auto it = speculativeHists.begin(); it != speculativeHists.end();
             it++) {
            DPRINTFR(DecoupleBPVerbose,
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

    const unsigned historyBits{488};

    const Addr MaxAddr{~(0ULL)};

    StreamTAGE *streamTAGE{};

    Addr s0PC;
    Addr s0StreamStartPC;
    boost::dynamic_bitset<> s0History;
    StreamPrediction s0UbtbPred;

    boost::dynamic_bitset<> commitHistory;

    bool squashing{false};

    HistoryManager historyManager;

    void tryEnqFetchStream();

    void tryEnqFetchTarget();

    void makeNewPrediction(bool create_new_stream);

    Addr alignToCacheLine(Addr addr)
    {
        return addr & ~((1 << cacheLineOffsetBits) - 1);
    }

    Addr computePathHash(Addr br, Addr target);

    void histShiftIn(Addr hash, boost::dynamic_bitset<> &history);

    void printStream(const FetchStream &e)
    {
        if (!e.resolved) {
            DPRINTFR(DecoupleBP, "FSQ Predicted stream: ");
        } else {
            DPRINTFR(DecoupleBP, "FSQ Resolved stream: ");
        }
        DPRINTFR(DecoupleBP,
                 "%#lx-[%#lx, %#lx) --> %#lx, ended: %i, taken: %i\n",
                 e.streamStart, e.getControlPC(), e.getEndPC(),
                 e.getNextStreamStart(), e.getEnded(), e.getTaken());
    }

    void printStreamFull(const FetchStream &e)
    {
        DPRINTFR(
            DecoupleBP,
            "FSQ prediction:: %#lx-[%#lx, %#lx) --> %#lx\n",
            e.streamStart, e.predBranchPC, e.predEndPC, e.predTarget);
        DPRINTFR(
            DecoupleBP,
            "Resolved: %i, resolved stream:: %#lx-[%#lx, %#lx) --> %#lx\n",
            e.exeEnded, e.streamStart, e.exeBranchPC, e.exeEndPC,
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

    bool trySupplyFetchWithTarget(Addr fetch_demand_pc);

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

    void squashStreamAfter(unsigned squash_stream_id);

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

    bool useStreamRAS(FetchStreamId sid);

    std::string buf1, buf2;

    std::stack<Addr> streamRAS;
    
    bool lastCyclePredicted;

    void dumpRAS() {
        for (std::stack<Addr> dump = streamRAS; !dump.empty(); dump.pop())
            DPRINTF(DecoupleBPRAS, "RAS: %#lx\n", dump.top());
    }

    bool debugFlagOn{false};

    struct MispredictEntry {
        Addr streamStart;
        Addr controlAddr;
        unsigned count;

        bool operator<(const MispredictEntry &rhs) const
        {
            return count < rhs.count;
        }

        bool operator==(const MispredictEntry &rhs) const
        {
            return count == rhs.count;
        }

        MispredictEntry() {};
        MispredictEntry(Addr streamStart, Addr controlAddr) :
                        streamStart(streamStart), controlAddr(controlAddr), count(1) {} ;
    };

    std::map<Addr, MispredictEntry> topMispredicts;
    std::map<uint64_t, uint64_t> topMispredHist;

    void setTakenEntryWithStream(const FetchStream &stream_entry, FtqEntry &ftq_entry);

    void setNTEntryWithStream(FtqEntry &ftq_entry, Addr endPC);

    bool popRAS(FetchStreamId stream_id, const char *when);

    void pushRAS(FetchStreamId stream_id, const char *when, Addr ra);

    StreamLoopPred *streamLoopPred{};
};

}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_DECOUPLED_BPRED_HH__
