#ifndef __CPU_PRED_FTB_DECOUPLED_BPRED_HH__
#define __CPU_PRED_FTB_DECOUPLED_BPRED_HH__

#include <array>
#include <queue>
#include <stack>
#include <utility> 
#include <vector>

#include "arch/generic/pcstate.hh"
#include "config/the_isa.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/general_arch_db.hh"
#include "cpu/pred/ftb/fetch_target_queue.hh"
#include "cpu/pred/ftb/ftb.hh"
#include "cpu/pred/ftb/ftb_tage.hh"
#include "cpu/pred/ftb/ftb_ittage.hh"
#include "cpu/pred/ftb/loop_predictor.hh"
#include "cpu/pred/ftb/loop_buffer.hh"
#include "cpu/pred/ftb/ras.hh"
#include "cpu/pred/ftb/stream_struct.hh"
#include "cpu/pred/ftb/timed_base_pred.hh"
#include "debug/DecoupleBP.hh"
#include "debug/DecoupleBPHist.hh"
#include "debug/DecoupleBPProbe.hh"
#include "debug/DecoupleBPRAS.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DBPFTBStats.hh"
#include "debug/LoopBuffer.hh"
#include "debug/LoopPredictor.hh"
#include "params/DecoupledBPUWithFTB.hh"

namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

using DynInstPtr = o3::DynInstPtr;

class HistoryManager
{
  public:
    struct HistoryEntry
    {
        HistoryEntry(Addr _pc, int _shamt, bool _cond_taken, bool _is_call, bool _is_return,
            Addr _retAddr, uint64_t stream_id)
            : pc(_pc), shamt(_shamt), cond_taken(_cond_taken), is_call(_is_call),
                is_return(_is_return), retAddr(_retAddr), streamId(stream_id)
        {
        }
      Addr pc;
      Addr shamt;
      bool cond_taken;
      bool is_call;
      bool is_return;
      Addr retAddr;
      uint64_t streamId;
    };

    HistoryManager(unsigned _maxShamt) : maxShamt(_maxShamt) {}

  private:
    std::list<HistoryEntry> speculativeHists;

    unsigned IdealHistLen{246};

    unsigned maxShamt;

  public:
    void addSpeculativeHist(const Addr addr, const int shamt,
                            bool cond_taken, BranchInfo &bi,
                            const uint64_t stream_id)
    {
        bool is_call = bi.isCall;
        bool is_return = bi.isReturn;
        Addr retAddr = bi.getEnd();

        speculativeHists.emplace_back(addr, shamt, cond_taken, is_call,
            is_return, retAddr, stream_id);

        const auto &it = speculativeHists.back();
        printEntry("Add", it);

    }


    void commit(const uint64_t stream_id)
    {
        auto it = speculativeHists.begin();
        while (it != speculativeHists.end()) {
            if (it->streamId <= stream_id) {
                printEntry("Commit", *it);
                it = speculativeHists.erase(it);
            } else {
                ++it;
            }
        }
    }

    const std::list<HistoryEntry> &getSpeculativeHist()
    {
        return speculativeHists;
    }

