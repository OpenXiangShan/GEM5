#ifndef __CPU_PRED_BTB_INTERFLUSH_HH__
#define __CPU_PRED_BTB_INTERFLUSH_HH__

#include <cstddef>

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

unsigned interflushBubblePenalty(std::size_t entryCount,
                                 unsigned entryLimit,
                                 unsigned penaltyCycles);

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_BTB_INTERFLUSH_HH__
