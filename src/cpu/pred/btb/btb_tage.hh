#ifndef __CPU_PRED_BTB_TAGE_HH__
#define __CPU_PRED_BTB_TAGE_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/pred/btb/test_stats.hh"
#include "cpu/pred/btb/timed_base_pred.hh"

// Conditional includes based on build mode
#ifdef UNIT_TEST
    #include "cpu/pred/btb/test/test_dprintf.hh"
#else
    #include "debug/DecoupleBP.hh"
    #include "debug/TAGEUseful.hh"
    #include "debug/TAGEHistory.hh"
    #include "params/BTBTAGE.hh"
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

class BTBTAGE : public TimedBaseBTBPredictor
{
    using defer = std::shared_ptr<void>;
    using bitset = boost::dynamic_bitset<>;
    static constexpr unsigned MaxThreads = o3::MaxThreads;
  public:
#ifdef UNIT_TEST
    // Test constructor
    BTBTAGE(unsigned numPredictors = 4, unsigned numWays = 2,
            unsigned tableSize = 1024, unsigned numBanks = 4,
            bool usePathHistory = true);
#else
    // Production constructor
    typedef BTBTAGEParams Params;
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
            unsigned lruCounter; // Counter for LRU replacement policy

            TageEntry() : valid(false), tag(0), counter(0), useful(false), pc(0), lruCounter(0) {}

            TageEntry(Addr tag, short counter, Addr pc) :
                      valid(true), tag(tag), counter(counter), useful(false), pc(pc), lruCounter(0) {}
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
            TageTableInfo altInfo;  // Alternative prediction info
            bool useAlt;           // Whether to use alternative prediction, true if main is weak or no main prediction
            bool taken;            // Final prediction (taken/not taken) = use_alt ? alt_provided ? alt_taken : base_taken : main_taken
            bool altPred;          // Alternative prediction = alt_provided ? alt_taken : base_taken;
            int finalProviderTable; // Table that supplied the final prediction, -1 means base BTB
            bool finalProviderIsAlt; // Whether final prediction came from alternate provider
            Addr useAltIdx;        // useAltOnNa index consulted at prediction time
            short useAltCtr;       // useAltOnNa counter value before update
            uint64_t hitTableMask; // Bitmask of all TAGE tables that matched during lookup


            TagePrediction() : btb_pc(0), useAlt(false), taken(false), altPred(false),
                               finalProviderTable(-1), finalProviderIsAlt(false),
                               useAltIdx(0), useAltCtr(0), hitTableMask(0) {}

            TagePrediction(Addr btb_pc, TageTableInfo mainInfo, TageTableInfo altInfo,
                            bool useAlt, bool taken, bool altPred,
                            int finalProviderTable, bool finalProviderIsAlt,
                            Addr useAltIdx, short useAltCtr, uint64_t hitTableMask) :
                            btb_pc(btb_pc), mainInfo(mainInfo), altInfo(altInfo),
                            useAlt(useAlt), taken(taken), altPred(altPred),
                            finalProviderTable(finalProviderTable),
                            finalProviderIsAlt(finalProviderIsAlt),
                            useAltIdx(useAltIdx), useAltCtr(useAltCtr),
                            hitTableMask(hitTableMask) {}
    };


#ifndef UNIT_TEST
    BTBTAGE(const Params& p);
