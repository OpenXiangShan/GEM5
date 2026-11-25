#include "mem/cache/prefetch/compound_bop.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <unordered_set>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/stats/group.hh"
#include "debug/CompoundBOP.hh"
#include "mem/cache/base.hh"
#include "params/CompoundBOPPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

namespace
{

uint64_t
lowMask(unsigned bits)
{
    return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
}

uint64_t
foldBitGroups(uint64_t value, unsigned high, unsigned low,
        unsigned group_width)
{
    assert(high >= low);
    assert(group_width > 0);

    uint64_t folded = 0;
    unsigned bit = low;
    while (bit + group_width - 1 <= high) {
        folded ^= (value >> bit) & lowMask(group_width);
        bit += group_width;
    }
    if (bit <= high) {
        folded ^= (value >> bit) & lowMask(high - bit + 1);
    }
    return folded & lowMask(group_width);
}

uint64_t
coveragePct(uint32_t coverage, uint32_t train_count)
{
    if (train_count == 0) {
        return 0;
    }
    return static_cast<uint64_t>(std::round(
        100.0 * static_cast<double>(coverage) / train_count));
}

} // anonymous namespace

size_t
CompoundBOP::RecentKeyHash::operator()(const RecentKey &key) const
{
    uint64_t value = key.addr;
    value ^= static_cast<uint64_t>(static_cast<uint32_t>(key.context)) << 1;
    value ^= static_cast<uint64_t>(key.secure) << 63;
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return static_cast<size_t>(value);
}

size_t
CompoundBOP::familyIndex(Family family)
{
    return static_cast<size_t>(family);
}

CompoundBOP::CompoundBOP(const CompoundBOPPrefetcherParams &p)
    : Queued(p),
      teacherTestsPerAccess(p.teacher_tests_per_access),
      teacherScoreMax(p.teacher_score_max),
      teacherRoundMax(p.teacher_round_max),
      teacherFallbackBadScore(p.teacher_fallback_bad_score),
      teacherMinNominationScore(p.teacher_min_nomination_score),
      rrEntries(p.rr_size),
      tagMask(lowMask(p.tag_bits)),
      rrDelayQueueSize(p.delay_queue_size),
      rrDelayTicks(cyclesToTicks(p.delay_queue_cycles)),
      crossPage(p.cross_page),
      studentVBOPSlots(p.student_vbop_slots),
      studentPBOPSlots(p.student_pbop_slots),
      studentConfAlpha(p.student_conf_alpha),
      studentFilterEntries(p.student_filter_entries),
      studentHashAddrBits(p.student_hash_addr_bits),
      studentPrimaryThresholdPct(
          p.student_primary_coverage_threshold_pct),
      studentExtraThresholdPct(
          p.student_extra_marginal_threshold_pct),
      issueConfidenceThreshold(p.issue_confidence_threshold),
      issueConfidenceMax(p.issue_confidence_max),
      issueConfidencePassDecrement(
          p.issue_confidence_pass_decrement),
      issueConfidenceFailIncrement(p.issue_confidence_fail_increment),
      recentFilterEntries(p.recent_filter_entries),
      regroupOverlapPct(p.regroup_overlap_pct),
      assignments(p.vbop_offsets.size() + p.pbop_offsets.size(),
          p.regroup_hold_phases),
      rrDelayQueueEvent(
          [this] { rrDelayQueueEventWrapper(); }, name() + ".rrDelay"),
      studentDelayQueues(p.student_delay_queue_size,
          cyclesToTicks(p.student_delay_cycles)),
      primaryFifo(p.primary_fifo_entries),
      extraFifo(p.extra_fifo_entries),
      outputArbiter(p.primary_arbiter_slots, p.extra_arbiter_slots),
      stats(this, p.teacher_round_max,
          p.student_vbop_slots + p.student_pbop_slots,
          FamilyCount * p.student_delay_queue_size)
{
    fatal_if(teacherTestsPerAccess == 0,
        "%s: teacher_tests_per_access must be non-zero", name());
    fatal_if(teacherScoreMax == 0 || teacherRoundMax == 0,
        "%s: Teacher score and round thresholds must be non-zero", name());
    fatal_if(!isPowerOf2(rrEntries),
        "%s: rr_size must be a power of two", name());
    fatal_if(rrDelayQueueSize == 0,
        "%s: delay_queue_size must be non-zero", name());
    fatal_if(studentVBOPSlots == 0 || studentPBOPSlots == 0,
        "%s: both Student family partitions must be non-zero", name());
    fatal_if(studentVBOPSlots + studentPBOPSlots > 64,
        "%s: Student slots exceed the 64-bit filter mask", name());
    fatal_if(!isPowerOf2(studentFilterEntries) ||
            studentFilterEntries < 1024,
        "%s: student_filter_entries must be a power of two >= 1024",
        name());
    fatal_if(studentHashAddrBits <= lBlkSize + 6 ||
            studentHashAddrBits > 64,
        "%s: invalid pairs_low9 address width", name());
    fatal_if(p.student_delay_queue_size == 0,
        "%s: student_delay_queue_size must be non-zero", name());
    fatal_if(studentConfAlpha < 0.0 || studentConfAlpha > 1.0,
        "%s: student_conf_alpha must be in [0, 1]", name());
    fatal_if(regroupOverlapPct > 100 || studentPrimaryThresholdPct > 100 ||
            studentExtraThresholdPct > 100,
        "%s: Student coverage thresholds must be percentages", name());
    fatal_if(issueConfidenceMax > 15 ||
            issueConfidenceThreshold > issueConfidenceMax + 1,
        "%s: family issue confidence must fit in four bits", name());
    fatal_if(p.primary_fifo_entries == 0 || p.extra_fifo_entries == 0,
        "%s: Primary and Extra FIFO capacities must be non-zero", name());
    fatal_if(p.primary_arbiter_slots == 0 || p.extra_arbiter_slots == 0,
        "%s: arbitration slot counts must be non-zero", name());

    fatal_if(p.vbop_offsets.empty() || p.pbop_offsets.empty(),
        "%s: both Teacher offset families must be non-empty", name());

    std::unordered_set<int64_t> vbop_offsets;
    std::unordered_set<int64_t> pbop_offsets;
    for (const auto offset : p.vbop_offsets) {
        fatal_if(offset == 0, "%s: VBOP offset cannot be zero", name());
        fatal_if(!vbop_offsets.insert(offset).second,
            "%s: duplicate VBOP family offset %d", name(), offset);
    }
    for (const auto offset : p.pbop_offsets) {
        fatal_if(offset == 0, "%s: PBOP offset cannot be zero", name());
        fatal_if(!pbop_offsets.insert(offset).second,
            "%s: duplicate PBOP family offset %d", name(), offset);
        fatal_if(vbop_offsets.count(offset),
            "%s: offset %d belongs to both BOP families", name(), offset);
    }

    const auto merged_offsets = compound_bop::mergeFamilyOffsets(
        p.vbop_offsets, p.pbop_offsets);
    teacherOffsets.reserve(merged_offsets.size());
    for (const auto &entry : merged_offsets) {
        teacherOffsets.push_back({
            entry.offset, 0, entry.family});
    }

    rrLeft.resize(rrEntries);
    rrRight.resize(rrEntries);

    const unsigned student_slots = studentVBOPSlots + studentPBOPSlots;
    studentEntries.resize(student_slots);
    for (unsigned slot = 0; slot < student_slots; ++slot) {
        studentEntries[slot].family = slot < studentVBOPSlots ?
            Family::VBOP : Family::PBOP;
    }
    studentFilter.assign(studentFilterEntries, 0);
    crossFamilyOverlap.assign(studentVBOPSlots * studentPBOPSlots, 0);
}

