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

#include "mem/cache/partial_store_predictor.hh"

#include <cassert>

namespace gem5
{

PartialStorePredictor::PartialStorePredictor(
    unsigned window_size, unsigned min_samples,
    unsigned enter_skip_percent, unsigned exit_skip_percent)
    : history(window_size, 0), minSamples(min_samples),
      enterSkipPercent(enter_skip_percent),
      exitSkipPercent(exit_skip_percent)
{
}

PartialStorePredictor::Update
PartialStorePredictor::train(bool store_completed_line)
{
    assert(!history.empty());

    if (sampleCount == history.size()) {
        positiveCount -= history[nextEntry];
    } else {
        ++sampleCount;
    }

    history[nextEntry] = store_completed_line;
    positiveCount += store_completed_line;
    nextEntry = (nextEntry + 1) % history.size();

    const bool old_prediction = skipDataFetch;
    if (!skipDataFetch) {
        if (sampleCount >= minSamples &&
            uint64_t(positiveCount) * 100 >=
                uint64_t(enterSkipPercent) * sampleCount) {
            skipDataFetch = true;
        }
    } else if (uint64_t(positiveCount) * 100 <
               uint64_t(exitSkipPercent) * sampleCount) {
        skipDataFetch = false;
    }

    return {old_prediction != skipDataFetch, skipDataFetch};
}

} // namespace gem5
