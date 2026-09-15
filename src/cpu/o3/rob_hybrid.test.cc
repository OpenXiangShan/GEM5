/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "cpu/o3/rob.hh"

namespace gem5
{
namespace o3
{
namespace
{

using InstClass = ROB::HybridInstClass;
using GroupType = ROB::HybridGroupType;
using Plan = ROB::HybridPlan;

Plan
plan(const std::string &classes, unsigned limit = 8)
{
    Plan result;
    for (char inst : classes) {
        const auto cls = inst == 'S' ? InstClass::Simple :
            inst == 'C' ? InstClass::Complex : InstClass::NoCompress;
        ROB::appendHybridClass(result, cls, limit);
    }
    return result;
}

void
expectPlan(const Plan &actual, const Plan &expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (unsigned i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(actual[i].length, expected[i].length) << "group " << i;
        EXPECT_EQ(actual[i].type, expected[i].type) << "group " << i;
    }
}

TEST(HybridROBPlanner, RequiredExamples)
{
    expectPlan(plan("SSSSSSSS"), {{8, GroupType::NormalS}});
    expectPlan(plan("CSSSSSSS"), {{8, GroupType::CS}});
    expectPlan(plan("SSSCSS"), {{4, GroupType::SC},
                                {2, GroupType::NormalS}});
    expectPlan(plan("CCCCCCCC"), {{2, GroupType::CC}, {2, GroupType::CC},
                                  {2, GroupType::CC}, {2, GroupType::CC}});
    expectPlan(plan("SSNSSCC"), {{2, GroupType::NormalS},
                                 {1, GroupType::NormalN},
                                 {3, GroupType::SC},
                                 {1, GroupType::NormalC}});
}

TEST(HybridROBPlanner, EveryFinalTypeAndClosedGroup)
{
    expectPlan(plan("S"), {{1, GroupType::NormalS}});
    expectPlan(plan("C"), {{1, GroupType::NormalC}});
    expectPlan(plan("N"), {{1, GroupType::NormalN}});
    expectPlan(plan("CCS"), {{2, GroupType::CC}, {1, GroupType::NormalS}});
    expectPlan(plan("CSC"), {{2, GroupType::CS}, {1, GroupType::NormalC}});
    expectPlan(plan("SCS"), {{2, GroupType::SC}, {1, GroupType::NormalS}});
    expectPlan(plan("NNSNCN"), {{1, GroupType::NormalN},
                                 {1, GroupType::NormalN},
                                 {1, GroupType::NormalS},
                                 {1, GroupType::NormalN},
                                 {1, GroupType::NormalC},
                                 {1, GroupType::NormalN}});
}

TEST(HybridROBPlanner, AllTypesRespectInstructionLimit)
{
    expectPlan(plan("SSSSSSSSC"), {{8, GroupType::NormalS},
                                    {1, GroupType::NormalC}});
    expectPlan(plan("CSSSSSSSS"), {{8, GroupType::CS},
                                    {1, GroupType::NormalS}});
    expectPlan(plan("SSSSSSCS"), {{7, GroupType::SC},
                                   {1, GroupType::NormalS}});
    expectPlan(plan("SCCSNC", 1), {{1, GroupType::NormalS},
                                    {1, GroupType::NormalC},
                                    {1, GroupType::NormalC},
                                    {1, GroupType::NormalS},
                                    {1, GroupType::NormalN},
                                    {1, GroupType::NormalC}});
    expectPlan(plan("SSCSCSS", 2), {{2, GroupType::NormalS},
                                     {2, GroupType::CS},
                                     {2, GroupType::CS},
                                     {1, GroupType::NormalS}});
}

TEST(HybridROBPlanner, EmptyAndIndependentBatches)
{
    EXPECT_TRUE(plan("").empty());
    // A caller constructs a fresh plan for each rename window. Two batches
    // ending/starting in S must each allocate their own physical group.
    const auto first = plan("SSS");
    const auto second = plan("SSSSS");
    expectPlan(first, {{3, GroupType::NormalS}});
    expectPlan(second, {{5, GroupType::NormalS}});
    EXPECT_EQ(first.size() + second.size(), 2);
}

TEST(HybridROBPlanner, ExhaustiveSegmentLegality)
{
    // Validate the result's semantic shape independently of the planner's
    // transition table for every S/C/N sequence up to the rename width.
    for (unsigned length = 0, sequences = 1; length <= 8;
         ++length, sequences *= 3) {
        for (unsigned encoding = 0; encoding < sequences; ++encoding) {
            std::string classes;
            unsigned digits = encoding;
            for (unsigned i = 0; i < length; ++i, digits /= 3) {
                classes += "SCN"[digits % 3];
            }
            for (unsigned limit : {1, 2, 3, 8}) {
                SCOPED_TRACE(classes + ", limit=" + std::to_string(limit));
                const auto groups = plan(classes, limit);
                unsigned pos = 0;
                for (const auto &group : groups) {
                    ASSERT_GE(group.length, 1);
                    ASSERT_LE(group.length, limit);
                    ASSERT_LE(pos + group.length, classes.size());
                    const auto members = classes.substr(pos, group.length);
                    const auto simple = std::count(members.begin(),
                                                   members.end(), 'S');
                    switch (group.type) {
                      case GroupType::NormalS:
                        EXPECT_EQ(simple, group.length);
                        break;
                      case GroupType::NormalC:
                        EXPECT_EQ(members, "C");
                        break;
                      case GroupType::NormalN:
                        EXPECT_EQ(members, "N");
                        break;
                      case GroupType::CC:
                        EXPECT_EQ(members, "CC");
                        break;
                      case GroupType::CS:
                        EXPECT_EQ(members.front(), 'C');
                        EXPECT_EQ(simple + 1, group.length);
                        break;
                      case GroupType::SC:
                        EXPECT_EQ(members.back(), 'C');
                        EXPECT_EQ(simple + 1, group.length);
                        break;
                      default:
                        FAIL() << "Invalid final group type";
                    }
                    pos += group.length;
                }
                EXPECT_EQ(pos, classes.size());
            }
        }
    }
}

} // anonymous namespace
} // namespace o3
} // namespace gem5
