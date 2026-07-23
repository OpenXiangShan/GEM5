/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
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

#ifndef __ARCH_RISCV_MPT_UNIT_HH__
#define __ARCH_RISCV_MPT_UNIT_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "arch/generic/mmu.hh"
#include "arch/riscv/pagetable.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/RiscvMptUnit.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{

class System;
class ThreadContext;

namespace RiscvISA
{

enum class MptRequestSource : uint8_t
{
    Instruction = 0,
    Data = 1,
    Ptw = 2,
    NumSources = 3
};

struct MptResult
{
    bool allowed = false;
    bool valid = false;
    uint8_t permission = 0;
    int level = -1;
};

class MptClient
{
  public:
    virtual ~MptClient() = default;
    virtual void finishMptLookup(const MptResult &result) = 0;
};

/**
 * Per-core MPT permission lookup unit.
 *
 * The unit models a finite-bandwidth pipelined MPT cache and a dedicated,
 * non-blocking MPTE read engine. Walks to the same MPTE coalesce in an MSHR;
 * unrelated walks may be outstanding simultaneously.
 */
class MptUnit : public ClockedObject
{
  private:
    static constexpr unsigned NumSources =
        static_cast<unsigned>(MptRequestSource::NumSources);
    static constexpr unsigned NumLevels = 4;
    static constexpr unsigned SuperpageCache = 4;

    class MptPort : public RequestPort
    {
      public:
        MptPort(const std::string &name, MptUnit &owner)
            : RequestPort(name, &owner), owner(owner)
        {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;

      private:
        MptUnit &owner;
    };

    struct Target
    {
        uint64_t id = 0;
        MptClient *client = nullptr;
        Addr paddr = 0;
        BaseMMU::Mode mode = BaseMMU::Read;
        MptRequestSource source = MptRequestSource::Data;
        uint64_t epoch = 0;
        Addr rootPpn = 0;
        int level = 3;
        Addr tableBase = 0;
        unsigned depth = 0;
        Tick enqueueTick = 0;
    };

    enum class ProbeKind
    {
        LeafHit,
        InternalHit,
        Miss
    };

    struct ProbeResult
    {
        ProbeKind kind = ProbeKind::Miss;
        MPTCacheEntry entry;
        int level = -1;
    };

    struct PipelineEntry
    {
        Target target;
        ProbeResult probe;
        Tick readyTick = 0;
    };

    struct MshrKey
    {
        uint64_t epoch = 0;
        Addr rootPpn = 0;
        int level = -1;
        Addr mptePaddr = 0;

        bool operator==(const MshrKey &other) const
        {
            return epoch == other.epoch && rootPpn == other.rootPpn &&
                   level == other.level && mptePaddr == other.mptePaddr;
        }
    };

    struct MshrKeyHash
    {
        size_t operator()(const MshrKey &key) const;
    };

    struct Mshr
    {
        bool allocated = false;
        bool inFlight = false;
        uint64_t generation = 0;
        MshrKey key;
        PacketPtr packet = nullptr;
        std::vector<Target> targets;
        Tick issueTick = 0;
    };

    struct MptSenderState : public Packet::SenderState
    {
        const unsigned slot;
        const uint64_t generation;

        MptSenderState(unsigned slot, uint64_t generation)
            : slot(slot), generation(generation)
        {}
    };

    struct CachePartition
    {
        size_t capacity = 0;
        uint64_t sequence = 0;
        std::unordered_map<Addr, MPTCacheEntry> entries;
        std::unordered_map<Addr, uint64_t> lastUse;
    };

    struct MptStats : public statistics::Group
    {
        MptStats(statistics::Group *parent);