    void squash(const uint64_t stream_id, const int shamt,
                const bool cond_taken, BranchInfo bi)
    {
        dump("before squash");
        auto it = speculativeHists.begin();
        while (it != speculativeHists.end()) {
            // why is it empty in logs?
            if (it->streamId == stream_id) {
                it->cond_taken = cond_taken;
                it->shamt = shamt;
                bool is_call = bi.isCall;
                bool is_return = bi.isReturn;
                Addr retAddr = bi.getEnd();
                it->is_call = is_call;
                it->is_return = is_return;
                it->retAddr = retAddr;
            } if (it->streamId > stream_id) {
                printEntry("Squash", *it);
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
        auto cur = speculativeHists.begin();
        cur++;
        while (cur != speculativeHists.end()) {
            if (cur->shamt > maxShamt) {
                dump("before warn");
                warn("entry shifted more than %d bits\n", maxShamt);
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
            printEntry("", *it);
        }
    }

    void printEntry(const char* when, const HistoryEntry& entry)
    {
        DPRINTF(DecoupleBPVerbose, "%s stream: %lu, pc %#lx, shamt %d, cond_taken %d, is_call %d, is_ret %d, retAddr %#lx\n",
            when, entry.streamId, entry.pc, entry.shamt, entry.cond_taken, entry.is_call, entry.is_return, entry.retAddr);
    }
};

class DecoupledBPUWithFTB : public BPredUnit
{
    using defer = std::shared_ptr<void>;
  public:
    typedef DecoupledBPUWithFTBParams Params;

    DecoupledBPUWithFTB(const Params &params);
    LoopPredictor lp;
    LoopBuffer lb;
    bool enableLoopBuffer{false};
    bool enableLoopPredictor{false};

  private:
    std::string _name;

    FetchTargetQueue fetchTargetQueue;

    std::map<FetchStreamId, FetchStream> fetchStreamQueue;
    unsigned fetchStreamQueueSize;
    FetchStreamId fsqId{1};
    FetchStream lastCommittedStream;

    unsigned numBr;

    unsigned cacheLineOffsetBits{6};  // TODO: parameterize this
    unsigned cacheLineSize{64};

    const unsigned historyTokenBits{8};

    constexpr Addr foldingTokenMask() { return (1 << historyTokenBits) - 1; }

    constexpr unsigned numFoldingTokens() { return 64/historyTokenBits; }

    const unsigned historyBits{488};

    const Addr MaxAddr{~(0ULL)};

    // StreamTAGE *streamTAGE{};
    DefaultFTB *uftb{};
    DefaultFTB *ftb{};
    FTBTAGE *tage{};
    FTBITTAGE *ittage{};
    
    ftb_pred::RAS *ras{};

    bool enableDB;
    DataBase bpdb;
    TraceManager *bptrace;

    std::vector<TimedBaseFTBPredictor*> components{};
    std::vector<FullFTBPrediction> predsOfEachStage{};
    unsigned numComponents{};
    unsigned numStages{};

    bool sentPCHist{false};
    bool receivedPred{false};

    Addr s0PC;
    // Addr s0StreamStartPC;
    boost::dynamic_bitset<> s0History;
    FullFTBPrediction finalPred;

    boost::dynamic_bitset<> commitHistory;

    bool squashing{false};

    HistoryManager historyManager;

    unsigned numOverrideBubbles{0};

    void tryEnqFetchStream();

    void tryEnqFetchTarget();

    void makeNewPrediction(bool create_new_stream);

    Addr alignToCacheLine(Addr addr)
    {
        return addr & ~((1 << cacheLineOffsetBits) - 1);
    }

    bool crossCacheLine(Addr addr)
    {
        return (addr & (1 << (cacheLineOffsetBits - 1))) != 0;
    }

    Addr computePathHash(Addr br, Addr target);

    // TODO: compare phr and ghr
    void histShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history);

    void printStream(const FetchStream &e)
    {
        if (!e.resolved) {
            DPRINTFR(DecoupleBP, "FSQ Predicted stream: ");
        } else {
            DPRINTFR(DecoupleBP, "FSQ Resolved stream: ");
        }
        // TODO:fix this
        DPRINTFR(DecoupleBP,
                 "%#lx-[%#lx, %#lx) --> %#lx, taken: %i\n",
                 e.startPC, e.predBranchInfo.pc, e.predEndPC,
                 e.getTakenTarget(), e.getTaken());
    }

    void printStreamFull(const FetchStream &e)
    {
        // TODO: fix this
        // DPRINTFR(
        //     DecoupleBP,
        //     "FSQ prediction:: %#lx-[%#lx, %#lx) --> %#lx\n",
        //     e.startPC, e.predBranchPC, e.predEndPC, e.predTarget);
        // DPRINTFR(
        //     DecoupleBP,
        //     "Resolved: %i, resolved stream:: %#lx-[%#lx, %#lx) --> %#lx\n",
        //     e.exeEnded, e.startPC, e.exeBranchPC, e.exeEndPC,
        //     e.exeTarget);
    }

    void printFetchTarget(const FtqEntry &e, const char *when)
    {
        DPRINTFR(DecoupleBP,
                 "%s:: %#lx - [%#lx, %#lx) --> %#lx, taken: %d, fsqID: %lu, loop: %d, iter: %d, exit: %d\n",
                 when, e.startPC, e.takenPC, e.endPC, e.target, e.taken,
                 e.fsqID, e.inLoop, e.iter, e.isExit);
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

    void generateFinalPredAndCreateBubbles();

    // const bool dumpLoopPred;

    void printFTBEntry(const FTBEntry &entry) {
        DPRINTF(FTB, "FTB entry: valid %d, tag %#lx, fallThruAddr:%#lx, slots:\n",
            entry.valid, entry.tag, entry.fallThruAddr);
        for (auto &slot : entry.slots) {
            DPRINTF(FTB, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d\n",
                slot.pc, slot.size, slot.target, slot.isCond, slot.isIndirect, slot.isCall, slot.isReturn);
        }
    }

    void printFullFTBPrediction(const FullFTBPrediction &pred) {
        DPRINTF(DecoupleBP, "dumping FullFTBPrediction\n");
        DPRINTF(DecoupleBP, "bbStart: %#lx, ftbEntry:\n", pred.bbStart);
        printFTBEntry(pred.ftbEntry);
        DPRINTF(DecoupleBP, "condTakens: ");
        for (auto taken : pred.condTakens) {
            DPRINTFR(DecoupleBP, "%d ", taken);
        }
        DPRINTFR(DecoupleBP, "\n");
        DPRINTF(DecoupleBP, "indirectTarget: %#lx, returnTarget: %#lx\n",
            pred.indirectTarget, pred.returnTarget);
    }

    struct DBPFTBStats : public statistics::Group {
        statistics::Scalar condNum;
        statistics::Scalar uncondNum;
        statistics::Scalar returnNum;
        statistics::Scalar otherNum;

        statistics::Scalar condMiss;
        statistics::Scalar uncondMiss;
        statistics::Scalar returnMiss;
        statistics::Scalar otherMiss;

        statistics::Vector predsOfEachStage;
        statistics::Vector commitPredsFromEachStage;
        statistics::Distribution fsqEntryDist;
        // statistics::Distribution ftqEntryDist;
        statistics::Scalar controlSquash;
        statistics::Scalar nonControlSquash;
        statistics::Scalar trapSquash;

        statistics::Scalar ftqNotValid;
        statistics::Scalar fsqNotValid;
        statistics::Scalar fsqFullCannotEnq;

        statistics::Scalar ftbHit;
        statistics::Scalar ftbMiss;

        statistics::Scalar predFalseHit;
        statistics::Scalar commitFalseHit;

        statistics::Scalar predLoopPredictorExit;
        statistics::Scalar predLoopPredictorUnconfNotExit;
        statistics::Scalar commitLoopPredictorExit;
        statistics::Scalar commitLoopPredictorExitCorrect;
        statistics::Scalar commitLoopPredictorExitWrong;
        statistics::Scalar commitLoopExitLoopPredictorNotPredicted;
        statistics::Scalar commitLoopExitLoopPredictorNotConf;
        statistics::Scalar controlSquashOnLoopPredictorPredExit;
        statistics::Scalar nonControlSquashOnLoopPredictorPredExit;
        statistics::Scalar trapSquashOnLoopPredictorPredExit;

        statistics::Scalar predBlockInLoopBuffer;
        statistics::Scalar predDoubleBlockInLoopBuffer;
        statistics::Scalar squashOnLoopBufferPredBlock;
        statistics::Scalar squashOnLoopBufferDoublePredBlock;
        statistics::Scalar commitBlockInLoopBuffer;
        statistics::Scalar commitDoubleBlockInLoopBuffer;
        statistics::Scalar commitBlockInLoopBufferSquashed;
        statistics::Scalar commitDoubleBlockInLoopBufferSquashed;
        statistics::Distribution commitLoopBufferEntryInstNum;
        statistics::Distribution commitLoopBufferDoubleEntryInstNum;

        DBPFTBStats(statistics::Group* parent, unsigned numStages, unsigned fsqSize);
    } dbpFtbStats;

  public:
    void tick();

    bool trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &fetchTargetInLoop);

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

    bool fetchTargetAvailable()
    {
        return fetchTargetQueue.fetchTargetAvailable();
    }

    FtqEntry& getSupplyingFetchTarget()
    {
        return fetchTargetQueue.getTarget();
    }

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
    
    bool debugFlagOn{false};

    std::map<std::pair<Addr, Addr>, int> topMispredicts;
    std::map<uint64_t, uint64_t> topMispredHist;
    std::map<int, int> misPredTripCount;
    unsigned int missCount{0};

    void setTakenEntryWithStream(const FetchStream &stream_entry, FtqEntry &ftq_entry);

    void setNTEntryWithStream(FtqEntry &ftq_entry, Addr endPC);

    bool popRAS(FetchStreamId stream_id, const char *when);

    void pushRAS(FetchStreamId stream_id, const char *when, Addr ra);

    void updateTAGE(FetchStream &stream);

    std::vector<Addr> storeTargets;

    void resetPC(Addr new_pc);

    enum CfiType {
        COND,
        UNCOND,
        RETURN,
        OTHER
    };

    void addCfi(CfiType type, bool miss) {
        switch (type) {
            case COND:
                dbpFtbStats.condNum++;
                if (miss)
                    dbpFtbStats.condMiss++;
                break;
            case UNCOND:
                dbpFtbStats.uncondNum++;
                if (miss)
                    dbpFtbStats.uncondMiss++;
                break;
            case RETURN:
                dbpFtbStats.returnNum++;
                if (miss)
                    dbpFtbStats.returnMiss++;
                break;
            case OTHER:
                dbpFtbStats.otherNum++;
                if (miss)
                    dbpFtbStats.otherMiss++;
                break;
        }
        DPRINTF(DBPFTBStats, "Miss type: %d\n", type);
    }

    void addFtqNotValid() {
        dbpFtbStats.ftqNotValid++;
    }

    void commitBranch(const DynInstPtr &inst, bool miss, bool loop_exit);

    std::map<Addr, unsigned> topMispredIndirect;
};

}  // namespace ftb_pred
}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_FTB_DECOUPLED_BPRED_HH__