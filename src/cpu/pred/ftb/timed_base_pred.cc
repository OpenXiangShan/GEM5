#include "cpu/pred/ftb/timed_base_pred.hh"

namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

TimedBaseFTBPredictor::TimedBaseFTBPredictor(const Params &p)
    : SimObject(p),
    numDelay(p.numDelay)
{
}

} // namespace ftb_pred

} // namespace branch_prediction

} // namespace gem5