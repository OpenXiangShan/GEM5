#include "cpu/pred/btb/timed_base_pred.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

#ifdef UNIT_TEST
namespace test
{
TimedBaseBTBPredictor::TimedBaseBTBPredictor()
    : blockSize(32),
      predictWidth(64),
      numDelay(0),
      resolvedUpdate(false),
      enabled(true),
      smtTidPartitioned(false)
{}
}  // namespace test
#else
TimedBaseBTBPredictor::TimedBaseBTBPredictor(const Params &p)
    : SimObject(p),
      blockSize(p.blockSize),
      predictWidth(p.predictWidth),
      numDelay(p.numDelay),
      resolvedUpdate(p.resolvedUpdate),
      enabled(p.enabled),
      smtTidPartitioned(p.smtTidPartitioned)
{
}
#endif

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
