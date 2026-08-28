#include "cpu/valuepred/ideal_constant_lvp.hh"

#include <algorithm>
#include <cassert>

#include "base/output.hh"
#include "base/stats/units.hh"
#include "cpu/valuepred/valuepred_metadata.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

namespace valuepred
{

IdealConstantLVP::IdealConstantLVP(const Params &params)
    : VPUnit(params),
      idealConstTables(params.numThreads),
      lifetimeProfileTables(params.numThreads),
      roiProfileTables(params.numThreads),
      lifetimeProfileUpdateSequences(params.numThreads, 0),
      roiProfileUpdateSequences(params.numThreads, 0),
      satCounterBits(params.satCounterBits),
      resetConfidence(params.resetConfidence),
      enableProfiling(params.enableProfiling),
      profileStats(this)
{
    if (enableProfiling) {
        statistics::registerResetCallback([this] { resetRoiProfile(); });
        statistics::registerDumpCallback([this] { refreshProfileStats(); });
        registerExitCallback([this] { dumpProfile(); });
    }
}

IdealConstantLVP::IdealConstantLVPStats::IdealConstantLVPStats(
        statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(profileRoiUpdates, statistics::units::Count::get(),
              "Committed IdealConstantLVP updates after the last stats reset"),
      ADD_STAT(profileRoiDistinctPcs, statistics::units::Count::get(),
              "Distinct committed PCs after the last stats reset"),
      ADD_STAT(profileRoiValueChanges, statistics::units::Count::get(),
              "Committed updates that changed the tracked value after stats reset"),
      ADD_STAT(profileRoiSaturationTransitions, statistics::units::Count::get(),
              "Counter transitions into saturation after the last stats reset"),
      ADD_STAT(profileRoiEverSaturatedPcs, statistics::units::Count::get(),
              "Distinct PCs observed saturated after the last stats reset"),
      ADD_STAT(profileRoiSaturatedAtEndPcs, statistics::units::Count::get(),
              "Profiled PCs still saturated when statistics are dumped"),
      ADD_STAT(profileLifetimePcsAtEnd, statistics::units::Count::get(),
              "All IdealConstantLVP PC entries resident when statistics are dumped"),
      ADD_STAT(profileLifetimeEverSaturatedPcs,
              statistics::units::Count::get(),
              "All distinct PCs that reached saturation since process start"),
      ADD_STAT(profileLifetimeSaturatedPcsAtEnd,
              statistics::units::Count::get(),
              "All IdealConstantLVP entries saturated when statistics are dumped")
{
}

void
IdealConstantLVP::updateProfile(ProfileTable &profile_table, Addr pc,
        uint64_t update_sequence,
        bool value_changed, bool was_saturated, bool is_saturated,
        bool update_roi_stats)
{
    auto [it, inserted] = profile_table.try_emplace(pc);
    auto &profile_entry = it->second;

    if (inserted) {
        profile_entry.firstUpdate = update_sequence;
    }
    profile_entry.updates++;
    profile_entry.lastUpdate = update_sequence;
    if (value_changed) {
        profile_entry.valueChanges++;
    }
    if (!was_saturated && is_saturated) {
        profile_entry.saturationTransitions++;
    }
    if (is_saturated) {
        profile_entry.saturatedUpdates++;
    }

    const bool first_saturated_observation =
        !profile_entry.everSaturated && is_saturated;
    if (first_saturated_observation) {
        profile_entry.firstSaturationUpdate = update_sequence;
    }
    profile_entry.everSaturated |= is_saturated;

    if (!update_roi_stats) {
        return;
    }

    profileStats.profileRoiUpdates++;
    if (inserted) {
        profileStats.profileRoiDistinctPcs++;
    }
    if (value_changed) {
        profileStats.profileRoiValueChanges++;
    }
    if (!was_saturated && is_saturated) {
        profileStats.profileRoiSaturationTransitions++;
    }
    if (first_saturated_observation) {
        profileStats.profileRoiEverSaturatedPcs++;
    }
}

void
IdealConstantLVP::resetRoiProfile()
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        roiProfileTables[tid].clear();
        roiProfileUpdateSequences[tid] = 0;
    }
}

void
IdealConstantLVP::refreshProfileStats()
{
    uint64_t lifetime_pcs = 0;
    uint64_t lifetime_ever_saturated_pcs = 0;
    uint64_t lifetime_saturated_pcs = 0;
    uint64_t roi_saturated_pcs = 0;

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        const auto &ideal_const_table = idealConstTables[tid];
        lifetime_pcs += ideal_const_table.size();
        for (const auto &profile_item : lifetimeProfileTables[tid]) {
            if (profile_item.second.everSaturated) {
                lifetime_ever_saturated_pcs++;
            }
        }
        for (const auto &[pc, entry] : ideal_const_table) {
            if (entry.confidence.isSaturated()) {
                lifetime_saturated_pcs++;
            }
        }

        for (const auto &[pc, profile_entry] : roiProfileTables[tid]) {
            const auto it = ideal_const_table.find(pc);
            if (it != ideal_const_table.end() &&
                    it->second.confidence.isSaturated()) {
                roi_saturated_pcs++;
            }
        }
    }

    profileStats.profileRoiSaturatedAtEndPcs = roi_saturated_pcs;
    profileStats.profileLifetimePcsAtEnd = lifetime_pcs;
    profileStats.profileLifetimeEverSaturatedPcs =
        lifetime_ever_saturated_pcs;
    profileStats.profileLifetimeSaturatedPcsAtEnd = lifetime_saturated_pcs;
}

