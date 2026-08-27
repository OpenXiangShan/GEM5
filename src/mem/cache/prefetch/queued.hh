/*
 * Copyright (c) 2014-2015 ARM Limited
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

#ifndef __MEM_CACHE_PREFETCH_QUEUED_HH__
#define __MEM_CACHE_PREFETCH_QUEUED_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <list>
#include <optional>
#include <utility>

#include "arch/generic/mmu.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/base.hh"
#include "mem/packet.hh"

namespace gem5
{

struct QueuedPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

using PFTriggerInfo = Base::PFtriggerInfo;

class Queued : public Base
{
  public:
    /**
     * Opaque identity for a candidate staged in a finite producer buffer.
     *
     * Queued and Worker only carry this value. The originating prefetcher
     * validates it before changing its own buffer or trigger state.
     */
    struct StagedPrefetchToken
    {
        Addr region = 0;
        ContextID contextId = InvalidContextID;
        uint64_t entryGeneration = 0;
        uint64_t candidateId = 0;
        uint64_t decisionId = 0;
        uint64_t targetLevel = 0;
        uint8_t offset = 0;
        bool secure = false;
    };

    struct PrefetchCmd
    {
        Addr addr;
        int32_t priority;
        bool isVA;
        bool isBOP;
        int pfahead_host = 0; // which level should process pfahead (e.g 2 is l2...)
        bool pfahead = false;
        int depth=0;
        PrefetchSourceType pfSource;
        PFTriggerInfo pf_trigger_info{};
        // A finite producer can defer its own state transition until the
        // target queue accepts or terminally rejects this candidate.
        std::optional<StagedPrefetchToken> stagedToken;
        PrefetchCmd(Addr a, int32_t p) : addr(a), priority(p), isVA(true), isBOP(false)
        {
            panic("PrefetchCmd: no source specified");
        }
        PrefetchCmd(Addr a, int32_t p, PrefetchSourceType src)
            : addr(a), priority(p), isVA(true), isBOP(false), pfSource(src)
        {
        }
        PrefetchCmd(Addr a, int32_t p, PrefetchSourceType src, PFTriggerInfo pf_info)
            : addr(a), priority(p), isVA(true), isBOP(false), pfSource(src), pf_trigger_info(pf_info)
        {
        }
        PrefetchCmd(Addr a, int32_t p, PrefetchSourceType src, bool va, bool bop)
            : addr(a), priority(p), isVA(va), isBOP(bop), pfSource(src)
        {
        }
    };
    // using AddrPriority = std::pair<Addr, int32_t>;
    using AddrPriority = PrefetchCmd;

    /** Terminal state of one candidate submitted to the prefetch queue. */
    enum class InsertResult : uint8_t
    {
        Rejected,
        Accepted,
        PendingTranslation,
        PendingForward,
    };

  protected:
    struct DeferredPacket : public BaseMMU::Translation
    {
        /** Owner of the packet */
        Queued *owner;
        /** Prefetch info corresponding to this packet */
        PrefetchInfo pfInfo;
        /** Time when this prefetch becomes ready */
        Tick tick;
        /** The memory packet generated by this prefetch */
        PacketPtr pkt;
        /** The priority of this prefetch */
        int32_t priority;
        bool pfahead;
        int pfahead_host;
        /** Request used when a translation is needed */
        RequestPtr translationRequest;
        ThreadContext *tc;
        bool ongoingTranslation;
        // Keep only an opaque producer token while this packet is deferred or
        // forwarded. The producer owns interpretation and may be upstream of
        // the Queued instance that ultimately reaches the target PFQ.
        std::optional<StagedPrefetchToken> stagedToken;
        Queued *stagedCompletionOwner = nullptr;

        /**
         * Constructor
         * @param o QueuedPrefetcher in charge of this request
         * @param pfi PrefechInfo object associated to this packet
         * @param t Time when this prefetch becomes ready
         * @param p PacketPtr with the memory request of the prefetch
         * @param prio This prefetch priority
         */
        DeferredPacket(Queued *o, PrefetchInfo const &pfi, Tick t,
            int32_t prio) : owner(o), pfInfo(pfi), tick(t), pkt(nullptr),
            priority(prio), pfahead(false), pfahead_host(0),
            translationRequest(), tc(nullptr),
            ongoingTranslation(false) {
        }

        bool operator>(const DeferredPacket& that) const
        {
            return priority > that.priority;
        }
        bool operator<(const DeferredPacket& that) const
        {
            return priority < that.priority;
        }
        bool operator<=(const DeferredPacket& that) const
        {
            return !(*this > that);
        }

        /**
         * Create the associated memory packet
         * @param paddr physical address of this packet
         * @param blk_size block size used by the prefetcher
         * @param requestor_id Requestor ID of the access that generated
         * this prefetch
         * @param tag_prefetch flag to indicate if the packet needs to be
         *        tagged
         * @param t time when the prefetch becomes ready
         * @param pf_desc prefetch info associated to this packet
         */
        void createPkt(Addr paddr, unsigned blk_size, RequestorID requestor_id,
                       bool tag_prefetch, Tick t, PrefetchSourceType pf_src, int prf_depth);

        /**
         * Sets the translation request needed to obtain the physical address
         * of this request.
         * @param req The Request with the virtual address of this request
         */
        void setTranslationRequest(const RequestPtr &req)
        {
            translationRequest = req;
        }

        void markDelayed() override
        {}

        void finish(const Fault &fault, const RequestPtr &req,
                            ThreadContext *tc, BaseMMU::Mode mode) override;

        /**
         * Issues the translation request to the provided TLB
         * @param tlb the tlb that has to translate the address
         */
        void startTranslation(BaseTLB *tlb);
    };

    std::list<DeferredPacket> pfq;
    std::list<DeferredPacket> pfqMissingTranslation;
    std::list<DeferredPacket> pfqSquashed;

    using const_iterator = std::list<DeferredPacket>::const_iterator;
    using iterator = std::list<DeferredPacket>::iterator;

    // PARAMETERS

    /** Maximum size of the prefetch queue */
    const unsigned queueSize;

    /**
     * Maximum size of the queue holding prefetch requests with missing
     * address translations
     */
    const unsigned missingTranslationQueueSize;

    /** Cycles after generation when a prefetch can first be issued */
    const Cycles latency;

    /** Squash queued prefetch if demand access observed */
    const bool queueSquash;

    /** Filter prefetches if already queued */
    const bool queueFilter;

    /** Snoop the cache before generating prefetch (cheating basically) */
    const bool cacheSnoop;

    /** Tag prefetch with PC of generating access? */
    const bool tagPrefetch;

    /** Percentage of requests that can be throttled */
    const unsigned int throttleControlPct;

    /** Enable windowed admission sweep for prefetch control. */
    const bool pfControl;

    /** Window length in this prefetcher's clock domain. */
    const Cycles pfControlWindow;

    /** Fallback admission percentage when no sweep action is active. */
    const unsigned pfControlDefaultAdmitPct;

    /** Optional table of admission percentages to sweep over time. */
    const std::vector<unsigned> pfControlSweep;

    /** Optional per-source admission override; -1 means use global action. */
    const std::array<int, NUM_PF_SOURCES> pfControlSourceAdmitPct;

    /** Number of windows to stay at each sweep entry. */
    const unsigned pfControlSweepWindows;

    /** Initial windows that keep the fallback admission percentage. */
    const unsigned pfControlWarmupWindows;

    /** Enable online PFBad-guided adaptive admission. */
    const bool pfAdaptive;

    /** Lowest adaptive admission percentage for an active source. */
    const unsigned pfAdaptiveMinPct;

    /** Adaptive admission quantization step. */
    const unsigned pfAdaptivePctQuantum;

    /** Per-window gradient step in percentage points. */
    const int pfAdaptiveGradientStep;

    /** PFBad penalty multiplier as numerator / denominator. */
    const unsigned pfAdaptivePfBadWeightNumerator;
    const unsigned pfAdaptivePfBadWeightDenominator;

    /** Minimum useful+bad samples before trusting a source gradient. */
    const unsigned pfAdaptiveGradientMinSamples;

    /** Deadband around zero useful-minus-weighted-bad score. */
    const int pfAdaptiveGradientDeadband;

    /** Required miss-rate improvement in basis points. */
    const unsigned pfAdaptiveImproveMarginBps;

    /** Enable historical best-sample fallback in adaptive updates. */
    const bool pfAdaptiveHistoryFallback;

    /** Number of best FIFO samples to average for fallback pct. */
    const unsigned pfAdaptiveBestTopK;

    /** FIFO experience table capacity. */
    const unsigned pfAdaptiveTableEntries;

    /** PFBad detection FIFO capacity. */
    const unsigned pfAdaptivePfBadEntries;

    /** Number of completed initial windows before adaptive updates. */
    const unsigned pfAdaptiveWarmupWindows;

    /** Maximum pct movement per source per adaptive update. */
    const unsigned pfAdaptiveMaxSourceStep;

    /** Windowed counters used by the online adaptive controller. */
    uint64_t pfAdaptiveWindowDemandAccesses;
    uint64_t pfAdaptiveWindowDemandMisses;
    std::array<uint64_t, NUM_PF_SOURCES> pfAdaptiveWindowPfUsefulBySource;
    std::array<uint64_t, NUM_PF_SOURCES> pfAdaptiveWindowPfUnusedBySource;
    /** Cache miss requests hitting the PFBad table; overflow is not included. */
    std::array<uint64_t, NUM_PF_SOURCES> pfAdaptiveWindowPfBadHitsBySource;
    uint64_t pfAdaptiveSampleCount;

    struct PfAdaptiveSample
    {
        uint64_t windowIndex = 0;
        uint64_t demandAccesses = 0;
        uint64_t demandMisses = 0;
        std::array<unsigned, NUM_PF_SOURCES> pctBySource{};
        std::array<int, NUM_PF_SOURCES> gradientBySource{};
        std::array<uint64_t, NUM_PF_SOURCES> pfUsefulBySource{};
        std::array<uint64_t, NUM_PF_SOURCES> pfUnusedBySource{};
        std::array<uint64_t, NUM_PF_SOURCES> pfBadHitsBySource{};
    };

    struct PfBadEntry
    {
        bool valid = false;
        Addr blockAddr = 0;
        bool secure = false;
        PrefetchSourceType evictorSource = PrefetchSourceType::PF_NONE;
        uint64_t insertWindow = 0;
    };

    std::deque<PfAdaptiveSample> pfAdaptiveSamples;
    std::deque<PfBadEntry> pfAdaptivePfBadTable;

    Cycles pfControlWindowStart;
    bool pfControlWindowStarted;
    uint64_t pfControlWindowIndex;
    uint64_t pfControlWindowCandidates;
    uint64_t pfControlWindowAdmitted;
    unsigned pfControlCurrentAdmitPct;
    std::array<uint64_t, NUM_PF_SOURCES> pfControlWindowCandidatesBySource;
    std::array<uint64_t, NUM_PF_SOURCES> pfControlWindowAdmittedBySource;
    std::array<unsigned, NUM_PF_SOURCES> pfControlCurrentAdmitPctBySource;

    EventFunctionWrapper tlbReqEvent;

    struct QueuedStats : public statistics::Group
    {
        QueuedStats(statistics::Group *parent);
        // STATS
        statistics::Scalar pfIdentified;
        statistics::Scalar pfBufferHit;
        statistics::Scalar pfInCache;
        statistics::Scalar pfRemovedDemand;
        statistics::Scalar pfRemovedFull;
        statistics::Scalar pfSpanPage;
        statistics::Scalar pfUsefulSpanPage;
        statistics::Vector pfRemovedFull_srcs;
        statistics::Scalar pfControlCandidates;
        statistics::Scalar pfControlAdmitted;
        statistics::Scalar pfControlDropped;
        statistics::Scalar pfControlWindows;
        statistics::Scalar pfControlCurrentAdmitPct;
        statistics::Vector pfControlCandidatesByPct;
        statistics::Vector pfControlAdmittedByPct;
        statistics::Vector pfControlDroppedByPct;
        statistics::Vector pfControlWindowsByPct;
        statistics::Vector pfControlCandidatesBySource;
        statistics::Vector pfControlAdmittedBySource;
        statistics::Vector pfControlDroppedBySource;
        statistics::Vector pfControlCurrentAdmitPctBySource;
        statistics::Scalar pfAdaptiveWindows;
        statistics::Scalar pfAdaptiveUpdates;
        statistics::Scalar pfAdaptiveWarmupWindows;
        statistics::Scalar pfAdaptiveDemandAccesses;
        statistics::Scalar pfAdaptiveDemandMisses;
        statistics::Scalar pfAdaptiveTableSize;
        statistics::Scalar pfAdaptiveTableEvictions;
        statistics::Scalar pfAdaptivePfBadTableSize;
        statistics::Scalar pfAdaptivePfBadTableEvictions;
        statistics::Scalar pfAdaptivePfBadCandidates;
        statistics::Vector pfAdaptivePfBadCandidatesBySource;
        statistics::Scalar pfAdaptivePfBadOverflowEvictions;
        statistics::Scalar pfAdaptivePfBadHits;
        statistics::Vector pfAdaptivePctBySource;
        statistics::Vector pfAdaptiveGradientBySource;
        statistics::Vector pfAdaptivePfUsefulBySource;
        statistics::Vector pfAdaptivePfBadHitsBySource;
        statistics::Vector pfAdaptivePfBadOverflowBySource;
    } statsQueued;

  public:

    Queued(const QueuedPrefetcherParams &p);
    virtual ~Queued();

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;

    InsertResult insert(const PacketPtr &pkt, PrefetchInfo &new_pfi,
                        const AddrPriority &addr_prio);

    virtual void calculatePrefetch(const PrefetchInfo &pfi,
                                   std::vector<AddrPriority> &addresses) = 0;
    virtual void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                                   PrefetchSourceType source, bool miss_repeat);
    PacketPtr getPacket() override;

    bool hasPendingPacket() override;

    bool admitIncomingPrefetchPacket(const PacketPtr &pkt) override;

    Tick nextPrefetchReadyTime() const override
    {
        return pfq.empty() ? MaxTick : pfq.front().tick;
    }

    void printQueue(const std::list<DeferredPacket> &queue) const;

  protected:

    /**
     * Adds a DeferredPacket to the specified queue
     * @param queue selected queue to use
     * @param dpp DeferredPacket to add
     */
    virtual void addToQueue(std::list<DeferredPacket> &queue, DeferredPacket &dpp);

    /** Whether addToQueue() will hand this packet to the next cache level. */
    bool willForwardToDownStream(const std::list<DeferredPacket> &queue,
                                 const DeferredPacket &dpp) const;

    /**
     * Notify a staged producer once this queue has accepted or terminally
     * rejected a candidate. Ordinary producers leave the marker clear.
     */
    virtual void completeStagedPrefetch(const StagedPrefetchToken &token,
                                        bool accepted)
    {}

    /** Return a reserved staged candidate to its producer without retiring it. */
    virtual void releaseStagedPrefetch(const StagedPrefetchToken &token)
    {}

    /** Complete a deferred staged producer exactly once. */
    void completeDeferredStagedPrefetch(DeferredPacket &dpp, bool accepted);

    /**
     * Starts the translations of the queued prefetches with a
     * missing translation. It performs a maximum specified number of
     * translations. Successful translations cause the prefetch request to be
     * queued in the queue of ready requests.
     * @param max maximum number of translations to perform
     */
    void processMissingTranslations(unsigned max);

    /**
     * Indicates that the translation of the address of the provided  deferred
     * packet has been successfully completed, and it can be enqueued as a
     * new prefetch request.
     * @param dp the deferred packet that has completed the translation request
     * @param failed whether the translation was successful
     */
    void translationComplete(DeferredPacket *dp, bool failed);

    /**
     * Checks whether the specified prefetch request is already in the
     * specified queue. If the request is found, its priority is updated.
     * @param queue selected queue to check
     * @param pfi information of the prefetch request to be added
     * @param priority priority of the prefetch request to be added
     * @return True if the prefetch request was found in the queue
     */
    bool alreadyInQueue(std::list<DeferredPacket> &queue,
                        const PrefetchInfo &pfi, int32_t priority);
    bool alreadyInQueue(std::list<DeferredPacket> &queue,
                                    Addr addr, bool isSecure, int32_t priority);

    /**
     * Returns the maxmimum number of prefetch requests that are allowed
     * to be created from the number of prefetch candidates provided.
     * The behavior of this service is controlled with the throttleControlPct
     * parameter.
     * @param total number of prefetch candidates generated by the prefetcher
     * @return the number of these request candidates are allowed to be created
     */
    size_t getMaxPermittedPrefetches(size_t total) const;

    RequestPtr createPrefetchRequest(Addr addr, PrefetchInfo const &pfi, PacketPtr pkt, PrefetchSourceType pf_src, int prf_depth);

    unsigned offloadBandwidth{1};

    unsigned sanitizePfControlPct(unsigned pct) const;
    PrefetchSourceType sanitizePfControlSourceType(
        PrefetchSourceType source) const;
    unsigned sanitizePfControlSource(PrefetchSourceType source) const;
    bool isPfControlSourceNone(PrefetchSourceType source) const;
    unsigned getPfControlActionPct(uint64_t window_index) const;
    unsigned getPfControlActionPctForSource(
        uint64_t window_index, PrefetchSourceType source) const;
    void refreshPfControlCurrentPcts();
    void updatePfControlWindow();
    bool shouldPfControlAdmitLocally(
        bool pfahead, int pfahead_host) const;
    bool admitPfControlCandidate(PrefetchSourceType source);
    bool admitPfControlDeferredPacket(const DeferredPacket &dpp);
    bool isPfAdaptiveLevel() const;
    unsigned quantizePfAdaptivePct(unsigned pct) const;
    unsigned clampPfAdaptivePct(int pct) const;
    int computePfAdaptiveSourceGradient(
        uint64_t useful, uint64_t pfbad_hits, uint64_t unused) const;
    uint64_t pfAdaptiveMissRateBps(
        uint64_t misses, uint64_t accesses) const;
    unsigned getPfAdaptiveBestPct(PrefetchSourceType source) const;
    void pushPfAdaptiveSample(const PfAdaptiveSample &sample);
    void applyPfAdaptiveUpdate(const PfAdaptiveSample &sample);
    void resetPfAdaptiveWindowCounters();
    std::deque<PfBadEntry>::iterator findPfAdaptivePfBadEntry(
        Addr paddr, bool is_secure);
    void recordPfAdaptiveDemandMiss();
    void recordPfAdaptivePfBadHit(PrefetchSourceType evictor_source);
    bool recordPfAdaptivePfBadMissHit(Addr paddr, bool is_secure);
    bool clearPfAdaptivePfBadEntry(Addr paddr, bool is_secure);
    void recordPfAdaptivePfBadOverflowEviction(
        PrefetchSourceType evictor_source);

  public:
    void notifyDemandAccess(Addr paddr, bool is_secure, bool miss) override;
    void notifyCacheMissRequest(Addr paddr, bool is_secure) override;
    void notifyDemandMshrMiss(Addr paddr, bool is_secure) override;
    void notifyPrefetchUseful(PrefetchSourceType source) override;
    void notifyPrefetchEvictsDemand(
        Addr victim_paddr, bool is_secure,
        PrefetchSourceType evictor_source) override;
    void notifyCachelineRefill(Addr paddr, bool is_secure) override;
    void prefetchUnused(PrefetchSourceType pf_source) override;

  public:
    void rxHint(BaseMMU::Translation *dpp) override {
        panic("QueuedPrefetcher: rxHint not implemented");
    }
    void pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt) override {
    }
    void offloadToDownStream() override;
  protected:
    const bool usePFBuffer{false};
    std::list<AddrPriority> PFRequestBuffer;
    const int max_pf_buffer_size{8};
    //here we implement a buffer that drop the pf requests when the buffer is full
    virtual void InsertPFRequestToBuffer(const AddrPriority &addr_prio) {
        if (PFRequestBuffer.size() < max_pf_buffer_size) {
            PFRequestBuffer.push_back(addr_prio);
        }else{
            PFRequestBuffer.pop_front();
            PFRequestBuffer.push_back(addr_prio);
        }
    };
    /** Event to handle the delay queue processing */
    void PFSendEventWrapper();
    EventFunctionWrapper PFReqSendEvent;

  public:
    virtual bool hasPFRequestsInBuffer()  {
        return !PFRequestBuffer.empty();
    }
    virtual bool GetPFRequestsFromBuffer(std::vector<AddrPriority> &addresses) {
        if (PFRequestBuffer.empty()) {
            return false;
        }
        AddrPriority addr_prio = PFRequestBuffer.front();
        PFRequestBuffer.pop_front();
        addresses.push_back(addr_prio);
        return true;
    };
};

} // namespace prefetch
} // namespace gem5

#endif //__MEM_CACHE_PREFETCH_QUEUED_HH__
