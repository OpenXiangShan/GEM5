#include <gtest/gtest.h>

#include "cpu/pred/btb/common.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

TEST(FetchCoverageSpanHelper, CoverageSpan_Rvc2B)
{
    EXPECT_EQ(fetchCoverageSpan(0x1000, 0x1002, 66), 2u);
}

TEST(FetchCoverageSpanHelper, CoverageSpan_Rvi4B_InBlock)
{
    EXPECT_EQ(fetchCoverageSpan(0x1008, 0x100c, 66), 4u);
}

TEST(FetchCoverageSpanHelper, CoverageSpan_Rvi4B_CrossBoundary)
{
    EXPECT_EQ(fetchCoverageSpan(0x101e, 0x1022, 66), 4u);
}

TEST(FetchCoverageSpanHelper, FetchCoverageWindow_SmallerThan64B)
{
    EXPECT_EQ(fetchCoverageSpan(0x90032, 0x90060, 66), 46u);
}

TEST(FetchCoverageSpanHelper, CoverageSpan_ClampsBeforeUnsignedNarrowing)
{
    EXPECT_EQ(fetchCoverageSpan(0x0, static_cast<Addr>(1) << 40, 66), 66u);
}

TEST(BranchInfoHelper, SplitRvi4BStartAndControlPCAreDistinct)
{
    BranchInfo info;
    info.pc = controlPCFromStartPC(0x101e, 4);
    info.size = 4;

    EXPECT_EQ(info.startPC(), 0x101e);
    EXPECT_EQ(info.controlPC(), 0x1020);
}

TEST(BranchInfoHelper, SplitRvi4BFallThroughUsesArchitecturalStartPC)
{
    BranchInfo info;
    info.pc = controlPCFromStartPC(0x101e, 4);
    info.setStartPC(0x101e);
    info.size = 4;

    EXPECT_EQ(info.fallThroughPC(), 0x1022);
}

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
