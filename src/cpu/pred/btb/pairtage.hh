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
  public:
    typedef PairTAGEParams Params;

    struct PTAGEEntryModelInfo
    {
        bool dummy;
    };

    struct PTAGEBlockInfo
    {
        Addr directionCounter;
        Addr branchPC;
        Addr targetPC;
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
    };
};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

#endif
