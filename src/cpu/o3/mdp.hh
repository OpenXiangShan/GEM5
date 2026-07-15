#ifndef __CPU_O3_MDP_HH__
#define __CPU_O3_MDP_HH__

namespace gem5
{
namespace o3
{

enum class MDPFeedbackSource
{
    NoForward,
    StoreQueue,
    StoreBuffer,
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MDP_HH__
