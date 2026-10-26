/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu/o3/fp_load_m4.hh"

#include <algorithm>
#include <stdexcept>

namespace gem5
{
namespace o3
{
namespace fpw
{

LoadM4Predictor::LoadM4Predictor(unsigned entries, unsigned failurePenalty,
                               unsigned enableThreshold,
                               unsigned disableThreshold)
    : entries(entries), failurePenalty(failurePenalty),
      enableThreshold(enableThreshold), disableThreshold(disableThreshold)
{
    if (entries == 0 || (entries & (entries - 1)) != 0) {
        throw std::invalid_argument(
            "load M4 predictor entries must be a nonzero power of two");
    }
    if (failurePenalty == 0 || failurePenalty > 7) {
        throw std::invalid_argument(
            "load M4 failure penalty must be in [1, 7]");
    }
    if (enableThreshold > 7 || disableThreshold >= enableThreshold) {
        throw std::invalid_argument(
            "load M4 thresholds must satisfy 0 <= disable < enable <= 7");
    }
}

LoadM4Predictor::Decision
LoadM4Predictor::predict(uint32_t tid, uint64_t pc)
{
    auto found = tables.find(tid);
    if (found == tables.end())
        found = tables.emplace(tid, std::vector<Entry>(entries)).first;

    // RISC-V instructions may start on a two-byte boundary.
    auto &entry = found->second[(pc >> 1) & (entries - 1)];
    const bool cold = !entry.valid || entry.pc != pc;
    const bool replaced = cold && entry.valid;
    if (cold) {
        entry = Entry{};
        entry.pc = pc;
        entry.generation = nextGeneration++;
        entry.valid = true;
    }
    return {tid, pc, entry.generation, entry.enabled, cold, replaced};
}

LoadM4Predictor::Update
LoadM4Predictor::train(const Decision &decision, bool success)
{
    const auto found = tables.find(decision.tid);
    if (found == tables.end())
        return {};

    auto &entry = found->second[(decision.pc >> 1) & (entries - 1)];
    if (!entry.valid || entry.pc != decision.pc ||
        entry.generation != decision.generation) {
        return {};
    }

    const bool wasEnabled = entry.enabled;
    if (success) {
        entry.confidence = std::min(7U, entry.confidence + 1);
    } else {
        entry.confidence = entry.confidence > failurePenalty ?
            entry.confidence - failurePenalty : 0;
    }
    if (entry.confidence >= enableThreshold)
        entry.enabled = true;
    else if (entry.confidence <= disableThreshold)
        entry.enabled = false;

    return {true, entry.enabled, !wasEnabled && entry.enabled,
            wasEnabled && !entry.enabled, entry.confidence};
}

void
LoadM4Predictor::clear()
{
    tables.clear();
}

bool
LoadM4Sample::admit()
{
    if (admitted || outcome != LoadM4Outcome::Pending)
        return false;
    admitted = true;
    return true;
}

bool
LoadM4Sample::resolve(LoadM4Outcome result)
{
    if (outcome != LoadM4Outcome::Pending || result == LoadM4Outcome::Pending)
        return false;
    if (result != LoadM4Outcome::Aborted && !admitted)
        return false;
    outcome = result;
    return true;
}

bool
LoadM4Sample::noteDeadlineFailure()
{
    deadlineFailed = true;
    if (!m4Sent || outcome != LoadM4Outcome::Success || deadlineCounted)
        return false;
    deadlineCounted = true;
    return true;
}

} // namespace fpw
} // namespace o3
} // namespace gem5
