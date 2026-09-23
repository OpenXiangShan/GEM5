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

#ifndef __MEM_CACHE_PARTIAL_STORE_PREDICTOR_HH__
#define __MEM_CACHE_PARTIAL_STORE_PREDICTOR_HH__

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gem5
{

class PartialStorePredictor
{
  public:
    struct Update
    {
        bool changed;
        bool skipDataFetch;
    };

    PartialStorePredictor(unsigned window_size, unsigned min_samples,
                          unsigned enter_skip_percent,
                          unsigned exit_skip_percent);

    bool predictSkipDataFetch() const { return skipDataFetch; }
    Update train(bool store_completed_line);

    unsigned samples() const { return sampleCount; }
    unsigned positives() const { return positiveCount; }

  private:
    std::vector<uint8_t> history;
    const unsigned minSamples;
    const unsigned enterSkipPercent;
    const unsigned exitSkipPercent;
    size_t nextEntry = 0;
    unsigned sampleCount = 0;
    unsigned positiveCount = 0;
    bool skipDataFetch = false;
};

} // namespace gem5

#endif // __MEM_CACHE_PARTIAL_STORE_PREDICTOR_HH__
