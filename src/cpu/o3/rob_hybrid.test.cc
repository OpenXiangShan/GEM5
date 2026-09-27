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
        EXPECT_EQ(actual[i].memberCount(), expected[i].memberCount()) << "group " << i;
        EXPECT_EQ(actual[i].formerLength, expected[i].formerLength);
        EXPECT_EQ(actual[i].latterLength, expected[i].latterLength);
        EXPECT_EQ(actual[i].type, expected[i].type) << "group " << i;
    }
}

TEST(HybridROBPlanner, RequiredExamples)
{
    expectPlan(plan("SSSSSSSS"), {{8, 0, GroupType::NormalS}});
    expectPlan(plan("CSSSSSSS"), {{1, 7, GroupType::CS}});
    expectPlan(plan("SSSCSS"), {{3, 1, GroupType::SC},
                                {2, 0, GroupType::NormalS}});
    expectPlan(plan("CCCCCCCC"), {{1, 1, GroupType::CC}, {1, 1, GroupType::CC},
                                  {1, 1, GroupType::CC}, {1, 1, GroupType::CC}});
    expectPlan(plan("SSNSSCC"), {{2, 0, GroupType::NormalS},
                                 {1, 0, GroupType::NormalN},
                                 {2, 1, GroupType::SC},
                                 {1, 0, GroupType::NormalC}});
}

TEST(HybridROBPlanner, EveryFinalTypeAndClosedGroup)
{
    expectPlan(plan("S"), {{1, 0, GroupType::NormalS}});
    expectPlan(plan("C"), {{1, 0, GroupType::NormalC}});
    expectPlan(plan("N"), {{1, 0, GroupType::NormalN}});
    expectPlan(plan("CCS"), {{1, 1, GroupType::CC}, {1, 0, GroupType::NormalS}});
    expectPlan(plan("CSC"), {{1, 1, GroupType::CS}, {1, 0, GroupType::NormalC}});
    expectPlan(plan("SCS"), {{1, 1, GroupType::SC}, {1, 0, GroupType::NormalS}});
    expectPlan(plan("NNSNCN"), {{1, 0, GroupType::NormalN},
                                 {1, 0, GroupType::NormalN},
                                 {1, 0, GroupType::NormalS},
                                 {1, 0, GroupType::NormalN},
                                 {1, 0, GroupType::NormalC},
                                 {1, 0, GroupType::NormalN}});
}

TEST(HybridROBPlanner, AllTypesRespectInstructionLimit)
{
    expectPlan(plan("SSSSSSSSC"), {{8, 0, GroupType::NormalS},
                                    {1, 0, GroupType::NormalC}});
    expectPlan(plan("CSSSSSSSS"), {{1, 7, GroupType::CS},
                                    {1, 0, GroupType::NormalS}});
    expectPlan(plan("SSSSSSCS"), {{6, 1, GroupType::SC},
                                   {1, 0, GroupType::NormalS}});
    expectPlan(plan("SCCSNC", 1), {{1, 0, GroupType::NormalS},
                                    {1, 0, GroupType::NormalC},
                                    {1, 0, GroupType::NormalC},
                                    {1, 0, GroupType::NormalS},
                                    {1, 0, GroupType::NormalN},
                                    {1, 0, GroupType::NormalC}});
    expectPlan(plan("SSCSCSS", 2), {{2, 0, GroupType::NormalS},
                                     {1, 1, GroupType::CS},
                                     {1, 1, GroupType::CS},
                                     {1, 0, GroupType::NormalS}});
}

TEST(HybridROBPlanner, EmptyAndIndependentBatches)
{
    EXPECT_TRUE(plan("").empty());
    // A caller constructs a fresh plan for each rename window. Two batches
    // ending/starting in S must each allocate their own physical group.
    const auto first = plan("SSS");
    const auto second = plan("SSSSS");
    expectPlan(first, {{3, 0, GroupType::NormalS}});
    expectPlan(second, {{5, 0, GroupType::NormalS}});
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
                    ASSERT_GE(group.formerLength, 1);
                    ASSERT_EQ(group.hasLatter(),
                        group.type == GroupType::CC ||
                        group.type == GroupType::CS ||
                        group.type == GroupType::SC);
                    ASSERT_GE(group.memberCount(), 1);
                    ASSERT_LE(group.memberCount(), limit);
                    ASSERT_LE(pos + group.memberCount(), classes.size());
                    const auto members = classes.substr(pos, group.memberCount());
                    const auto simple = std::count(members.begin(),
                                                   members.end(), 'S');
                    switch (group.type) {
                      case GroupType::NormalS:
                        EXPECT_EQ(simple, group.memberCount());
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
                        EXPECT_EQ(simple + 1, group.memberCount());
                        break;
                      case GroupType::SC:
                        EXPECT_EQ(members.back(), 'C');
                        EXPECT_EQ(simple + 1, group.memberCount());
                        break;
                      default:
                        FAIL() << "Invalid final group type";
                    }
                    pos += group.memberCount();
                }
                EXPECT_EQ(pos, classes.size());
            }
        }
    }
}

