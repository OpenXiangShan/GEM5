#ifndef __CPU_PRED_BTB_PAIRTAGE_HH__
#define __CPU_PRED_BTB_PAIRTAGE_HH__

#include <array>
#include <memory>
#include <queue>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "params/PairTAGE.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

class PairTAGE : public TimedBaseBTBPredictor
{
    using bitset = boost::dynamic_bitset<>;
    static constexpr unsigned MaxThreads = o3::MaxThreads;

  public:
    typedef PairTAGEParams Params;

    // Fields the model needs only and should not appear in real units.
    struct PTAGEEntryModelInfo
    {
        // instruction size of the first and second block's exit
        uint8_t branchSize;
        uint8_t block2BranchSize;

        PTAGEEntryModelInfo() : branchSize(0), block2BranchSize(0) {}
    };

    // Prediction Block Information
    struct PTAGEBlockInfo
    {
        // Width of the signed direction counter
        static constexpr unsigned CounterWidth = 2;

        short directionCounter;
        Addr branchPC;
        Addr targetPC;

        // Type of the exit branch, as the RTL branch attribute
        bool isCond;
        bool isDirect;
        bool isIndirect;
        bool isCall;
        bool isReturn;

        PTAGEBlockInfo()
            : directionCounter(0), branchPC(0), targetPC(0), isCond(false),
              isDirect(false), isIndirect(false), isCall(false),
              isReturn(false)
        {}

        // A new block, weak in its direction. It leaves at exit when exit
        // is valid, and falls through to targetPC otherwise.
        PTAGEBlockInfo(const BTBEntry &exit, Addr targetPC)
            : directionCounter(exit.valid ? 0 : -1),
              branchPC(exit.valid ? exit.pc : 0), targetPC(targetPC),
              isCond(exit.valid && exit.isCond),
              isDirect(exit.valid && exit.isDirect),
              isIndirect(exit.valid && exit.isIndirect),
              isCall(exit.valid && exit.isCall),
              isReturn(exit.valid && exit.isReturn)
        {}

        bool taken() const { return directionCounter >= 0; }

        // Saturating update of the direction counter towards taken
        void
        updateCounter(bool taken)
        {
            const short max = (1 << (CounterWidth - 1)) - 1;
            const short min = -(1 << (CounterWidth - 1));
            if (taken && directionCounter < max) {
                ++directionCounter;
            } else if (!taken && directionCounter > min) {
                --directionCounter;
            }
        }

        // Whether the direction counter is at one of its two weakest values
        bool
        isWeak() const
        {
            return directionCounter == 0 || directionCounter == -1;
        }
    };

    struct PTAGEEntry
    {
        bool valid;
        bool b2Valid;
        Addr tag;
        bool useful;

        // Confidence that both blocks are predicted right
        uint8_t confidence;

        PTAGEBlockInfo block;
        PTAGEBlockInfo block2;

        PTAGEEntryModelInfo modelInfo;

        PTAGEEntry()
            : valid(false), b2Valid(false), tag(0), useful(false),
              confidence(0)
        {}

        // A new allocate entry holding one block, weak in its direction
        PTAGEEntry(Addr tag, const BTBEntry &exit, Addr targetPC)
            : valid(true), b2Valid(false), tag(tag), useful(false),
              confidence(0), block(exit, targetPC)
        {
            modelInfo.branchSize = exit.valid ? exit.size : 0;
        }
    };

    PairTAGE(const Params &p);

