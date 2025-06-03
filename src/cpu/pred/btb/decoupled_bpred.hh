#ifndef __CPU_PRED_BTB_DECOUPLED_BPRED_HH__
#define __CPU_PRED_BTB_DECOUPLED_BPRED_HH__

#include <array>
#include <queue>
#include <stack>
#include <utility>
#include <vector>

#include "arch/generic/pcstate.hh"
#include "config/the_isa.hh"
#include "cpu/o3/cpu_def.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/btb/btb.hh"
#include "cpu/pred/btb/btb_ittage.hh"
#include "cpu/pred/btb/btb_tage.hh"
#include "cpu/pred/btb/btb_ubtb.hh"
#include "cpu/pred/btb/btb_mgsc.hh"
#include "cpu/pred/btb/fetch_target_queue.hh"
#include "cpu/pred/btb/jump_ahead_predictor.hh"
#include "cpu/pred/btb/loop_buffer.hh"
#include "cpu/pred/btb/loop_predictor.hh"
#include "cpu/pred/btb/ras.hh"
#include "cpu/pred/general_arch_db.hh"

// #include "cpu/pred/btb/uras.hh"
#include "cpu/pred/btb/history_manager.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "debug/DBPBTBStats.hh"
#include "debug/DecoupleBP.hh"
#include "debug/DecoupleBPHist.hh"
#include "debug/DecoupleBPProbe.hh"
#include "debug/DecoupleBPRAS.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DecoupleBPuRAS.hh"
#include "debug/JumpAheadPredictor.hh"
#include "debug/LoopBuffer.hh"
#include "debug/LoopPredictor.hh"
#include "debug/LoopPredictorVerbose.hh"
#include "params/DecoupledBPUWithBTB.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

using DynInstPtr = o3::DynInstPtr;
using CPU = o3::CPU;

/**
 * @class DecoupledBPUWithBTB
 * @brief A decoupled branch predictor implementation using BTB-based design
 *
 * This predictor implements a decoupled front-end with:
 * - Multiple prediction stages (UBTB -> BTB/TAGE/ITTAGE)
 * - Fetch Target Queue (FTQ) for managing predicted targets
 * - Fetch Stream Queue (FSQ) for managing instruction streams
 * - Support for loop prediction and jump-ahead prediction
 */
class DecoupledBPUWithBTB : public BPredUnit
{
    using defer = std::shared_ptr<void>;
  public:
    typedef DecoupledBPUWithBTBParams Params;

    DecoupledBPUWithBTB(const Params &params);
    // TODO: remove loop predictor and loop buffer, jap, now fetch.cc need them
    LoopPredictor lp;
    LoopBuffer lb;
    bool enableLoopBuffer{false};
    bool enableLoopPredictor{false};

    JumpAheadPredictor jap;
    bool enableJumpAheadPredictor{false};

  private:
    std::string _name;

    FetchTargetQueue fetchTargetQueue;

    std::map<FetchStreamId, FetchStream> fetchStreamQueue;
    unsigned fetchStreamQueueSize;
    FetchStreamId fsqId{1};

    CPU *cpu;

    unsigned predictWidth;  // max predict width, default 64
    unsigned maxInstsNum;

    const unsigned historyBits{488};

    const Addr MaxAddr{~(0ULL)};

    UBTB *ubtb{};
    DefaultBTB *abtb{};
    DefaultBTB *btb{};
    BTBTAGE *tage{};
    BTBITTAGE *ittage{};
    BTBMGSC *mgsc{};

    btb_pred::BTBRAS *ras{};
    // btb_pred::BTBuRAS *uras{};

    // bool enableDB;
    std::vector<std::string> bpDBSwitches;
    bool someDBenabled{false};
    bool enableBranchTrace{false};
    bool enablePredFSQTrace{false};
    bool enablePredFTQTrace{false};
    bool enableLoopDB{false};
    bool checkGivenSwitch(std::vector<std::string> switches, std::string switchName) {
        for (auto &sw : switches) {
            if (sw == switchName) {
                return true;
            }
        }
        return false;
    }
    void removeGivenSwitch(std::vector<std::string> &switches, std::string switchName) {
        auto it = std::remove(switches.begin(), switches.end(), switchName);
        switches.erase(it, switches.end());
    }
    DataBase bpdb;
    TraceManager *bptrace;
    TraceManager *predTraceManager;  // Trace manager for prediction-time events
    TraceManager *ftqTraceManager;   // Trace manager for fetch target queue entries
    TraceManager *lptrace;