TEST(HybridROBSlots, RedirectTruthTable)
{
    // Literal retained slot sets: neither, former, former, both.
    const bool keep[4][2] = {{false, false}, {true, false},
                             {true, false}, {true, true}};
    for (unsigned row = 0; row < 4; ++row) {
        ROB::HybridSquashTarget target{7, row < 2, row % 2 == 0};
        EXPECT_FALSE(target.removes(6, true));
        EXPECT_FALSE(target.removes(6, false));
        EXPECT_TRUE(target.removes(8, true));
        EXPECT_TRUE(target.removes(8, false));
        EXPECT_EQ(target.removes(7, true), !keep[row][0]);
        EXPECT_EQ(target.removes(7, false), !keep[row][1]);
    }
}

TEST(HybridROBSlots, MultiMemberSlotSquash)
{
    // Exercise real entry updates with every slot target and sequence gaps.
    for (const std::string classes : {"CSS", "SSC", "CC"}) {
        const auto group = plan(classes).front();
        for (unsigned row = 0; row < 4; ++row) {
            ROB::HybridSquashTarget target{42, row < 2, row % 2 == 0};
            auto type = classes == "CSS" ? ROB::HybridEntryType::CS :
                classes == "SSC" ? ROB::HybridEntryType::SC :
                                   ROB::HybridEntryType::CC;
            ROB::HybridEntryState entry{42, type, group.formerLength,
                                      group.latterLength};
            std::vector<unsigned> survivors;
            unsigned downgrades = 0;
            for (unsigned i = group.memberCount(); i > 0; --i) {
                const bool former = i <= group.formerLength;
                if (target.removes(42, former)) {
                    downgrades += entry.removeMember(
                        former, !target.removes(42, true));
                } else {
                    survivors.push_back(i * 10);
                }
            }
            const unsigned expected = row == 0 ? 0 :
                row == 3 ? classes.size() : (classes == "SSC" ? 2 : 1);
            EXPECT_EQ(entry.memberCount(), expected);
            EXPECT_EQ(survivors.size(), expected);
            EXPECT_EQ(downgrades, row == 1 || row == 2 ? 1 : 0);
            for (unsigned i = 0; i < expected; ++i) {
                EXPECT_EQ(survivors[i], (expected - i) * 10);
            }
            EXPECT_EQ(entry.id, 42);
            if (row == 1 || row == 2) {
                EXPECT_EQ(entry.type, ROB::HybridEntryType::NORMAL);
            }
        }
    }
}

TEST(HybridROBSlots, LatterDowngradeAndPartialRemoval)
{
    using Type = ROB::HybridEntryType;
    for (auto type : {Type::CS, Type::SC, Type::CC}) {
        const unsigned former = type == Type::SC ? 3 : 1;
        const unsigned latter = type == Type::CS ? 2 : 1;
        ROB::HybridEntryState entry{42, type, former, latter};
        for (unsigned i = 1; i < latter; ++i) {
            EXPECT_FALSE(entry.removeMember(false, true));
            EXPECT_EQ(entry.type, type);
        }
        EXPECT_TRUE(entry.removeMember(false, true));
        EXPECT_EQ(entry.type, Type::NORMAL);
        EXPECT_EQ(entry.id, 42);
        EXPECT_EQ(entry.formerRemaining, former);
        EXPECT_EQ(entry.latterRemaining, 0);
        EXPECT_EQ(entry.memberCount(), former);
    }
}

TEST(HybridROBSlots, WholeEntrySquashDoesNotDowngrade)
{
    using Type = ROB::HybridEntryType;
    ROB::HybridEntryState entry{42, Type::CC, 1, 1};
    EXPECT_FALSE(entry.removeMember(false, false));
    EXPECT_EQ(entry.type, Type::CC);
    EXPECT_FALSE(entry.removeMember(true, false));
    EXPECT_EQ(entry.memberCount(), 0);
}

TEST(HybridROBSlots, RetiredFormerDoesNotRelabelLatter)
{
    using Type = ROB::HybridEntryType;
    ROB::HybridEntryState entry{42, Type::CS, 1, 3};
    EXPECT_FALSE(entry.removeMember(true, false));
    EXPECT_EQ(entry.type, Type::CS);
    EXPECT_EQ(entry.formerRemaining, 0);
    EXPECT_EQ(entry.latterRemaining, 3);
    EXPECT_FALSE(entry.removeMember(false, true));
    EXPECT_FALSE(entry.removeMember(false, true));
    EXPECT_FALSE(entry.removeMember(false, true));
    EXPECT_EQ(entry.memberCount(), 0);
}

} // anonymous namespace
} // namespace o3
} // namespace gem5
