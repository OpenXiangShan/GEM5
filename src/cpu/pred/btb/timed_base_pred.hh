#ifndef __CPU_PRED_BTB_TIMED_BASE_PRED_HH__
#define __CPU_PRED_BTB_TIMED_BASE_PRED_HH__


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

    virtual void specUpdateGHist(const boost::dynamic_bitset<> &history,
                                FullBTBPrediction &pred,
                                const DirectionHistoryUpdate &update) {}
    virtual void specUpdatePHist(const boost::dynamic_bitset<> &history,
                                 FullBTBPrediction &pred,
                                 const PathHistoryUpdate &update) {}
    virtual void recoverHist(const boost::dynamic_bitset<> &history,
                             const FetchTarget &entry, int shamt,
                             bool cond_taken) {}
    virtual void recoverPHist(const boost::dynamic_bitset<> &history,
                              const FetchTarget &entry,
                              const PathHistoryUpdate &update) {}
    virtual PredictorUpdateProtocol updateProtocol() const
    {
        return PredictorUpdateProtocol::None;
    }
    virtual void updateWithBranchUpdateContext(
        const BranchUpdateContext &,
        const std::vector<ResolvedBranch> &,
        const std::shared_ptr<void> &,
        const boost::dynamic_bitset<> &)
    {
    }
    virtual unsigned getDelay() {return numDelay;}
    virtual bool getResolvedUpdate() {return resolvedUpdate;}
#ifdef UNIT_TEST
    void setResolvedUpdate(bool value) { resolvedUpdate = value; }
#endif
    // Two-phase resolved update: probe first, then apply
    virtual bool canResolveUpdate(Addr) { return true; }
    virtual void noteResolveUpdateAccepted(Addr) {}
#ifndef UNIT_TEST
    // do some statistics on a per-branch and per-predictor basis
    virtual void recordCommittedBranchStats(
        const ResolvedBranch &branch,
        const std::shared_ptr<void> &prediction_meta) {}
#endif

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

private:
    unsigned numDelay;
    bool resolvedUpdate;
    bool enabled;
};

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_BTB_TIMED_BASE_PRED_HH__
