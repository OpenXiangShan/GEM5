#include "cpu/pred/btb/interflush.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

unsigned
interflushBubblePenalty(std::size_t entryCount,
                        unsigned entryLimit,
                        unsigned penaltyCycles)
{
    return entryCount > entryLimit ? penaltyCycles : 0;
}

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