size_t
CompoundBOP::offsetIndex(int64_t offset) const
{
    for (size_t i = 0; i < teacherOffsets.size(); ++i) {
        if (teacherOffsets[i].offset == offset) {
            return i;
        }
    }
    panic("%s: offset %ld is not in the merged Teacher list", name(), offset);
}

CompoundBOP::Family
CompoundBOP::offsetFamily(int64_t offset) const
{
    const auto id = offsetIndex(offset);
    return assignments.family(id, teacherOffsets[id].home);
}

unsigned
CompoundBOP::familySlotBegin(Family family) const
{
    return family == Family::VBOP ? 0 : studentVBOPSlots;
}

unsigned
CompoundBOP::familySlotEnd(Family family) const
{
    return family == Family::VBOP ?
        studentVBOPSlots : studentVBOPSlots + studentPBOPSlots;
}

unsigned
CompoundBOP::studentOccupancy() const
{
    return std::count_if(studentEntries.begin(), studentEntries.end(),
        [](const StudentEntry &entry) { return entry.valid; });
}

Addr
CompoundBOP::rrTag(Addr addr) const
{
    const unsigned rr_index_bits = floorLog2(rrEntries);
    const Addr line_addr = addr >> lBlkSize;
    return (line_addr >> rr_index_bits) & tagMask;
}

unsigned
CompoundBOP::rrIndex(Addr addr) const
{
    const unsigned rr_index_bits = floorLog2(rrEntries);
    const Addr line_addr = addr >> lBlkSize;
    const Addr mask = rrEntries - 1;
    return static_cast<unsigned>(
        ((line_addr & mask) ^ ((line_addr >> rr_index_bits) & mask)) & mask);
}

bool
CompoundBOP::testRR(Addr addr) const
{
    const unsigned index = rrIndex(addr);
    const Addr tag = rrTag(addr);
    return rrLeft[index].hashAddr == tag || rrRight[index].hashAddr == tag;
}

void
CompoundBOP::insertIntoRR(const RREntry &entry)
{
    rrLeft[rrIndex(entry.fullAddr)] = entry;
    rrInsertCount++;
}

void
CompoundBOP::enqueueRR(Addr addr)
{
    if (rrDelayQueue.size() >= rrDelayQueueSize) {
        stats.rrDelayQueueFullDropCount++;
        return;
    }

    const Tick ready_tick = curTick() + rrDelayTicks;
    rrDelayQueue.push_back({{addr, rrTag(addr)}, ready_tick});
    if (!rrDelayQueueEvent.scheduled()) {
        schedule(rrDelayQueueEvent, ready_tick);
    }
}

void
CompoundBOP::rrDelayQueueEventWrapper()
{
    if (!rrDelayQueue.empty() &&
            rrDelayQueue.front().readyTick <= curTick()) {
        insertIntoRR(rrDelayQueue.front().rrEntry);
        rrDelayQueue.pop_front();
    }

    if (!rrDelayQueue.empty() &&
            rrDelayQueue.front().readyTick <= curTick()) {
        schedule(rrDelayQueueEvent, nextCycle());
    } else if (!rrDelayQueue.empty()) {
        schedule(rrDelayQueueEvent, rrDelayQueue.front().readyTick);
    }
}

