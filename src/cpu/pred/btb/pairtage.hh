#ifndef __CPU_PRED_BTB_PAIRTAGE_HH__
#define __CPU_PRED_BTB_PAIRTAGE_HH__

#include <array>
#include <memory>
#include <queue>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
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

  public:
    typedef PairTAGEParams Params;

    struct PTAGEEntryModelInfo
    {
        bool dummy;

        PTAGEEntryModelInfo() : dummy(0) {}
    };

    struct PTAGEBlockInfo
    {
        Addr directionCounter;
        Addr branchPC;
        Addr targetPC;

        PTAGEBlockInfo(): directionCounter(0), branchPC(0), targetPC(0) {}
    };

    struct PTAGEEntry
    {
        bool valid;
        bool b2Valid;
        Addr tag;
        bool useful;

        PTAGEBlockInfo block;
        PTAGEBlockInfo block2;

        PTAGEEntryModelInfo modelInfo;

        PTAGEEntry() : valid(false), b2Valid(false), tag(0), useful(false) {}
    };

    PairTAGE(const Params &p);

    // The predictor is wired into the BPU but does not predict yet: it
    // leaves every stage prediction untouched.
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

    // Number of ways per set; the RTL tables are direct mapped
    const unsigned numWays;

    // ========== Bank Configuration ==========
    // Each table is split into banks selected by low PC bits, A bank
    // owns a contiguous range of indices, so an index is laid out
    // as {bank, set}.
    const unsigned numBanks;         // Number of banks (e.g., 4)
    const unsigned bankIdWidth;      // log2(numBanks), computed in constructor

    // The PairTAGE prediction tables (table x index x way)
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

    // ========== Table Access ==========
    // Plain reads and writes of one set. Which table, set and way to use,
    // and what to write, is left to the caller.

    // Result of reading one set of a table
    struct LookupResult
    {
        bool hit{false};    // a valid entry in the set matches the tag
        unsigned way{0};    // way of the matching entry, valid on hit
        PTAGEEntry entry;   // copy of the matching entry, valid on hit
    };

    // Read set index of table t and match its ways against tag
    LookupResult lookup(unsigned t, Addr index, Addr tag) const;

    // Overwrite one way of set index of table t
    void writeEntry(unsigned t, Addr index, unsigned way,
                    const PTAGEEntry &entry);

    struct PairTAGEStats : public statistics::Group
    {
        statistics::Scalar lookups;
        statistics::Scalar s3TrainEvents;

        PairTAGEStats(statistics::Group *parent);
    } pairTageStats;
};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

#endif
