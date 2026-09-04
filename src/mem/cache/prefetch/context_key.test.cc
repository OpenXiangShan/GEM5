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

}  // namespace prefetch
}  // namespace gem5