bool
CompoundBOP::runTeacherTest(Addr addr, const PrefetchInfo &pfi)
{
    auto &entry = teacherOffsets[teacherOffsetIndex];
    const int64_t byte_offset =
        entry.offset * static_cast<int64_t>(blkSize);
    const Addr lookup_addr = addr - static_cast<Addr>(byte_offset);
    stats.teacherTestCount++;

    if (testRR(lookup_addr)) {
        if (archDBer) {
            archDBer->bopTrainTraceWrite(
                curTick(), lookup_addr, pfi.getAddr(), entry.offset,
                entry.score + 1, pfi.isCacheMiss());
        }
        entry.score++;
        if (entry.score > teacherBestScore) {
            teacherBestScore = entry.score;
            teacherPhaseBestOffset = entry.offset;
        }
    }

    teacherOffsetIndex++;
    if (teacherOffsetIndex != teacherOffsets.size()) {
        return false;
    }

    teacherOffsetIndex = 0;
    teacherRound++;
    if (teacherBestScore < teacherScoreMax &&
            teacherRound != teacherRoundMax) {
        return false;
    }

    return true;
}

bool
CompoundBOP::runTeacherTests(Addr addr, const PrefetchInfo &pfi)
{
    for (unsigned test = 0; test < teacherTestsPerAccess; ++test) {
        if (runTeacherTest(addr, pfi)) {
            return true;
        }
    }
    return false;
}

void
CompoundBOP::commitTeacherPhase(
        std::vector<int64_t> &qualified_winners)
{
    stats.teacherPhaseCount++;
    stats.teacherRoundCount += teacherRound;

    const auto best = assignments.bestInFamilies(teacherOffsets);
    for (const Family family : {Family::VBOP, Family::PBOP}) {
        const size_t family_idx = familyIndex(family);
        const auto id = best[family_idx];
        const auto *winner = id ? &teacherOffsets[*id] : nullptr;
        const unsigned score = winner ? winner->score : 0;
        const bool qualified = winner && score >= teacherMinNominationScore;
        familyQualified[family_idx] = qualified;
        if (family == Family::VBOP) {
            stats.teacherVBOPBestScoreDist.sample(score);
            if (qualified) {
                stats.teacherVBOPQualifiedCount++;
                stats.teacherVBOPWinnerOffsetDist.sample(winner->offset);
            } else {
                stats.teacherVBOPRejectedCount++;
            }
        } else {
            stats.teacherPBOPBestScoreDist.sample(score);
            if (qualified) {
                stats.teacherPBOPQualifiedCount++;
                stats.teacherPBOPWinnerOffsetDist.sample(winner->offset);
            } else {
                stats.teacherPBOPRejectedCount++;
            }
        }
        if (qualified) {
            qualified_winners.push_back(winner->offset);
        }
    }

    teacherFallbackOffset = teacherPhaseBestOffset;
    teacherFallbackValid = compound_bop::qualifies(
        teacherBestScore, teacherFallbackBadScore);

    for (auto &entry : teacherOffsets) {
        entry.score = 0;
    }
    teacherBestScore = 0;
    teacherRound = 0;
    teacherPhaseBestOffset = 0;
}

void
CompoundBOP::updateFamilyIssueConfidence()
{
    for (const Family family : {Family::VBOP, Family::PBOP}) {
        const size_t index = familyIndex(family);
        familyIssueConfidence[index] =
            compound_bop::updatedIssueConfidence(
                familyIssueConfidence[index], familyQualified[index],
                issueConfidencePassDecrement,
                issueConfidenceFailIncrement, issueConfidenceMax);
        if (family == Family::VBOP) {
            stats.vbopIssueConfidenceDist.sample(
                familyIssueConfidence[index]);
        } else {
            stats.pbopIssueConfidenceDist.sample(
                familyIssueConfidence[index]);
        }
    }
}

unsigned
CompoundBOP::studentHashIndex(Addr line_addr) const
{
    constexpr unsigned raw_low_bits = 9;
    constexpr unsigned hash_start_bit = 6;
    const unsigned index_bits = floorLog2(studentFilterEntries);
    const unsigned hash_bits = index_bits - raw_low_bits;
    const unsigned line_addr_bits = studentHashAddrBits - lBlkSize;
    const uint64_t rtl_line_addr = line_addr & lowMask(line_addr_bits);
    const uint64_t folded_high = foldBitGroups(
        rtl_line_addr, line_addr_bits - 1, hash_start_bit, hash_bits);
    return static_cast<unsigned>(
        ((folded_high << raw_low_bits) |
         (rtl_line_addr & lowMask(raw_low_bits))) &
        (studentFilterEntries - 1));
}

void
CompoundBOP::studentInsertFilterMask(unsigned index, uint64_t mask)
{
    studentFilter[index] |= mask;
}

void
CompoundBOP::studentDrainDelayQueue()
{
    studentDelayQueues.drain(curTick(), [this](unsigned index, uint64_t mask) {
        studentInsertFilterMask(index, mask);
        stats.studentDelayQueueDrainCount++;
    });
    stats.studentDelayQueueOccupancyDist.sample(studentDelayQueues.size());
}

void
CompoundBOP::studentEnqueuePrediction(Addr train_addr, size_t slot)
{
    const auto &entry = studentEntries[slot];
    assert(entry.valid);
    const __int128 predicted = static_cast<__int128>(train_addr) +
        static_cast<__int128>(entry.offset) * blkSize;
    if (predicted < 0 ||
            predicted > std::numeric_limits<Addr>::max()) {
        return;
    }

    const Addr predicted_addr = static_cast<Addr>(predicted);
    if (!crossPage && !samePage(train_addr, predicted_addr)) {
        return;
    }

    const unsigned index = studentHashIndex(predicted_addr >> lBlkSize);
    const uint64_t mask = 1ULL << slot;
    if (!studentDelayQueues.enqueue(entry.family, curTick(), index, mask)) {
        stats.studentDelayQueueFullDropCount++;
        return;
    }
    stats.studentDelayQueueInsertCount++;
    stats.studentDelayQueueOccupancyDist.sample(studentDelayQueues.size());
}

