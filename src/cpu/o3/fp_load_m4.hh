/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CPU_O3_FP_LOAD_M4_HH__
#define __CPU_O3_FP_LOAD_M4_HH__

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gem5
{
namespace o3
{
namespace fpw
{

/**
 * A bounded PC predictor per caller-supplied thread. Prediction allocates cold
 * entries; training never allocates, so delayed feedback cannot evict live PCs.
 * This is an execution-time predictor: the caller resolves each dynamic load's
 * initial attempt once, independently of its wakeup token and later replays.
 */
class LoadM4Predictor
{
  public:
    struct Decision
    {
        uint32_t tid = 0;
        uint64_t pc = 0;
        uint64_t generation = 0;
        bool enabled = false;
        bool cold = false;
        bool replaced = false;
    };

    struct Update
    {
        bool applied = false;
        bool enabled = false;
        bool enabledTransition = false;
        bool disabledTransition = false;
        unsigned confidence = 0;
    };

    LoadM4Predictor(unsigned entries = 256, unsigned failurePenalty = 3,
                    unsigned enableThreshold = 6,
                    unsigned disableThreshold = 3);

    Decision predict(uint32_t tid, uint64_t pc);
    Update train(const Decision &decision, bool success);
    void clear();

  private:
    struct Entry
    {
        uint64_t pc = 0;
        uint64_t generation = 0;
        unsigned confidence = 0;
        bool valid = false;
        bool enabled = false;
    };

    const unsigned entries;
    const unsigned failurePenalty;
    const unsigned enableThreshold;
    const unsigned disableThreshold;
    // Retain the generation source across clear() to reject pre-reset samples.
    uint64_t nextGeneration = 1;
    std::unordered_map<uint32_t, std::vector<Entry>> tables;
};

enum class LoadM4Outcome { Pending, Success, Failure, Aborted };

/** Dynamic-load identity survives invalidation of its speculative wake token. */
struct LoadM4Sample
{
    LoadM4Predictor::Decision decision;
    LoadM4Outcome outcome = LoadM4Outcome::Pending;
    bool admitted = false;
    bool m4Sent = false;
    bool deadlineFailed = false;
    bool deadlineCounted = false;

    bool admit();
    bool resolve(LoadM4Outcome result);
    bool noteDeadlineFailure();
};

} // namespace fpw
} // namespace o3
} // namespace gem5

#endif // __CPU_O3_FP_LOAD_M4_HH__
