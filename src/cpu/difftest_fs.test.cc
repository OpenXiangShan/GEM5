// Copyright (c) 2026 The gem5 contributors
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include "cpu/difftest_fs.hh"

namespace gem5
{
namespace difftest
{
namespace
{

uint64_t
status(unsigned fp, unsigned vec = 0, unsigned extra = 0)
{
    return (uint64_t{fp} << 13) | (uint64_t{vec} << 9) |
           (uint64_t{extra} << 15) |
           ((fp == 3 || vec == 3 || extra == 3) ? SdMask : 0);
}

TEST(DifftestFs, AllowsOnlyConservativeDirtiness)
{
    for (unsigned a = 0; a < 4; ++a) {
        for (unsigned b = 0; b < 4; ++b) {
            const bool allowed = a == b ||
                (a != 0 && b != 0 && (a == 3 || b == 3));
            EXPECT_EQ(equivalentFsStatus(status(a), status(b)), allowed);
        }
    }
}

TEST(DifftestFs, DoesNotHideMissingDirtyEvenWhenBothModelsAgree)
{
    for (unsigned before = 0; before < 4; ++before) {
        for (unsigned after = 0; after < 4; ++after) {
            EXPECT_EQ(legalFsTransition(before, after, true), after == 3);
        }
    }
    EXPECT_TRUE(legalFsTransition(2, 2, false));
    EXPECT_TRUE(legalFsTransition(2, 3, false));
    EXPECT_FALSE(legalFsTransition(3, 2, false));
    EXPECT_FALSE(legalFsTransition(1, 2, false));
    EXPECT_FALSE(legalFsTransition(0, 3, false));
}

TEST(DifftestFs, ChecksSummaryAndUnrelatedStatusBits)
{
    EXPECT_FALSE(equivalentFsStatus(status(3) ^ SdMask, status(3)));
    EXPECT_FALSE(equivalentFsStatus(status(2) | SdMask, status(2)));
    EXPECT_FALSE(equivalentFsStatus(status(2, 3), status(3, 2)));
    EXPECT_FALSE(equivalentFsStatus(status(2) ^ 8, status(3)));
    EXPECT_TRUE(validSd(status(0, 3)));
    EXPECT_TRUE(validSd(status(0, 0, 3)));
}

TEST(DifftestFs, ExplicitWritesCanClearDirtyButCannotIgnoreOff)
{
    EXPECT_TRUE(legalFsTransition(3, 0, false, 0));
    EXPECT_FALSE(legalFsTransition(3, 3, false, 0));
    EXPECT_TRUE(legalFsTransition(3, 2, false, 2));
    EXPECT_TRUE(legalFsTransition(3, 3, false, 2));
    EXPECT_FALSE(legalFsTransition(3, 1, false, 2));
    // A write of the old CSR value is still a write (csrrw x0, mstatus, x1).
    EXPECT_EQ(requestedFs(0x30009073, status(2), status(2)), 2);
    EXPECT_EQ(requestedFs(0x30009073, 0, status(3)), 0);
    // CSRRS/CSRRC with rs1=x0 are reads; CSRRS x1 sets the FS bits.
    EXPECT_FALSE(requestedFs(0x30002073, 0, status(2)).has_value());
    EXPECT_FALSE(requestedFs(0x10003073, 0, status(2)).has_value());
    EXPECT_EQ(requestedFs(0x1000a073, FsMask, status(0)), 3);
    EXPECT_EQ(requestedFs(0x1000b073, FsMask, status(3)), 0);
    EXPECT_EQ(requestedFs(0x30005073, 0, status(3)), 0);
    EXPECT_FALSE(requestedFs(0x00309073, 0, status(2)).has_value());
    EXPECT_FALSE(requestedFs(0x30200073, 0, status(2)).has_value());
}

TEST(DifftestFs, RecognizesFpAccessWithOff)
{
    EXPECT_TRUE(accessesFp(0, true));
    EXPECT_TRUE(accessesFp(0x00302073, false));
    EXPECT_TRUE(accessesFp(0x00001057, false));
    EXPECT_TRUE(accessesFp(0x00005057, false));
    EXPECT_FALSE(accessesFp(0x30002073, false));
    EXPECT_FALSE(accessesFp(0x00007057, false));
}

} // anonymous namespace
} // namespace difftest
} // namespace gem5