void
CompoundBOP::studentObserve(Addr addr)
{
    studentPhaseTrainCount++;
    if (studentOccupancy() == 0) {
        return;
    }

    studentDrainDelayQueue();
    const unsigned index = studentHashIndex(addr >> lBlkSize);
    const uint64_t hit_mask = studentFilter[index];

    for (size_t slot = 0; slot < studentEntries.size(); ++slot) {
        if (studentEntries[slot].valid && (hit_mask & (1ULL << slot))) {
            studentEntries[slot].currentCoverage++;
        }
    }

    for (unsigned vbop_slot = 0; vbop_slot < studentVBOPSlots;
            ++vbop_slot) {
        if (!studentEntries[vbop_slot].valid ||
                !(hit_mask & (1ULL << vbop_slot))) {
            continue;
        }
        for (unsigned pbop_local = 0; pbop_local < studentPBOPSlots;
                ++pbop_local) {
            const unsigned pbop_slot = studentVBOPSlots + pbop_local;
            if (studentEntries[pbop_slot].valid &&
                    (hit_mask & (1ULL << pbop_slot))) {
                crossFamilyOverlap[
                    vbop_slot * studentPBOPSlots + pbop_local]++;
            }
        }
    }

    for (size_t slot = 0; slot < studentEntries.size(); ++slot) {
        if (studentEntries[slot].valid) {
            studentEnqueuePrediction(addr, slot);
        }
    }
}

uint32_t
CompoundBOP::overlapFor(size_t lhs, size_t rhs) const
{
    if (studentEntries[lhs].family == Family::PBOP) {
        std::swap(lhs, rhs);
    }
    assert(studentEntries[lhs].family == Family::VBOP);
    assert(studentEntries[rhs].family == Family::PBOP);
    const size_t pbop_local = rhs - studentVBOPSlots;
    return crossFamilyOverlap[lhs * studentPBOPSlots + pbop_local];
}

void
CompoundBOP::updateStudentEntryConfidence()
{
    for (const Family family : {Family::VBOP, Family::PBOP}) {
        const auto best = compound_bop::pickBest(studentEntries, family);
        const auto worst = compound_bop::pickWorst(studentEntries, family);
        if (!best || !worst) {
            continue;
        }

        for (unsigned slot = familySlotBegin(family);
                slot < familySlotEnd(family); ++slot) {
            auto &entry = studentEntries[slot];
            if (!entry.valid) {
                continue;
            }
            double update = 0.0;
            if (slot == *best) {
                update = 1.0;
            }
            if (slot == *worst) {
                update = -1.0;
            }
            entry.confidence = entry.confidence * studentConfAlpha +
                update * (1.0 - studentConfAlpha);
            entry.lastCoverage = entry.currentCoverage;
        }
    }
}

void
CompoundBOP::insertTeacherWinner(int64_t offset)
{
    const Family family = offsetFamily(offset);
    const unsigned begin = familySlotBegin(family);
    const unsigned end = familySlotEnd(family);

    for (unsigned slot = begin; slot < end; ++slot) {
        if (studentEntries[slot].valid &&
                studentEntries[slot].offset == offset) {
            return;
        }
    }

    size_t target = end;
    for (unsigned slot = begin; slot < end; ++slot) {
        if (!studentEntries[slot].valid) {
            target = slot;
            break;
        }
    }
    if (target == end) {
        const auto victim = compound_bop::pickVictim(studentEntries, family);
        assert(victim);
        target = *victim;
        if (family == Family::VBOP) {
            stats.studentVBOPEvictedCount++;
        } else {
            stats.studentPBOPEvictedCount++;
        }
    }

    studentEntries[target] = {
        true, family, offset, 0.0, 0, 0};
    if (family == Family::VBOP) {
        stats.studentVBOPInjectedCount++;
    } else {
        stats.studentPBOPInjectedCount++;
    }
}