    void initDB();

    std::vector<TimedBaseBTBPredictor*> components{};
    std::vector<FullBTBPrediction> predsOfEachStage{};
    unsigned numComponents{};
    unsigned numStages{};

    bool sentPCHist{false};     ///< get prediction from BP
    bool receivedPred{false};   ///< get final prediction from predsOfEachStage[numStages-1]

    Addr s0PC;                  ///< Current PC
    // Addr s0StreamStartPC;
    boost::dynamic_bitset<> s0History;  ///< global History bits
    boost::dynamic_bitset<> s0PHistory;  ///< path History bits
    boost::dynamic_bitset<> s0BwHistory;  ///< global backward History bits
    boost::dynamic_bitset<> s0IHistory;  ///< IMLI History bits
    std::vector<boost::dynamic_bitset<>> s0LHistory;  ///< local History bits
    FullBTBPrediction finalPred;      ///< Final prediction

    boost::dynamic_bitset<> commitHistory;

    bool squashing{false};

    HistoryManager historyManager;

    unsigned numOverrideBubbles{0};


    using JAInfo = JumpAheadPredictor::JAInfo;
    JAInfo jaInfo;

    void tryEnqFetchStream();

    void tryEnqFetchTarget();

    // Helper function to validate FTQ and FSQ state before enqueueing
    bool validateFTQEnqueue();

    bool validateFSQEnqueue();

    void makeNewPrediction(bool create_new_stream);

    FtqEntry createFtqEntryFromStream(const FetchStream &stream, const FetchTargetEnqState &ftq_enq_state);

    FetchStream createFetchStreamEntry();

    void updateHistoryForPrediction(FetchStream &entry);

    void fillAheadPipeline(FetchStream &entry);

    // Tick helper functions
    void processEnqueueAndBubbles();
    void requestNewPrediction();

    Addr computePathHash(Addr br, Addr target);

