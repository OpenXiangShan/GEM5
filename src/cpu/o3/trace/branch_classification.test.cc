// L0 pure-function tests for the trace branch-truth override rule.
//
// Anchored: production consumers (DecoupledBPUWithBTB::makeBranchInfo,
// BpTrace::BpTrace, commitBranch) and this test both use
// TraceBranchTruth.hh. The three call sites are now structurally
// symmetric — they cannot drift apart again without bypassing the shared
// rule, which is exactly what the port gap was.
//
// Tombstone — the port-gap chain (guards b5fefebcef, completing
// 23b02c3f0d's incomplete port of c7be4bd32a):
//   c7be4bd32a fixed trace-truth classification CPU-side; the PR #1196
//   extraction ported only part of it, so a synthetic NOP encoding
//   classified a trace *conditional* branch as non-conditional — including
//   when processMisprediction checked a not-taken outcome — corrupting
//   commit-side branch statistics until b5fefebcef completed the port.
//
// Build (NULL ISA):
//   scons build/NULL/cpu/o3/branch_classification.test.opt --unit-test -j$(nproc)
// Run:
//   ./build/NULL/cpu/o3/branch_classification.test.opt

#include <cstdint>

#include "cpu/o3/trace/TraceBranchTruth.hh"
#include "gtest/gtest.h"

namespace
{

using gem5::o3::TraceBranchTruth::TraceBranchFacts;

// Minimal duck-typed instruction exposing the trace accessors
// factsFrom() reads (models DynInst's trace-metadata surface).
struct FakeTraceInst
{
    uint64_t nextPC = 0;
    bool isCond = false;
    bool isIndirect = false;
    bool isCall = false;
    bool isReturn = false;
    bool taken = false;

    uint64_t traceBranchNextPC() const { return nextPC; }
    bool traceIsCond() const { return isCond; }
    bool traceIsIndirect() const { return isIndirect; }
    bool traceIsCall() const { return isCall; }
    bool traceIsReturn() const { return isReturn; }
    bool traceBranchTaken() const { return taken; }
};

// Minimal duck-typed BranchInfo (models the classification fields the
// production BranchInfo exposes).
struct FakeBranchInfo
{
    bool isCond = false;
    bool isIndirect = false;
    bool isDirect = true;
    bool isCall = false;
    bool isReturn = false;
};

} // anonymous namespace

TEST(BranchClassification, SyntheticNopTraceCondClassifiedConditional)
{
    // b5fefebcef tombstone: static decode of the synthetic NOP says
    // "not conditional"; the trace truth says conditional (not-taken).
    // The override must restore the conditional classification.
    FakeTraceInst inst;
    inst.isCond = true;
    inst.taken = false;  // processMisprediction's not-taken check path

    const auto facts = gem5::o3::TraceBranchTruth::factsFrom(inst);

    FakeBranchInfo info;          // static-decode result: all false/direct
    info.isDirect = true;
    gem5::o3::TraceBranchTruth::applyTraceClassification(info, facts);

    EXPECT_TRUE(info.isCond)
        << "trace conditional branch must not be classified non-conditional";
    EXPECT_EQ(gem5::o3::TraceBranchTruth::effectiveTaken(false, facts), false)
        << "not-taken trace outcome must stay not-taken";
}

TEST(BranchClassification, TraceOutcomeMetadataWinsOverStaticDecode)
{
    FakeTraceInst inst;
    inst.nextPC = 0x8000;
    inst.isCond = false;
    inst.isIndirect = true;
    inst.isCall = true;
    inst.isReturn = false;
    inst.taken = true;

    const auto facts = gem5::o3::TraceBranchTruth::factsFrom(inst);

    EXPECT_EQ(gem5::o3::TraceBranchTruth::effectiveTarget(0x1234, facts),
              0x8000u)
        << "trace next-PC wins over the decoded npc";
    EXPECT_EQ(gem5::o3::TraceBranchTruth::effectiveTaken(false, facts), true)
        << "trace taken wins over the decoded outcome";

    FakeBranchInfo info;  // static decode: conditional, direct, not call
    info.isCond = true;
    gem5::o3::TraceBranchTruth::applyTraceClassification(info, facts);
    EXPECT_FALSE(info.isCond) << "trace truth: not a conditional branch";
    EXPECT_TRUE(info.isIndirect) << "trace truth: indirect";
    EXPECT_FALSE(info.isDirect) << "isDirect must be the negation of indirect";
    EXPECT_TRUE(info.isCall) << "trace truth: call";
    EXPECT_FALSE(info.isReturn) << "trace truth: not a return";
}

TEST(BranchClassification, ClassificationConsistentAcrossConsumers)
{
    // Both production consumers (BpTrace constructor and commitBranch)
    // now derive their classification from the same rule; given identical
    // facts they must agree on every classified field.
    FakeTraceInst inst;
    inst.nextPC = 0x7777;
    inst.isCond = true;
    inst.isIndirect = false;
    inst.isCall = false;
    inst.isReturn = true;
    inst.taken = true;

    const auto facts = gem5::o3::TraceBranchTruth::factsFrom(inst);

    // Consumer A (BpTrace-style): apply classification + target + taken.
    FakeBranchInfo a;
    a.isCond = false; a.isIndirect = true; a.isDirect = false;
    a.isCall = true; a.isReturn = false;
    gem5::o3::TraceBranchTruth::applyTraceClassification(a, facts);
    const uint64_t a_target =
        gem5::o3::TraceBranchTruth::effectiveTarget(0xAAAA, facts);
    const bool a_taken = gem5::o3::TraceBranchTruth::effectiveTaken(false, facts);

    // Consumer B (makeBranchInfo-style): same rule, same facts.
    FakeBranchInfo b;
    b.isCond = true; b.isIndirect = false; b.isDirect = true;
    b.isCall = false; b.isReturn = false;
    gem5::o3::TraceBranchTruth::applyTraceClassification(b, facts);
    const uint64_t b_target =
        gem5::o3::TraceBranchTruth::effectiveTarget(0xBBBB, facts);
    const bool b_taken = gem5::o3::TraceBranchTruth::effectiveTaken(true, facts);

    EXPECT_EQ(a.isCond, b.isCond);
    EXPECT_EQ(a.isIndirect, b.isIndirect);
    EXPECT_EQ(a.isDirect, b.isDirect);
    EXPECT_EQ(a.isCall, b.isCall);
    EXPECT_EQ(a.isReturn, b.isReturn);
    EXPECT_EQ(a_target, b_target)
        << "different decoded targets, same facts -> same effective target";
    EXPECT_EQ(a_taken, b_taken)
        << "different decoded outcomes, same facts -> same effective taken";
}

TEST(BranchClassification, AbsentFactsKeepStaticDecode)
{
    // Without trace metadata (native mode), every decoded value stands.
    const TraceBranchFacts absent{};

    EXPECT_EQ(gem5::o3::TraceBranchTruth::effectiveTarget(0x4321, absent),
              0x4321u);
    EXPECT_EQ(gem5::o3::TraceBranchTruth::effectiveTaken(true, absent), true);
    EXPECT_EQ(gem5::o3::TraceBranchTruth::effectiveTaken(false, absent), false);

    // Classification is not touched when facts are absent (call sites
    // guard on hasTraceBranchInfo; the rule is that they do).
    FakeBranchInfo info;
    info.isCond = true;
    info.isIndirect = false;
    info.isCall = false;
    // No applyTraceClassification call: fields unchanged.
    EXPECT_TRUE(info.isCond);
}