void
CompoundBOP::finishStudentPhase()
{
    std::optional<int64_t> redundant;
    stats.studentPhaseCount++;
    stats.studentPoolOccupancyDist.sample(studentOccupancy());
    studentSelectedOffsets.clear();

    const auto vbop_best =
        compound_bop::pickBest(studentEntries, Family::VBOP);
    const auto pbop_best =
        compound_bop::pickBest(studentEntries, Family::PBOP);

    if (vbop_best || pbop_best) {
        size_t primary = vbop_best ? *vbop_best : *pbop_best;
        std::optional<size_t> extra;
        if (vbop_best && pbop_best) {
            if (compound_bop::selectionKey(studentEntries[*pbop_best]) >
                    compound_bop::selectionKey(studentEntries[*vbop_best])) {
                primary = *pbop_best;
                extra = *vbop_best;
            } else {
                primary = *vbop_best;
                extra = *pbop_best;
            }
        }

        const uint32_t primary_coverage =
            studentEntries[primary].currentCoverage;
        studentSelectedValid = true;
        studentSelectedEnable =
            static_cast<uint64_t>(primary_coverage) * 100 >=
            static_cast<uint64_t>(studentPrimaryThresholdPct) *
                studentPhaseTrainCount;
        stats.studentPrimaryCoveragePctDist.sample(
            coveragePct(primary_coverage, studentPhaseTrainCount));

        if (studentSelectedEnable) {
            studentSelectedOffsets.push_back({
                studentEntries[primary].offset,
                studentEntries[primary].family});
            stats.studentPrimaryOffsetDist.sample(
                studentEntries[primary].offset);

            if (extra) {
                const uint32_t overlap = overlapFor(primary, *extra);
                const uint32_t marginal = compound_bop::marginalCoverage(
                    studentEntries[*extra].currentCoverage, overlap);
                stats.studentExtraMarginalPctDist.sample(
                    coveragePct(marginal, studentPhaseTrainCount));
                stats.studentCrossOverlapPctDist.sample(
                    coveragePct(overlap, studentPhaseTrainCount));
                if (static_cast<uint64_t>(marginal) * 100 >=
                        static_cast<uint64_t>(studentExtraThresholdPct) *
                            studentPhaseTrainCount) {
                    studentSelectedOffsets.push_back({
                        studentEntries[*extra].offset,
                        studentEntries[*extra].family});
                } else if (assignments.enabled() &&
                        compound_bop::redundantExtra(
                            studentEntries[*extra].currentCoverage, overlap,
                            studentPhaseTrainCount, studentExtraThresholdPct,
                            regroupOverlapPct)) {
                    redundant = studentEntries[*extra].offset;
                }
            }
        }
    } else {
        studentSelectedValid = false;
        studentSelectedEnable = false;
    }

    updateStudentEntryConfidence();
    const auto regroup = updateAssignments(redundant);
    if (studentSelectedOffsets.size() == 2) {
        stats.studentExtraOffsetDist.sample(studentSelectedOffsets[1].offset);
    }

    // Scores belong to individual offsets. Nominate from the updated
    // partitions before clearing the finished round's scores.
    std::vector<int64_t> teacher_winners;
    commitTeacherPhase(teacher_winners);
    for (const auto winner : teacher_winners) {
        if (regroup.canAdmit(winner)) {
            insertTeacherWinner(winner);
        }
    }
    clearStudentPhase();
}

compound_bop::RegroupChange
CompoundBOP::updateAssignments(std::optional<int64_t> redundant)
{
    if (!assignments.enabled()) {
        return {};
    }
    const auto expired = assignments.advance();
    if (expired) {
        stats.regroupExpired++;
        DPRINTF(CompoundBOP, "Offset %ld returns to its home family\n",
                teacherOffsets[*expired].offset);
    }
    bool changed = false;
    if (redundant) {
        assert(!studentSelectedOffsets.empty());
        const auto primary = studentSelectedOffsets.front().offset;
        const auto target = offsetFamily(primary);
        const auto id = offsetIndex(*redundant);
        changed = assignments.moveTo(id, teacherOffsets[id].home, target);
        if (changed) {
            if (target == Family::VBOP) {
                stats.regroupPToV++;
            } else {
                stats.regroupVToP++;
            }
            DPRINTF(CompoundBOP, "Regroup extra %ld with primary %ld in %s\n",
                    *redundant, primary, target == Family::VBOP ? "V" : "P");
        }
    }
    if (!expired && !changed) {
        return {};
    }

    const auto change = compound_bop::reconcileFamilies(
        studentEntries, studentSelectedOffsets,
        [this](int64_t offset) { return offsetFamily(offset); });
    stats.regroupResidentInvalidations += change.invalidated;
    if (change.extraRemoved) {
        stats.regroupExtraCollisions++;
    }
    return change;
}

void
CompoundBOP::clearStudentPhase()
{
    std::fill(studentFilter.begin(), studentFilter.end(), 0);
    std::fill(crossFamilyOverlap.begin(), crossFamilyOverlap.end(), 0);
    stats.studentDelayQueueOccupancyDist.sample(studentDelayQueues.size());
    studentDelayQueues.clear();
    studentPhaseTrainCount = 0;
    for (auto &entry : studentEntries) {
        entry.currentCoverage = 0;
    }
}

CompoundBOP::RecentKey
CompoundBOP::recentKey(const PrefetchInfo &pfi, Addr addr) const
{
    return {
        blockAddress(addr),
        pfi.hasContextId() ? pfi.contextId() : InvalidContextID,
        pfi.isSecure()};
}

bool
CompoundBOP::recentFilterAccept(const RecentKey &key)
{
    if (recentFilterEntries == 0) {
        return true;
    }
    if (recentFilterSet.find(key) != recentFilterSet.end()) {
        return false;
    }
    if (recentFilterOrder.size() == recentFilterEntries) {
        recentFilterSet.erase(recentFilterOrder.front());
        recentFilterOrder.pop_front();
    }
    recentFilterOrder.push_back(key);
    recentFilterSet.insert(key);
    return true;
}

void
CompoundBOP::enqueueCandidate(const PrefetchInfo &pfi, int64_t offset,
        Role role)
{
    const Addr train_addr = blockAddress(pfi.getAddr());
    const Addr prefetch_addr = train_addr + static_cast<Addr>(
        offset * static_cast<int64_t>(blkSize));

    prefetchStats.pfGenerated++;
    stats.issuedOffsetDist.sample(offset);
    if (role == Role::Primary) {
        stats.primaryGeneratedCount++;
    } else {
        stats.extraGeneratedCount++;
    }

    if (!crossPage && !samePage(train_addr, prefetch_addr)) {
        prefetchStats.pfFiltered++;
        return;
    }

    const RecentKey key = recentKey(pfi, prefetch_addr);
    if (!recentFilterAccept(key)) {
        prefetchStats.pfFiltered++;
        if (role == Role::Primary) {
            stats.primaryRecentFilteredCount++;
        } else {
            stats.extraRecentFilteredCount++;
        }
        return;
    }

    PendingRequest pending{
        AddrPriority(prefetch_addr, 32, PrefetchSourceType::HWP_BOP,
            pfi.trigger_info),
        role,
        key,
        curTick(),
        false};
    if (role == Role::Primary) {
        const auto overwritten = primaryFifo.push(std::move(pending));
        stats.primaryEnqueuedCount++;
        if (overwritten) {
            stats.primaryOverwrittenCount++;
        }
    } else {
        const auto overwritten = extraFifo.push(std::move(pending));
        stats.extraEnqueuedCount++;
        if (overwritten) {
            stats.extraOverwrittenCount++;
        }
    }
}

