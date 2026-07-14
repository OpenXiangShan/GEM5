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

    struct PairBlockInfo
    {
        bool valid;
        bool taken;
        bool fallThrough;
        Addr branchPC;
        Addr targetPC;
        bool isCond;
        bool isDirect;
        bool isIndirect;
        bool isCall;
        bool isReturn;
        uint8_t size;

        PairBlockInfo()
            : valid(false), taken(false), fallThrough(false),
              branchPC(0), targetPC(0),
              isCond(false), isDirect(false), isIndirect(false),
              isCall(false), isReturn(false), size(0)
        {
        }

        PairBlockInfo(bool taken, Addr branchPC, Addr targetPC,
                      bool fallThrough = false)
            : valid(true), taken(taken), fallThrough(fallThrough),
              branchPC(branchPC), targetPC(targetPC),
              isCond(!fallThrough), isDirect(!fallThrough), isIndirect(false),
              isCall(false), isReturn(false), size(fallThrough ? 0 : 4)
        {
        }

        PairBlockInfo(bool taken, Addr branchPC, Addr targetPC,
                      bool isCond, bool isDirect, bool isIndirect,
                      bool isCall, bool isReturn, uint8_t size,
                      bool fallThrough = false)
            : valid(true), taken(taken), fallThrough(fallThrough),
              branchPC(branchPC), targetPC(targetPC),
              isCond(isCond), isDirect(isDirect), isIndirect(isIndirect),
              isCall(isCall), isReturn(isReturn), size(size)
        {
        }

        void clear()
        {
            valid = false;
            taken = false;
            fallThrough = false;
            branchPC = 0;
            targetPC = 0;
            isCond = false;
            isDirect = false;
            isIndirect = false;
            isCall = false;
            isReturn = false;
            size = 0;
        }

        bool isFallThrough() const { return valid && fallThrough; }
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

    void putPCHistory(Addr startAddr, const bitset &history, std::vector<FullBTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;
    void refreshPredictionMeta(Addr startAddr,
                               const boost::dynamic_bitset<> &history,
                               FullBTBPrediction &pred) override;
    void specUpdatePHist(const bitset &history,
                         FullBTBPrediction &pred,
                         const PathHistoryUpdate &update) override;
    void recoverPHist(const bitset &history,
                      const FetchTarget &entry,
                      const PathHistoryUpdate &update) override;
    PairBlockInfo getSecondPredBlock() const;
    void setPredictionPhase(PairPhase phase);
    void trainFromS3Pred(const FetchTarget &entry,
                         const FullBTBPrediction *secondPred = nullptr);
    bool secondBlockEnabled() const { return enableSecondBlock; }
    bool phaseEnabled(PairPhase phase) const
    {
        return allowOddPhase || phase == PairPhase::Even;
    }

  private:
    ProviderInfo lookupProviders(Addr startPC) const;
    ProviderInfo lookupProviders(Addr startPC, const TageMeta &predMeta) const;
    TageTableInfo lookupEntry(Addr startPC) const;
    BTBEntry buildBTBEntry(const PairBlockInfo &block) const;
    void fillStagePrediction(const PairBlockInfo &block, FullBTBPrediction &pred) const;
    PairBlockInfo buildTrainingBlock(const FetchTarget &entry) const;
    PairBlockInfo buildTrainingBlock(const FullBTBPrediction &pred) const;
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
};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

#endif
