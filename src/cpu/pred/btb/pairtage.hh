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
        bool hasBranch;
        bool taken;
        Addr branchPC;
        Addr targetPC;
        bool isCond;
        bool isDirect;
        bool isIndirect;
        bool isCall;
        bool isReturn;
        uint8_t size;

        PairBlockInfo()
            : valid(false), hasBranch(false), taken(false),
              branchPC(0), targetPC(0),
              isCond(false), isDirect(false), isIndirect(false),
              isCall(false), isReturn(false), size(0)
        {
        }

        static PairBlockInfo
        makeBranchless(Addr startPC, Addr targetPC)
        {
            PairBlockInfo block;
            block.valid = true;
            block.branchPC = startPC;
            block.targetPC = targetPC;
            return block;
        }

        PairBlockInfo(bool taken, Addr branchPC, Addr targetPC,
                      bool isCond, bool isDirect, bool isIndirect,
                      bool isCall, bool isReturn, uint8_t size)
            : valid(true), hasBranch(true), taken(taken),
              branchPC(branchPC), targetPC(targetPC),
              isCond(isCond), isDirect(isDirect), isIndirect(isIndirect),
              isCall(isCall), isReturn(isReturn), size(size)
        {
        }

        void clear()
        {
            valid = false;
            hasBranch = false;
            taken = false;
            branchPC = 0;
            targetPC = 0;
            isCond = false;
            isDirect = false;
            isIndirect = false;
            isCall = false;
            isReturn = false;
            size = 0;
        }

        bool hasConsistentState() const
        {
            return (!hasBranch || valid) && (!taken || hasBranch);
        }

        bool isBranchlessFallthrough() const { return valid && !hasBranch; }

        BTBEntry buildBTBEntry(int componentIdx) const {
            BTBEntry entry;
            entry.valid = valid && hasBranch;
            entry.pc = branchPC;
            entry.target = targetPC;
            entry.size = hasBranch ? size : 0;
            entry.isCond = isCond;
            entry.isDirect = isDirect;
            entry.isIndirect = isIndirect;
            entry.isCall = isCall;
            entry.isReturn = isReturn;
            entry.alwaysTaken = entry.valid && !isCond;
            entry.ctr = taken ? 0 : -1;
            entry.source = componentIdx;

            return entry;
        };
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

    struct TrainPacket
    {
        enum class BranchFlag : uint8_t
        {
            Conditional = 1U << 0,
            Direct = 1U << 1,
            Indirect = 1U << 2,
            Call = 1U << 3,
            Return = 1U << 4
        };

        Addr startPC{0};
        PairPhase phase{PairPhase::Even};
        std::shared_ptr<const TageMeta> meta;
        bool valid{false};
        bool hasBranch{false};
        bool taken{false};
        Addr branchPC{0};
        Addr targetPC{0};
        uint8_t branchFlags{0};
        uint8_t size{0};

        static constexpr uint8_t branchFlag(BranchFlag flag) { return static_cast<uint8_t>(flag); }

        bool hasBranchFlag(BranchFlag flag) const { return branchFlags & branchFlag(flag); }

        bool hasConsistentState() const
        {
            return (!hasBranch || valid) && (!taken || hasBranch);
        }

        bool isBranchlessFallthrough() const { return valid && !hasBranch; }

        void setBranchFlags(const BranchInfo &branch)
        {
            uint8_t flags = 0;
            if (branch.isCond) {
                flags |= PairTAGE::TrainPacket::branchFlag(BranchFlag::Conditional);
            }
            if (branch.isDirect) {
                flags |= PairTAGE::TrainPacket::branchFlag(BranchFlag::Direct);
            }
            if (branch.isIndirect) {
                flags |= PairTAGE::TrainPacket::branchFlag(BranchFlag::Indirect);
            }
            if (branch.isCall) {
                flags |= PairTAGE::TrainPacket::branchFlag(BranchFlag::Call);
            }
            if (branch.isReturn) {
                flags |= PairTAGE::TrainPacket::branchFlag(BranchFlag::Return);
            }

            branchFlags = flags;
        }
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
    bool secondBlockMatches(const TrainPacket &packet) const;
    void setPredictionPhase(PairPhase phase);
    TrainPacket buildTrainPacketFromPredForFirstBlock(FullBTBPrediction &finalPred) const;
    TrainPacket buildTwoTakenTrainPacket(Addr startPC, PairPhase phase, const std::vector<BTBEntry> &btbEntries,
                                         const CondTakens &condTakens) const;
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
    void fillStagePrediction(const PairBlockInfo &block, FullBTBPrediction &pred) const;
    PairBlockInfo buildTrainingBlock(const TrainPacket &packet) const;
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
