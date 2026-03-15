#ifndef __CPU_PRED_BTB_TIMED_BASE_PRED_HH__
#define __CPU_PRED_BTB_TIMED_BASE_PRED_HH__

#include <boost/dynamic_bitset.hpp>

#ifdef UNIT_TEST
#include "base/types.hh"
#include "cpu/pred/btb/common.hh"

#else
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/pred/btb/common.hh"
#include "params/TimedBaseBTBPredictor.hh"
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
namespace test
{
#endif

#ifndef UNIT_TEST
using DynInstPtr = o3::DynInstPtr;
#endif

#ifdef UNIT_TEST
class TimedBaseBTBPredictor
#else
class TimedBaseBTBPredictor : public SimObject
#endif
{
  public:
#ifdef UNIT_TEST
    TimedBaseBTBPredictor();
    void setNumDelay(unsigned delay) { numDelay = delay; }
    void setBlock1Participate(bool enable) { block1Participate = enable; }
#else
    typedef TimedBaseBTBPredictorParams Params;

    TimedBaseBTBPredictor(const Params &params);
#endif

    virtual void tickStart() {}
    virtual void tick() {}
    virtual void dryRunCycle(Addr startAddr) {};
    // make predictions, record in stage preds
    virtual void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                              std::vector<FullBTBPrediction> &stagePreds)
    {
    }
    virtual void putPCHistoryForBlock1(Addr startAddr, const boost::dynamic_bitset<> &history,
                                       const boost::dynamic_bitset<> &phistory,
                                       const boost::dynamic_bitset<> &bwhistory,
                                       const std::vector<boost::dynamic_bitset<>> &lhistory,
                                       std::vector<FullBTBPrediction> &stagePreds, const FullBTBPrediction &lowerPred)
    {
        (void)phistory;
        (void)bwhistory;
        (void)lhistory;
        (void)lowerPred;
        putPCHistory(startAddr, history, stagePreds);
    }

    virtual std::shared_ptr<void> getPredictionMeta() { return nullptr; }

    virtual void specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) {}
    virtual void specUpdatePHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) {}
    virtual void specUpdateBwHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) {}
    virtual void specUpdateIHist(FullBTBPrediction &pred) {}
    virtual void specUpdateLHist(const std::vector<boost::dynamic_bitset<>> &history, FullBTBPrediction &pred) {}
    virtual void recoverHist(const boost::dynamic_bitset<> &history, const FetchTarget &entry, int shamt,
                             bool cond_taken)
    {
    }
    virtual void recoverPHist(const boost::dynamic_bitset<> &history, const FetchTarget &entry, int shamt,
                              bool cond_taken)
    {
    }
    virtual void recoverBwHist(const boost::dynamic_bitset<> &history, const FetchTarget &entry, int shamt,
                               bool cond_taken)
    {
    }
    virtual void recoverIHist(const FetchTarget &entry, int shamt, bool cond_taken) {}
    virtual void recoverLHist(const std::vector<boost::dynamic_bitset<>> &history, const FetchTarget &entry, int shamt,
                              bool cond_taken)
    {
    }
    virtual void update(const FetchTarget &entry) {}
    virtual unsigned getDelay() { return numDelay; }
    virtual bool getResolvedUpdate() { return resolvedUpdate; }
    // Two-phase resolved update: probe first, then apply
    virtual bool canResolveUpdate(const FetchTarget &entry) { return true; }
    virtual void doResolveUpdate(const FetchTarget &entry) { update(entry); }
#ifndef UNIT_TEST
    // do some statistics on a per-branch and per-predictor basis
    virtual void commitBranch(const FetchTarget &entry, const DynInstPtr &inst) {}
#endif

    int componentIdx{0};
    unsigned aheadPipelinedStages{0};
    bool needMoreHistories{false};
    int getComponentIdx() { return componentIdx; }
    void setComponentIdx(int idx) { componentIdx = idx; }

    uint64_t blockSize;
    unsigned predictWidth;

#ifndef UNIT_TEST
    bool hasDB{false};
    std::string dbName;
    bool enableDB{false};
    void setDB(DataBase *db) { _db = db; }
    DataBase *_db;
#endif
    virtual void setTrace() {}

    // Check if this component is enabled
    bool isEnabled() const { return enabled; }
    bool participatesInBlock1() const { return block1Participate; }

  private:
    unsigned numDelay;
    bool resolvedUpdate;
    bool enabled;
    bool block1Participate;
};

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
}  // namespace test
#endif

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

#endif  // __CPU_PRED_BTB_TIMED_BASE_PRED_HH__
