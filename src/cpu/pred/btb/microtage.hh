#ifndef __CPU_PRED_BTB_MICROTAGE_HH__
#define __CPU_PRED_BTB_MICROTAGE_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/pred/btb/timed_base_pred.hh"

// Conditional includes based on build mode
#ifdef UNIT_TEST
    #include "cpu/pred/btb/test/test_dprintf.hh"
#else
    #include "debug/DecoupleBP.hh"
    #include "debug/TAGEUseful.hh"
    #include "debug/TAGEHistory.hh"
    #include "params/MicroTAGE.hh"
    #include "sim/sim_object.hh"
#endif

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

// Conditional namespace wrapper for testing
#ifdef UNIT_TEST
namespace test {
#endif

class MicroTAGE : public TimedBaseBTBPredictor
{
    using bitset = boost::dynamic_bitset<>;
    static constexpr unsigned MaxThreads = o3::MaxThreads;
  public:
#ifdef UNIT_TEST
    // Test constructor
    MicroTAGE(unsigned numPredictors = 4, unsigned numWays = 2, unsigned tableSize = 1024, unsigned numBanks = 4);
#else
    // Production constructor
    typedef MicroTAGEParams Params;
#endif

    // Represents a single entry in the TAGE prediction table
    struct TageEntry
    {
        public:
            bool valid;      // Whether this entry is valid
            Addr tag;       // Tag for matching
            short counter;  // Prediction counter (-4 to 3), 3bits， 0 and -1 are weak
            bool useful;    // 1-bit usefulness counter; true means useful
            Addr pc;        // branch pc, like branch position, for btb entry pc check

            TageEntry() : valid(false), tag(0), counter(0), useful(false), pc(0) {}

            TageEntry(Addr tag, short counter, Addr pc) :
                      valid(true), tag(tag), counter(counter), useful(false), pc(pc) {}
            bool taken() const {
                return counter >= 0;
            }
    };

    // Contains information about a TAGE table lookup
    struct TageTableInfo
    {
        public:
            bool found;     // Whether a matching entry was found
            TageEntry entry; // The matching entry
            unsigned table; // Which table this entry was found in
            Addr index;     // Index in the table
            Addr tag;       // Tag that was matched
            unsigned way;    // Which way this entry was found in
            TageTableInfo() : found(false), table(0), index(0), tag(0), way(0) {}
            TageTableInfo(bool found, TageEntry entry, unsigned table, Addr index, Addr tag, unsigned way) :
                        found(found), entry(entry), table(table), index(index), tag(tag), way(way) {}
            bool taken() const {
                return entry.taken();
            }
    };

    // Contains the complete prediction result
    struct TagePrediction
    {
        public:
            Addr btb_pc;           // btb entry pc, same as tage entry pc
            TageTableInfo mainInfo; // Main prediction info
            //TageTableInfo altInfo;  // Alternative prediction info
            bool mainprovided;    // Whether to use alternative prediction, true if main is weak or no main prediction
            bool taken;           // Final prediction outcome
            bool basePred;          // Alternative prediction = alt_provided ? alt_taken : base_taken;

            TagePrediction() : btb_pc(0), mainprovided(false), taken(false), basePred(false) {}
            TagePrediction(Addr btb_pc, TageTableInfo mainInfo,
                            bool mainprovided, bool taken, bool basePred) :
                            btb_pc(btb_pc), mainInfo(mainInfo),
                            mainprovided(mainprovided), taken(taken), basePred(basePred) {}
    };


#ifndef UNIT_TEST
    MicroTAGE(const Params& p);
#endif
    ~MicroTAGE();

    void tickStart() override;

