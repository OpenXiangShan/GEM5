/*
 * Copyright (c) 2026 OpenXiangShan
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

#include "arch/riscv/regs/vector.hh"
#include "arch/riscv/types.hh"
#include "arch/riscv/utility.hh"

using namespace gem5;
using namespace gem5::RiscvISA;

namespace
{

VTYPE
makeVtype(uint8_t vsew, uint8_t vlmul)
{
    VTYPE vtype = 0;
    vtype.vsew = vsew;
    vtype.vlmul = vlmul;
    return vtype;
}

TEST(RiscvVlenTest, MaxContainerCoversSupportedVlens)
{
    EXPECT_EQ(MaxVecLenInBits, 512);
    EXPECT_EQ(MaxVecLenInBytes, 64);
    EXPECT_GE(MaxVecLenInBits, DefaultVecLenInBits);
    EXPECT_EQ(DefaultVecLenInBits, 128);
}

TEST(RiscvVlenTest, GetVlmaxScalesWithVlen)
{
    // SEW=8 (vsew=0), LMUL=1 (vlmul=0)
    EXPECT_EQ(getVlmax(makeVtype(0, 0), 128), 16u);
    EXPECT_EQ(getVlmax(makeVtype(0, 0), 256), 32u);
    EXPECT_EQ(getVlmax(makeVtype(0, 0), 512), 64u);

    // SEW=64 (vsew=3), LMUL=2 (vlmul=1)
    EXPECT_EQ(getVlmax(makeVtype(3, 1), 128), 4u);
    EXPECT_EQ(getVlmax(makeVtype(3, 1), 256), 8u);
    EXPECT_EQ(getVlmax(makeVtype(3, 1), 512), 16u);

    // SEW=16 (vsew=1), LMUL=1/8 (vlmul=5)
    EXPECT_EQ(getVlmax(makeVtype(1, 5), 128), 1u);
    EXPECT_EQ(getVlmax(makeVtype(1, 5), 256), 2u);
    EXPECT_EQ(getVlmax(makeVtype(1, 5), 512), 4u);
}

TEST(RiscvVlenTest, VtypeVlmaxMatchesGetVlmax)
{
    for (uint32_t vlen : {128u, 256u, 512u}) {
        for (uint8_t vsew = 0; vsew < 4; ++vsew) {
            for (uint8_t vlmul : {0, 1, 2, 3, 5, 6, 7}) {
                VTYPE vtype = makeVtype(vsew, vlmul);
                EXPECT_EQ(getVlmax(vtype, vlen),
                          vtype_VLMAX(vtype, vlen));
            }
        }
    }
}

TEST(RiscvVlenTest, VecRegContainerUsesMaxWidth)
{
    // Prefer the ISA typedef over the generic template name (ambiguous under
    // `using namespace gem5` + `using namespace gem5::RiscvISA`).
    EXPECT_EQ(sizeof(RiscvISA::VecRegContainer), MaxVecLenInBytes);
}

} // namespace
