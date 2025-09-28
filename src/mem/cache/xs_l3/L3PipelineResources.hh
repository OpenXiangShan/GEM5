#ifndef __MEM_CACHE_xs_l3_PIPELINE_RESOURCES_HH__
#define __MEM_CACHE_xs_l3_PIPELINE_RESOURCES_HH__

#include <cstdint>

namespace gem5
{

// For response pipeline scoreboard
// Using a bitmask to allow for multiple resource acquisitions per stage
enum L3PipelineResources : uint8_t
{
    Free      = 0,
    DataRead  = 1 << 0, // need a data read operation
    DataWrite = 1 << 1, // need a data write operation
    DirRead   = 1 << 2, // need a directory read operation
    DirWrite  = 1 << 3, // need a directory write operation
    GrantBuf  = 1 << 4, // need a grant buffer operation
};

inline L3PipelineResources
operator|(L3PipelineResources a, L3PipelineResources b)
{
    return static_cast<L3PipelineResources>(
        static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline L3PipelineResources
operator&(L3PipelineResources a, L3PipelineResources b)
{
    return static_cast<L3PipelineResources>(
        static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline L3PipelineResources
operator~(L3PipelineResources a)
{
    return static_cast<L3PipelineResources>(~static_cast<uint8_t>(a));
}

inline L3PipelineResources&
operator|=(L3PipelineResources& a, L3PipelineResources b)
{
    a = a | b;
    return a;
}

inline L3PipelineResources&
operator&=(L3PipelineResources& a, L3PipelineResources b)
{
    a = a & b;
    return a;
}

} // namespace gem5

#endif // __MEM_CACHE_xs_l3_PIPELINE_RESOURCES_HH__