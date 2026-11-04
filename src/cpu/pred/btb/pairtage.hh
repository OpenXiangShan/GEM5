#ifndef __CPU_PRED_BTB_PAIRTAGE_HH__
#define __CPU_PRED_BTB_PAIRTAGE_HH__

#include <array>
#include <memory>
#include <queue>
#include <vector>

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
    static constexpr unsigned NumBlocksPerEntry = 2;

    typedef PairTAGEParams Params;

    struct PairBlockInfo : BranchInfo
    {
        bool valid = false;
        bool hasBranch = false;
        bool taken = false;

        PairBlockInfo() = default;
        void clear() { *this = PairBlockInfo{}; }

        bool hasConsistentState() const
        {
            return (!hasBranch || valid) && (!taken || hasBranch);
        }

        bool isBranchlessFallthrough() const { return valid && !hasBranch; }

        BranchPrediction buildBTBEntry(int componentIdx) const
        {
            BranchPrediction entry(*this);
            entry.valid = valid && hasBranch;
            entry.size = hasBranch ? size : 0;
            entry.taken = taken;
            entry.baseTaken = taken;
            entry.source = componentIdx;
            return entry;
        }
    };

    struct PairTAGEEntry
    {
        static constexpr uint8_t MaxIdentityConfidence = 3;
        static constexpr uint8_t InitialIdentityConfidence = 1;

        bool valid;
        Addr tag;
        short counter;
        bool useful;
        uint8_t identityConfidence;
        std::array<PairBlockInfo, NumBlocksPerEntry> blocks;

        PairTAGEEntry()
            : valid(false), tag(0), counter(0), useful(false),
              identityConfidence(0), blocks{}
        {
        }

        const PairBlockInfo &firstBlock() const { return blocks[0]; }

        const PairBlockInfo &secondBlock() const { return blocks[1]; }

        bool hasSecondBlock() const { return blocks[1].valid; }

        void setBlock(unsigned blockIdx, const PairBlockInfo &block);
        void clearBlock(unsigned blockIdx);
        void strengthenIdentity();
        bool weakenIdentity();
    };

    struct TageMeta
    {
        std::vector<PathFoldedHist> tagFoldedHist;
        std::vector<PathFoldedHist> indexFoldedHist;
        std::vector<PathFoldedHist> altTagFoldedHist;
        bool aheadIndexFoldedHistValid{false};
        std::vector<PathFoldedHist> aheadIndexFoldedHist;
        PairBlockInfo predictedFirstBlock;
        PairBlockInfo predictedSecondBlock;
    };

    struct TrainPacket : PairBlockInfo
    {
        Addr startPC{0};
        PairPhase phase{PairPhase::Even};
        std::shared_ptr<const TageMeta> meta;
    };

    struct TageTableInfo
    {
        bool found;
        PairTAGEEntry entry;
        unsigned table;
        Addr index;
        Addr tag;
        unsigned way;

        TageTableInfo() : found(false), table(0), index(0), tag(0), way(0) {}

        TageTableInfo(bool found, const PairTAGEEntry &entry, unsigned table, Addr index, Addr tag, unsigned way)
            : found(found), entry(entry), table(table), index(index), tag(tag), way(way)
        {
        }
    };

    struct ProviderInfo
    {
        TageTableInfo main;
        TageTableInfo alt;
    };

    PairTAGE(const Params &p);

    PairBlockInfo predictBlock(Addr startAddr, const bitset &history);

    std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;
    void refreshPredictionMeta(Addr startAddr,
                               const boost::dynamic_bitset<> &history,
                               FullBTBPrediction &pred) override;
    void specUpdatePHist(const bitset &history,
                         FullBTBPrediction &pred,
                         const PathHistoryUpdate &update) override;
    void recoverPHist(const bitset &history,
                      const HistoryRecoveryContext &context,
                      const PathHistoryUpdate &update) override;
    PairBlockInfo getSecondPredBlock() const;
    bool secondBlockMatches(const TrainPacket &packet) const;
    void setPredictionPhase(PairPhase phase);
    TrainPacket buildTrainPacketFromPredForFirstBlock(FullBTBPrediction &finalPred) const;
    TrainPacket buildTwoTakenTrainPacket(
        Addr startPC, PairPhase phase,
        const std::vector<BranchPrediction> &btbEntries) const;
    void trainFromS3Pred(
        const TrainPacket &finalTrainPacket,
        const TrainPacket *twoTakenTrainPacket = nullptr);
    void recordTwoTakenBlockEnqueued();
    bool secondBlockEnabled() const { return enableSecondBlock; }
    bool phaseEnabled(PairPhase phase) const
    {
        return allowOddPhase || phase == PairPhase::Even;
    }

  private:
    enum TrainingType : unsigned
    {
        Fallthrough,
        HasBranchNotTaken,
        IsCond,
        IsDirect,
        IsIndirect,
        IsCall,
        IsReturn,
        NumTrainingTypes
    };

    struct PairTAGEStats : public statistics::Group
    {
        statistics::Scalar firstBlockLookups;
        statistics::Scalar firstBlockHits;
        statistics::Formula firstBlockHitRate;
        statistics::Scalar firstBlockAccuracySamples;
        statistics::Scalar firstBlockCorrect;
        statistics::Formula firstBlockAccuracy;

        statistics::Scalar secondBlockProduced;
        statistics::Formula secondBlockProductionRate;
        statistics::Scalar secondBlockAccuracySamples;
        statistics::Scalar secondBlockCorrect;
        statistics::Formula secondBlockAccuracy;

        statistics::Scalar twoTakenBlocksEnqueued;
        statistics::Formula twoTakenEnqueueRate;

        statistics::Vector firstBlockTrainTypes;
        statistics::Vector secondBlockTrainTypes;

        statistics::Scalar allocations;
        statistics::Scalar evictions;

        PairTAGEStats(statistics::Group *parent);
    };

    ProviderInfo lookupProviders(Addr startPC) const;
    ProviderInfo lookupProviders(Addr startPC, const TageMeta &predMeta) const;
    TageTableInfo lookupEntry(Addr startPC) const;
    void recordTrainingTypes(statistics::Vector &typeStats,
                             const PairBlockInfo &block);
    bool blocksMatch(const PairBlockInfo &lhs, const PairBlockInfo &rhs) const;
    bool blockIdentityMatches(const PairBlockInfo &lhs,
                              const PairBlockInfo &rhs) const;
    bool entryMatchesTraining(const PairTAGEEntry &entry, const PairBlockInfo &firstBlock,
                              const PairBlockInfo &secondBlock) const;
    void updateCounter(bool taken, unsigned width, short &counter);
    bool satIncrement(int max, short &counter);
    bool satDecrement(int min, short &counter);
    bool betterProviderCandidate(const TageTableInfo &candidate,
                                 const TageTableInfo &current) const;
    int selectMatchingReplacementWay(
        const std::vector<PairTAGEEntry> &set, Addr tag,
        const PairBlockInfo &trainedBlock,
        const PairBlockInfo &trainedSecondBlock) const;
    int selectAllocationWay(const std::vector<PairTAGEEntry> &set) const;
    void resetUsefulBits();
    bool allocateEntries(Addr startPC, const TageMeta &predMeta,
                         const PairBlockInfo &trainedBlock,
                         const PairBlockInfo &trainedSecondBlock,
                         unsigned startTable);
    Addr getTageIndex(Addr pc, int table, uint64_t foldedHist) const;
    Addr getTageTag(Addr pc, int table, uint64_t foldedHist, uint64_t altFoldedHist) const;
    Addr getFallThrough(Addr startPC) const;
    bool isBranchInFirstBlock(Addr branchPC, Addr startPC) const;
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
    LFSR64 allocLFSR;
    const unsigned numTablesToAlloc;
    const unsigned numWays;
    std::vector<std::vector<std::vector<PairTAGEEntry>>> tageTable;
    int usefulResetCnt{0};
    unsigned instShiftAmt{1};
    std::queue<std::vector<PathFoldedHist>> aheadIndexFoldedHist;
    std::shared_ptr<TageMeta> meta;
    PairBlockInfo secondPredBlock;
    PairPhase predictionPhase{PairPhase::Even};
    const bool enableSecondBlock;
    const bool allowOddPhase;
    const bool trainStandaloneFallThrough;
    PairTAGEStats pairTageStats;
};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

#endif
