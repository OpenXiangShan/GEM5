/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu/o3/fp_wakeup.hh"

#include <tuple>

namespace gem5
{
namespace o3
{
namespace fpw
{

bool
Identity::operator==(const Identity &other) const
{
    return std::tie(tid, seq, attempt, preg) ==
           std::tie(other.tid, other.seq, other.attempt, other.preg);
}

bool
Identity::operator!=(const Identity &other) const
{
    return !(*this == other);
}

void
WakeToken::confirm()
{
    if (!cancelled)
        confirmed = true;
}

void
WakeToken::cancel()
{
    cancelled = true;
    confirmed = false;
}

bool
WakeToken::canUse(Cycle deadline) const
{
    return !cancelled && readyCycle != Never && readyCycle <= deadline;
}

bool
WakeToken::canInherit(Cycle now) const
{
    return !cancelled && confirmed && m2Cycle != Never &&
        now >= m2Cycle;
}

void
SourceBinding::observeM2(bool launched)
{
    if (!launched)
        lateUse = false;
}

bool
WritebackReservations::reserve(unsigned port, Cycle when, const Identity &owner)
{
    const auto result = reservations.emplace(std::make_pair(when, port), owner);
    return result.second || result.first->second == owner;
}

void
WritebackReservations::release(const Identity &owner)
{
    for (auto it = reservations.begin(); it != reservations.end();) {
        if (it->second == owner)
            it = reservations.erase(it);
        else
            ++it;
    }
}

void
WritebackReservations::advance(Cycle now)
{
    reservations.erase(reservations.begin(),
                       reservations.lower_bound(std::make_pair(now, 0U)));
}

bool
WritebackReservations::empty() const
{
    return reservations.empty();
}

bool
ExecutionResources::acquire(unsigned resource, const Identity &owner)
{
    const auto result = owners.emplace(resource, owner);
    return result.second || result.first->second == owner;
}

void
ExecutionResources::release(const Identity &owner)
{
    for (auto it = owners.begin(); it != owners.end();) {
        if (it->second == owner)
            it = owners.erase(it);
        else
            ++it;
    }
}

bool
ExecutionResources::busy(unsigned resource) const
{
    return owners.find(resource) != owners.end();
}

bool
ExecutionResources::empty() const
{
    return owners.empty();
}

} // namespace fpw
} // namespace o3
} // namespace gem5
