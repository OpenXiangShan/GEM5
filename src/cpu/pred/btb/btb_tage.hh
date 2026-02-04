#ifndef __CPU_PRED_BTB_TAGE_HH__
#define __CPU_PRED_BTB_TAGE_HH__

#include <cstdint>
#include <deque>
#include <map>
#include <utility>
#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/folded_hist.hh"
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
  public:
#ifdef UNIT_TEST
    // Test constructor
    BTBTAGE(unsigned numPredictors = 4, unsigned numWays = 2, unsigned tableSize = 1024, unsigned numBanks = 4);
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
            // Confidence counter for payload correctness (recommended: reuse 3-bit signed [-4..3]).
            // Weak states follow existing heuristic: conf==0 or conf==-1.
            short conf;
            bool useful;    // 1-bit usefulness counter; true means useful
            uint8_t exitSlotEnc; // 0=No-Cond-Exit, 1..32 => slot=enc-1
            unsigned lruCounter; // Counter for LRU replacement policy

            TageEntry() : valid(false), tag(0), conf(0), useful(false), exitSlotEnc(0), lruCounter(0) {}

            TageEntry(Addr tag, short conf, uint8_t exitSlotEnc) :
                      valid(true), tag(tag), conf(conf), useful(false),
                      exitSlotEnc(exitSlotEnc), lruCounter(0) {}
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
    };

    enum class PredSource : uint8_t
    {
        Provider = 0,
        Alt = 1,
        Base = 2,
    };

    // Contains the complete prediction result
    struct TagePrediction
    {
        public:
            Addr startPC;            // Fetch block start PC (aligned as used by MBTB/TAGE)
            TageTableInfo mainInfo;  // Provider info
            TageTableInfo altInfo;   // Alternative provider info
            bool useAlt;             // Whether weak-provider useAltOnNa gate selects alt/base
            PredSource source;       // Final decision source
            uint8_t predEnc;         // Final ExitSlotEnc used by this component (0..32)
            uint8_t baseEnc;         // Base ExitSlotEnc (computed from MBTB ctr, 0..32)
            bool payloadMapped;      // predEnc!=0 and found matching cond entry in btbEntries
            Addr predCondPC;         // PC of predicted cond exit (0 if No-Cond-Exit or map fail)

            TagePrediction()
                : startPC(0), useAlt(false), source(PredSource::Base),
                  predEnc(0), baseEnc(0), payloadMapped(false), predCondPC(0) {}

            TagePrediction(Addr startPC, TageTableInfo mainInfo, TageTableInfo altInfo,
                           bool useAlt, PredSource source,
                           uint8_t predEnc, uint8_t baseEnc,
                           bool payloadMapped, Addr predCondPC)
                : startPC(startPC), mainInfo(mainInfo), altInfo(altInfo),
                  useAlt(useAlt), source(source),
                  predEnc(predEnc), baseEnc(baseEnc),
                  payloadMapped(payloadMapped), predCondPC(predCondPC) {}
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

    std::shared_ptr<void> getPredictionMeta() override;

    // speculative update 3 folded history, according history and pred.taken
    // the other specUpdateHist methods are left blank
    void specUpdatePHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;

    // Recover 3 folded history after a misprediction, then update 3 folded history according to history and pred.taken
    // the other recoverHist methods are left blank
    void recoverPHist(const boost::dynamic_bitset<> &history,
                        const FetchTarget &entry,int shamt, bool cond_taken) override;

#ifdef UNIT_TEST
    // API compatibility wrappers for testing
    void specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override
    {
        specUpdatePHist(history, pred);
    }

    void recoverHist(const boost::dynamic_bitset<> &history, const FetchTarget &entry, int shamt,
                     bool cond_taken) override
    {
        recoverPHist(history, entry, shamt, cond_taken);
    }
#endif

    // Update predictor state based on actual branch outcomes
    void update(const FetchTarget &entry) override;
    bool canResolveUpdate(const FetchTarget &entry) override;
    void doResolveUpdate(const FetchTarget &entry) override;

#ifndef UNIT_TEST
    void commitBranch(const FetchTarget &stream, const DynInstPtr &inst) override;
#endif

    void setTrace() override;

    // check folded hists after speculative update and recover
    void checkFoldedHist(const bitset &history, const char *when);

#ifndef UNIT_TEST
  private:
#endif

    // Look up predictions in TAGE tables for a stream of instructions
    void lookupHelper(const Addr &startPC, const std::vector<BTBEntry> &btbEntries,
                    std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs, CondTakens& results);

    // Calculate TAGE index for a given PC and table
    Addr getTageIndex(Addr pc, int table);

    // Calculate TAGE index with folded history (uint64_t version for performance)
    Addr getTageIndex(Addr pc, int table, uint64_t foldedHist);

    // Calculate TAGE tag for a given PC and table
    Addr getTageTag(Addr pc, int table);

    // Calculate TAGE tag with folded history (uint64_t version for performance)
    Addr getTageTag(Addr pc, int table, uint64_t foldedHist, uint64_t altFoldedHist);

    // Get offset within a block for a given PC
    Addr getOffset(Addr pc) {
        return (pc & (blockSize - 1)) >> 1;
    }

    // Get branch index within a prediction block
    unsigned getBranchIndexInBlock(Addr branchPC, Addr startPC) const;

    // Get bank ID from PC (after removing instruction alignment bits)
    // Extract bits [bankBaseShift + bankIdWidth - 1 : bankBaseShift]
    unsigned getBankId(Addr pc) const;

    // Update branch history
    void doUpdateHist(const bitset &history, bool taken, Addr pc, Addr target);

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

    // Folded history for tag calculation
    std::vector<PathFoldedHist> tagFoldedHist;

    // Folded history for alternative tag calculation
    std::vector<PathFoldedHist> altTagFoldedHist;

    // Folded history for index calculation
    std::vector<PathFoldedHist> indexFoldedHist;

    // Linear feedback shift register for allocation
    LFSR64 allocLFSR;

    // Maximum history length, not used
    unsigned maxHistLen;

    // Number of ways for set associative design
    const unsigned numWays;

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

    // Check if a tag matches
    bool matchTag(Addr expected, Addr found);

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
    Addr getUseAltIdx(Addr pc);

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
        Scalar updateUseNaCorrect;
        Scalar updateUseNaWrong;
        Scalar updateUseAltOnNaCorrect;
        Scalar updateUseAltOnNaWrong;
        Scalar updateAllocFailure;
        Scalar updateAllocFailureNoValidTable;
        Scalar updateAllocSuccess;
        Scalar updateMispred;
        Scalar updateResetU;

        // ===== Exit-Slot specific counters (block-level) =====
        Scalar predNoCondExit;
        Scalar predBaseFallback;
        Scalar predPayloadMapFail;

        Scalar updateAllocOnMiss;
        Scalar updateAllocStrongWrong;
        Scalar updateRewriteWeakWrong;
        Scalar updateNoAllocWeakCorrect;

        // Recomputed prediction difference statistics (per fetchBlock)
        Scalar recomputedVsActualDiff;   // recomputed.taken != actual_taken
        Scalar recomputedVsOriginalDiff; // recomputed.taken != original pred.taken

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

        Scalar s3PredwrongTage;

        int bankIdx;
        int numPredictors;
        int numBanks;

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

    // Recover folded history after misprediction
    void recoverFoldedHist(const bitset& history);

public:


    // Metadata for TAGE prediction
    typedef struct TageMeta
    {
        TagePrediction pred;
        bool hasPred{false};
        std::vector<PathFoldedHist> tagFoldedHist;
        std::vector<PathFoldedHist> altTagFoldedHist;
        std::vector<PathFoldedHist> indexFoldedHist;
        bitset history;     // for viewing
        TageMeta() {}
    } TageMeta;

private:

    // Lookup provider/alt in TAGE tables for this fetch block (startPC + PHR snapshot).
    // If predMeta is provided, use snapshot folded history for index/tag calculation (update path).
    std::pair<TageTableInfo, TageTableInfo>
    lookupProviders(const Addr &startPC,
                    const std::shared_ptr<TageMeta> predMeta = nullptr);

    // Compute Base exit-slot encoding from MBTB entries (ctr/alwaysTaken), 0..32.
    uint8_t getBaseExitSlotEnc(const Addr &startPC,
                               const std::vector<BTBEntry> &btbEntries) const;

    // Map predicted exit slot to a cond BTB entry in this block. Returns 0 on failure.
    Addr mapExitSlotToCondPC(const Addr &startPC,
                             const std::vector<BTBEntry> &btbEntries,
                             uint8_t predEnc) const;

    // Allocation helper for block-level entry (payload = RealEnc).
    bool handleNewEntryAllocation(const Addr &startPC,
                                 uint8_t realEnc,
                                 unsigned start_table,
                                 std::shared_ptr<TageMeta> meta,
                                 uint64_t &allocated_table,
                                 uint64_t &allocated_index,
                                 uint64_t &allocated_way);


    // Helper methods for LRU management
    void updateLRU(int table, Addr index, unsigned way);
    unsigned getLRUVictim(int table, Addr index);

    std::shared_ptr<TageMeta> meta;
};

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif  // __CPU_PRED_BTB_TAGE_HH__
