/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of
 * Sciences
 * All rights reserved.
 *
 * The license is the same as that in register_prefetcher.hh.
 */

#include <gtest/gtest.h>

#include <limits>

#include "cpu/o3/rfp_stride_table.hh"
#include "cpu/o3/rfp_wakeup_state.hh"

namespace gem5
{
namespace o3
{

namespace
{

constexpr unsigned ConfidenceBits = 4;
constexpr unsigned ConfidenceThreshold = 15;
constexpr unsigned SamplesToConfidence = ConfidenceThreshold + 2;
constexpr uint64_t DefaultMaxStride = 4096;

TEST(RfpWakeupStateTest, ResponseWaitsForProducerIssue)
{
    RfpWakeupState state;

    EXPECT_EQ(state.onDataReady(), RfpWakeupState::Action::None);
    EXPECT_TRUE(state.hasData());
    EXPECT_FALSE(state.producerIssued());
    EXPECT_FALSE(state.woken());

    EXPECT_EQ(state.onProducerIssue(), RfpWakeupState::Action::Wake);
    EXPECT_TRUE(state.producerIssued());
    EXPECT_TRUE(state.woken());
    EXPECT_EQ(state.onProducerIssue(), RfpWakeupState::Action::None);
    EXPECT_EQ(state.onDataReady(), RfpWakeupState::Action::None);
}

TEST(RfpWakeupStateTest, ProducerIssueDoesNotWaitForResponse)
{
    RfpWakeupState state;

    EXPECT_EQ(state.onProducerIssue(), RfpWakeupState::Action::Wake);
    EXPECT_TRUE(state.producerIssued());
    EXPECT_TRUE(state.woken());
    EXPECT_FALSE(state.hasData());

    EXPECT_EQ(state.onDataReady(), RfpWakeupState::Action::None);
    EXPECT_TRUE(state.hasData());
    EXPECT_TRUE(state.woken());
}

TEST(RfpWakeupStateTest, RollbackAllowsASecondIssueWake)
{
    RfpWakeupState state;

    EXPECT_EQ(state.onProducerIssue(), RfpWakeupState::Action::Wake);
    EXPECT_EQ(state.onProducerIssueCanceled(),
              RfpWakeupState::Action::Retract);
    EXPECT_FALSE(state.producerIssued());
    EXPECT_FALSE(state.woken());

    EXPECT_EQ(state.onDataReady(), RfpWakeupState::Action::None);
    EXPECT_EQ(state.onProducerIssue(), RfpWakeupState::Action::Wake);
    EXPECT_TRUE(state.hasData());
    EXPECT_TRUE(state.woken());
    EXPECT_EQ(state.onProducerIssueCanceled(),
              RfpWakeupState::Action::Retract);
    EXPECT_EQ(state.onProducerIssueCanceled(),
              RfpWakeupState::Action::None);
}

Addr
trainUniformStride(RfpStrideTable &table, Addr pc, Addr first,
                   int64_t stride, unsigned samples,
                   uint64_t generation = 1, InstSeqNum first_seq = 1,
                   Tick first_tick = 1)
{
    Addr address = first;
    for (unsigned sample = 0; sample < samples; ++sample) {
        table.train(pc, address, generation, first_seq + sample,
                    first_tick + sample);
        if (sample + 1 == samples) {
            continue;
        }
        if (stride >= 0) {
            address += static_cast<Addr>(stride);
        } else {
            address -= static_cast<Addr>(-(stride + 1)) + 1;
        }
    }
    return address;
}

Addr
trainToConfidence(RfpStrideTable &table, Addr pc, Addr first,
                  int64_t stride, uint64_t generation = 1)
{
    return trainUniformStride(
        table, pc, first, stride, SamplesToConfidence, generation);
}

TEST(RfpStreamTrackerTest, AssignsRanksAndShiftsThemAtCommit)
{
    RfpStreamTracker tracker;
    constexpr Addr Pc = 0x100;
    constexpr uint64_t Generation = 7;

    EXPECT_EQ(tracker.onRename(Pc, Generation, 10), 1);
    EXPECT_EQ(tracker.onRename(Pc, Generation, 20), 2);
    EXPECT_EQ(tracker.onRename(Pc, Generation, 30), 3);
    EXPECT_EQ(tracker.outstanding(Pc, Generation), 3);

    tracker.onCommit(Pc, Generation, 10);
    EXPECT_EQ(tracker.outstanding(Pc, Generation), 2);
    EXPECT_EQ(tracker.onRename(Pc, Generation, 40), 3);
    tracker.checkInvariants();

    tracker.onCommit(Pc, Generation, 20);
    tracker.onCommit(Pc, Generation, 30);
    tracker.onCommit(Pc, Generation, 40);
    EXPECT_TRUE(tracker.empty());
    EXPECT_EQ(tracker.outstanding(Pc, Generation), 0);
}

TEST(RfpStreamTrackerTest, KeepsGenerationsIndependent)
{
    RfpStreamTracker tracker;
    constexpr Addr Pc = 0x180;

    EXPECT_EQ(tracker.onRename(Pc, 1, 10), 1);
    EXPECT_EQ(tracker.onRename(Pc, 2, 20), 1);
    EXPECT_EQ(tracker.onRename(Pc, 1, 30), 2);
    EXPECT_EQ(tracker.outstanding(Pc, 1), 2);
    EXPECT_EQ(tracker.outstanding(Pc, 2), 1);

    tracker.onCommit(Pc, 1, 10);
    EXPECT_EQ(tracker.outstanding(Pc, 1), 1);
    EXPECT_EQ(tracker.outstanding(Pc, 2), 1);
    tracker.onCommit(Pc, 2, 20);
    tracker.onCommit(Pc, 1, 30);
    tracker.checkInvariants();
    EXPECT_TRUE(tracker.empty());
}

TEST(RfpStreamTrackerTest, SquashesOnlyTheYoungerSuffix)
{
    RfpStreamTracker tracker;
    constexpr Addr PcA = 0x200;
    constexpr Addr PcB = 0x280;

    EXPECT_EQ(tracker.onRename(PcA, 1, 10), 1);
    EXPECT_EQ(tracker.onRename(PcA, 1, 20), 2);
    EXPECT_EQ(tracker.onRename(PcB, 2, 30), 1);
    EXPECT_EQ(tracker.onRename(PcA, 1, 40), 3);

    tracker.squash(20);
    EXPECT_EQ(tracker.size(), 2);
    EXPECT_EQ(tracker.outstanding(PcA, 1), 2);
    EXPECT_EQ(tracker.outstanding(PcB, 2), 0);
    EXPECT_EQ(tracker.onRename(PcA, 1, 35), 3);
    EXPECT_EQ(tracker.onRename(PcB, 2, 36), 1);
    tracker.checkInvariants();

    tracker.onCommit(PcA, 1, 10);
    tracker.onCommit(PcA, 1, 20);
    tracker.onCommit(PcA, 1, 35);
    tracker.onCommit(PcB, 2, 36);
    EXPECT_TRUE(tracker.empty());
}

TEST(RfpStrideTableTest, PredictsLookaheadOneTwoAndThree)
{
    RfpStrideTable table(8, 2, ConfidenceBits, ConfidenceThreshold,
                         DefaultMaxStride, false);
    constexpr Addr Pc = 0x300;
    constexpr int64_t Stride = 8;
    const Addr last = trainToConfidence(table, Pc, 0x4000, Stride);

    for (uint64_t lookahead = 1; lookahead <= 3; ++lookahead) {
        const auto result = table.lookup(Pc, 1, lookahead, 20 + lookahead);
        ASSERT_TRUE(result.prediction);
        EXPECT_EQ(result.reject, RfpStrideTable::RejectReason::None);
        EXPECT_EQ(result.prediction->address, last + lookahead * Stride);
        EXPECT_EQ(result.prediction->lookahead, lookahead);
    }
}

TEST(RfpStrideTableTest, PredictsNegativeStrideAtMultipleLookaheads)
{
    RfpStrideTable table(8, 2, ConfidenceBits, ConfidenceThreshold,
                         DefaultMaxStride, false);
    constexpr Addr Pc = 0x380;
    constexpr int64_t Stride = -16;
    const Addr last = trainToConfidence(table, Pc, 0x5000, Stride);

    for (uint64_t lookahead = 1; lookahead <= 3; ++lookahead) {
        const auto result = table.lookup(Pc, 1, lookahead, 20 + lookahead);
        ASSERT_TRUE(result.prediction);
        EXPECT_EQ(result.prediction->address, last - lookahead * 16);
        EXPECT_EQ(result.prediction->lookahead, lookahead);
    }
}

TEST(RfpStrideTableTest, RequiresTheFourBitSaturationThreshold)
{
    RfpStrideTable table(8, 2, ConfidenceBits, ConfidenceThreshold,
                         DefaultMaxStride, false);
    constexpr Addr Pc = 0x400;
    constexpr int64_t Stride = 8;
    const Addr below_threshold = trainUniformStride(
        table, Pc, 0x6000, Stride, SamplesToConfidence - 1);

    EXPECT_EQ(table.lookup(Pc, 1, 1, 20).reject,
              RfpStrideTable::RejectReason::LowConfidence);

    const Addr at_threshold = below_threshold + Stride;
    const auto train = table.train(
        Pc, at_threshold, 1, SamplesToConfidence, 21);
    EXPECT_TRUE(train.strideMatch);
    EXPECT_TRUE(train.confidenceInc);
    const auto prediction = table.lookup(Pc, 1, 1, 22);
    ASSERT_TRUE(prediction.prediction);
    EXPECT_EQ(prediction.prediction->address, at_threshold + Stride);
}

TEST(RfpStrideTableTest, RecoversFromHalfConfidenceWithoutChangingVersion)
{
    RfpStrideTable table(8, 2, ConfidenceBits, ConfidenceThreshold,
                         DefaultMaxStride, false);
    constexpr Addr Pc = 0x480;
    constexpr int64_t Stride = 8;
    Addr address = trainToConfidence(table, Pc, 0x7000, Stride);
    const auto saturated = table.lookup(Pc, 1, 1, 20);
    ASSERT_TRUE(saturated.prediction);
    const uint64_t version = saturated.prediction->version;

    address += 16;
    const auto wrong = table.train(Pc, address, 1, 18, 21);
    EXPECT_TRUE(wrong.strideMismatch);
    EXPECT_TRUE(wrong.confidenceDec);
    EXPECT_FALSE(wrong.strideChange);
    EXPECT_TRUE(table.versionMatches(Pc, 1, version));
    EXPECT_EQ(table.lookup(Pc, 1, 1, 22).reject,
              RfpStrideTable::RejectReason::LowConfidence);

    for (unsigned match = 0; match < 7; ++match) {
        address += Stride;
        const auto recovery = table.train(
            Pc, address, 1, 19 + match, 23 + match);
        EXPECT_TRUE(recovery.strideMatch);
        EXPECT_TRUE(recovery.confidenceInc);
        EXPECT_FALSE(recovery.strideChange);
    }
    EXPECT_EQ(table.lookup(Pc, 1, 1, 30).reject,
              RfpStrideTable::RejectReason::LowConfidence);

    address += Stride;
    const auto recovered = table.train(Pc, address, 1, 26, 31);
    EXPECT_TRUE(recovered.strideMatch);
    EXPECT_TRUE(recovered.confidenceInc);
    const auto prediction = table.lookup(Pc, 1, 1, 32);
    ASSERT_TRUE(prediction.prediction);
    EXPECT_EQ(prediction.prediction->version, version);
    EXPECT_EQ(prediction.prediction->address, address + Stride);
}

TEST(RfpStrideTableTest, ChangesVersionOnlyAtTheActualPhaseChange)
{
    RfpStrideTable table(8, 2, ConfidenceBits, ConfidenceThreshold,
                         DefaultMaxStride, false);
    constexpr Addr Pc = 0x500;
    Addr address = trainToConfidence(table, Pc, 0x8000, 8);
    const auto saturated = table.lookup(Pc, 1, 1, 20);
    ASSERT_TRUE(saturated.prediction);
    const uint64_t old_version = saturated.prediction->version;

    for (unsigned mismatch = 0; mismatch < 4; ++mismatch) {
        address += 16;
        const auto phase = table.train(
            Pc, address, 1, 18 + mismatch, 21 + mismatch);
        EXPECT_TRUE(phase.strideMismatch);
        EXPECT_TRUE(phase.confidenceDec);
        if (mismatch < 3) {
            EXPECT_FALSE(phase.strideChange);
            EXPECT_TRUE(table.versionMatches(Pc, 1, old_version));
        } else {
            EXPECT_TRUE(phase.strideChange);
            EXPECT_FALSE(table.versionMatches(Pc, 1, old_version));
        }
    }
    EXPECT_EQ(table.lookup(Pc, 1, 1, 30).reject,
              RfpStrideTable::RejectReason::LowConfidence);
}

TEST(RfpStrideTableTest, ClassifiesIllegalStridesAndStableZeroPhase)
{
    RfpStrideTable table(8, 2, ConfidenceBits, ConfidenceThreshold, 64, false);
    constexpr Addr Pc = 0x580;
    Addr address = trainToConfidence(table, Pc, 0x9000, 8);

    for (unsigned mismatch = 0; mismatch < 4; ++mismatch) {
        const auto zero_stride = table.train(
            Pc, address, 1, 18 + mismatch, 20 + mismatch);
        EXPECT_TRUE(zero_stride.illegalStride);
        EXPECT_FALSE(zero_stride.strideMatch);
        EXPECT_FALSE(zero_stride.strideMismatch);
        EXPECT_TRUE(zero_stride.confidenceDec);
        EXPECT_EQ(zero_stride.strideChange, mismatch == 3);
    }

    const auto stable_zero = table.train(Pc, address, 1, 22, 24);
    EXPECT_TRUE(stable_zero.illegalStride);
    EXPECT_FALSE(stable_zero.strideMismatch);
    EXPECT_FALSE(stable_zero.strideChange);

    const auto out_of_range = table.train(Pc, address + 128, 1, 23, 25);
    EXPECT_TRUE(out_of_range.illegalStride);
    EXPECT_FALSE(out_of_range.strideMismatch);
    EXPECT_FALSE(out_of_range.strideChange);
}

TEST(RfpStrideTableTest, RejectsPageCrossingAtRequestedLookahead)
{
    RfpStrideTable table(8, 2, ConfidenceBits, ConfidenceThreshold,
                         DefaultMaxStride, true);
    constexpr Addr Pc = 0x600;
    constexpr Addr LastBeforePage = 0xff8;
    constexpr int64_t Stride = 8;
    const Addr first = LastBeforePage -
        (SamplesToConfidence - 1) * static_cast<Addr>(Stride);
    EXPECT_EQ(trainToConfidence(table, Pc, first, Stride), LastBeforePage);

    EXPECT_EQ(table.lookup(Pc, 1, 1, 20).reject,
              RfpStrideTable::RejectReason::CrossPage);
}

TEST(RfpStrideTableTest, RejectsPositiveAndNegativeMultiStepOverflow)
{
    const Addr max = std::numeric_limits<Addr>::max();

    RfpStrideTable high_table(8, 2, ConfidenceBits, ConfidenceThreshold,
                              DefaultMaxStride, false);
    constexpr Addr HighPc = 0x680;
    constexpr int64_t PositiveStride = 8;
    const Addr high_last = max - 16;
    const Addr high_first = high_last -
        (SamplesToConfidence - 1) * static_cast<Addr>(PositiveStride);
    EXPECT_EQ(trainToConfidence(
                  high_table, HighPc, high_first, PositiveStride),
              high_last);
    ASSERT_TRUE(high_table.lookup(HighPc, 1, 2, 20).prediction);
    EXPECT_EQ(high_table.lookup(HighPc, 1, 3, 21).reject,
              RfpStrideTable::RejectReason::AddressOverflow);

    RfpStrideTable low_table(8, 2, ConfidenceBits, ConfidenceThreshold,
                             DefaultMaxStride, false);
    constexpr Addr LowPc = 0x700;
    constexpr int64_t NegativeStride = -8;
    constexpr Addr LowLast = 16;
    const Addr low_first = LowLast +
        (SamplesToConfidence - 1) * static_cast<Addr>(-NegativeStride);
    EXPECT_EQ(trainToConfidence(
                  low_table, LowPc, low_first, NegativeStride),
              LowLast);
    ASSERT_TRUE(low_table.lookup(LowPc, 1, 2, 20).prediction);
    EXPECT_EQ(low_table.lookup(LowPc, 1, 3, 21).reject,
              RfpStrideTable::RejectReason::AddressOverflow);
}

TEST(RfpStrideTableTest, KeepsAddressSpaceGenerationsIndependent)
{
    RfpStrideTable table(8, 2, ConfidenceBits, ConfidenceThreshold,
                         DefaultMaxStride, false);
    constexpr Addr Pc = 0x780;
    const Addr last = trainToConfidence(table, Pc, 0xa000, 8, 3);

    const auto generation_three = table.lookup(Pc, 3, 2, 20);
    ASSERT_TRUE(generation_three.prediction);
    EXPECT_EQ(generation_three.prediction->address, last + 16);
    EXPECT_EQ(table.lookup(Pc, 4, 1, 21).reject,
              RfpStrideTable::RejectReason::Miss);
}

} // anonymous namespace
} // namespace o3
} // namespace gem5