void
IdealConstantLVP::dumpProfile() const
{
    auto out_handle = simout.create("ideal_constant_lvp_profile.csv", false,
            true);
    auto &out = *out_handle->stream();

    out << "# ideal_constant_lvp_profile_v1\n";
    out << "# sat_counter_bits=" << satCounterBits << "\n";
    out << "# reset_confidence=" << resetConfidence << "\n";
    out << "scope,tid,pc,updates,first_update,last_update,value_changes,"
           "saturation_transitions,saturated_updates,first_saturation_update,"
           "ever_saturated,saturated_at_end,confidence,value\n";

    auto dump_scope = [this, &out](const char *scope,
            const std::vector<ProfileTable> &profile_tables) {
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            std::vector<std::pair<Addr, const ProfileEntry *>> entries;
            entries.reserve(profile_tables[tid].size());
            for (const auto &[pc, profile_entry] : profile_tables[tid]) {
                entries.emplace_back(pc, &profile_entry);
            }
            std::sort(entries.begin(), entries.end(),
                    [](const auto &left, const auto &right) {
                        return left.first < right.first;
                    });

            const auto &ideal_const_table = idealConstTables[tid];
            for (const auto &[pc, profile_entry] : entries) {
                const auto ideal_it = ideal_const_table.find(pc);
                gem5_assert(ideal_it != ideal_const_table.end(),
                        "Profiled PC %#llx is missing from IdealConstantLVP\n",
                        static_cast<unsigned long long>(pc));
                const auto &ideal_entry = ideal_it->second;
                out << scope << ',' << tid << ",0x" << std::hex << pc
                    << std::dec << ',' << profile_entry->updates << ','
                    << profile_entry->firstUpdate << ','
                    << profile_entry->lastUpdate << ','
                    << profile_entry->valueChanges << ','
                    << profile_entry->saturationTransitions << ','
                    << profile_entry->saturatedUpdates << ','
                    << profile_entry->firstSaturationUpdate << ','
                    << profile_entry->everSaturated << ','
                    << ideal_entry.confidence.isSaturated() << ','
                    << static_cast<uint64_t>(ideal_entry.confidence) << ",0x"
                    << std::hex << ideal_entry.value << std::dec << '\n';
            }
        }
    };

    dump_scope("lifetime", lifetimeProfileTables);
    dump_scope("roi", roiProfileTables);
    simout.close(out_handle);
}

VPResult
IdealConstantLVP::doPredict(Addr pc, ThreadID tid) const
{
    assertValidTid(tid);
    const auto &idealConstTable = idealConstTables[tid];
    auto it = idealConstTable.find(pc);
    if (it != idealConstTable.end()) {
        if (it->second.confidence.isSaturated()) {
            return {true, it->second.value};
        }
    }
    return {false, 0};
}

VPPredictionCandidate
IdealConstantLVP::predict(const VPPredictRequest &request)
{
    VPPredictionCandidate candidate;
    candidate.result = doPredict(request.pc, request.tid);
    if (candidate.result.speculative) {
        candidate.record = std::make_unique<VPPredictionRecord>();
        candidate.record->offeredPrediction = true;
        candidate.record->predictedValue = candidate.result.value;
    }
    return candidate;
}

void
IdealConstantLVP::doUpdate(Addr pc, ThreadID tid, RegVal actualValue)
{
    assertValidTid(tid);
    auto &idealConstTable = idealConstTables[tid];
    auto it = idealConstTable.find(pc);
    const bool had_entry = it != idealConstTable.end();
    const bool was_saturated = had_entry && it->second.confidence.isSaturated();
    bool value_changed = false;
    if (it == idealConstTable.end()) {
        // Not found, allocate a new entry
        auto [it, success] = idealConstTable.emplace(std::piecewise_construct,
            std::forward_as_tuple(pc),
            std::forward_as_tuple(satCounterBits, actualValue));

        assert(success);
    } else {
        // Found
        bool validActualValue = actualValue != 0xdeadbeefULL;
        if (validActualValue && actualValue == it->second.value) {
            it->second.confidence++;
        } else {
            value_changed = true;
            if (resetConfidence) {
                it->second.confidence.reset();
            } else {
                it->second.confidence--;
            }
            it->second.value = actualValue;
        }
    }

    if (enableProfiling) {
        const bool is_saturated = idealConstTable.at(pc).confidence.isSaturated();
        updateProfile(lifetimeProfileTables[tid], pc,
                ++lifetimeProfileUpdateSequences[tid], value_changed,
                was_saturated, is_saturated, false);
        updateProfile(roiProfileTables[tid], pc,
                ++roiProfileUpdateSequences[tid], value_changed,
                was_saturated, is_saturated, true);
    }
}

void
IdealConstantLVP::update(const VPUpdateInfo &updateInfo,
        const VPPredictionRecord *record, const VPFeedback &feedback)
{
    (void)record;
    (void)feedback;
    doUpdate(updateInfo.pc, updateInfo.tid, updateInfo.actualValue);
}

void
IdealConstantLVP::specUpdate(const VPSpecUpdateInfo &specUpdateInfo)
{
    (void)specUpdateInfo;
}

void
IdealConstantLVP::squash(ThreadID tid, const uint64_t seq_no)
{
    (void)tid;
    (void)seq_no;
    // Do nothing
}

} // namespace valuepred

} // namespace gem5
