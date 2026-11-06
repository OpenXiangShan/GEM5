/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CPU_O3_FP_WAKEUP_HH__
#define __CPU_O3_FP_WAKEUP_HH__

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <utility>
#include <vector>


namespace gem5
{
namespace o3
{
namespace fpw
{

using Cycle = uint64_t;
constexpr Cycle Never = std::numeric_limits<Cycle>::max();

enum class Phase { M2, M4 };
enum class ReplayReason { MissingConfirmation, LoadCancel, ResourceReject };

/** An execution attempt, including the destination physical register. */
struct Identity
{
    uint32_t tid = 0;
    uint64_t seq = 0;
    uint64_t attempt = 0;
    uint32_t preg = 0;

    bool operator==(const Identity &other) const;
    bool operator!=(const Identity &other) const;
};

/** Timing promises survive confirmation; cancellation is terminal. */
struct WakeToken
{
    Identity producer;
    unsigned wbPort = 0;
    unsigned outputLane = 0;
    Cycle m4Cycle = Never;
    Cycle m2Cycle = Never;
    Cycle readyCycle = Never;
    Cycle wbCycle = Never;
    Cycle cancelSafeCycle = Never;
    bool load = false;
    bool confirmed = false;
    bool dataReady = false;
    bool cancelled = false;

    void confirm();
    void cancel();
    bool canUse(Cycle deadline) const;
    bool canInherit(Cycle now) const;
};

struct SourceBinding
{
    std::shared_ptr<WakeToken> token;
    Phase phase = Phase::M2;
    Cycle wakeCycle = Never;
    bool lateUse = false;

    void observeM2(bool launched);
};

struct InstState
{
    uint64_t attempt = 1;
    Cycle enqueued = Never;
    Cycle selected = Never;
    Cycle atFu = Never;
    // An EX0 busy response makes this IQ entry selectable next cycle.
    // Keep the deadline across attempt invalidation in the rejection cycle.
    Cycle retrySelectCycle = 0;
    bool ownsIqSlot = false;
    bool managed = false;
    bool validationDone = false;
    bool divResource = false;
    std::vector<SourceBinding> sources;
    std::shared_ptr<WakeToken> output;
};

/**
 * Sparse calendar for configured writeback ports. The caller reserves only for
 * live instruction destinations, so the live bound is the configured in-flight
 * instruction count times its maximum destination count. Release on replay or
 * squash, and advance every cycle; no history is retained after a slot expires.
 */
class WritebackReservations
{
  public:
    bool reserve(unsigned port, Cycle when, const Identity &owner);
    void release(const Identity &owner);
    void advance(Cycle now);
    bool empty() const;
    std::size_t size() const { return reservations.size(); }

  private:
    // Time first makes expiration proportional to the expired prefix.
    std::map<std::pair<Cycle, unsigned>, Identity> reservations;
};

/**
 * Exclusive resources, keyed by a configured functional-unit index. The live
 * bound is the configured number of exclusive execution resources. The caller
 * releases an owner at completion, replay, or squash.
 */
class ExecutionResources
{
  public:
    bool acquire(unsigned resource, const Identity &owner);
    void release(const Identity &owner);
    bool busy(unsigned resource) const;
    bool empty() const;
    std::size_t size() const { return owners.size(); }

  private:
    std::map<unsigned, Identity> owners;
};

} // namespace fpw
} // namespace o3
} // namespace gem5

#endif // __CPU_O3_FP_WAKEUP_HH__
