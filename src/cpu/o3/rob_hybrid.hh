/*
 * Copyright (c) 2012 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#ifndef __CPU_O3_ROB_HYBRID_HH__
#define __CPU_O3_ROB_HYBRID_HH__

#include <cassert>
#include <cstdint>
#include <vector>

#include "cpu/inst_seq.hh"

namespace gem5
{

namespace o3
{

namespace hybrid_rob
{

/** Types exist only in a batch plan and allocation statistics. */
enum class GroupType
{
    NormalS,
    NormalC,
    NormalN,
    CC,
    CS,
    SC,
    NumTypes
};

struct Group
{
    unsigned formerLength;
    unsigned latterLength;
    GroupType type;

    unsigned memberCount() const { return formerLength + latterLength; }
    bool hasLatter() const { return latterLength != 0; }
};
using Plan = std::vector<Group>;

enum class EntryType { NORMAL, CC, CS, SC };

struct EntryState
{
    uint64_t id;
    EntryType type;
    unsigned formerRemaining;
    unsigned latterRemaining;

    unsigned memberCount() const
    { return formerRemaining + latterRemaining; }

    // Returns true only for a surviving-former downgrade.
    bool removeMember(bool former, bool retain_former)
    {
        auto &remaining = former ? formerRemaining : latterRemaining;
        assert(remaining > 0);
        --remaining;
        if (!former && retain_former && latterRemaining == 0 &&
            formerRemaining != 0 && type != EntryType::NORMAL) {
            type = EntryType::NORMAL;
            return true;
        }
        return false;
    }
};

struct SquashTarget
{
    uint64_t entryId;
    InstSeqNum targetSeqNum;
    bool flushItself;
    bool retainFormer;

    /** Remove younger entries and the suffix of the target entry. */
    bool removes(uint64_t id, InstSeqNum seq_num) const
    {
        if (id != entryId) {
            return id > entryId;
        }
        if (seq_num != targetSeqNum) {
            return seq_num > targetSeqNum;
        }
        return flushItself;
    }
};

enum class InstClass { Simple, Complex, NoCompress };

/** One constant-time transition of the temporary batch planner. */
inline void
appendInstClass(Plan &plan, InstClass inst_class, unsigned group_limit)
{
    assert(group_limit > 0);
    bool append = false;
    if (!plan.empty() && plan.back().memberCount() < group_limit) {
        auto &group = plan.back();
        switch (group.type) {
          case GroupType::NormalS:
            if (inst_class == InstClass::Simple) {
                append = true;
            } else if (inst_class == InstClass::Complex) {
                group.type = GroupType::SC;
                append = true;
            }
            break;
          case GroupType::NormalC:
            if (inst_class == InstClass::Simple) {
                group.type = GroupType::CS;
                append = true;
            } else if (inst_class == InstClass::Complex) {
                group.type = GroupType::CC;
                append = true;
            }
            break;
          case GroupType::CS:
            append = inst_class == InstClass::Simple;
            break;
          default:
            break;
        }
    }

    if (append) {
        auto &group = plan.back();
        if (group.type == GroupType::NormalS) {
            ++group.formerLength;
        } else {
            ++group.latterLength;
        }
    } else {
        const auto type = inst_class == InstClass::Simple ?
            GroupType::NormalS :
            inst_class == InstClass::Complex ?
            GroupType::NormalC : GroupType::NormalN;
        plan.push_back({1, 0, type});
    }
}

} // namespace hybrid_rob
} // namespace o3
} // namespace gem5

#endif // __CPU_O3_ROB_HYBRID_HH__