    // TODO: compare phr and ghr
    void histShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history);

    void pHistShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history, Addr pc);

    void printStream(const FetchStream &e)
    {
        if (!e.resolved) {
            DPRINTFR(DecoupleBPProbe, "FSQ Predicted stream: ");
        } else {
            DPRINTFR(DecoupleBPProbe, "FSQ Resolved stream: ");
        }
        // TODO:fix this
        DPRINTFR(DecoupleBPProbe,
                 "%#lx-[%#lx, %#lx) --> %#lx, taken: %lu\n",
                 e.startPC, e.getBranchInfo().pc, e.getEndPC(),
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
                 "%s:: %#lx - [%#lx, %#lx) --> %#lx, taken: %d, fsqID: %lu\n",
                 when, e.startPC, e.takenPC, e.endPC, e.target, e.taken, e.fsqID);
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

    /**
     * @brief Generate final prediction from all stages
     *
     * Collects predictions from all stages and:
     * - Selects most accurate prediction
     * - Generates necessary bubbles
     * - Updates prediction state
     */
    void generateFinalPredAndCreateBubbles();

    void clearPreds() {
        for (auto &stagePred : predsOfEachStage) {
            stagePred.condTakens.clear();
            stagePred.indirectTargets.clear();
            stagePred.btbEntries.clear();
        }
    }

    // const bool dumpLoopPred;

    void printBTBEntry(const BTBEntry &e) {
        DPRINTF(BTB, "BTB entry: valid %d, pc:%#lx, tag: %#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d, always_taken:%d\n",
            e.valid, e.pc, e.tag, e.size, e.target, e.isCond, e.isIndirect, e.isCall, e.isReturn, e.alwaysTaken);
    }

    void printFullBTBPrediction(const FullBTBPrediction &pred) {
        DPRINTF(DecoupleBP, "dumping FullBTBPrediction\n");
        DPRINTF(DecoupleBP, "bbStart: %#lx, btbEntry:\n", pred.bbStart);
        for (auto &e: pred.btbEntries) {
            printBTBEntry(e);
        }
        DPRINTF(DecoupleBP, "condTakens: ");
        for (auto pair : pred.condTakens) {
            DPRINTFR(DecoupleBP, "%#lx %d ,", pair.first, pair.second);
        }
        DPRINTFR(DecoupleBP, "\n");
        for (auto pair : pred.indirectTargets) {
            DPRINTF(DecoupleBP, "indirectTarget of %#lx: %#lx\n",
            pair.first, pair.second);
        }
        DPRINTF(DecoupleBP, "returnTarget %#lx\n", pred.returnTarget);
    }

    /**
     * @brief Statistics collection for branch prediction
     *
     * Tracks detailed statistics about:
     * - Branch types and mispredictions
     * - Predictor component usage
     * - Queue utilization
     * - Loop and jump-ahead prediction performance
     */
    struct DBPBTBStats : public statistics::Group {
        // Branch type statistics
        statistics::Scalar condNum;      ///< Number of conditional branches
        statistics::Scalar uncondNum;    ///< Number of unconditional branches
        statistics::Scalar returnNum;    ///< Number of return instructions
        statistics::Scalar otherNum;     ///< Number of other control instructions

        // Misprediction statistics
        statistics::Scalar condMiss;     ///< Conditional branch mispredictions
        statistics::Scalar uncondMiss;   ///< Unconditional branch mispredictions
        statistics::Scalar returnMiss;   ///< Return mispredictions
        statistics::Scalar otherMiss;    ///< Other control mispredictions

        // Branch coverage statistics
        statistics::Scalar staticBranchNum;           ///< Total static branches seen
        statistics::Scalar staticBranchNumEverTaken;  ///< Static branches ever taken

        statistics::Vector predsOfEachStage;
        statistics::Scalar overrideBubbleNum;
        statistics::Scalar overrideCount;

        statistics::Vector commitPredsFromEachStage;
        statistics::Formula commitOverrideBubbleNum;
        statistics::Formula commitOverrideCount;
        // Track override reasons
        statistics::Scalar overrideFallThruMismatch;
        statistics::Scalar overrideControlAddrMismatch;
        statistics::Scalar overrideTargetMismatch;
        statistics::Scalar overrideEndMismatch;
        statistics::Scalar overrideHistInfoMismatch;

        statistics::Distribution fsqEntryDist;
        statistics::Scalar fsqEntryEnqueued;
        statistics::Scalar fsqEntryCommitted;
        // statistics::Distribution ftqEntryDist;
        statistics::Scalar controlSquashFromDecode;
        statistics::Scalar controlSquashFromCommit;
        statistics::Scalar nonControlSquash;
        statistics::Scalar trapSquash;

        statistics::Scalar ftqNotValid;
        statistics::Scalar fsqNotValid;
        statistics::Scalar fsqFullCannotEnq;
        statistics::Scalar ftqFullCannotEnq;
        statistics::Scalar fsqFullFetchHungry;
        statistics::Scalar fsqEmpty;
        //
        statistics::Distribution commitFsqEntryHasInsts;
        // write back once an fsq entry finishes fetch
        statistics::Distribution commitFsqEntryFetchedInsts;
        statistics::Scalar commitFsqEntryOnlyHasOneJump;

        statistics::Scalar btbHit;
        statistics::Scalar btbMiss;
        statistics::Scalar btbEntriesWithDifferentStart;
        statistics::Scalar btbEntriesWithOnlyOneJump;

        statistics::Scalar predFalseHit;
        statistics::Scalar commitFalseHit;

        DBPBTBStats(statistics::Group* parent, unsigned numStages, unsigned fsqSize, unsigned maxInstsNum);
    } dbpBtbStats;

  public:
    /**
     * @brief Main prediction cycle function
     *
     * This function handles:
     * - FSQ/FTQ management
     * - Prediction generation
     * - Loop buffer management
     * - Statistics collection
     */
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

    void setCpu(CPU *_cpu) { cpu = _cpu; }

    struct BpTrace : public Record {
        void set(uint64_t fsqId, uint64_t startPC, uint64_t controlPC, uint64_t controlType,
            uint64_t taken, uint64_t mispred, uint64_t fallThruPC,
            uint64_t source, uint64_t target) {
            _uint64_data["fsqId"] = fsqId;
            _uint64_data["startPC"] = startPC;
            _uint64_data["controlPC"] = controlPC;
            _uint64_data["controlType"] = controlType;
            _uint64_data["taken"] = taken;
            _uint64_data["mispred"] = mispred;
            _uint64_data["fallThruPC"] = fallThruPC;
            _uint64_data["source"] = source;
            _uint64_data["target"] = target;
        }
        BpTrace(uint64_t fsqId, FetchStream &stream, const DynInstPtr &inst, bool mispred);
    };

    // Prediction trace record for tracking prediction-time information
    struct PredictionTrace : public Record
    {
        void set(uint64_t fsqId, uint64_t startPC, uint64_t predTaken, uint64_t predEndPC,
                 uint64_t controlPC, uint64_t target,
                 uint64_t predSource, uint64_t btbHit) {
            _uint64_data["fsqId"] = fsqId;
            _uint64_data["startPC"] = startPC;
            _uint64_data["predTaken"] = predTaken;
            _uint64_data["predEndPC"] = predEndPC;
            _uint64_data["controlPC"] = controlPC;
            _uint64_data["target"] = target;
            _uint64_data["predSource"] = predSource;
            _uint64_data["btbHit"] = btbHit;
        }

        PredictionTrace(uint64_t id, const FetchStream &entry) {
            _tick = curTick();
            set(id, entry.startPC, entry.predTaken, entry.predEndPC,
                entry.getControlPC(), entry.getTakenTarget(),
                entry.predSource, entry.isHit ? 1 : 0);
        }
    };

    // FTQ trace record for tracking fetch target queue entries
    struct FtqTrace : public Record
    {
        void set(uint64_t ftqId, uint64_t fsqId, uint64_t startPC, uint64_t endPC,
                 uint64_t takenPC, uint64_t taken, uint64_t target) {
            _uint64_data["ftqId"] = ftqId;
            _uint64_data["fsqId"] = fsqId;
            _uint64_data["startPC"] = startPC;
            _uint64_data["endPC"] = endPC;
            _uint64_data["takenPC"] = takenPC;
            _uint64_data["taken"] = taken;
            _uint64_data["target"] = target;
        }

        FtqTrace(uint64_t ftqId, uint64_t fsqId, const FtqEntry &entry) {
            _tick = curTick();
            set(ftqId, fsqId, entry.startPC, entry.endPC,
                entry.takenPC, entry.taken ? 1 : 0, entry.target);
        }
    };

    std::pair<bool, bool> decoupledPredict(const StaticInstPtr &inst,
                                           const InstSeqNum &seqNum,
                                           PCStateBase &pc, ThreadID tid,
                                           unsigned &currentLoopIter);

    // redirect the stream
    void controlSquash(unsigned ftq_id, unsigned fsq_id,
                       const PCStateBase &control_pc,
                       const PCStateBase &target_pc,
                       const StaticInstPtr &static_inst, unsigned inst_bytes,
                       bool actually_taken, const InstSeqNum &squashed_sn,
                       ThreadID tid, const unsigned &currentLoopIter,
                       const bool fromCommit);

    // keep the stream: original prediction might be right
    // For memory violation, stream continues after squashing
    void nonControlSquash(unsigned ftq_id, unsigned fsq_id,
                          const PCStateBase &inst_pc, const InstSeqNum seq,
                          ThreadID tid, const unsigned &currentLoopIter);

    // Not a control. But stream is actually disturbed
    void trapSquash(unsigned ftq_id, unsigned fsq_id, Addr last_committed_pc,
                    const PCStateBase &inst_pc, ThreadID tid, const unsigned &currentLoopIter);

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

    bool lookup(ThreadID tid, Addr instPC, void *&bp_history) override { return false; }
    // end Dummy overriding

    void overrideStats(OverrideReason overrideReason);

    void checkHistory(const boost::dynamic_bitset<> &history);

    bool useStreamRAS(FetchStreamId sid);

    Addr getPreservedReturnAddr(const DynInstPtr &dynInst);

    std::string buf1, buf2;

    std::stack<Addr> streamRAS;

    bool debugFlagOn{false};

    std::unordered_map<Addr, int> takenBranches;      // branch address -> taken count
    std::unordered_map<Addr, int> currentPhaseTakenBranches;
    std::unordered_map<Addr, int> currentSubPhaseTakenBranches;

    /**
     * @brief Types of control flow instruction mispredictions
     */
    enum MispredType {
        DIR_WRONG,      ///< Direction prediction error (predicted taken when not taken or vice versa)
        TARGET_WRONG,   ///< Target address prediction was wrong (branch taken but to the wrong address)
        NO_PRED,        ///< No prediction was made (branch wasn't predicted at all)
        FAKE_LAST       ///< Sentinel value
    };

    /**
     * @brief Branch statistics structure holding detailed misprediction data
     *
     * This structure captures comprehensive statistics about a specific branch,
     * tracking both execution counts and various types of mispredictions.
     * It allows for detailed analysis of predictor performance for each branch.
     */
    struct BranchStats
    {
        Addr pc;                ///< Branch PC address
        int branchType;         ///< Branch type (0=cond, 1=uncond, 2=call, 3=ind, etc.)
        int totalCount;         ///< Total number of times branch was executed
        int mispredCount;       ///< Total number of mispredictions for this branch
        int dirWrongCount;      ///< Number of times direction was mispredicted
        int targetWrongCount;   ///< Number of times target address was mispredicted
        int noPredCount;        ///< Number of times no prediction was made

        /**
         * @brief Default constructor
         */
        BranchStats()
            : pc(0), branchType(0), totalCount(0), mispredCount(0),
              dirWrongCount(0), targetWrongCount(0), noPredCount(0) {}

        /**
         * @brief Create branch stats with initial values
         */
        BranchStats(Addr _pc, int _type)
            : pc(_pc), branchType(_type), totalCount(0), mispredCount(0),
              dirWrongCount(0), targetWrongCount(0), noPredCount(0) {}

        /**
         * @brief Increment total execution count
         */
        void incrementTotal() { totalCount++; }

        /**
         * @brief Increment misprediction count for a specific type
         */
        void incrementMispred(MispredType type) {
            mispredCount++;
            switch(type) {
                case DIR_WRONG: dirWrongCount++; break;
                case TARGET_WRONG: targetWrongCount++; break;
                case NO_PRED: noPredCount++; break;
                default: break;
            }
        }

        /**
         * @brief Calculate misprediction rate (per 1000 executions)
         *
         * @return Misprediction rate scaled to per-mille (0-1000)
         */
        double getMispredRate() const {
            return totalCount > 0 ? (double)(mispredCount * 1000) / totalCount : 0;
        }
    };

    /**
     * @brief Index type for branch statistics maps
     */
    using BranchKey = std::pair<Addr, int>;

    /**
     * @brief Map type for branch statistics
     */
    using BranchStatsMap = std::map<BranchKey, BranchStats>;

    // Branch statistics maps
    /**
     * @brief Maps (startPC, controlPC) pairs to misprediction counts
     *
     * This tracks mispredictions based on the starting address of a basic block
     * and the address of the control instruction that was mispredicted.
     */
    std::map<std::pair<Addr, Addr>, int> topMispredicts;

    /**
     * @brief Maps branch keys (PC, type) to detailed branch statistics
     *
     * Main container for branch prediction statistics, storing information about
     * each branch's execution count, mispredictions, and error types.
     */
    BranchStatsMap topMispredictsByBranch;

    /**
     * @brief Maps branch history patterns to misprediction counts
     *
     * Tracks which history patterns tend to cause more mispredictions.
     * Key: History pattern value
     * Value: Count of mispredictions with this pattern
     */
    std::map<uint64_t, uint64_t> topMispredHist;

    // Phase-based statistics
    /**
     * @brief Stores branch statistics from the previous phase
     *
     * Used to calculate delta statistics between phases.
     */
    BranchStatsMap lastPhaseTopMispredictsByBranch;

    /**
     * @brief Vector of branch statistics for each phase
     *
     * Each entry contains the branch statistics for a complete phase.
     */
    std::vector<BranchStatsMap> topMispredictsByBranchByPhase;

    /**
     * @brief Vector of branch statistics for each sub-phase
     *
     * Similar to topMispredictsByBranchByPhase but with finer granularity.
     */
    std::vector<BranchStatsMap> topMispredictsByBranchBySubPhase;

    /**
     * @brief Vector of taken branches for each phase
     *
     * Each entry maps branch addresses to execution counts for a phase.
     */
    std::vector<std::unordered_map<Addr, int>> takenBranchesByPhase;

    /**
     * @brief Vector of taken branches for each sub-phase
     *
     * Each entry maps branch addresses to execution counts for a sub-phase.
     */
    std::vector<std::unordered_map<Addr, int>> takenBranchesBySubPhase;

    // BTB entry tracking
    /**
     * @brief BTB entries from the previous phase
     *
     * Maps start address to (BTBEntry, visit count) to track BTB entry usage.
     */
    std::unordered_map<Addr, std::pair<BTBEntry, int>> lastPhaseBTBEntries;

    /**
     * @brief Cumulative BTB entries seen so far
     *
     * Maps start address to (BTBEntry, visit count) with total usage.
     */
    std::unordered_map<Addr, std::pair<BTBEntry, int>> totalBTBEntries;

    /**
     * @brief Vector of BTB entries for each phase
     *
     * Each entry contains the BTB entries used during a phase.
     */
    std::vector<std::unordered_map<Addr, std::pair<BTBEntry, int>>> BTBEntriesByPhase;

    /**
     * @brief Next phase ID to dump statistics for
     */
    int phaseIdToDump{1};

    /**
     * @brief Total number of instructions committed
     */
    int numInstCommitted{0};

    /**
     * @brief Number of instructions per phase
     */
    int phaseSizeByInst{100000};

    /**
     * @brief Next sub-phase ID to dump statistics for
     */
    int subPhaseIdToDump{1};

    /**
     * @brief Ratio between phase and sub-phase sizes
     */
    int subPhaseRatio{10};

    /**
     * @brief Calculate sub-phase size in instructions
     * @return Number of instructions per sub-phase
     */
    int subPhaseSizeByInst() { return phaseSizeByInst/subPhaseRatio; }

    /**
     * @brief Distribution of committed instructions from previous phase
     *
     * Vector indexed by instruction count, values are frequency.
     */
    std::vector<int> lastPhaseFsqEntryNumCommittedInstDist;

    /**
     * @brief Total committed instruction count distribution
     *
     * Tracks how many FSQ entries have each number of committed instructions.
     */
    std::vector<int> commitFsqEntryHasInstsVector;

    /**
     * @brief Committed instruction distributions by phase
     *
     * Each entry contains the distribution of committed instructions for a phase.
     */
    std::vector<std::vector<int>> fsqEntryNumCommittedInstDistByPhase;

    /**
     * @brief Distribution of fetched instructions from previous phase
     *
     * Vector indexed by instruction count, values are frequency.
     */
    std::vector<int> lastPhaseFsqEntryNumFetchedInstDist;

    /**
     * @brief Total fetched instruction count distribution
     *
     * Tracks how many FSQ entries have each number of fetched instructions.
     */
    std::vector<int> commitFsqEntryFetchedInstsVector;

    /**
     * @brief Fetched instruction distributions by phase
     *
     * Each entry contains the distribution of fetched instructions for a phase.
     */
    std::vector<std::vector<int>> fsqEntryNumFetchedInstDistByPhase;

    /**
     * @brief Total branch misprediction count
     */
    unsigned int missCount{0};

    /**
     * @brief Branch statistics from the previous sub-phase
     *
     * Used to calculate delta statistics between sub-phases.
     */
    BranchStatsMap lastSubPhaseTopMispredictsByBranch;
    // These are already declared above, so no need to redeclare
    // std::vector<BranchStatsMap> topMispredictsByBranchBySubPhase;
    // std::vector<std::map<Addr, int>> takenBranchesBySubPhase;


    void setTakenEntryWithStream(FtqEntry &ftq_entry, const FetchStream &stream_entry);

    void setNTEntryWithStream(FtqEntry &ftq_entry, Addr endPC);

    void recoverHistoryForSquash(
        FetchStream &stream,
        unsigned stream_id,
        const PCStateBase &squash_pc,
        bool is_conditional,
        bool actually_taken,
        SquashType squash_type,
        Addr redirect_pc);

    // Common logic for squash handling
    void handleSquash(unsigned target_id,
                      unsigned stream_id,
                      SquashType squash_type,
                      const PCStateBase &squash_pc,
                      Addr redirect_pc,
                      bool is_conditional = false,
                      bool actually_taken = false,
                      const StaticInstPtr &static_inst = nullptr,
                      unsigned control_inst_size = 0);

    void resetPC(Addr new_pc);

    // Helper functions for update
    void updatePredictorComponents(FetchStream &stream);
    void updateStatistics(const FetchStream &stream);

    // Helper function to process FTQ entry completion
    void processFetchTargetCompletion(const FtqEntry &target_to_fetch);

    /**
     * @brief Types of control flow instructions for misprediction tracking
     */
    enum CfiType
    {
        COND,     ///< Conditional branch
        UNCOND,   ///< Unconditional branch
        RETURN,   ///< Return instruction
        OTHER     ///< Other control flow instruction
    };

    void addCfi(CfiType type, bool mispred) {
        switch (type) {
            case COND:
                dbpBtbStats.condNum++;
                if (mispred)
                    dbpBtbStats.condMiss++;
                break;
            case UNCOND:
                dbpBtbStats.uncondNum++;
                if (mispred)
                    dbpBtbStats.uncondMiss++;
                break;
            case RETURN:
                dbpBtbStats.returnNum++;
                if (mispred)
                    dbpBtbStats.returnMiss++;
                break;
            case OTHER:
                dbpBtbStats.otherNum++;
                if (mispred)
                    dbpBtbStats.otherMiss++;
                break;
        }
        DPRINTF(DBPBTBStats, "Miss type: %d\n", type);
    }

    void addFtqNotValid() {
        dbpBtbStats.ftqNotValid++;
    }

    /**
     * @brief Process a branch instruction during commit
     *
     * Updates branch prediction statistics and trains predictor components.
     *
     * @param inst Dynamic instruction pointer
     * @param miss Whether the branch was mispredicted
     */
    void commitBranch(const DynInstPtr &inst, bool miss);

    /**
     * @brief Process branch misprediction, determine type and update statistics
     *
     * @param entry The fetch stream entry
     * @param branchAddr Branch instruction address
     * @param info Branch information
     * @param taken Whether the branch was taken
     * @param mispred Whether the branch was mispredicted
     */
    void processMisprediction(
        const FetchStream &entry,
        Addr branchAddr,
        const BranchInfo &info,
        bool taken,
        bool mispred);

    /**
     * @brief Track statistics for taken branches
     *
     * @param branchAddr Branch instruction address
     */
    void trackTakenBranch(Addr branchAddr);

    /**
     * @brief Process phase-based statistics at phase boundaries
     *
     * @param isSubPhase Whether this is a sub-phase or main phase
     * @param phaseID Current phase ID
     * @param phaseToDump Phase ID to be processed
     * @param lastPhaseStats Last phase branch statistics map
     * @param phaseStatsList List of all phases branch statistics
     * @param currentPhaseBranches Current phase taken branches map
     * @param phaseBranchesList List of all phases taken branches
     * @return true If the phase was processed
     */
    bool processPhase(bool isSubPhase, int phaseID, int &phaseToDump,
                     BranchStatsMap &lastPhaseStats,
                     std::vector<BranchStatsMap> &phaseStatsList,
                     std::unordered_map<Addr, int> &currentPhaseBranches,
                     std::vector<std::unordered_map<Addr, int>> &phaseBranchesList);

    /**
     * @brief Process fetch instruction distributions for a phase
     *
     * @param currentPhaseCommittedDist Output vector for committed instruction distribution
     * @param currentPhaseFetchedDist Output vector for fetched instruction distribution
     */
    void processFetchDistributions(std::vector<int> &currentPhaseCommittedDist,
                                  std::vector<int> &currentPhaseFetchedDist);

    /**
     * @brief Process BTB entries for a phase
     *
     * @return std::map<Addr, std::pair<BTBEntry, int>> Map of BTB entries for the phase
     */
    std::unordered_map<Addr, std::pair<BTBEntry, int>> processBTBEntries();

    /**
     * @brief Process instruction commit and update phase-based statistics
     *
     * Called whenever an instruction is committed, updating instruction counts
     * and phase-based statistics when phase boundaries are reached.
     *
     * @param inst Dynamic instruction pointer of the committed instruction
     */
    void notifyInstCommit(const DynInstPtr &inst);

    /**
     * @brief Tracks mispredictions of indirect branches
     *
     * Maps indirect branch addresses to misprediction counts.
     */
    std::map<Addr, unsigned> topMispredIndirect;

    /**
     * @brief Current FTQ entry instruction count
     */
    int currentFtqEntryInstNum{0};

    /**
     * @brief Dump statistics on program exit
     *
     * This method dumps various predictor statistics to output files when
     * the simulation ends. It tracks mispredictions by branch type,
     * phase information, and other metrics.
     */
    void dumpStats();

};

}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_BTB_DECOUPLED_BPRED_HH__
