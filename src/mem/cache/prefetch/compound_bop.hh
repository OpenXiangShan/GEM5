#ifndef __MEM_CACHE_PREFETCH_COMPOUND_BOP_HH__
#define __MEM_CACHE_PREFETCH_COMPOUND_BOP_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/prefetch/compound_bop_policy.hh"
#include "mem/cache/prefetch/queued.hh"
#include "params/CompoundBOPPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class CompoundBOP : public Queued
{
  public:
    explicit CompoundBOP(const CompoundBOPPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses) override;
    using Queued::calculatePrefetch;
    void calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses, bool late);

    bool hasPFRequestsInBuffer() override;
    bool GetPFRequestsFromBuffer(
        std::vector<AddrPriority> &addresses) override;

    uint64_t getRRInsertCount() const { return rrInsertCount; }
    void recordUseful(Addr addr);
    void recordUnused(Addr addr);

  private:
    using Family = compound_bop::Family;
    using Role = compound_bop::Role;
    using StudentEntry = compound_bop::StudentEntry;
    using SelectedOffset = compound_bop::SelectedOffset;

    static constexpr size_t FamilyCount = 2;

    struct RREntry
    {
        Addr fullAddr = 0;
        Addr hashAddr = 0;
    };

    struct DelayQueueEntry
    {
        RREntry rrEntry;
        Tick readyTick;
    };

    struct RecentKey
    {
        Addr addr;
        ContextID context;
        bool secure;

        bool operator==(const RecentKey &other) const
        {
            return addr == other.addr && context == other.context &&
                secure == other.secure;
        }
    };

    struct RecentKeyHash
    {
        size_t operator()(const RecentKey &key) const;
    };

    struct PendingRequest
    {
        AddrPriority request;
        Role role;
        RecentKey key;
        Tick enqueueTick;
        bool lateObserved = false;
    };

    struct CompoundBOPStats : public statistics::Group
    {
        explicit CompoundBOPStats(statistics::Group *parent,
            unsigned round_max, unsigned student_slots,
            unsigned student_delay_entries);

        statistics::Scalar teacherPhaseCount;
        statistics::Scalar teacherTestCount;
        statistics::Scalar teacherRoundCount;
        statistics::Distribution teacherVBOPBestScoreDist;
        statistics::Distribution teacherPBOPBestScoreDist;
        statistics::Distribution teacherVBOPWinnerOffsetDist;
        statistics::Distribution teacherPBOPWinnerOffsetDist;
        statistics::Scalar teacherVBOPQualifiedCount;
        statistics::Scalar teacherVBOPRejectedCount;
        statistics::Scalar teacherPBOPQualifiedCount;
        statistics::Scalar teacherPBOPRejectedCount;
        statistics::Scalar rrDelayQueueFullDropCount;

        statistics::Distribution vbopIssueConfidenceDist;
        statistics::Distribution pbopIssueConfidenceDist;
        statistics::Scalar vbopGateBlockedCount;
        statistics::Scalar pbopGateBlockedCount;
        statistics::Scalar extraPromotedCount;

        statistics::Scalar studentPhaseCount;
        statistics::Distribution studentPoolOccupancyDist;
        statistics::Distribution studentPrimaryOffsetDist;
        statistics::Distribution studentExtraOffsetDist;
        statistics::Distribution studentPrimaryCoveragePctDist;
        statistics::Distribution studentExtraMarginalPctDist;
        statistics::Distribution studentCrossOverlapPctDist;
        statistics::Scalar studentVBOPInjectedCount;
        statistics::Scalar studentPBOPInjectedCount;
        statistics::Scalar studentVBOPEvictedCount;
        statistics::Scalar studentPBOPEvictedCount;
        statistics::Scalar regroupVToP;
        statistics::Scalar regroupPToV;
        statistics::Scalar regroupExpired;
        statistics::Scalar regroupResidentInvalidations;
        statistics::Scalar regroupExtraCollisions;
        statistics::Scalar studentDelayQueueInsertCount;
        statistics::Scalar studentDelayQueueDrainCount;
        statistics::Scalar studentDelayQueueFullDropCount;
        statistics::Distribution studentDelayQueueOccupancyDist;
        statistics::Scalar teacherFallbackIssueCount;

        statistics::Distribution issuedOffsetDist;
        statistics::Scalar primaryGeneratedCount;
        statistics::Scalar extraGeneratedCount;
        statistics::Scalar primaryRecentFilteredCount;
        statistics::Scalar extraRecentFilteredCount;
        statistics::Scalar primaryEnqueuedCount;
        statistics::Scalar extraEnqueuedCount;
        statistics::Scalar primaryOverwrittenCount;
        statistics::Scalar extraOverwrittenCount;
        statistics::Scalar primaryDequeuedCount;
        statistics::Scalar extraDequeuedCount;
        statistics::Scalar primaryLocalLateCount;
        statistics::Scalar extraLocalLateCount;
        statistics::Scalar primaryDownstreamLateCount;
        statistics::Scalar extraDownstreamLateCount;
        statistics::Scalar primaryUsefulCount;
        statistics::Scalar extraUsefulCount;
        statistics::Scalar primaryUnusedCount;
        statistics::Scalar extraUnusedCount;
        statistics::Scalar primaryQueueDelayCycles;
        statistics::Scalar extraQueueDelayCycles;
        statistics::Scalar primaryQueueDelayMax;
        statistics::Scalar extraQueueDelayMax;
    };

    const unsigned teacherTestsPerAccess;
    const unsigned teacherScoreMax;
    const unsigned teacherRoundMax;
    const unsigned teacherFallbackBadScore;
    const unsigned teacherMinNominationScore;

    const unsigned rrEntries;
    const Addr tagMask;
    const unsigned rrDelayQueueSize;
    const Tick rrDelayTicks;
    const bool crossPage;

    const unsigned studentVBOPSlots;
    const unsigned studentPBOPSlots;
    const double studentConfAlpha;
    const unsigned studentFilterEntries;
    const unsigned studentHashAddrBits;
    const unsigned studentPrimaryThresholdPct;
    const unsigned studentExtraThresholdPct;

    const unsigned issueConfidenceThreshold;
    const unsigned issueConfidenceMax;
    const unsigned issueConfidencePassDecrement;
    const unsigned issueConfidenceFailIncrement;
    const unsigned recentFilterEntries;

    const unsigned regroupOverlapPct;
    compound_bop::FamilyAssignments assignments;
    std::vector<compound_bop::TeacherEntry> teacherOffsets;
    size_t teacherOffsetIndex = 0;
    unsigned teacherBestScore = 0;
    unsigned teacherRound = 0;
    int64_t teacherPhaseBestOffset = 0;
    int64_t teacherFallbackOffset = 0;
    bool teacherFallbackValid = false;
    std::array<bool, FamilyCount> familyQualified{};
    std::array<uint8_t, FamilyCount> familyIssueConfidence{};

    std::vector<RREntry> rrLeft;
    std::vector<RREntry> rrRight;
    std::deque<DelayQueueEntry> rrDelayQueue;
    uint64_t rrInsertCount = 0;
    EventFunctionWrapper rrDelayQueueEvent;

    std::vector<StudentEntry> studentEntries;
    std::vector<uint64_t> studentFilter;
    std::vector<uint32_t> crossFamilyOverlap;
    compound_bop::StudentDelayQueues studentDelayQueues;
    uint32_t studentPhaseTrainCount = 0;
    std::vector<SelectedOffset> studentSelectedOffsets;
    bool studentSelectedValid = false;
    bool studentSelectedEnable = false;

    compound_bop::OverwriteFifo<PendingRequest> primaryFifo;
    compound_bop::OverwriteFifo<PendingRequest> extraFifo;
    compound_bop::WeightedArbiter outputArbiter;
    std::deque<RecentKey> recentFilterOrder;
    std::unordered_set<RecentKey, RecentKeyHash> recentFilterSet;
    std::unordered_map<Addr, Role> dequeuedRoleByAddress;
    std::unordered_set<Addr> downstreamLateRecorded;

    CompoundBOPStats stats;

    static size_t familyIndex(Family family);
    size_t offsetIndex(int64_t offset) const;
    Family offsetFamily(int64_t offset) const;
    unsigned familySlotBegin(Family family) const;
    unsigned familySlotEnd(Family family) const;
    unsigned studentOccupancy() const;

    Addr rrTag(Addr addr) const;
    unsigned rrIndex(Addr addr) const;
    bool testRR(Addr addr) const;
    void insertIntoRR(const RREntry &entry);
    void enqueueRR(Addr addr);
    void rrDelayQueueEventWrapper();
    bool runTeacherTest(Addr addr, const PrefetchInfo &pfi);
    bool runTeacherTests(Addr addr, const PrefetchInfo &pfi);
    void commitTeacherPhase(std::vector<int64_t> &qualified_winners);
    void updateFamilyIssueConfidence();

    unsigned studentHashIndex(Addr line_addr) const;
    void studentDrainDelayQueue();
    void studentInsertFilterMask(unsigned index, uint64_t mask);
    void studentEnqueuePrediction(Addr train_addr, size_t slot);
    void studentObserve(Addr addr);
    uint32_t overlapFor(size_t lhs, size_t rhs) const;
    void updateStudentEntryConfidence();
    void insertTeacherWinner(int64_t offset);
    void finishStudentPhase();
    compound_bop::RegroupChange updateAssignments(
        std::optional<int64_t> redundant);
    void clearStudentPhase();

    RecentKey recentKey(const PrefetchInfo &pfi, Addr addr) const;
    bool recentFilterAccept(const RecentKey &key);
    void enqueueCandidate(const PrefetchInfo &pfi, int64_t offset,
        Role role);
    void observeLocalFifoLate(const PrefetchInfo &pfi);
    void recordDownstreamLate(Addr addr);
    void recordRoleEvent(Addr addr, statistics::Scalar &primary_stat,
        statistics::Scalar &extra_stat, bool erase);

    void InsertPFRequestToBuffer(const AddrPriority &addr_prio) override
    {
        panic("CompoundBOP uses separate Primary and Extra FIFOs");
    }
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_COMPOUND_BOP_HH__
