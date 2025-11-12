/**
 * Copyright (c) 2019, 2020 Inria
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

#include "mem/cache/replacement_policies/dueling_rp.hh"

#include "base/logging.hh"
#include "params/DuelingRP.hh"

namespace gem5
{

namespace replacement_policy
{

Dueling::Dueling(const Params &p)
  : Base(p),
    num_slices(p.num_slices),
    num_sets_per_slice(p.num_sets_per_slice),
    num_ways_per_slice(p.num_ways),
    replPolicyA(p.replacement_policy_a),
    replPolicyB(p.replacement_policy_b),
    duelingMonitor(p.constituency_size, p.team_size),
    duelingStats(this)
{
    fatal_if((replPolicyA == nullptr) || (replPolicyB == nullptr),
        "All replacement policies must be instantiated");

    int set_bits = floorLog2(num_sets_per_slice); // 9
    set_shift = set_bits - set_bits / 2 - 1; // 9 - 4 - 1 = 4
    set_mask = (1 << (set_bits - set_bits/2)) - 1; // (1 << 5) - 1 = 0b11111
}

void
Dueling::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    std::shared_ptr<DuelerReplData> casted_replacement_data =
        std::static_pointer_cast<DuelerReplData>(replacement_data);

    auto duler = static_cast<Dueler*>(std::static_pointer_cast<DuelerReplData>(replacement_data).get());
    bool team_a = getReplType(duler);
    if (team_a) {
        replPolicyA->invalidate(casted_replacement_data->replData);
    } else {
        replPolicyB->invalidate(casted_replacement_data->replData);
    }
}

void
Dueling::touch(const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    std::shared_ptr<DuelerReplData> casted_replacement_data =
        std::static_pointer_cast<DuelerReplData>(replacement_data);

    auto duler = static_cast<Dueler*>(std::static_pointer_cast<DuelerReplData>(replacement_data).get());
    bool team_a = getReplType(duler);
    if (team_a) {
        replPolicyA->touch(casted_replacement_data->replData, pkt);
    } else {
        replPolicyB->touch(casted_replacement_data->replData, pkt);
    }
}

void
Dueling::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    std::shared_ptr<DuelerReplData> casted_replacement_data =
        std::static_pointer_cast<DuelerReplData>(replacement_data);

    auto duler = static_cast<Dueler*>(std::static_pointer_cast<DuelerReplData>(replacement_data).get());
    bool team_a = getReplType(duler);
    if (team_a) {
        replPolicyA->touch(casted_replacement_data->replData);
    } else {
        replPolicyB->touch(casted_replacement_data->replData);
    }
}

void
Dueling::reset(const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    std::shared_ptr<DuelerReplData> casted_replacement_data =
        std::static_pointer_cast<DuelerReplData>(replacement_data);

    auto duler = static_cast<Dueler*>(std::static_pointer_cast<DuelerReplData>(replacement_data).get());
    bool team_a = getReplType(duler);
    if (team_a) {
        replPolicyA->reset(casted_replacement_data->replData, pkt);
    } else {
        replPolicyB->reset(casted_replacement_data->replData, pkt);
    }

    // A miss in a set is a sample to the duel. A call to this function
    // implies in the replacement of an entry, which was either caused by
    // a miss, an external invalidation, or the initialization of the table
    // entry (when warming up)
    duelingMonitor.sample(static_cast<Dueler*>(casted_replacement_data.get()));
}

void
Dueling::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    std::shared_ptr<DuelerReplData> casted_replacement_data =
        std::static_pointer_cast<DuelerReplData>(replacement_data);

    auto duler = static_cast<Dueler*>(std::static_pointer_cast<DuelerReplData>(replacement_data).get());
    bool team_a = getReplType(duler);
    if (team_a) {
        replPolicyA->reset(casted_replacement_data->replData);
    } else {
        replPolicyB->reset(casted_replacement_data->replData);
    }

    // A miss in a set is a sample to the duel. A call to this function
    // implies in the replacement of an entry, which was either caused by
    // a miss, an external invalidation, or the initialization of the table
    // entry (when warming up)
    duelingMonitor.sample(static_cast<Dueler*>(casted_replacement_data.get()));
}

ReplaceableEntry*
Dueling::getVictim(const ReplacementCandidates& candidates) const
{
    // This function assumes that all candidates are either part of the same
    // sampled set, or are not samples.
    // @todo This should be improved at some point.
    panic_if(candidates.size() != params().team_size, "We currently only "
        "support team sizes that match the number of replacement candidates");

    // // If the entry is a sample, it can only be used with a certain policy.
    auto duler = static_cast<Dueler*>(
        std::static_pointer_cast<DuelerReplData>(
            candidates[0]->replacementData).get());

    bool team_a = getReplType(duler);

    if (team_a) {
        duelingStats.selectedA++;
        team_a = true;
    } else {
        duelingStats.selectedB++;
        team_a = false;
    }

    // Create a temporary list of replacement candidates which re-routes the
    // replacement data of the selected team
    std::vector<std::shared_ptr<ReplacementData>> dueling_replacement_data;
    for (auto& candidate : candidates) {
        std::shared_ptr<DuelerReplData> dueler_repl_data =
            std::static_pointer_cast<DuelerReplData>(
            candidate->replacementData);

        // Copy the original entry's data, re-routing its replacement data
        // to the selected one
        dueling_replacement_data.push_back(dueler_repl_data);
        candidate->replacementData = dueler_repl_data->replData;
    }

    // Use the selected replacement policy to find the victim
    ReplaceableEntry* victim = team_a ? replPolicyA->getVictim(candidates) :
        replPolicyB->getVictim(candidates);

    // Search for entry within the original candidates and clean-up duplicates
    for (int i = 0; i < candidates.size(); i++) {
        candidates[i]->replacementData = dueling_replacement_data[i];
    }

    return victim;
}

std::shared_ptr<ReplacementData>
Dueling::instantiateEntry()
{
    DuelerReplData* replacement_data = new DuelerReplData(replPolicyA->instantiateEntry());

    int set_index = block_num / num_ways_per_slice;
    block_num++;
    bool match_a = (set_index >> set_shift) == (set_index & set_mask);
    bool match_b = (set_index >> set_shift) == ((~set_index) & set_mask);
    auto dueler =static_cast<Dueler*>(replacement_data);

    uint64_t id = duelingMonitor.getID();
    if (match_a) {
        dueler->setSample(id, true);
    } else if (match_b) {
        dueler->setSample(id, false);
    }

    // duelingMonitor.initEntry(static_cast<Dueler*>(replacement_data));
    return std::shared_ptr<DuelerReplData>(replacement_data);
}

bool
Dueling::getReplType(Dueler* dueler) const {
    bool winner = !duelingMonitor.getWinner();

    // // If the entry is a sample, it can only be used with a certain policy.
    bool team;
    bool is_sample = duelingMonitor.isSample(dueler, team);

    bool team_a;
    if (is_sample && team) {
        team_a = true;
    } else if (is_sample) {
        team_a = false;
    } else if (winner) {
        team_a = true;
    } else {
        team_a = false;
    }

    return team_a;
}

Dueling::DuelingStats::DuelingStats(statistics::Group* parent)
  : statistics::Group(parent),
    ADD_STAT(selectedA, "Number of times A was selected to victimize"),
    ADD_STAT(selectedB, "Number of times B was selected to victimize")
{
}

} // namespace replacement_policy
} // namespace gem5