    void tick() override;
    void dryRunCycle(Addr startAddr) override;
    // Make predictions for a stream of instructions and record in stage preds
    void putPCHistory(Addr startAddr,
                      const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;

    // Speculatively update path folded histories.
    void specUpdatePHist(const boost::dynamic_bitset<> &history,
                         FullBTBPrediction &pred,
                         const PathHistoryUpdate &update) override;

    // Recover path folded histories after a misprediction.
    void recoverPHist(const boost::dynamic_bitset<> &history,
                      const HistoryRecoveryContext &context,
                      const PathHistoryUpdate &update) override;

    // Update predictor state based on actual branch outcomes
    void update(const PredictionUpdateContext &context,
                const PreparedUpdate &update) override;
    bool canResolveUpdate(const PredictionUpdateContext &context,
                          const PreparedUpdate &update) override;
    void doResolveUpdate(const PredictionUpdateContext &context,
                         const PreparedUpdate &update) override;
    // Train MicroTAGE from the final-stage teacher prediction instead of commit-time truth.
    void updateUsingS3Pred(FullBTBPrediction &s3Pred);
    void setAbtbComponentIdx(int idx) { abtbComponentIdx = idx; }

#ifndef UNIT_TEST
    void commitBranch(const PredictionUpdateContext &context,
                      const BranchOutcome &outcome) override;
#endif

    void setTrace() override;

    // check folded hists after speculative update and recover
    void checkFoldedHist(const bitset &history, const char *when);
    void checkFoldedHist(const bitset &history, ThreadID tid, const char *when);

#ifndef UNIT_TEST
  private:
#endif

    // Look up predictions in TAGE tables for a stream of instructions
    void lookupHelper(const Addr &startPC, const std::vector<BTBEntry> &btbEntries,
                      CondTakens& results, ThreadID tid, uint8_t asidHash);

    // Calculate TAGE index for a given PC and table
    Addr getTageIndex(Addr pc, int table, uint8_t asidHash = 0,
                      ThreadID tid = 0);

    // Calculate TAGE index with folded history (uint64_t version for performance)
    Addr getTageIndex(Addr pc, int table, uint64_t foldedHist,
                      uint8_t asidHash = 0, ThreadID tid = 0);

    // Calculate TAGE tag with folded history (uint64_t version for performance)
    // position: branch position within the block (xored into tag like RTL)
    Addr getTageTag(Addr pc, int table, uint64_t foldedHist, uint64_t altFoldedHist,
                    Addr position = 0, uint8_t asidHash = 0);

    // Get branch index within a prediction block
    unsigned getBranchIndexInBlock(Addr branchPC, Addr startPC);

    // Get bank ID from PC (after removing instruction alignment bits)
    // Extract bits [bankBaseShift + bankIdWidth - 1 : bankBaseShift]
    unsigned getBankId(Addr pc) const;

    // Update branch history
    void doUpdateHist(const bitset &history, bool taken, Addr pc, Addr target,
                      ThreadID tid);

    // Number of TAGE predictor tables
    const unsigned numPredictors;

    // Size of each prediction table
    std::vector<unsigned> tableSizes;

    // Number of bits used for indexing each table
    std::vector<unsigned> tableIndexBits;

    // Number of bits used for tags in each table
    std::vector<unsigned> tableTagBits;

    // PC shift amounts for each table
    std::vector<unsigned> tablePcShifts;

    // History lengths for each table
    std::vector<unsigned> histLengths;

    struct ThreadHistoryState
    {
        std::vector<PathFoldedHist> tagFoldedHist;
        std::vector<PathFoldedHist> altTagFoldedHist;
        std::vector<PathFoldedHist> indexFoldedHist;
        std::queue<std::vector<PathFoldedHist>> aheadIndexFoldedHist;
    };

    std::vector<ThreadHistoryState> threadHistory;

    // Maximum history length, not used
    unsigned maxHistLen;

    // Number of ways for set associative design
    const unsigned numWays;

    // The actual TAGE prediction tables (table x index x way)
    std::vector<std::vector<std::vector<TageEntry>>> tageTable;

    const unsigned maxBranchPositions;  // Maximum branch positions per 64-byte block

    // useful bit reset counter, when cnt >= 256, reset useful bit of all entries
    int usefulResetCnt{0};
    std::array<int, MaxThreads> usefulResetCntByThread{};

    // Instruction shift amount
    unsigned instShiftAmt {1};

    // used for MicroTAGE update misprediction counting
    void checkUtageUpdateMisspred(
        const PredictionUpdateContext &context,
        const PreparedUpdate &update);

    // Update prediction counter with saturation
    void updateCounter(bool taken, unsigned width, short &counter);

    // Increment counter with saturation
    bool satIncrement(int max, short &counter);

    // Decrement counter with saturation
    bool satDecrement(int min, short &counter);

    // Whether to update on read
    bool updateOnRead;
    bool usingS3Pred;

    // ========== Bank Configuration ==========
    // Bank mechanism to simulate hardware bank conflicts
    // When prediction and update access the same bank in one cycle, update is dropped
    const unsigned numBanks;         // Number of banks (e.g., 4)
    const unsigned bankIdWidth;      // log2(numBanks), computed in constructor
    const unsigned bankBaseShift;    // Bits removed before bank selection (default: instShiftAmt)
    const unsigned indexShift;       // bankBaseShift + bankIdWidth when banking enabled
    bool enableBankConflict;         // Enable/disable bank conflict simulation

    // Track last prediction bank for conflict detection
    unsigned lastPredBankId;         // Bank ID of last prediction
    bool predBankValid;              // Whether lastPredBankId is valid

#ifdef UNIT_TEST
    typedef uint64_t Scalar;
#else
    typedef statistics::Scalar Scalar;
#endif

    // Statistics for TAGE predictor
#ifdef UNIT_TEST
    struct TageStats
    {
#else
    struct TageStats : public statistics::Group
    {
#endif
        Scalar predNoHitUseBim;
        Scalar predUseAlt;
        Scalar updateNoHitUseBim;
        Scalar updateUseAlt;
        Scalar updateUseAltCorrect;
        Scalar updateUseAltWrong;
        Scalar updateAltDiffers;
        Scalar updateUseAltOnNaUpdated;
        Scalar updateProviderNa;
        Scalar updateUseAltOnNaCorrect;
        Scalar updateUseAltOnNaWrong;
        Scalar updateAllocFailure;
        Scalar updateAllocFailureNoValidTable;
        Scalar updateAllocSuccess;
        Scalar updateMispred;
        Scalar updateResetU;

        Scalar updateUtageHit;
        Scalar updateUtageHitWrong;

        Scalar s3UpdateEntries;
        Scalar s3UpdateNoMeta;
        Scalar s3UpdateNoHitUseBim;
        Scalar s3UpdateUseAlt;
        Scalar s3UpdateUseAltCorrect;
        Scalar s3UpdateUseAltWrong;
        Scalar s3UpdateAltDiffers;
        Scalar s3UpdateUseAltOnNaUpdated;
        Scalar s3UpdateProviderNa;
        Scalar s3UpdateUseAltOnNaCorrect;
        Scalar s3UpdateUseAltOnNaWrong;
        Scalar s3UpdateAllocFailure;
        Scalar s3UpdateAllocFailureNoValidTable;
        Scalar s3UpdateAllocSuccess;
        Scalar s3UpdateMispred;
        Scalar s3UpdateResetU;
        Scalar s3UpdateUtageHit;
        Scalar s3UpdateUtageHitWrong;

        // Bank conflict statistics
        Scalar updateBankConflict;           // Number of bank conflicts detected
        Scalar updateDeferredDueToConflict;  // Number of updates deferred due to bank conflict (retried later)

#ifndef UNIT_TEST
        // Fine-grained per-bank statistics
        statistics::Vector updateBankConflictPerBank;  // Conflicts per bank
        statistics::Vector updateAccessPerBank;        // Update accesses per bank
        statistics::Vector predAccessPerBank;          // Prediction accesses per bank

        statistics::Distribution predTableHits;
        statistics::Distribution updateTableHits;

        statistics::Vector updateTableMispreds;
#endif

        Scalar condPredwrong;
        Scalar condMissTakens;
        Scalar condCorrect;
        Scalar condMissNoTakens;
        Scalar predHit;
        Scalar predMiss;

#ifndef UNIT_TEST
        TageStats(statistics::Group* parent, int numPredictors, int numBanks);
#endif
        void updateStatsWithTagePrediction(const TagePrediction &pred, bool when_pred);
    } ;

    TageStats tageStats;

#ifndef UNIT_TEST
    TraceManager *tageMissTrace;
#endif

public:
    // Metadata for TAGE prediction
    typedef struct TageMeta
    {
        std::unordered_map<Addr, TagePrediction> preds;
        std::vector<PathFoldedHist> tagFoldedHist;
        std::vector<PathFoldedHist> indexFoldedHist;
        std::vector<PathFoldedHist> altTagFoldedHist;
        std::vector<BTBEntry> abtbEntries;
        bool aheadIndexFoldedHistValid;
        std::vector<PathFoldedHist> aheadIndexFoldedHist;
        bitset history;     // for viewing
        TageMeta() : aheadIndexFoldedHistValid(false) {}
    } TageMeta;

    enum class TrainingMode
    {
        Resolved,
        S3Update
    };

    struct TrainingEntry
    {
        BTBEntry entry;
        bool actualTaken;
        bool controlMispred;
    };

    void trainEntries(const std::vector<TrainingEntry> &entries_to_update,
                      const std::shared_ptr<TageMeta> &predMeta,
                      const Addr &startPC,
                      ThreadID tid,
                      uint8_t asidHash,
                      TrainingMode mode);
    void trainResolvedEntries(const PreparedUpdate &update,
                              const std::shared_ptr<TageMeta> &predMeta,
                              const Addr &startPC,
                              const PredictionUpdateContext &context);

#ifdef UNIT_TEST
  public:
#else
  private:
#endif

    // Helper method to generate prediction for a single BTB entry
    // If predMeta is provided, use snapshot folded history for index/tag calculation (update path)
    // If predMeta is nullptr, use current folded history (prediction path)
    TagePrediction generateSinglePrediction(const BTBEntry &btb_entry,
                                           const Addr &startPC,
                                           const std::shared_ptr<TageMeta> predMeta = nullptr,
                                           ThreadID tid = 0,
                                           uint8_t asidHash = 0);

    // Build the reachable conditional prefix for S3 teacher update.
    std::vector<BTBEntry> prepareS3UpdateEntries(const FullBTBPrediction &s3Pred);
    std::vector<BTBEntry> prepareS3UpdateEntriesFromAbtbMeta(
        const std::vector<BTBEntry> &abtbEntries,
        FullBTBPrediction &s3Pred,
        CondTakens &teacherCondTakens);
    std::vector<BTBEntry> getAbtbConditionalEntries(
        const std::vector<BTBEntry> &btbEntries) const;
    bool isAbtbEntry(const BTBEntry &entry) const;

    // Helper method to update predictor state for a single entry
    bool updatePredictorStateAndCheckAllocation(const BTBEntry &entry,
                                 bool actual_taken,
                                 const TagePrediction &pred,
                                 bool control_mispred);
    // Reuse the provider/allocation policy under an S3-teacher mismatch definition.
    bool updatePredictorStateAndCheckAllocationS3(const BTBEntry &entry,
                                 bool actual_taken,
                                 const TagePrediction &pred);

    // Helper method to handle new entry allocation
    bool handleNewEntryAllocation(const Addr &startPC,
                                 const BTBEntry &entry,
                                 bool actual_taken,
                                 unsigned main_table,
                                 std::shared_ptr<TageMeta> meta,
                                 uint8_t asidHash,
                                 TrainingMode mode,
                                 uint64_t &allocated_table,
                                 uint64_t &allocated_index,
                                 uint64_t &allocated_way,
                                 ThreadID tid = 0);

    int abtbComponentIdx{-1};
    std::vector<std::shared_ptr<TageMeta>> threadMeta;
    ThreadID predictorTid(const std::vector<FullBTBPrediction> &stagePreds) const;
    ThreadHistoryState &historyState(ThreadID tid);
    const ThreadHistoryState &historyState(ThreadID tid) const;
};

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif  // __CPU_PRED_BTB_MICROTAGE_HH__