        statistics::Vector requests;
        statistics::Vector leafHits;
        statistics::Vector internalHits;
        statistics::Vector mpteMisses;
        statistics::Scalar totalCacheMisses;
        statistics::Scalar cacheBypasses;
        statistics::Scalar lookupQueueOccupancy;
        statistics::Scalar lookupQueueSamples;
        statistics::Scalar lookupQueueFullCycles;
        statistics::Scalar pipelineCompletions;
        statistics::Scalar pipelineLatency;
        statistics::Scalar completedLookups;
        statistics::Scalar totalLookupLatency;
        statistics::Scalar mshrAllocations;
        statistics::Scalar mshrMerges;
        statistics::Scalar mshrFullEvents;
        statistics::Scalar mshrTargetFullEvents;
        statistics::Scalar mshrOccupancy;
        statistics::Scalar mshrOccupancySamples;
        statistics::Scalar memoryRequests;
        statistics::Scalar memoryRetries;
        statistics::Scalar memoryLatency;
        statistics::Scalar maxMemoryInflight;
        statistics::Vector walkDepth;
        statistics::Scalar staleEpochResponses;
        statistics::Scalar fenceFlushes;
        statistics::Scalar squashes;
        statistics::Formula avgLookupQueueOccupancy;
        statistics::Formula avgMshrOccupancy;
        statistics::Formula avgLookupLatency;
        statistics::Formula avgMemoryLatency;
    } stats;

    MptPort port;
    System *const system;
    const RequestorID requestorId;

    const bool enableMptCache;
    const Cycles hitLatency;
    const unsigned lookupWidth;
    const std::array<unsigned, NumSources> acceptWidth;
    const std::array<unsigned, NumSources> queueCapacity;
    const unsigned numMshrs;
    const unsigned targetsPerMshr;
    const unsigned memoryIssueWidth;
    const unsigned maxMemoryInflight;

    std::array<std::deque<Target>, NumSources> arrivals;
    std::array<std::deque<Target>, NumSources> lookupQueues;
    std::deque<PipelineEntry> pipeline;
    std::deque<Target> pendingWalks;
    std::deque<Target> bypassCompletions;

    std::array<CachePartition, NumLevels + 1> cache;
    std::vector<Mshr> mshrs;
    std::unordered_map<MshrKey, unsigned, MshrKeyHash> mshrIndex;
    std::deque<unsigned> readyMshrs;
    std::optional<unsigned> blockedMshr;
    unsigned memoryInflight = 0;

    MMPT mmpt = 0;
    uint64_t epoch = 1;
    uint64_t nextTargetId = 1;
    unsigned nextSource = 0;
    bool simulatedTreeBuilt = false;
    Addr simulatedRootPaddr = 0;

    EventFunctionWrapper serviceEvent;

    static unsigned sourceIndex(MptRequestSource source)
    {
        return static_cast<unsigned>(source);
    }

    void scheduleService(Cycles delay = Cycles(0));
    void process();
    bool hasServiceWork() const;
    void sampleOccupancy();

    void acceptArrivals();
    void issueLookups();
    void completePipeline();
    void retryPendingWalks();
    void completeBypasses();

    ProbeResult probeCache(Addr paddr);
    void insertCache(int level, Addr paddr, const MPTE52 &mpte);
    void clearCache();
    static Addr cacheKey(Addr paddr, int level, bool superpage);

    bool startTargetRead(Target target);
    Addr mpteAddress(const Target &target) const;
    PacketPtr createReadPacket(unsigned slot, uint64_t generation,
                               Addr paddr);
    void issueMemoryRequests();
    bool sendMshr(unsigned slot);
    bool recvTimingResp(PacketPtr pkt);
    void recvReqRetry();
    void releaseMshr(unsigned slot);
    void discardPacket(PacketPtr pkt);

    void consumeMpte(Target target, uint64_t raw);
    void completeTarget(const Target &target, const MptResult &result);
    void restartTarget(Target target);
    void invalidateForNewEpoch();

    Addr buildSimulatedMptTree();
    void ensureSimulatedMptTree(ThreadContext *tc);

  public:
    using Params = RiscvMptUnitParams;
    explicit MptUnit(const Params &params);
    ~MptUnit() override;

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    bool enabled() const { return mmpt.mode != 0; }
    MMPT currentMMPT() const { return mmpt; }

    void syncMMPT(ThreadContext *tc);
    void flush();

    void submit(MptClient *client, Addr paddr, BaseMMU::Mode mode,
                MptRequestSource source);
    void cancel(MptClient *client);

    bool checkFunctional(Addr paddr, BaseMMU::Mode mode) const;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_MPT_UNIT_HH__
