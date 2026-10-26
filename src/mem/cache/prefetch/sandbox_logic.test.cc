#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

#include "mem/cache/prefetch/sandbox_logic.hh"

using gem5::prefetch::SandboxLogic;

TEST(SandboxLogic, PositiveStreamWaitsForEvaluationAndHonorsDegree)
{
    SandboxLogic::Config cfg;
    cfg.accessesPerPhase = 8;
    cfg.degree = 1;
    SandboxLogic logic(cfg);

    for (unsigned i = 0; i < cfg.accessesPerPhase; ++i) {
        const auto result = logic.access(0x1000 + i * 64);
        if (i + 1 < cfg.accessesPerPhase) {
            EXPECT_FALSE(result.phaseEnd);
            EXPECT_TRUE(result.predictions.empty());
        } else {
            EXPECT_TRUE(result.phaseEnd);
        }
    }
    EXPECT_EQ(logic.evaluatingOffset(), 2);
    EXPECT_TRUE(logic.completedFor(1));
    EXPECT_GT(logic.scoreFor(1), cfg.accessesPerPhase);

    const auto result = logic.access(0x1100);
    ASSERT_EQ(result.predictions.size(), 1u);
    EXPECT_EQ(result.predictions.front().address, 0x1140ULL);
    EXPECT_EQ(result.predictions.front().lookahead, 1u);
}

TEST(SandboxLogic, NegativeOffsetsStayWithinPage)
{
    SandboxLogic::Config cfg;
    cfg.accessesPerPhase = 8;
    cfg.degree = 4;
    SandboxLogic logic(cfg);

    for (unsigned phase = 0; phase < 15; ++phase) {
        for (unsigned i = 0; i < cfg.accessesPerPhase; ++i)
            logic.access(0x2000 + i * 64);
    }
    ASSERT_EQ(logic.evaluatingOffset(), -1);
    for (unsigned i = 0; i < cfg.accessesPerPhase; ++i)
        logic.access(0x2380 - i * 64);
    EXPECT_TRUE(logic.completedFor(-1));
    EXPECT_GT(logic.scoreFor(-1), cfg.accessesPerPhase);

    const auto boundary = logic.access(0x2000);
    for (const auto &prediction : boundary.predictions)
        EXPECT_GE(prediction.address, 0x2000ULL);
    const auto inner = logic.access(0x2200);
    EXPECT_TRUE(std::any_of(inner.predictions.begin(), inner.predictions.end(),
                            [](const auto &prediction) {
                                return prediction.offset < 0 &&
                                    prediction.address < 0x2200;
                            }));
}

TEST(SandboxLogic, ResetsScoreWhenCandidateStartsNextEvaluation)
{
    SandboxLogic::Config cfg;
    cfg.accessesPerPhase = 8;
    SandboxLogic logic(cfg);
    for (unsigned i = 0; i < cfg.accessesPerPhase; ++i)
        logic.access(0x1000 + i * 64);
    EXPECT_GT(logic.scoreFor(1), 0u);
    for (unsigned phase = 1; phase < 16; ++phase) {
        for (unsigned i = 0; i < cfg.accessesPerPhase; ++i) {
            const auto result = logic.access(0x1000 + i * 64);
            if (phase == 15 && i == cfg.accessesPerPhase - 1) {
                EXPECT_TRUE(result.roundEnd);
            }
        }
    }
    // After rotation, the first candidate begins with fresh evidence.
    EXPECT_EQ(logic.phaseAccesses(), 0u);
    EXPECT_EQ(logic.scoreFor(logic.evaluatingOffset()), 0u);
    EXPECT_FALSE(logic.completedFor(logic.evaluatingOffset()));
    const auto offsets = logic.evaluatedOffsets();
    EXPECT_EQ(offsets.size(), 16u);
    EXPECT_TRUE(std::find(offsets.begin(), offsets.end(), 9) != offsets.end());
}

TEST(SandboxLogic, PhaseClearsBloomBeforeNextCandidate)
{
    SandboxLogic::Config cfg;
    cfg.accessesPerPhase = 2;
    cfg.detectStreams = false;
    SandboxLogic logic(cfg);

    logic.access(0x1000);
    auto end = logic.access(0x1080);
    ASSERT_TRUE(end.phaseEnd);
    EXPECT_FALSE(end.bloomHit);
    const auto next = logic.access(0x1040);
    EXPECT_FALSE(next.bloomHit);
    EXPECT_EQ(logic.scoreFor(2), 0u);
}

TEST(SandboxLogic, OneDirectionNeverExceedsConfiguredDegree)
{
    SandboxLogic::Config cfg;
    cfg.accessesPerPhase = 8;
    cfg.degree = 2;
    SandboxLogic logic(cfg);

    for (unsigned phase = 0; phase < 3; ++phase) {
        for (unsigned access = 0; access < cfg.accessesPerPhase; ++access)
            logic.access(0x1000 + access * 64);
    }
    const auto result = logic.access(0x1200);
    const auto positive = std::count_if(
        result.predictions.begin(), result.predictions.end(),
        [](const auto &prediction) { return prediction.offset > 0; });
    EXPECT_LE(positive, 2);
}
