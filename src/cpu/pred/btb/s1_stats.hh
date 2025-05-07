#ifndef __CPU_PRED_BTB_S1_STATS_HH__
#define __CPU_PRED_BTB_S1_STATS_HH__

#include "base/stats/group.hh"
#include "cpu/pred/btb/btb.hh"
#include "cpu/pred/btb/btb_ubtb.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "params/S1StatsHelper.hh"
#include "stream_struct.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

class S1StatsHelper : public TimedBaseBTBPredictor
{
  private:
    // create
    typedef struct S1StatsMeta
    {
        UBTB::UBTBMeta ubtbMeta;
        DefaultBTB::BTBMeta abtbMeta;
        // the constructor
        S1StatsMeta(const UBTB::UBTBMeta& ubtbMeta, const DefaultBTB::BTBMeta& abtbMeta)
            : ubtbMeta(ubtbMeta), abtbMeta(abtbMeta) {}
        // default constructor
        S1StatsMeta() {}
    }S1StatsMeta;



    UBTB* ubtb;
    DefaultBTB* abtb;
    Addr currentStartAddr; // keeps track of the FB that's currently being commited,

  public:
    typedef S1StatsHelperParams Params;

    S1StatsHelper(const Params& p)
        : TimedBaseBTBPredictor(p),
          currentStartAddr(0),
          s1Stats(this),
          abtbMisPredDirectCause(this, "abtb_direct"),
          ubtbMisPredDirectCause(this, "ubtb_direct"),
          abtbMisPredIndirectCause(this, "abtb_indirect"),
          ubtbMisPredIndirectCause(this, "ubtb_indirect")
    { }

    void subscribe(UBTB* ubtb, DefaultBTB* abtb) {
        this->ubtb = ubtb;
        this->abtb = abtb;
    }

    void tickStart() override { }

    void tick() override { }

    void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override {}

    std::shared_ptr<void> getPredictionMeta() override {
        S1StatsMeta meta(ubtb->meta, abtb->meta);
        return std::make_shared<S1StatsMeta>(meta);
    }

    void specUpdateHist(const boost::dynamic_bitset<> &history,
                       FullBTBPrediction &pred) override { }

    void recoverHist(const boost::dynamic_bitset<> &history,
        const FetchStream &entry, int shamt, bool cond_taken) override { }

    void update(const FetchStream &stream) override;



    void commitBranch(const FetchStream &stream, const DynInstPtr &inst) override { }

    void setTrace() override { }

  private:

          struct MispredictionDirectCause : public statistics::Group
    {
        statistics::Scalar misPredCount;

        // direct cause: mutually exclusive
        statistics::Scalar fallThrough;
        statistics::Scalar controlAddr;
        statistics::Scalar target;
        MispredictionDirectCause(statistics::Group* parent, const char* name);
    };


    struct MispredictionIndirectCause : public statistics::Group
    {
        // indirect cause: mutually exclusive also
        /* this set of statistics keeps track of the first branch that's
         * mispredicted, The hope is that this set of statistics can draw
         * a connection between Misprediction type and branch coverage
         * (which is recorded in the commitBranch() function of ubtb)
         */
         statistics::Scalar condTButPredMiss;
         statistics::Scalar condTButPredNT;
         statistics::Scalar condNTButPredT;
         statistics::Scalar uncondMiss;
         statistics::Scalar indirectTargetWrong;
         statistics::Scalar others;
         MispredictionIndirectCause(statistics::Group* parent, const char* name);
    };

    void countMispredictionCause(const FetchStream &stream, FullBTBPrediction &fullPred,
                                 MispredictionDirectCause &cause,
                                 MispredictionIndirectCause &indirectCause);
    void updateIndirectCauseForTakenBranch(const FetchStream &stream,
                                          const FullBTBPrediction &fullPred,
                                          MispredictionIndirectCause &indirectCause);
    void updateIndirectCauseForNotTakenBranch(FullBTBPrediction &fullPred,
                                       MispredictionIndirectCause &indirectCause);

    FullBTBPrediction generateFullPred(const std::vector<BTBEntry> &entries, Addr startPC, bool forceTaken = false);

    struct S1Stats : public statistics::Group
    {
        statistics::Scalar S1Predmiss;
        statistics::Scalar S1PredUseUBTB;
        statistics::Scalar S1PredUseABTB;

        S1Stats(statistics::Group *parent);
    } s1Stats;

    MispredictionDirectCause abtbMisPredDirectCause;
    MispredictionDirectCause ubtbMisPredDirectCause;

    MispredictionIndirectCause abtbMisPredIndirectCause;
    MispredictionIndirectCause ubtbMisPredIndirectCause;


};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_S1_STATS_HH__
