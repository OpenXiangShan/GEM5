/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gtest/gtest.h>

#include "cpu/o3/spec_store_fwd.hh"
#include "cpu/o3/spec_store_fwd_types.hh"

namespace gem5::o3
{

TEST(SpecStoreFwdArbitration, SameEntryUsesSqAndConfirmsPrediction)
{
    EXPECT_EQ(selectSpecStoreFwdSource(
                  true, 100, SpecStoreFwdSqResult::FullForward, 100),
              SpecStoreFwdDecision::ConfirmWithSq);
}

TEST(SpecStoreFwdArbitration, YoungerSourceWins)
{
    EXPECT_EQ(selectSpecStoreFwdSource(
                  true, 100, SpecStoreFwdSqResult::FullForward, 90),
              SpecStoreFwdDecision::KeepSpec);
    EXPECT_EQ(selectSpecStoreFwdSource(
                  true, 90, SpecStoreFwdSqResult::FullForward, 100),
              SpecStoreFwdDecision::CorrectWithSq);
}

TEST(SpecStoreFwdArbitration, PartialReplaysOnlyForSameOrYoungerSqStore)
{
    EXPECT_EQ(selectSpecStoreFwdSource(
                  true, 100, SpecStoreFwdSqResult::PartialForward, 90),
              SpecStoreFwdDecision::KeepSpec);
    EXPECT_EQ(selectSpecStoreFwdSource(
                  true, 100, SpecStoreFwdSqResult::PartialForward, 100),
              SpecStoreFwdDecision::ReplayForSq);
    EXPECT_EQ(selectSpecStoreFwdSource(
                  true, 100, SpecStoreFwdSqResult::PartialForward, 110),
              SpecStoreFwdDecision::ReplayForSq);
    EXPECT_EQ(selectSpecStoreFwdSource(
                  true, 100, SpecStoreFwdSqResult::DataNotReady, 100),
              SpecStoreFwdDecision::ReplayForSq);
}

TEST(SpecStoreFwdArbitration, SqMissKeepsOnlyUnvalidatedPrediction)
{
    EXPECT_EQ(selectSpecStoreFwdSource(
                  true, 100, SpecStoreFwdSqResult::Miss),
              SpecStoreFwdDecision::KeepSpec);
    EXPECT_EQ(selectSpecStoreFwdSource(
                  true, 100, SpecStoreFwdSqResult::Miss, 0, true),
              SpecStoreFwdDecision::NormalPath);
}

TEST(SpecStoreFwdArbitration, NoPredictionUsesNormalSqBehavior)
{
    EXPECT_EQ(selectSpecStoreFwdSource(
                  false, 0, SpecStoreFwdSqResult::FullForward, 100),
              SpecStoreFwdDecision::NormalPath);
    EXPECT_EQ(selectSpecStoreFwdSource(
                  false, 0, SpecStoreFwdSqResult::PartialForward, 100),
              SpecStoreFwdDecision::ReplayForSq);
}

TEST(SpecStoreFwdRange, RequiresIdenticalStartAndSize)
{
    EXPECT_TRUE(isSameStoreLoadRange(0x1000, 8, 0x1000, 8));
    EXPECT_FALSE(isSameStoreLoadRange(0x1004, 4, 0x1000, 8));
    EXPECT_FALSE(isSameStoreLoadRange(0x1000, 4, 0x1000, 8));
    EXPECT_FALSE(isSameStoreLoadRange(0x1000, 8, 0x1004, 8));
}

TEST(SpecStoreFwdPredictor, DecrementSaturatesAndSuppressesPrediction)
{
    SpecStoreFwdPredictor predictor;
    predictor.init(true, 1024, 4);
    constexpr Addr pc = 0x1000;
    for (int i = 0; i < 16; ++i) {
        predictor.train(pc, 3);
    }
    ASSERT_TRUE(predictor.predict(pc));
    predictor.decrement(pc);
    EXPECT_FALSE(predictor.predict(pc));
    predictor.train(pc, 3);
    EXPECT_TRUE(predictor.predict(pc));
    for (int i = 0; i < 20; ++i) {
        predictor.decrement(pc);
    }
    EXPECT_FALSE(predictor.predict(pc));
}

TEST(SpecStoreFwdPredictor, DistanceFeedbackReplacesMetadataAndDecrements)
{
    SpecStoreFwdPredictor predictor;
    predictor.init(true, 1024, 4);
    constexpr Addr pc = 0x1000;
    for (int i = 0; i < 16; ++i) {
        predictor.train(pc, 3);
    }
    ASSERT_EQ(predictor.predict(pc), 3);
    predictor.updateDistanceAndDecrement(pc, 7);
    EXPECT_FALSE(predictor.predict(pc));
    predictor.train(pc, 7);
    ASSERT_EQ(predictor.predict(pc), 7);
}

} // namespace gem5::o3