void
CompoundBOP::observeLocalFifoLate(const PrefetchInfo &pfi)
{
    const RecentKey demand = recentKey(pfi, pfi.getAddr());
    for (auto &entry : primaryFifo.entries()) {
        if (!entry.lateObserved && entry.key == demand) {
            entry.lateObserved = true;
            stats.primaryLocalLateCount++;
        }
    }
    for (auto &entry : extraFifo.entries()) {
        if (!entry.lateObserved && entry.key == demand) {
            entry.lateObserved = true;
            stats.extraLocalLateCount++;
        }
    }
}

void
CompoundBOP::recordDownstreamLate(Addr addr)
{
    const Addr key = blockAddress(addr);
    const auto role = dequeuedRoleByAddress.find(key);
    if (role == dequeuedRoleByAddress.end() ||
            downstreamLateRecorded.find(key) !=
                downstreamLateRecorded.end()) {
        return;
    }
    if (role->second == Role::Primary) {
        stats.primaryDownstreamLateCount++;
    } else {
        stats.extraDownstreamLateCount++;
    }
    downstreamLateRecorded.insert(key);
}

void
CompoundBOP::recordRoleEvent(Addr addr,
        statistics::Scalar &primary_stat, statistics::Scalar &extra_stat,
        bool erase)
{
    const Addr key = blockAddress(addr);
    const auto role = dequeuedRoleByAddress.find(key);
    if (role == dequeuedRoleByAddress.end()) {
        return;
    }
    if (role->second == Role::Primary) {
        primary_stat++;
    } else {
        extra_stat++;
    }
    if (erase) {
        dequeuedRoleByAddress.erase(role);
        downstreamLateRecorded.erase(key);
    }
}

void
CompoundBOP::recordUseful(Addr addr)
{
    recordRoleEvent(
        addr, stats.primaryUsefulCount, stats.extraUsefulCount, true);
}

void
CompoundBOP::recordUnused(Addr addr)
{
    recordRoleEvent(
        addr, stats.primaryUnusedCount, stats.extraUnusedCount, true);
}

void
CompoundBOP::calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses)
{
    calculatePrefetch(pfi, addresses, false);
}

void
CompoundBOP::calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses, bool late)
{
    (void)addresses;
    const Addr addr = blockAddress(pfi.getAddr());
    observeLocalFifoLate(pfi);
    if (late) {
        recordDownstreamLate(addr);
    }

    enqueueRR(addr);
    const bool phase_end = runTeacherTests(addr, pfi);
    studentObserve(addr);
    if (phase_end) {
        finishStudentPhase();
    }

    const bool student_issue =
        studentSelectedValid && studentSelectedEnable;
    std::vector<SelectedOffset> selected;
    if (student_issue) {
        selected = studentSelectedOffsets;
    } else if (teacherFallbackValid) {
        selected.push_back(
            {teacherFallbackOffset, offsetFamily(teacherFallbackOffset)});
    }

    const bool vbop_allowed = compound_bop::issueGateAllows(
        familyIssueConfidence[familyIndex(Family::VBOP)],
        issueConfidenceThreshold);
    const bool pbop_allowed = compound_bop::issueGateAllows(
        familyIssueConfidence[familyIndex(Family::PBOP)],
        issueConfidenceThreshold);
    for (const auto &candidate : selected) {
        const bool allowed = candidate.family == Family::VBOP ?
            vbop_allowed : pbop_allowed;
        if (!allowed) {
            if (candidate.family == Family::VBOP) {
                stats.vbopGateBlockedCount++;
            } else {
                stats.pbopGateBlockedCount++;
            }
        }
    }

    const auto gated = compound_bop::applyFamilyGates(
        selected, vbop_allowed, pbop_allowed);
    if (selected.size() > 1 && !gated.empty() &&
            gated.front().offset != selected.front().offset) {
        stats.extraPromotedCount++;
    }
    if (!student_issue && !gated.empty()) {
        stats.teacherFallbackIssueCount++;
    }
    for (size_t rank = 0; rank < gated.size(); ++rank) {
        enqueueCandidate(
            pfi, gated[rank].offset,
            rank == 0 ? Role::Primary : Role::Extra);
    }

    if (phase_end) {
        updateFamilyIssueConfidence();
    }
}

bool
CompoundBOP::hasPFRequestsInBuffer()
{
    return !primaryFifo.empty() || !extraFifo.empty();
}

