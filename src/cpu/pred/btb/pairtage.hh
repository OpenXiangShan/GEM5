#ifndef __CPU_PRED_BTB_PAIRTAGE_HH__
#define __CPU_PRED_BTB_PAIRTAGE_HH__

#include <array>
#include <memory>
#include <queue>
#include <vector>

#include "base/types.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/pred/btb/timed_base_pred.hh"

#ifdef UNIT_TEST
#include "cpu/pred/btb/test/test_dprintf.hh"

#else
#include "params/PairTAGE.hh"

#endif

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

#ifdef UNIT_TEST
namespace test
{
#endif

class PairTAGE : public TimedBaseBTBPredictor
{
    using bitset = boost::dynamic_bitset<>;

  public:
    static constexpr unsigned NumBlocksPerEntry = 2;

#ifdef UNIT_TEST
    PairTAGE(unsigned numPredictors = 4, unsigned numWays = 1, unsigned tableSize = 512);
#else
    typedef PairTAGEParams Params;
#endif

    struct PairBlockInfo
    {
        bool valid;
        bool taken;
        Addr branchPC;
        Addr targetPC;

        PairBlockInfo() : valid(false), taken(false), branchPC(0), targetPC(0) {}

        PairBlockInfo(bool taken, Addr branchPC, Addr targetPC)
            : valid(true), taken(taken), branchPC(branchPC), targetPC(targetPC)
        {
        }

        void clear()
        {
            valid = false;
            taken = false;
            branchPC = 0;
            targetPC = 0;
        }
    };

    struct TageEntry
    {
        bool valid;
        Addr tag;
        short counter;
        bool useful;
        std::array<PairBlockInfo, NumBlocksPerEntry> blocks;

        TageEntry() : valid(false), tag(0), counter(0), useful(false), blocks{} {}

        TageEntry(Addr tag, short counter, const PairBlockInfo &firstBlock,
                  const PairBlockInfo &secondBlock = PairBlockInfo())
            : valid(true), tag(tag), counter(counter), useful(false), blocks{firstBlock, secondBlock}
        {
        }

        bool taken() const { return counter >= 0; }

        const PairBlockInfo &firstBlock() const { return blocks[0]; }

        const PairBlockInfo &secondBlock() const { return blocks[1]; }

        bool hasSecondBlock() const { return blocks[1].valid; }

        void setBlock(unsigned blockIdx, const PairBlockInfo &block);
        void clearBlock(unsigned blockIdx);
        unsigned numValidBlocks() const;
    };

    struct TageMeta
    {
        std::vector<PathFoldedHist> tagFoldedHist;
        std::vector<PathFoldedHist> indexFoldedHist;
        std::vector<PathFoldedHist> altTagFoldedHist;
        bool aheadIndexFoldedHistValid;
        std::vector<PathFoldedHist> aheadIndexFoldedHist;
        bitset history;
        bool firstBlockValid;
        bool secondBlockValid;
        PairBlockInfo predictedFirstBlock;
        PairBlockInfo predictedSecondBlock;

        TageMeta() : aheadIndexFoldedHistValid(false), firstBlockValid(false), secondBlockValid(false) {}
    };

    struct TageTableInfo
    {
        bool found;
        TageEntry entry;
        unsigned table;
        Addr index;
        Addr tag;
        unsigned way;

        TageTableInfo() : found(false), table(0), index(0), tag(0), way(0) {}

        TageTableInfo(bool found, const TageEntry &entry, unsigned table, Addr index, Addr tag, unsigned way)
            : found(found), entry(entry), table(table), index(index), tag(tag), way(way)
        {
        }
    };

#ifndef UNIT_TEST
    PairTAGE(const Params &p);
#endif
    ~PairTAGE();

    void tickStart() override;
    void tick() override;
    void dryRunCycle(Addr startAddr) override;
    void putPCHistory(Addr startAddr, const bitset &history, std::vector<FullBTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta() override;
    void specUpdateHist(const bitset &history, FullBTBPrediction &pred) override;
    void recoverHist(const bitset &history, const FetchTarget &entry, int shamt, bool cond_taken) override;
    void update(const FetchTarget &entry) override;
    PairBlockInfo getSecondPredBlock() const;
    void trainFromActualPred(const FetchTarget &entry);

    unsigned getNumPredictors() const { return numPredictors; }
    unsigned getNumWays() const { return numWays; }
    unsigned getBlocksPerEntry() const { return NumBlocksPerEntry; }

    const std::vector<std::vector<std::vector<TageEntry>>> &getTageTable() const { return tageTable; }

    std::vector<std::vector<std::vector<TageEntry>>> &getTageTable() { return tageTable; }

  private:
    TageTableInfo lookupEntry(Addr startPC) const;
    TageTableInfo lookupEntry(Addr startPC, const TageMeta &predMeta) const;
    BTBEntry buildBTBEntry(const PairBlockInfo &block) const;
    void fillStagePrediction(const PairBlockInfo &block, FullBTBPrediction &pred) const;
    PairBlockInfo buildTrainingBlock(const FetchTarget &entry) const;
    Addr getTageIndex(Addr pc, int table, uint64_t foldedHist) const;
    Addr getTageIndex(Addr pc, int table) const;
    Addr getTageTag(Addr pc, int table, uint64_t foldedHist, uint64_t altFoldedHist, Addr position = 0) const;
    unsigned getBranchIndexInBlock(Addr branchPC, Addr startPC) const;
    void doUpdateHist(const bitset &history, bool taken, Addr pc, Addr target);

    const unsigned numPredictors;
    std::vector<unsigned> tableSizes;
    std::vector<unsigned> tableIndexBits;
    std::vector<unsigned> tableTagBits;
    std::vector<unsigned> tablePcShifts;
    std::vector<unsigned> histLengths;
    std::vector<PathFoldedHist> tagFoldedHist;
    std::vector<PathFoldedHist> altTagFoldedHist;
    std::vector<PathFoldedHist> indexFoldedHist;
    unsigned maxHistLen;
    const unsigned numWays;
    std::vector<std::vector<std::vector<TageEntry>>> tageTable;
    const unsigned maxBranchPositions;
    unsigned instShiftAmt{1};
    std::queue<std::vector<PathFoldedHist>> aheadIndexFoldedHist;
    std::shared_ptr<TageMeta> meta;
    PairBlockInfo secondPredBlock;
};

#ifdef UNIT_TEST
}  // namespace test
#endif

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

#endif
