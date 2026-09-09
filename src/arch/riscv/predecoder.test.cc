#include <gtest/gtest.h>

#include "arch/riscv/predecoder.hh"

namespace gem5
{
namespace RiscvISA
{

TEST(RiscvPredecoder, BasicControlClasses)
{
    const auto jal = RiscvPredecoder::decode(0x0000006f);
    EXPECT_EQ(jal.branchType, PredecodeInfo::BranchType::Direct);
    EXPECT_EQ(jal.instSize, 4);

    const auto jalBack = RiscvPredecoder::decode(0xffdff06f);
    EXPECT_EQ(jalBack.targetOffset, -4);

    const auto jalr = RiscvPredecoder::decode(0x00008067);
    EXPECT_EQ(jalr.branchType, PredecodeInfo::BranchType::Indirect);
    EXPECT_TRUE(jalr.isReturn);
    EXPECT_TRUE(jalr.hasPop);

    const auto branch = RiscvPredecoder::decode(0x00000063);
    EXPECT_EQ(branch.branchType, PredecodeInfo::BranchType::Conditional);
}

TEST(RiscvPredecoder, CompressedPriorityAndSize)
{
    const auto ebreak = RiscvPredecoder::decode(0x9002);
    EXPECT_EQ(ebreak.branchType, PredecodeInfo::BranchType::None);
    EXPECT_EQ(ebreak.instSize, 2);

    const auto cjump = RiscvPredecoder::decode(0xa001);
    EXPECT_EQ(cjump.branchType, PredecodeInfo::BranchType::Direct);
    EXPECT_EQ(cjump.instSize, 2);

    const auto cjr = RiscvPredecoder::decode(0x8082);
    EXPECT_EQ(cjr.branchType, PredecodeInfo::BranchType::Indirect);
    EXPECT_TRUE(cjr.isReturn);

    const auto cjalr = RiscvPredecoder::decode(0x9082);
    EXPECT_EQ(cjalr.branchType, PredecodeInfo::BranchType::Indirect);
    EXPECT_TRUE(cjalr.isCall);
}

} // namespace RiscvISA
} // namespace gem5
