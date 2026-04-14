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

TEST(FetchCoverageSpanHelper, CoverageLastLine_StaysInFirstLineForInBlockRvi4B)
{
    EXPECT_EQ(fetchCoverageLastLineAddr(0x1008, 0x100c, 66, 64), 0x1000u);
}

TEST(FetchCoverageSpanHelper, CoverageLastLine_ExtendsToNextLineForCrossBoundaryRvi4B)
{
    EXPECT_EQ(fetchCoverageLastLineAddr(0x103e, 0x1042, 66, 64), 0x1040u);
}

TEST(FetchCoverageSpanHelper,
     CoverageLastLine_UsesActualControlTailWithoutLegacyOverfetch)
{
    const Addr start_pc = 0x103e;
    const Addr first_line = start_pc - (start_pc % 64);
    const Addr last_line =
        fetchCoverageLastLineAddr(start_pc, 0x1042, 66, 64);

    EXPECT_NE(first_line, last_line);
}

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
