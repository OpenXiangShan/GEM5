#ifndef __MEM_CACHE_XS_L2_TASK_SOURCE_HH__
#define __MEM_CACHE_XS_L2_TASK_SOURCE_HH__

namespace gem5
{

// Indicate the source of a task processed by the L2 pipeline.
enum TaskSource
{
    NoWhere,
    L1MSHR,
    L1WQ,
    L2PF,
    L3Snoop,
    L2MSHRGrant,
    L2MSHRRelease,
};

} // namespace gem5

#endif // __MEM_CACHE_XS_L2_TASK_SOURCE_HH__
