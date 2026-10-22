// L0 pure-function tests for the CBP2025 register mapping table.
//
// Anchored: production (CBP2025TraceReader.cc) and this test both use
// CBPRegMap.hh, so any change to the mapping table fails here.
//
// Tombstone — the x5 fix-of-fix chain (three commits, same day):
//   - 89b00fb712 "normalize cbp trace register deps": first normalization
//     kept the CALL_IND x5 dummy source as-is (still alt-RA semantics);
//   - a28e34a2cc "map cbp call-indirect x5 source to x0": dropped the
//     dependency edge entirely (x0 is constant zero);
//   - 9d97cbb232 "remap cbp call-indirect x5 to neutral gpr": final form —
//     x28 (t3) keeps the dependency edge without alt-RA semantics.
//
// Build (NULL ISA):
//   scons build/NULL/cpu/o3/cbp_reg_map.test.opt --unit-test -j$(nproc)
// Run:
//   ./build/NULL/cpu/o3/cbp_reg_map.test.opt

#include <cstdint>

#include "cpu/o3/trace/CBPRegMap.hh"
#include "gtest/gtest.h"

namespace
{

using gem5::o3::CBPRegMap::isIntReg;
using gem5::o3::CBPRegMap::mapFpReg;
using gem5::o3::CBPRegMap::mapIntReg;

} // anonymous namespace

TEST(CBPRegMap, CallIndirectX5MapsToNeutralGpr)
{
    // 9d97cbb232 final form: x28, not x0, not x5.
    EXPECT_EQ(mapIntReg(5, /*call_indirect=*/true), 28u);
}

TEST(CBPRegMap, CallIndirectX5RetainsDependencyEdge)
{
    // a28e34a2cc regression: mapping to x0 silently dropped the dependency
    // edge. The mapped register must be a real, non-zero GPR.
    const uint8_t mapped = mapIntReg(5, /*call_indirect=*/true);
    EXPECT_NE(mapped, 0u) << "x5 -> x0 drops the dependency edge";
    // And it must not stay x5 (alt-RA semantics) either.
    EXPECT_NE(mapped, 5u) << "x5 -> x5 keeps alt-RA semantics";
}

TEST(CBPRegMap, NonCallIndirectX5StaysX5)
{
    // The x5 special case is CALL_IND-only: a plain instruction using x5
    // keeps the identity mapping (normal GPR use).
    EXPECT_EQ(mapIntReg(5, /*call_indirect=*/false), 5u);
}

TEST(CBPRegMap, SpecialRegistersMapToArchitecturalCounterparts)
{
    // 89b00fb712 normalization table.
    EXPECT_EQ(mapIntReg(31, false), 2u);   // SP -> x2
    EXPECT_EQ(mapIntReg(30, false), 1u);   // LR (x30) -> RA (x1)
    EXPECT_EQ(mapIntReg(0,  false), 0u);   // zero -> x0
    EXPECT_EQ(mapIntReg(65, false), 0u);   // zero alias -> x0
    EXPECT_EQ(mapIntReg(64, false), 0u);   // flags best-effort -> x0
    EXPECT_EQ(mapIntReg(26, false), 0u);   // IP not a GPR -> x0
}

TEST(CBPRegMap, GeneralPurposeRegistersAreIdentity)
{
    for (uint8_t r = 1; r < 32; ++r) {
        if (r == 5 || r == 26 || r == 30 || r == 31)
            continue;  // special-cased above
        EXPECT_EQ(mapIntReg(r, false), r) << "GPR r=" << int(r);
    }
}

TEST(CBPRegMap, IntRegClassification)
{
    // 0-31 int (incl. SP/LR), 64 flags, 65 zero are int-class;
    // 32-63 are SIMD/FP.
    for (uint8_t r = 0; r < 32; ++r)
        EXPECT_TRUE(isIntReg(r)) << "r=" << int(r);
    EXPECT_TRUE(isIntReg(64));
    EXPECT_TRUE(isIntReg(65));
    for (uint8_t r = 32; r < 64; ++r)
        EXPECT_FALSE(isIntReg(r)) << "r=" << int(r);
}

TEST(CBPRegMap, FpRegistersMapToF0ThroughF31)
{
    EXPECT_EQ(mapFpReg(32), 0u);
    EXPECT_EQ(mapFpReg(63), 31u);
    EXPECT_EQ(mapFpReg(40), 8u);
    // Non-FP input is outside this function's contract (the isIntReg gate
    // keeps it away); anchored as current behavior: -> f0.
    EXPECT_EQ(mapFpReg(5), 0u);
}