bool
CompoundBOP::GetPFRequestsFromBuffer(
        std::vector<AddrPriority> &addresses)
{
    const auto role = outputArbiter.choose(
        !primaryFifo.empty(), !extraFifo.empty());
    if (!role) {
        return false;
    }

    auto pending = *role == Role::Primary ?
        primaryFifo.pop() : extraFifo.pop();
    assert(pending);
    const uint64_t delay = static_cast<uint64_t>(
        ticksToCycles(curTick() - pending->enqueueTick));
    if (*role == Role::Primary) {
        stats.primaryDequeuedCount++;
        stats.primaryQueueDelayCycles += delay;
        stats.primaryQueueDelayMax = std::max<uint64_t>(
            stats.primaryQueueDelayMax.value(), delay);
    } else {
        stats.extraDequeuedCount++;
        stats.extraQueueDelayCycles += delay;
        stats.extraQueueDelayMax = std::max<uint64_t>(
            stats.extraQueueDelayMax.value(), delay);
    }

    const Addr addr = blockAddress(pending->request.addr);
    dequeuedRoleByAddress[addr] = *role;
    downstreamLateRecorded.erase(addr);
    addresses.push_back(std::move(pending->request));
    return true;
}

CompoundBOP::CompoundBOPStats::CompoundBOPStats(
        statistics::Group *parent, unsigned round_max,
        unsigned student_slots, unsigned student_delay_entries)
    : statistics::Group(parent),
      ADD_STAT(teacherPhaseCount, statistics::units::Count::get(),
          "Number of shared Teacher phases"),
      ADD_STAT(teacherTestCount, statistics::units::Count::get(),
          "Number of Teacher offset tests"),
      ADD_STAT(teacherRoundCount, statistics::units::Count::get(),
          "Sum of completed Teacher rounds"),
      ADD_STAT(teacherVBOPBestScoreDist, statistics::units::Count::get(),
          "VBOP family best score at shared Teacher phase end"),
      ADD_STAT(teacherPBOPBestScoreDist, statistics::units::Count::get(),
          "PBOP family best score at shared Teacher phase end"),
      ADD_STAT(teacherVBOPWinnerOffsetDist, statistics::units::Count::get(),
          "Qualified VBOP Teacher winner offsets"),
      ADD_STAT(teacherPBOPWinnerOffsetDist, statistics::units::Count::get(),
          "Qualified PBOP Teacher winner offsets"),
      ADD_STAT(teacherVBOPQualifiedCount, statistics::units::Count::get(),
          "Shared Teacher phases with a qualified VBOP winner"),
      ADD_STAT(teacherVBOPRejectedCount, statistics::units::Count::get(),
          "Shared Teacher phases with a rejected VBOP winner"),
      ADD_STAT(teacherPBOPQualifiedCount, statistics::units::Count::get(),
          "Shared Teacher phases with a qualified PBOP winner"),
      ADD_STAT(teacherPBOPRejectedCount, statistics::units::Count::get(),
          "Shared Teacher phases with a rejected PBOP winner"),
      ADD_STAT(rrDelayQueueFullDropCount, statistics::units::Count::get(),
          "RR insertions dropped because the shared delay queue was full"),
      ADD_STAT(vbopIssueConfidenceDist, statistics::units::Count::get(),
          "VBOP family issue-confidence values after phase updates"),
      ADD_STAT(pbopIssueConfidenceDist, statistics::units::Count::get(),
          "PBOP family issue-confidence values after phase updates"),
      ADD_STAT(vbopGateBlockedCount, statistics::units::Count::get(),
          "VBOP candidates blocked by family issue confidence"),
      ADD_STAT(pbopGateBlockedCount, statistics::units::Count::get(),
          "PBOP candidates blocked by family issue confidence"),
      ADD_STAT(extraPromotedCount, statistics::units::Count::get(),
          "Extra candidates promoted after the original primary was gated"),
      ADD_STAT(studentPhaseCount, statistics::units::Count::get(),
          "Teacher-aligned Student phases"),
      ADD_STAT(studentPoolOccupancyDist, statistics::units::Count::get(),
          "Valid Student entries at phase end"),
      ADD_STAT(studentPrimaryOffsetDist, statistics::units::Count::get(),
          "Student-selected primary offsets"),
      ADD_STAT(studentExtraOffsetDist, statistics::units::Count::get(),
          "Student-selected extra offsets passing marginal threshold"),
      ADD_STAT(studentPrimaryCoveragePctDist, statistics::units::Ratio::get(),
          "Student primary coverage percentage"),
      ADD_STAT(studentExtraMarginalPctDist, statistics::units::Ratio::get(),
          "Student extra marginal coverage percentage"),
      ADD_STAT(studentCrossOverlapPctDist, statistics::units::Ratio::get(),
          "Selected cross-family pair overlap percentage"),
      ADD_STAT(studentVBOPInjectedCount, statistics::units::Count::get(),
          "Qualified Teacher winners inserted into the VBOP Student pool"),
      ADD_STAT(studentPBOPInjectedCount, statistics::units::Count::get(),
          "Qualified Teacher winners inserted into the PBOP Student pool"),
      ADD_STAT(studentVBOPEvictedCount, statistics::units::Count::get(),
          "VBOP Student entries replaced by VBOP Teacher winners"),
      ADD_STAT(studentPBOPEvictedCount, statistics::units::Count::get(),
          "PBOP Student entries replaced by PBOP Teacher winners"),
      ADD_STAT(regroupVToP, statistics::units::Count::get(),
          "Redundant extras reassigned from V to P by feedback"),
      ADD_STAT(regroupPToV, statistics::units::Count::get(),
          "Redundant extras reassigned from P to V by feedback"),
      ADD_STAT(regroupExpired, statistics::units::Count::get(),
          "Temporary family assignments returned home on expiry"),
      ADD_STAT(regroupResidentInvalidations, statistics::units::Count::get(),
          "Student residents removed after a family change"),
      ADD_STAT(regroupExtraCollisions, statistics::units::Count::get(),
          "Selected extras dropped because expiry reunited the pair"),
      ADD_STAT(studentDelayQueueInsertCount, statistics::units::Count::get(),
          "Delayed Student predictions (both families) enqueued"),
      ADD_STAT(studentDelayQueueDrainCount, statistics::units::Count::get(),
          "Delayed Student predictions (both families) made visible"),
      ADD_STAT(studentDelayQueueFullDropCount, statistics::units::Count::get(),
          "Delayed Student predictions (both families) dropped on a full queue"),
      ADD_STAT(studentDelayQueueOccupancyDist, statistics::units::Count::get(),
          "Total occupancy of both Student delay queues"),
      ADD_STAT(teacherFallbackIssueCount, statistics::units::Count::get(),
          "Accesses issuing the cold-start global Teacher fallback"),
      ADD_STAT(issuedOffsetDist, statistics::units::Count::get(),
          "Generated Compound BOP offsets"),
      ADD_STAT(primaryGeneratedCount, statistics::units::Count::get(),
          "Generated Primary candidates"),
      ADD_STAT(extraGeneratedCount, statistics::units::Count::get(),
          "Generated Extra candidates"),
      ADD_STAT(primaryRecentFilteredCount, statistics::units::Count::get(),
          "Primary candidates removed by RF128"),
      ADD_STAT(extraRecentFilteredCount, statistics::units::Count::get(),
          "Extra candidates removed by RF128"),
      ADD_STAT(primaryEnqueuedCount, statistics::units::Count::get(),
          "Primary candidates enqueued"),
      ADD_STAT(extraEnqueuedCount, statistics::units::Count::get(),
          "Extra candidates enqueued"),
      ADD_STAT(primaryOverwrittenCount, statistics::units::Count::get(),
          "Primary FIFO oldest entries overwritten"),
      ADD_STAT(extraOverwrittenCount, statistics::units::Count::get(),
          "Extra FIFO oldest entries overwritten"),
      ADD_STAT(primaryDequeuedCount, statistics::units::Count::get(),
          "Primary candidates dequeued"),
      ADD_STAT(extraDequeuedCount, statistics::units::Count::get(),
          "Extra candidates dequeued"),
      ADD_STAT(primaryLocalLateCount, statistics::units::Count::get(),
          "Demand accesses catching Primary candidates in the local FIFO"),
      ADD_STAT(extraLocalLateCount, statistics::units::Count::get(),
          "Demand accesses catching Extra candidates in the local FIFO"),
      ADD_STAT(primaryDownstreamLateCount, statistics::units::Count::get(),
          "Late Primary candidates after local dequeue"),
      ADD_STAT(extraDownstreamLateCount, statistics::units::Count::get(),
          "Late Extra candidates after local dequeue"),
      ADD_STAT(primaryUsefulCount, statistics::units::Count::get(),
          "Useful Compound BOP blocks attributed to Primary"),
      ADD_STAT(extraUsefulCount, statistics::units::Count::get(),
          "Useful Compound BOP blocks attributed to Extra"),
      ADD_STAT(primaryUnusedCount, statistics::units::Count::get(),
          "Unused Compound BOP blocks attributed to Primary"),
      ADD_STAT(extraUnusedCount, statistics::units::Count::get(),
          "Unused Compound BOP blocks attributed to Extra"),
      ADD_STAT(primaryQueueDelayCycles, statistics::units::Cycle::get(),
          "Total Primary local FIFO delay"),
      ADD_STAT(extraQueueDelayCycles, statistics::units::Cycle::get(),
          "Total Extra local FIFO delay"),
      ADD_STAT(primaryQueueDelayMax, statistics::units::Cycle::get(),
          "Maximum Primary local FIFO delay"),
      ADD_STAT(extraQueueDelayMax, statistics::units::Cycle::get(),
          "Maximum Extra local FIFO delay")
{
    teacherVBOPBestScoreDist.init(0, round_max + 1, 1)
        .prereq(teacherVBOPBestScoreDist);
    teacherPBOPBestScoreDist.init(0, round_max + 1, 1)
        .prereq(teacherPBOPBestScoreDist);
    teacherVBOPWinnerOffsetDist.init(-512, 513, 1)
        .prereq(teacherVBOPWinnerOffsetDist);
    teacherPBOPWinnerOffsetDist.init(-512, 513, 1)
        .prereq(teacherPBOPWinnerOffsetDist);
    vbopIssueConfidenceDist.init(0, 16, 1)
        .prereq(vbopIssueConfidenceDist);
    pbopIssueConfidenceDist.init(0, 16, 1)
        .prereq(pbopIssueConfidenceDist);
    studentPoolOccupancyDist.init(0, student_slots + 1, 1)
        .prereq(studentPoolOccupancyDist);
    studentPrimaryOffsetDist.init(-512, 513, 1)
        .prereq(studentPrimaryOffsetDist);
    studentExtraOffsetDist.init(-512, 513, 1)
        .prereq(studentExtraOffsetDist);
    studentPrimaryCoveragePctDist.init(0, 101, 1)
        .prereq(studentPrimaryCoveragePctDist);
    studentExtraMarginalPctDist.init(0, 101, 1)
        .prereq(studentExtraMarginalPctDist);
    studentCrossOverlapPctDist.init(0, 101, 1)
        .prereq(studentCrossOverlapPctDist);
    studentDelayQueueOccupancyDist.init(
        0, student_delay_entries + 1, 1)
        .prereq(studentDelayQueueOccupancyDist);
    issuedOffsetDist.init(-512, 513, 1).prereq(issuedOffsetDist);
}

} // namespace prefetch
} // namespace gem5
