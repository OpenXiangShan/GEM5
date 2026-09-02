#ifndef __CPU_O3_CROB_KMHV2_BREAK_ANALYSIS_HH__
#define __CPU_O3_CROB_KMHV2_BREAK_ANALYSIS_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gem5::o3
{

enum class Kmhv2InstClass : uint8_t
{
    SimpleIntegerAlu,
    SimpleFloatingAlu,
    SimpleOther,
    Load,
    Store,
    Branch,
    Jump,
    OtherComplex,
    NumClasses
};

constexpr size_t Kmhv2InstClassCount =
    static_cast<size_t>(Kmhv2InstClass::NumClasses);

struct Kmhv2InstSample
{
    Kmhv2InstClass instClass;
    unsigned ftqId;
};

struct Kmhv2BundleAnalysis
{
    std::array<uint64_t, Kmhv2InstClassCount> classCounts{};
    std::array<uint64_t, Kmhv2InstClassCount> breakingClassCounts{};
    std::vector<uint64_t> simpleRunLengths;
    uint64_t breakBlocks = 0;
    uint64_t physicalEntries = 0;
    uint64_t noBreakPhysicalEntries = 0;
    uint64_t breakLostEntries = 0;
};

bool isKmhv2SimpleClass(Kmhv2InstClass inst_class);

Kmhv2BundleAnalysis analyzeKmhv2Bundle(
    const std::vector<Kmhv2InstSample> &samples,
    unsigned group_width);

} // namespace gem5::o3

#endif // __CPU_O3_CROB_KMHV2_BREAK_ANALYSIS_HH__
