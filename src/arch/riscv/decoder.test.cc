#include <gtest/gtest.h>

#include "arch/riscv/decoder.hh"

namespace gem5
{

namespace RiscvISA
{

TEST(RiscvDecoderHelper, Compressed16_ReadyAfter2B)
{
    PartialInstBuffer partial;

    const auto result = partial.pushChunk(0x1000, 0x1000, 0x00000001, 2);

    EXPECT_EQ(result, PartialInstResult::ReadyCompressed);
    EXPECT_EQ(partial.compressedBits(), 0x0001);
    EXPECT_EQ(partial.assembledBytes, 2u);
}

TEST(RiscvDecoderHelper, Rvi32_WaitsForSecondHalf)
{
    PartialInstBuffer partial;

    const auto result = partial.pushChunk(0x1000, 0x1000, 0x00000013, 2);

    EXPECT_EQ(result, PartialInstResult::NeedMoreBytes);
    EXPECT_TRUE(partial.hasBytes());
    EXPECT_EQ(partial.assembledBytes, 2u);
    EXPECT_EQ(partial.fullBits(), 0x00000013u);
}

TEST(RiscvDecoderHelper, Rvi32_ReadyAfterSecondHalf)
{
    PartialInstBuffer partial;

    EXPECT_EQ(partial.pushChunk(0x1000, 0x1000, 0x00000013, 2),
              PartialInstResult::NeedMoreBytes);

    const auto result = partial.pushChunk(0x1000, 0x1002, 0x00001234, 2);

    EXPECT_EQ(result, PartialInstResult::ReadyFullWidth);
    EXPECT_EQ(partial.assembledBytes, 4u);
    EXPECT_EQ(partial.fullBits(), 0x12340013u);
}

TEST(RiscvDecoderHelper, ResetClearsPartialState)
{
    PartialInstBuffer partial;

    EXPECT_EQ(partial.pushChunk(0x1000, 0x1000, 0x00000013, 2),
              PartialInstResult::NeedMoreBytes);
    partial.reset();

    EXPECT_FALSE(partial.hasBytes());
    EXPECT_EQ(partial.instPC, MaxAddr);
    EXPECT_EQ(partial.fullBits(), 0u);
    EXPECT_EQ(partial.pushChunk(0x1000, 0x1002, 0x00001234, 2),
              PartialInstResult::NeedMoreBytes);
    EXPECT_EQ(partial.assembledBytes, 2u);
}

} // namespace RiscvISA
} // namespace gem5