#endif
    ~BTBTAGE();

    void tickStart() override;

    void tick() override;
    void dryRunCycle(Addr startAddr) override;
    // Make predictions for a stream of instructions and record in stage preds
    void putPCHistory(Addr startAddr,
                      const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;
    void lookupNoSideEffect(const Addr &startPC,
                            const std::vector<BTBEntry> &btbEntries,
                            CondTakens &results,
                            ThreadID tid = 0,
                            uint8_t asidHash = 0) const;

    std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;
    void refreshPredictionMeta(Addr startAddr,
                               const boost::dynamic_bitset<> &history,
                               FullBTBPrediction &pred) override;

    // Update folded history from GHR when configured in direction-history mode.
    void specUpdateGHist(const boost::dynamic_bitset<> &history,
                        FullBTBPrediction &pred,
                        const DirectionHistoryUpdate &update) override;
    // Update folded history from PHR when configured in path-history mode.
    void specUpdatePHist(const boost::dynamic_bitset<> &history,
                         FullBTBPrediction &pred,
                         const PathHistoryUpdate &update) override;

    void recoverHist(const boost::dynamic_bitset<> &history,
                     const FetchTarget &entry, int shamt,
                     bool cond_taken) override;
    void recoverPHist(const boost::dynamic_bitset<> &history,
                      const FetchTarget &entry,
                      const PathHistoryUpdate &update) override;

    // Update predictor state based on actual branch outcomes
    void update(const FetchTarget &entry) override;
    bool canResolveUpdate(const FetchTarget &entry) override;
    void doResolveUpdate(const FetchTarget &entry) override;

#ifndef UNIT_TEST
    void commitBranch(const FetchTarget &stream, const DynInstPtr &inst) override;
#endif

    void setTrace() override;

    // check folded hists after speculative update and recover
    virtual void checkFoldedHist(const bitset &history, const char *when);
    void checkFoldedHist(const bitset &history, ThreadID tid, const char *when);

#ifndef UNIT_TEST
  protected:
#endif

    // Look up predictions in TAGE tables for a stream of instructions
    void lookupHelper(const Addr &startPC, const std::vector<BTBEntry> &btbEntries,
                    std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs,
                    CondTakens& results, ThreadID tid, uint8_t asidHash);

    // Calculate TAGE index for a given PC and table
    Addr getTageIndex(Addr pc, int table, uint8_t asidHash = 0,
                      ThreadID tid = 0) const;

    // Calculate TAGE index with folded history (uint64_t version for performance)
    Addr getTageIndex(Addr pc, int table, uint64_t foldedHist,
                      uint8_t asidHash = 0, ThreadID tid = 0) const;

    // Calculate TAGE tag for a given PC and table
    // position: branch position within the block (xored into tag like RTL)
    Addr getTageTag(Addr pc, int table, Addr position = 0, uint8_t asidHash = 0) const;

    // Calculate TAGE tag with folded history (uint64_t version for performance)
    // position: branch position within the block (xored into tag like RTL)
    Addr getTageTag(Addr pc, int table, uint64_t foldedHist, uint64_t altFoldedHist,
                    Addr position = 0, uint8_t asidHash = 0) const;

    // Get offset within a block for a given PC
    Addr getOffset(Addr pc) const {
        return (pc & (blockSize - 1)) >> 1;
    }

    // Get branch index within a prediction block
    unsigned getBranchIndexInBlock(Addr branchPC, Addr startPC) const;

    // Get bank ID from PC (after removing instruction alignment bits)
    // Extract bits [bankBaseShift + bankIdWidth - 1 : bankBaseShift]
    unsigned getBankId(Addr pc) const;

    // Update branch history
    void doUpdateHist(const bitset &history, int shamt, bool taken,
                      Addr pc, Addr target, ThreadID tid);
    void recoverFoldedHist(const FetchTarget &entry);

    // Number of TAGE predictor tables
    const unsigned numPredictors;

    // Size of each prediction table
    std::vector<unsigned> tableSizes;

    // Number of bits used for indexing each table
    std::vector<unsigned> tableIndexBits;

    // Masks for table indexing
    std::vector<bitset> tableIndexMasks;

    // Number of bits used for tags in each table
    std::vector<unsigned> tableTagBits;

    // Masks for tag matching
    std::vector<bitset> tableTagMasks;

    // PC shift amounts for each table
    std::vector<unsigned> tablePcShifts;

    // History lengths for each table
    std::vector<unsigned> histLengths;

    const bool usePathHistory;

    struct ThreadHistoryState
    {
        std::vector<TageFoldedHist> tagFoldedHist;
        std::vector<TageFoldedHist> altTagFoldedHist;
        std::vector<TageFoldedHist> indexFoldedHist;
    };

    std::vector<ThreadHistoryState> threadHistory;

    // Linear feedback shift register for allocation
    LFSR64 allocLFSR;

    // Maximum history length, not used
    unsigned maxHistLen;

    // Number of ways for each table in the set associative design
    std::vector<unsigned> numWays;

    // The actual TAGE prediction tables (table x index x way)
    std::vector<std::vector<std::vector<TageEntry>>> tageTable;

    const unsigned maxBranchPositions;  // Maximum branch positions per 64-byte block

    // Table for tracking when to use alternative prediction on provider weak
    // use_alt_on_na: indexed by PC, 7-bit signed saturating counter [-64, 63]
    const unsigned useAltOnNaSize;
    const unsigned useAltOnNaWidth;
    std::vector<short> useAlt;

    // useful bit reset counter, when cnt >= 256, reset useful bit of all entries
    int usefulResetCnt{0};
    std::array<int, MaxThreads> usefulResetCntByThread{};

    // Check if a tag matches
    bool matchTag(Addr expected, Addr found) const;

    // Set tag bits for a given table
    void setTag(Addr &dest, Addr src, int table);

    // Number of tables to allocate on misprediction
    unsigned numTablesToAlloc;

    // Instruction shift amount
    unsigned instShiftAmt {1};

    // use for microtage updatemispred counting
    void checkUtageUpdateMisspred(const FetchTarget &stream);

    // Update prediction counter with saturation
    void updateCounter(bool taken, unsigned width, short &counter);

    // Increment counter with saturation
    bool satIncrement(int max, short &counter);

    // Decrement counter with saturation
    bool satDecrement(int min, short &counter);

    // Get index for useAlt table
    Addr getUseAltIdx(Addr pc) const;

    // Cache for TAGE indices
    std::vector<Addr> tageIndex;

    // Cache for TAGE tags
    std::vector<Addr> tageTag;

    // Whether statistical corrector is enabled
    bool enableSC;

    // Whether to update on read
    bool updateOnRead;

    // ========== Bank Configuration ==========
    // Bank mechanism to simulate hardware bank conflicts
    // When prediction and update access the same bank in one cycle, update is dropped
    const unsigned numBanks;         // Number of banks (e.g., 4)
    const unsigned bankIdWidth;      // log2(numBanks), computed in constructor
    const unsigned blockWidth;       // floorLog2(blockSize), e.g., 5 for 32B blocks
    const unsigned bankBaseShift;    // Bits removed before bank selection (default: instShiftAmt)
    const unsigned indexShift;       // bankBaseShift + bankIdWidth when banking enabled
    bool enableBankConflict;         // Enable/disable bank conflict simulation

    // Track last prediction bank for conflict detection
    unsigned lastPredBankId;         // Bank ID of last prediction
    bool predBankValid;              // Whether lastPredBankId is valid

    using Scalar = test_stats::Scalar;
    using Vector = test_stats::Vector;
    using Distribution = test_stats::Distribution;

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
        Scalar updateUseNaCorrect;
        Scalar updateUseNaWrong;
        Scalar updateUseAltOnNaCorrect;
        Scalar updateUseAltOnNaWrong;
        Scalar updateAllocFailure;
        Scalar updateAllocFailureNoValidTable;
        Scalar updateAllocSuccess;
        Scalar updateMispred;
        Scalar updateResetU;
        Scalar resolveBranchHasProvider;
        Scalar resolveBranchUseProvider;
        Scalar resolveBranchHasAlt;
        Scalar resolveBranchUseAltTable;
        Scalar resolveBranchUseBaseTable;
        Scalar mispredictBranchHasProvider;
        Scalar mispredictBranchUseProvider;
        Scalar mispredictBranchHasAlt;
        Scalar mispredictBranchUseAltTable;
        Scalar mispredictBranchUseBaseTable;
        Scalar predFinalSourceBase;
        Scalar updateFinalSourceBaseCorrect;
        Scalar updateFinalSourceBaseWrong;

        // Recomputed prediction difference statistics (per fetchBlock)
        Scalar recomputedVsActualDiff;   // recomputed.taken != actual_taken
        Scalar recomputedVsOriginalDiff; // recomputed.taken != original pred.taken

        // Bank conflict statistics
        Scalar updateBankConflict;           // Number of bank conflicts detected
        Scalar updateDeferredDueToConflict;  // Number of updates deferred due to bank conflict (retried later)

        // Fine-grained per-bank statistics
        Vector updateBankConflictPerBank;  // Conflicts per bank
        Vector updateAccessPerBank;        // Update accesses per bank
        Vector predAccessPerBank;          // Prediction accesses per bank

        Vector resolveProviderTable;
        Vector resolveAltTable;
        Vector resolveUseProviderTable;
        Vector resolveUseAltTable;
        Vector mispredictUseProviderTable;
        Vector mispredictUseAltTable;

        Distribution predTableHits;
        Distribution updateTableHits;

        Vector updateTableMispreds;
        Vector predFinalSourceTable;
        Vector updateFinalSourceTableCorrect;
        Vector updateFinalSourceTableWrong;

        Scalar condPredwrong;
        Scalar condMissTakens;
        Scalar condCorrect;
        Scalar condMissNoTakens;
        Scalar predHit;
        Scalar predMiss;

        Scalar s3PredwrongTage;

        int bankIdx;
        int numPredictors;
        int numBanks;

#ifndef UNIT_TEST
        TageStats(statistics::Group* parent, int numPredictors, int numBanks);
#endif
        void init(int numPredictors, int numBanks);
        void updateStatsWithTagePrediction(const TagePrediction &pred, bool when_pred);
    } ;

    TageStats tageStats;

