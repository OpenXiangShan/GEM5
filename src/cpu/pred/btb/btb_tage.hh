#ifndef __CPU_PRED_BTB_TAGE_HH__
#define __CPU_PRED_BTB_TAGE_HH__

#include <deque>
#include <map>
#include <vector>
#include <utility>
#include <cstdint>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/pred/btb/stream_struct.hh"
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
    BTBTAGE(unsigned numPredictors = 4, unsigned numWays = 2, unsigned tableSize = 1024);
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

            TagePrediction() : btb_pc(0), useAlt(false), taken(false), altPred(false) {}

            TagePrediction(Addr btb_pc, TageTableInfo mainInfo, TageTableInfo altInfo,
                            bool useAlt, bool taken, bool altPred) :
                            btb_pc(btb_pc), mainInfo(mainInfo), altInfo(altInfo),
                            useAlt(useAlt), taken(taken), altPred(altPred) {}
    };


#ifndef UNIT_TEST
    BTBTAGE(const Params& p);
#endif
    ~BTBTAGE();

    void tickStart() override;

    void tick() override;
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
                        const FetchStream &entry,int shamt, bool cond_taken) override;

#ifdef UNIT_TEST
    // API compatibility wrappers for testing
    void specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override
    {
        specUpdatePHist(history, pred);
    }

    void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt,
                     bool cond_taken) override
    {
        recoverPHist(history, entry, shamt, cond_taken);
    }
#endif

    // Update predictor state based on actual branch outcomes
    void update(const FetchStream &entry) override;

#ifndef UNIT_TEST
    void commitBranch(const FetchStream &stream, const DynInstPtr &inst) override;
#endif

    void setTrace() override;

    // check folded hists after speculative update and recover
    void checkFoldedHist(const bitset &history, const char *when);

#ifndef UNIT_TEST
  private:
#endif

    // Look up predictions in TAGE tables for a stream of instructions
    void lookupHelper(const Addr &alignedPC, const std::vector<BTBEntry> &btbEntries,
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

    // Get base table index for a given PC
    Addr getBaseTableIndex(Addr pc);

    // Get branch index within a prediction block
    unsigned getBranchIndexInBlock(Addr pc, Addr alignedPC);

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

    // Base table for fallback predictions (index x position)
    // Index based on 32-byte aligned address, covers 64-byte block
    // Each entry supports up to maxBranchPositions branch positions within the block
    std::vector<std::vector<short>> baseTable;
    const unsigned baseTableSize;  // Base table size
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

#ifndef UNIT_TEST
        statistics::Distribution predTableHits;
        statistics::Distribution updateTableHits;

        statistics::Vector updateTableMispreds;
#endif

        int bankIdx;
        int numPredictors;

#ifndef UNIT_TEST
        TageStats(statistics::Group* parent, int numPredictors);
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
        std::unordered_map<Addr, TagePrediction> preds;
        std::vector<PathFoldedHist> tagFoldedHist;
        std::vector<PathFoldedHist> altTagFoldedHist;
        std::vector<PathFoldedHist> indexFoldedHist;
        bitset history;     // for viewing
        TageMeta() {}
    } TageMeta;

private:

    // Helper method to generate prediction for a single BTB entry
    // If predMeta is provided, use snapshot folded history for index/tag calculation (update path)
    // If predMeta is nullptr, use current folded history (prediction path)
    TagePrediction generateSinglePrediction(const BTBEntry &btb_entry,
                                           const Addr &alignedPC,
                                           const std::shared_ptr<TageMeta> predMeta = nullptr);

    // Helper method to prepare BTB entries for update
    std::vector<BTBEntry> prepareUpdateEntries(const FetchStream &stream);

    // Helper method to update predictor state for a single entry
    bool updatePredictorStateAndCheckAllocation(const BTBEntry &entry,
                                 bool actual_taken,
                                 const TagePrediction &pred,
                                 const FetchStream &stream);

    // Helper method to handle new entry allocation
    bool handleNewEntryAllocation(const Addr &alignedPC,
                                 const BTBEntry &entry,
                                 bool actual_taken,
                                 unsigned main_table,
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
