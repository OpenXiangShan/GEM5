#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "cpu/o3/crob_kmhv2_break_analysis.hh"

namespace gem5::o3
{
namespace
{

using Class = Kmhv2InstClass;

Kmhv2InstSample
sample(Class inst_class, unsigned ftq_id = 0)
{
    return Kmhv2InstSample{inst_class, ftq_id};
}

TEST(CrobKmhv2BreakAnalysis, PacksEightSimpleInstructionsIntoOneEntry)
{
    const std::vector<Kmhv2InstSample> samples(8,
        sample(Class::SimpleIntegerAlu));

    const auto result = analyzeKmhv2Bundle(samples, 8);

    EXPECT_EQ(result.physicalEntries, 1);
    EXPECT_EQ(result.noBreakPhysicalEntries, 1);
    EXPECT_EQ(result.breakLostEntries, 0);
    EXPECT_EQ(result.simpleRunLengths[8], 1);
    EXPECT_EQ(result.classCounts[static_cast<size_t>(
                  Class::SimpleIntegerAlu)], 8);
}

TEST(CrobKmhv2BreakAnalysis, CountsLoadBetweenSimpleRunsAsBreak)
{
    const std::vector<Kmhv2InstSample> samples{
        sample(Class::SimpleIntegerAlu),
        sample(Class::Load),
        sample(Class::SimpleIntegerAlu),
    };

    const auto result = analyzeKmhv2Bundle(samples, 8);

    EXPECT_EQ(result.breakBlocks, 1);
    EXPECT_EQ(result.breakingClassCounts[static_cast<size_t>(Class::Load)],
              1);
    EXPECT_EQ(result.physicalEntries, 3);
    EXPECT_EQ(result.noBreakPhysicalEntries, 2);
    EXPECT_EQ(result.breakLostEntries, 1);
    EXPECT_EQ(result.simpleRunLengths[1], 2);
}

TEST(CrobKmhv2BreakAnalysis, AttributesEveryInstructionInBreakingComplexBlock)
{
    const std::vector<Kmhv2InstSample> samples{
        sample(Class::SimpleOther),
        sample(Class::Store),
        sample(Class::Branch),
        sample(Class::SimpleFloatingAlu),
    };

    const auto result = analyzeKmhv2Bundle(samples, 8);

    EXPECT_EQ(result.breakBlocks, 1);
    EXPECT_EQ(result.breakingClassCounts[static_cast<size_t>(Class::Store)],
              1);
    EXPECT_EQ(result.breakingClassCounts[static_cast<size_t>(Class::Branch)],
              1);
    EXPECT_EQ(result.physicalEntries, 4);
    EXPECT_EQ(result.noBreakPhysicalEntries, 3);
    EXPECT_EQ(result.breakLostEntries, 1);
}

TEST(CrobKmhv2BreakAnalysis, DoesNotAttributeBreakAcrossFtqBoundary)
{
    const std::vector<Kmhv2InstSample> samples{
        sample(Class::SimpleIntegerAlu, 0),
        sample(Class::Load, 1),
        sample(Class::SimpleIntegerAlu, 1),
    };

    const auto result = analyzeKmhv2Bundle(samples, 8);

    EXPECT_EQ(result.breakBlocks, 0);
    EXPECT_EQ(result.breakingClassCounts[static_cast<size_t>(Class::Load)],
              0);
    EXPECT_EQ(result.physicalEntries, 3);
    EXPECT_EQ(result.noBreakPhysicalEntries, 3);
    EXPECT_EQ(result.breakLostEntries, 0);
}

TEST(CrobKmhv2BreakAnalysis, DoesNotCountComplexInstructionAtBundleBoundary)
{
    const std::vector<Kmhv2InstSample> samples{
        sample(Class::Jump),
        sample(Class::SimpleIntegerAlu),
        sample(Class::SimpleIntegerAlu),
        sample(Class::OtherComplex),
    };

    const auto result = analyzeKmhv2Bundle(samples, 8);

    EXPECT_EQ(result.breakBlocks, 0);
    EXPECT_EQ(result.physicalEntries, 3);
    EXPECT_EQ(result.noBreakPhysicalEntries, 3);
    EXPECT_EQ(result.breakLostEntries, 0);
}

TEST(CrobKmhv2BreakAnalysis, SplitsSimpleRunAtConfiguredGroupWidth)
{
    const std::vector<Kmhv2InstSample> samples(9,
        sample(Class::SimpleIntegerAlu));

    const auto result = analyzeKmhv2Bundle(samples, 8);

    EXPECT_EQ(result.physicalEntries, 2);
    EXPECT_EQ(result.noBreakPhysicalEntries, 2);
    EXPECT_EQ(result.simpleRunLengths[9], 1);
}

TEST(CrobKmhv2BreakAnalysis, RejectsZeroGroupWidth)
{
    EXPECT_THROW(analyzeKmhv2Bundle({}, 0), std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::o3