#ifndef UNIT_TEST
    TraceManager *tageMissTrace;
#endif

public:

    // Recover folded history after misprediction
    void recoverFoldedHist(const bitset& history);
    bool usesPathHistory() const { return usePathHistory; }

public:


    // Metadata for TAGE prediction
    typedef struct TageMeta
    {
        std::unordered_map<Addr, TagePrediction> preds;
        std::vector<TageFoldedHist> tagFoldedHist;
        std::vector<TageFoldedHist> altTagFoldedHist;
        std::vector<TageFoldedHist> indexFoldedHist;
        bitset history;     // for viewing
        TageMeta() {}
    } TageMeta;

    struct AllocationTraceInfo
    {
        bool success = false;
        uint64_t table = 0;
        uint64_t index = 0;
        uint64_t way = 0;
        uint64_t tag = 0;
        bool victimValid = false;
        uint64_t victimTag = 0;
        short victimCounter = 0;
        bool victimUseful = false;
        uint64_t victimPC = 0;
    };

private:

    // Helper method to generate prediction for a single BTB entry
    // If predMeta is provided, use snapshot folded history for index/tag calculation (update path)
    // If predMeta is nullptr, use current folded history (prediction path)
    TagePrediction generateSinglePrediction(const BTBEntry &btb_entry,
                                           const Addr &startPC,
                                           const std::shared_ptr<TageMeta>& predMeta = nullptr,
                                           ThreadID tid = 0,
                                           uint8_t asidHash = 0) const;

    // Helper method to prepare BTB entries for update
    std::vector<BTBEntry> prepareUpdateEntries(const FetchTarget &stream);

    // Helper method to update predictor state for a single entry
    bool updatePredictorStateAndCheckAllocation(const BTBEntry &entry,
                                 bool actual_taken,
                                 const TagePrediction &pred,
                                 const FetchTarget &stream);

    // Helper method to handle new entry allocation
    bool handleNewEntryAllocation(const Addr &startPC,
                                 const BTBEntry &entry,
                                 bool actual_taken,
                                 unsigned main_table,
                                 const std::shared_ptr<TageMeta>& meta,
                                 uint8_t asidHash,
                                 ThreadID tid,
                                 AllocationTraceInfo &allocInfo);


    // Helper methods for LRU management
    void updateLRU(int table, Addr index, unsigned way);
    unsigned getLRUVictim(int table, Addr index);
    unsigned getNumWays(unsigned table) const;

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

#endif  // __CPU_PRED_BTB_TAGE_HH__
