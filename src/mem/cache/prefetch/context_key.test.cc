/*
 * Copyright (c) 2026 XiangShan
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

#include "mem/cache/prefetch/context_key.hh"

namespace gem5
{
namespace prefetch
{

TEST(ContextKey, PreservesSingleThreadIndexing)
{
    constexpr Addr address = 0x123456789abcULL;
    EXPECT_EQ(contextKey(address, 0), address);
    EXPECT_EQ(contextKey(address, InvalidContextID), address);
}

TEST(ContextKey, SeparatesSameVirtualAddressAcrossContexts)
{
    constexpr Addr address = 0x123456789abcULL;
    EXPECT_NE(contextKey(address, 0), contextKey(address, 1));
    EXPECT_NE(contextKey(address, 1), contextKey(address, 2));
}

TEST(ContextKey, IsStableWithinAContext)
{
    constexpr Addr address = 0x123456789abcULL;
    EXPECT_EQ(contextKey(address, 7), contextKey(address, 7));
}

TEST(ContextKey, StepAndStreamShareTheBlockFilterKey)
{
    // STEP candidates and stream candidates are both inserted into the
    // composite prefetcher's shared block filter.  The source type is
    // metadata only; it must not create a second key namespace.
    constexpr Addr block = 0x4000;
    constexpr ContextID context_id = 3;

    const Addr stream_key = contextKey(block, context_id);
    const Addr step_key = contextKey(block, context_id);

    EXPECT_EQ(step_key, stream_key);
}

TEST(ContextKey, SharedBlockFilterSeparatesContexts)
{
    constexpr Addr block = 0x4000;

    // A STEP candidate from another context must not be suppressed by a
    // stream candidate from context 1 (and vice versa).
    EXPECT_NE(contextKey(block, 1), contextKey(block, 2));
}

TEST(ContextKey, SharedBlockFilterAlignsFillAndCandidate)
{
    constexpr Addr block = 0x4000;
    constexpr Addr fill = block + 0x23;
    constexpr ContextID context_id = 3;

    // Cache fills may preserve a byte offset, while candidates are generated
    // per cache line. Both paths must qualify the aligned line address.
    EXPECT_EQ(contextKey(block, context_id),
              contextKey(fill & ~Addr(63), context_id));
}

}  // namespace prefetch
}  // namespace gem5
