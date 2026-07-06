#ifndef __CPU_PRED_BTB_TAGE_UB_HH__
#define __CPU_PRED_BTB_TAGE_UB_HH__

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "cpu/pred/btb/btb_tage.hh"

#ifndef UNIT_TEST
#include "params/BTBTAGEUpperBound.hh"

#endif

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

#ifdef UNIT_TEST
namespace test {
#endif

class BTBTAGEUpperBound : public BTBTAGE
{
    using bitset = boost::dynamic_bitset<>;

  public:
    static constexpr unsigned MaxSupportedHistBits = 512;
    static constexpr unsigned MaxHistoryWords = MaxSupportedHistBits / 64;
    static constexpr unsigned PathHistoryShift = 2;

    enum class HistorySource
    {
        Outcome,
        PathHash,
    };

    struct ExactHistoryKey
    {
        Addr branchPC = 0;
        std::array<uint64_t, MaxHistoryWords> words{};
        uint8_t activeWords = 0;

        bool
        operator==(const ExactHistoryKey &other) const
        {
            return branchPC == other.branchPC &&
                activeWords == other.activeWords &&
                words == other.words;
        }
    };

    struct ExactHistoryKeyHash
    {
        std::size_t
        operator()(const ExactHistoryKey &key) const;
    };

    struct ExactProviderHandle
    {
        bool found = false;
        unsigned table = 0;
        ExactHistoryKey key;
    };

    struct BranchPredictionMeta
    {
        ExactProviderHandle main;
        ExactProviderHandle alt;
    };

    struct UpperBoundMeta : public BTBTAGE::TageMeta
    {
        std::unordered_map<Addr, BranchPredictionMeta> branchMeta;
        std::array<uint64_t, MaxHistoryWords> historyWords{};
    };

#ifdef UNIT_TEST
    BTBTAGEUpperBound(unsigned numPredictors = 4,
                      unsigned tableSize = 1024,
                      unsigned numBanks = 4,
                      HistorySource source = HistorySource::Outcome);
#else
    typedef BTBTAGEUpperBoundParams Params;
    BTBTAGEUpperBound(const Params &p);
#endif

    void putPCHistory(Addr startAddr,
                      const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;

    void specUpdatePHist(const boost::dynamic_bitset<> &history,
                         FullBTBPrediction &pred,
                         const PathHistoryUpdate &update) override;
    void recoverHist(const boost::dynamic_bitset<> &history,
                     const FetchTarget &entry,
                     int shamt,
                     bool cond_taken) override;
    void recoverPHist(const boost::dynamic_bitset<> &history,
                      const FetchTarget &entry,
                      const PathHistoryUpdate &update) override;
    PredictorUpdateProtocol updateProtocol() const override
    {
        return PredictorUpdateProtocol::BranchContext;
    }
    void updateWithBranchUpdateContext(
        const BranchUpdateContext &ctx,
        const std::vector<ResolvedBranch> &update_branches,
        const std::shared_ptr<void> &prediction_meta) override;
    void checkFoldedHist(const bitset &history, const char *when) override;

#ifdef UNIT_TEST
    ExactHistoryKey makeExactKey(Addr branchPC, const bitset &history,
                                 unsigned table) const;
    bool insertExactEntry(unsigned table, Addr branchPC, const bitset &history,
                          short counter, bool useful = false);
    bool hasExactEntry(unsigned table, Addr branchPC,
                       const bitset &history) const;
#endif

  private:
    using ExactTable = std::unordered_map<ExactHistoryKey, TageEntry,
                                          ExactHistoryKeyHash>;

#ifdef UNIT_TEST
    struct UpperBoundStats
    {
        explicit UpperBoundStats(unsigned numPredictors)
          : liveContextsPerTable(numPredictors, 0)
        {}

        std::vector<uint64_t> liveContextsPerTable;
        uint64_t totalContexts = 0;
        uint64_t updateAllocInsert = 0;
        uint64_t updateAllocAllTablesHit = 0;
    };
#else
    struct UpperBoundStats : public statistics::Group
    {
        statistics::Vector liveContextsPerTable;
        statistics::Scalar totalContexts;
        statistics::Scalar updateAllocInsert;
        statistics::Scalar updateAllocAllTablesHit;

        UpperBoundStats(statistics::Group *parent, unsigned numPredictors);
    };
#endif

    void initUpperBoundState();
    void updatePathHistory(bitset &history, bool taken, Addr pc,
                           Addr target) const;
    const bitset &selectHistory(const bitset &outcomeHistory) const;
    void captureHistoryWords(const bitset &history,
                             std::array<uint64_t, MaxHistoryWords> &words) const;
    ExactHistoryKey buildKey(Addr branchPC,
                             const std::array<uint64_t, MaxHistoryWords> &words,
                             unsigned histLen) const;
    TageTableInfo makeTableInfo(bool found, const TageEntry *entry,
                                unsigned table) const;
    TagePrediction lookupExactPrediction(Addr branchPC, bool baseTaken,
                                         const std::array<uint64_t,
                                             MaxHistoryWords> &historyWords,
                                         BranchPredictionMeta *metaOut) const;
    bool updatePredictorStateAndCheckAllocation(Addr branchPC,
                                                bool baseTaken,
                                                bool actualTaken,
                                                const TagePrediction &pred,
                                                const BranchPredictionMeta &meta,
                                                bool actualMispred);
    bool allocateExactEntry(Addr branchPC, bool actualTaken,
                            unsigned startTable,
                            const std::array<uint64_t, MaxHistoryWords> &historyWords,
                            uint64_t &allocatedTable);
    void updateWithBranches(const std::vector<ResolvedBranch> &update_branches,
                            const BranchUpdateContext &ctx,
                            const UpperBoundMeta &predMeta);
    void refreshContextStats(unsigned table);
    void notePredictionResult(Addr branchPC,
                              const TagePrediction &pred,
                              std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs,
                              CondTakens &results) const;

    std::vector<ExactTable> exactTables;
    mutable std::vector<uint64_t> historyBlocksScratch;
    std::shared_ptr<UpperBoundMeta> ubMeta;
    UpperBoundStats ubStats;
    HistorySource historySource;
    bitset exactPathHistory;
};

#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_BTB_TAGE_UB_HH__