    // Predict at s1, alternating between the first block of a looked-up
    // entry and the second block of the same entry. A prediction takes
    // priority over uBTB and aBTB, and one that falls through overrides
    // their taken predictions as well.
    void putPCHistory(Addr startAddr, const bitset &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;

    void specUpdatePHist(const bitset &history, FullBTBPrediction &pred,
                         const PathHistoryUpdate &update) override;

    void recoverPHist(const bitset &history,
                      const HistoryRecoveryContext &context,
                      const PathHistoryUpdate &update) override;

    // Fast training hook, driven by the final-stage prediction of every
    // predicted block, like the other s1 predictors.
    void updateUsingS3Pred(FullBTBPrediction &s3Pred);

    // Check the folded histories of thread tid against its full history
    void checkFoldedHist(const bitset &history, ThreadID tid,
                         const char *when);

  private:
    // Number of PairTAGE tables
    const unsigned numPredictors;

    // Number of entries in each table, summed over all banks and ways
    std::vector<unsigned> tableSizes;

    // Number of bits used for indexing each table, bank bits included
    std::vector<unsigned> tableIndexBits;

    // Number of bits used for tags in each table
    std::vector<unsigned> tableTagBits;

    // History lengths for each table
    std::vector<unsigned> histLengths;

    // Number of ways per set;
    const unsigned numWays;

    // ========== Bank Configuration ==========
    // Each table is split into banks selected by low PC bits, A bank
    // owns a contiguous range of indices, so an index is laid out
    // as {bank, set}.
    const unsigned numBanks;         // Number of banks (e.g., 4)
    const unsigned bankIdWidth;      // log2(numBanks), computed in constructor

    // The PairTAGE prediction tables (table x index x way).
    std::vector<std::vector<std::vector<PTAGEEntry>>> table;

    // ========== Index and Tag ==========
    // PC fields from high to low bits: {tag, set, bank, instOffset}
    // The tag takes the PC bits right above the index, so it carries
    // information the index does not already have.

    // Instruction alignment bits stripped before any PC field
    const unsigned instShiftAmt{1};

    // Bank selected by the PC bits right above the instruction offset
    unsigned getBankId(Addr pc) const;

    // Set within a bank: the PC bits above the bank XORed with the folded
    // history of table t
    Addr getSetIndex(Addr pc, unsigned t, uint64_t foldedHist) const;

    // Index into table[t], laid out as {bank, set}
    Addr getIndex(unsigned t, unsigned bank, Addr set) const;

    // Tag: the PC bits above the index XORed with two folds of different
    // widths, so the tag does not simply repeat the history the index uses
    Addr getTag(Addr pc, unsigned t, uint64_t foldedHist,
                uint64_t altFoldedHist) const;

    // ========== History ==========
    // Path folded histories of every table, kept per thread. Each table
    // folds its own history length to the width of its set index and tag.
    struct ThreadHistoryState
    {
        std::vector<PathFoldedHist> indexFoldedHist;
        std::vector<PathFoldedHist> tagFoldedHist;
        std::vector<PathFoldedHist> altTagFoldedHist;
    };

    std::vector<ThreadHistoryState> threadHistory;

    ThreadHistoryState &historyState(ThreadID tid);
    const ThreadHistoryState &historyState(ThreadID tid) const;

    // Advance the folded histories of thread tid by one block
    void doUpdateHist(const bitset &history, const PathHistoryUpdate &update,
                      ThreadID tid);

    // ========== Table Access ==========
    // Entry index in one table, including useful bit for training.
    struct TableIndex
    {
        Addr index{0};
        Addr tag{0};
        bool useful{false};
    };

    // Result of looking up every table for one block. Tables are ordered
    // by history length, so the provider is the highest-numbered table
    // that hits.
    struct LookupResult
    {
        std::vector<TableIndex> tableIndexes;
        int provider{-1};
        unsigned providerWay{0};
        PTAGEEntry entry;
    };

    // Look up every table with the PC and folded histories of a block
    LookupResult lookup(Addr pc, const ThreadHistoryState &history) const;

    // Replace the stage predictions with one block
    void fillStagePredictions(const PTAGEBlockInfo &block, uint8_t branchSize,
                              std::vector<FullBTBPrediction> &stagePreds);

    // Overwrite one way of set index of table t
    void writeEntry(unsigned t, Addr index, unsigned way,
                    const PTAGEEntry &entry);

    // ========== Ahead Pipeline ==========
    // The table read for a block is issued one block ahead.
    //
    // The term "Anchor" means that ahead pipeline start to output result.
    struct AheadKey
    {
        // False at start and after a squash, until a first block enters
        // the FTQ
        bool valid{false};
        Addr pc{0};
        ThreadHistoryState history;
    };

    std::vector<AheadKey> threadAhead;

    // ========== Block Phase ==========
    // Predictions alternate between the two blocks of an entry. The first
    // block phase looks up an entry and predicts its first block. The
    // second block phase predicts the second block of the pending pair,
    // without a lookup. Where an entry holds a whole group, only the first
    // block of a group becomes the key of the next one. A squash restarts
    // at the first block phase.
    enum class StatePhase { First, Second };

    std::vector<StatePhase> threadPhase;

    // ========== Prediction Meta ==========
    // Recorded in the FTQ entry of every predicted block
    struct PairTAGEMeta
    {
        // Folded histories the block was predicted with, restored on squash
        ThreadHistoryState history;

        // Mark this block has no anchor so there is no meta for training
        // And the training will be skipped for this block.
        bool noAnchor{false};

        // Phase the block was predicted in
        StatePhase phase{StatePhase::First};

        // First block phase: lookup the block was predicted with
        LookupResult lookup;
    };

    std::vector<std::shared_ptr<PairTAGEMeta>> threadMeta;

    // ========== Training ==========
    // Allocations refused because every candidate entry is useful. Once the
    // count saturates, the next allocation may evict a useful entry, so
    // entries that stopped being used do not hold their tables forever.
    static constexpr unsigned AllocRefusalWidth = 5;
    unsigned allocRefusals{0};

    // Entry confidence rises when a strengthened first block is followed
    // by a strengthened second block, and falls when a valid second block
    // is wrong. An entry whose confidence saturates at confidenceSaturate
    // is ready to emit both blocks in one cycle.
    const unsigned confidenceSaturate;

    // A trained first block, held until the next block arrives so that
    // block can be trained as its second block.
    // The next block should start at the `nextPC`.
    struct PendingTrainPair
    {
        bool valid{false};
        unsigned table{0};
        Addr index{0};
        unsigned way{0};
        PTAGEEntry entry;
        Addr nextPC{0};
        bool firstCorrect{false};   // the first block was strengthened
    };

    std::vector<PendingTrainPair> threadPending;

    // Train the first block of the looked-up entry, and return it as a
    // pending pair, invalid when it is not written
    PendingTrainPair trainFirstBlock(const LookupResult &lookup,
                                const BTBEntry &exit, Addr targetPC);

    // Train the second block of a pending pair
    void trainSecondBlock(const PendingTrainPair &pending, const BTBEntry &exit,
                          Addr targetPC);

    struct PairTAGEStats : public statistics::Group
    {
        statistics::Scalar lookups;
        statistics::Scalar lookupNoAnchor;
        statistics::Vector providerTable;
        statistics::Scalar s3TrainEvents;
        statistics::Scalar trainStrengthen;
        statistics::Scalar trainCorrect;
        statistics::Scalar trainAllocate;
        statistics::Scalar trainAllocateEvicted;
        statistics::Scalar trainNoTableFree;
        statistics::Scalar secondPhasePreds;
        statistics::Scalar secondPhaseProvided;
        statistics::Scalar trainSecondStrengthen;
        statistics::Scalar trainSecondReplace;
        statistics::Scalar trainSecondBroken;
        statistics::Vector providerConfidence;
        statistics::Scalar twoTakenCandidates;

        PairTAGEStats(statistics::Group *parent, unsigned numPredictors,
                      unsigned confidenceSaturate);
    } pairTageStats;
};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

#endif
