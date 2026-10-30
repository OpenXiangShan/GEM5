#ifndef __CPU_PRED_BTB_TIMED_BASE_PRED_HH__
#define __CPU_PRED_BTB_TIMED_BASE_PRED_HH__


#include <functional>
#include <utility>

#include <boost/dynamic_bitset.hpp>

// Conditional includes based on build mode
#ifdef UNIT_TEST
    #include "base/types.hh"
    #include "cpu/pred/btb/common.hh"
#else
    #include "base/statistics.hh"
    #include "base/types.hh"
    #include "cpu/inst_seq.hh"
    #include "cpu/pred/btb/common.hh"
    #include "enums/TrainingStage.hh"
    #include "sim/sim_object.hh"
    #include "params/TimedBaseBTBPredictor.hh"
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

enum class PredictorTrainingStage
{
    Commit,
    Resolve,
};

#ifdef UNIT_TEST
class TimedBaseBTBPredictor
#else
class TimedBaseBTBPredictor: public SimObject
#endif
{
    public:

#ifdef UNIT_TEST
    TimedBaseBTBPredictor();
    void setNumDelay(unsigned delay) { numDelay = delay; }
    void setTrainingStage(PredictorTrainingStage stage)
    {
        trainingStage = stage;
    }
    void setSmtTidPartitioned(bool partitioned)
    {
        smtTidPartitioned = partitioned;
    }
#else
    typedef TimedBaseBTBPredictorParams Params;

    TimedBaseBTBPredictor(const Params &params);
#endif

    virtual void tickStart() {}
    virtual void tick() {}
    virtual void dryRunCycle(Addr startAddr) {};
    // make predictions, record in stage preds
    virtual void putPCHistory(Addr startAddr,
                              const boost::dynamic_bitset<> &history,
                              std::vector<FullBTBPrediction> &stagePreds) {}

    virtual std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0)
    {
        return nullptr;
    }
    virtual void refreshPredictionMeta(Addr startAddr,
                                       const boost::dynamic_bitset<> &history,
                                       FullBTBPrediction &pred) {}

    virtual void specUpdateGHist(const boost::dynamic_bitset<> &history,
                                FullBTBPrediction &pred,
                                const DirectionHistoryUpdate &update) {}
    virtual void specUpdatePHist(const boost::dynamic_bitset<> &history,
                                 FullBTBPrediction &pred,
                                 const PathHistoryUpdate &update) {}
    virtual void recoverHist(const boost::dynamic_bitset<> &history,
                             const HistoryRecoveryContext &context,
                             const DirectionHistoryUpdate &update) {}
    virtual void recoverPHist(const boost::dynamic_bitset<> &history,
                              const HistoryRecoveryContext &context,
                              const PathHistoryUpdate &update) {}
    virtual void update(const PredictionUpdateContext &context,
                        const PreparedUpdate &update) {}
    virtual unsigned getDelay() {return numDelay;}
    bool trainsAtResolve() const
    {
        return trainingStage == PredictorTrainingStage::Resolve;
    }
    bool trainsAtCommit() const
    {
        return trainingStage == PredictorTrainingStage::Commit;
    }
    // Two-phase resolved update: probe first, then apply
    virtual bool canResolveUpdate(const PredictionUpdateContext &context,
                                  const PreparedUpdate &update)
    {
        return true;
    }
    virtual void doResolveUpdate(const PredictionUpdateContext &context,
                                 const PreparedUpdate &update)
    {
        this->update(context, update);
    }
    // do some statistics on a per-branch and per-predictor basis
    virtual void commitBranch(const PredictionUpdateContext &context,
                              const BranchOutcome &outcome) {}

    int componentIdx{0};
    unsigned aheadPipelinedStages{0};
    int getComponentIdx() { return componentIdx; }
    void setComponentIdx(int idx) { componentIdx = idx; }

    uint64_t blockSize;
    unsigned predictWidth;

#ifndef UNIT_TEST
    bool hasDB {false};
    std::string dbName;
    bool enableDB {false};
    void setDB(DataBase *db) {
        _db = db;
    }
    DataBase *_db;
#endif
    virtual void setTrace() {}

    // Check if this component is enabled
    bool isEnabled() const { return enabled; }

    void setPartitionBorrowing(std::function<bool()> query)
    {
        partitionBorrowing = std::move(query);
    }

    bool borrowsPartitions() const
    {
        return smtTidPartitioned && partitionBorrowing && partitionBorrowing();
    }

    // Lookup may use the extra physical bank bit, selected by PC/ASID. Keep
    // partitionIndexBits() unchanged for folded histories and tag encoding;
    // no history resize, reset, or tag reinterpretation occurs on wake.
    unsigned lookupIndexBits(unsigned fullIndexBits) const
    {
        return borrowsPartitions() ? fullIndexBits :
            partitionIndexBits(fullIndexBits);
    }

    /**
     * Map to a private half while both threads participate, or to the full
     * table while the sole resource-active thread borrows the other half.
     * Physical storage and tag/fold formats stay fixed across transitions.
     */
    unsigned partitionIndex(unsigned index, unsigned totalEntries,
                            ThreadID tid) const
    {
        if (!smtTidPartitioned || borrowsPartitions()) {
            return index % totalEntries;
        }
        assert(totalEntries >= 2 && totalEntries % 2 == 0);
        assert(tid < 2);
        const unsigned entriesPerThread = totalEntries / 2;
        return tid * entriesPerThread + index % entriesPerThread;
    }

    unsigned partitionIndexBits(unsigned fullIndexBits) const
    {
        assert(!smtTidPartitioned || fullIndexBits > 0);
        return fullIndexBits - (smtTidPartitioned ? 1 : 0);
    }

    unsigned partitionBegin(unsigned totalEntries, ThreadID tid) const
    {
        if (!smtTidPartitioned || borrowsPartitions()) {
            return 0;
        }
        assert(totalEntries >= 2 && totalEntries % 2 == 0);
        assert(tid < 2);
        return tid * (totalEntries / 2);
    }

    unsigned partitionEnd(unsigned totalEntries, ThreadID tid) const
    {
        return smtTidPartitioned && !borrowsPartitions() ?
            partitionBegin(totalEntries, tid) + totalEntries / 2 :
            totalEntries;
    }

    bool usesTidPartitionedStorage() const { return smtTidPartitioned; }

private:
    unsigned numDelay;
    PredictorTrainingStage trainingStage;
    bool enabled;
    bool smtTidPartitioned;
    // Live query avoids a second allocation cache and is bounded by the
    // configured hardware thread count. An empty query keeps static behavior.
    std::function<bool()> partitionBorrowing;
};

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_BTB_TIMED_BASE_PRED_HH__
