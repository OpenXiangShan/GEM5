/*
 * Copyright (c) 2013-2014 ARM Limited
 * All rights reserved.
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
 * Copyright (c) 2005 The Regents of The University of Michigan
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

/**
 * @file
 * Miss and writeback queue declarations.
 */

#ifndef __MEM_CACHE_PREFETCH_BASE_HH__
#define __MEM_CACHE_PREFETCH_BASE_HH__

#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>

#include "arch/generic/tlb.hh"
#include "base/compiler.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/cache_probe_arg.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/arch_db.hh"
#include "sim/byteswap.hh"
#include "sim/clocked_object.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

struct BasePrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class PrefetcherForwarder;
struct CustomPfInfo
{
    float coverage;
};

class Base : public ClockedObject
{
    public:
    struct PFtriggerInfo;
    class PrefetchInfo;
    class PrefetchInfo_old;
    private:
    friend class PrefetcherForwarder;
    class PrefetchListener : public ProbeListenerArgBase<PacketPtr>
    {
      public:
        PrefetchListener(Base &_parent, ProbeManager *pm,
                         const std::string &name, bool _isFill = false,
                         bool _miss = false, bool _pftrain = false)
            : ProbeListenerArgBase(pm, name),
              parent(_parent), isFill(_isFill), miss(_miss), coreDirectNotify(_pftrain) {}
        void notify(const PacketPtr &pkt) override;
      protected:
        Base& parent;
        const bool isFill;
        const bool miss;

        // Core can directly pass address to train or trigger prefetchers, for example, store prefetch
        const bool coreDirectNotify;
    };

    std::vector<PrefetchListener *> listeners;

  public:
    struct PFtriggerInfo{
        PacketPtr pkt;
        std::unique_ptr<PrefetchInfo_old> pfi_old; 
        PrefetchSourceType pfSourceType;
        PFtriggerInfo() : pkt(nullptr), pfi_old(nullptr), pfSourceType(PrefetchSourceType::PF_NONE) {}
        PFtriggerInfo(PacketPtr p, const PrefetchInfo &a)
            : pkt(p ? new Packet(p, false, false) : nullptr),
            pfi_old(std::make_unique<PrefetchInfo_old>(a)), pfSourceType(PrefetchSourceType::PF_NONE) {}
        PFtriggerInfo(const PFtriggerInfo &other)
            : pkt(other.pkt ? new Packet(other.pkt, false, false) : nullptr),
              pfi_old(other.pfi_old ? std::make_unique<PrefetchInfo_old>(*(other.pfi_old)) : nullptr),
              pfSourceType(other.pfSourceType) {}
        PFtriggerInfo& operator=(const PFtriggerInfo &other)
        {
            if (this != &other) {
                delete pkt;
                pkt = other.pkt ? new Packet(other.pkt, false, false) : nullptr;
                pfi_old = std::make_unique<PrefetchInfo_old>(*(other.pfi_old));
                pfSourceType = other.pfSourceType;
            }
            return *this;
        }
        // PFtriggerInfo(PFtriggerInfo &&other) noexcept
        //     : pkt(other.pkt), pfi_old(std::move(other.pfi_old))
        // {
        //     other.pkt = nullptr;
        // }
        // PFtriggerInfo& operator=(PFtriggerInfo &&other) noexcept
        // {
        //     if (this != &other) {
        //         delete pkt;
        //         pkt = other.pkt;
        //         pfi_old = std::move(other.pfi_old);
        //         other.pkt = nullptr;
        //     }
        //     return *this;
        // }
        ~PFtriggerInfo()
        {
            delete pkt;
            pfi_old.reset();
        }
    };
    /**
     * Class containing the information needed by the prefetch to train and
     * generate new prefetch requests.
     */
    class PrefetchInfo
    {
        friend class PrefetchInfo_old;
        /** The address used to train and generate prefetches */
        Addr address;
        /** The program counter that generated this address. */
        Addr pc;
        /** The requestor ID that generated this address. */
        RequestorID requestorId;
        /** The thread context that generated this address. */
        ContextID _contextId;
        /** Whether the thread context is valid. */
        bool validContextId;
        /** Validity bit for the PC of this address. */
        bool validPC;
        /** Whether this address targets the secure memory space. */
        bool secure;
        /** Size in bytes of the request triggering this event */
        unsigned int size;
        /** Whether this event comes from a write request */
        bool write;
        /** Physical address, needed because address can be virtual */
        Addr paddress;
        /** Whether this event comes from a cache miss */
        bool cacheMiss;
        /** Pointer to the associated request data */
        uint8_t *data;
        /** XiangShan metadata of the block*/
        Request::XsMetadata xsMetadata;

        bool reqAfterSquash{false};

        bool everPrefetched{false};

        bool pfFirstHit{false};

        bool pfHit{false};

        bool storePFTrain{ false };

        uint64_t *data_ptr;

      public:
        uint64_t * getDataPtr()const{
            return data_ptr;
        }
        /**
         * Obtains the address value of this Prefetcher address.
         * @return the addres value.
         */
        Addr getAddr() const
        {
            return address;
        }

        /**
         * Returns true if the address targets the secure memory space.
         * @return true if the address targets the secure memory space.
         */
        bool isSecure() const
        {
            return secure;
        }

        /**
         * Returns the program counter that generated this request.
         * @return the pc value
         */
        Addr getPC() const
        {
            assert(hasPC());
            return pc;
        }

        /**
         * Returns true if the associated program counter is valid
         * @return true if the program counter has a valid value
         */
        bool hasPC() const
        {
            return validPC;
        }

        /**
         * Gets the requestor ID that generated this address
         * @return the requestor ID that generated this address
         */
        RequestorID getRequestorId() const
        {
            return requestorId;
        }

        bool hasContextId() const
        {
            return validContextId;
        }

        ContextID contextId() const
        {
            assert(hasContextId());
            return _contextId;
        }

        /**
         * Gets the size of the request triggering this event
         * @return the size in bytes of the request triggering this event
         */
        unsigned int getSize() const
        {
            return size;
        }

        /**
         * Checks if the request that caused this prefetch event was a write
         * request come from committed store inst
         * @return true if the request causing this event is a write request
         */
        bool isWrite() const
        {
            return write;
        }

        // is come from store prefetch train trigger
        bool isStore() const
        {
            return storePFTrain;
        }

        /**
         * Gets the physical address of the request
         * @return physical address of the request
         */
        Addr getPaddr() const
        {
            return paddress;
        }

        /**
         * Check if this event comes from a cache miss
         * @result true if this event comes from a cache miss
         */
        bool isCacheMiss() const
        {
            return cacheMiss;
        }

        /**
         * Gets the associated data of the request triggering the event
         * @param Byte ordering of the stored data
         * @return the data
         */
        template <typename T>
        inline T
        get(ByteOrder endian) const
        {
            if (data == nullptr) {
                panic("PrefetchInfo::get called with a request with no data.");
            }
            switch (endian) {
                case ByteOrder::big:
                    return betoh(*(T*)data);

                case ByteOrder::little:
                    return letoh(*(T*)data);

                default:
                    panic("Illegal byte order in PrefetchInfo::get()\n");
            };
        }

        /**
         * Check for equality
         * @param pfi PrefetchInfo to compare against
         * @return True if this object and the provided one are equal
         */
        bool sameAddr(PrefetchInfo const &pfi) const
        {
            return this->getAddr() == pfi.getAddr() &&
                this->isSecure() == pfi.isSecure() &&
                this->sameContext(pfi);
        }

        bool sameContext(PrefetchInfo const &pfi) const
        {
            if (hasContextId() != pfi.hasContextId()) {
                return false;
            }
            return !hasContextId() || _contextId == pfi.contextId();
        }

        bool sameAddr(Addr addr, bool isSecure) const
        {
            return this->getAddr() == addr &&
                this->isSecure() == isSecure;
        }

        Request::XsMetadata getXsMetadata() const
        {
            return xsMetadata;
        }

        void setXsMetadata(const Request::XsMetadata &xs_metadata)
        {
            this->xsMetadata = xs_metadata;
        }

        bool isReqAfterSquash() const
        {
            return reqAfterSquash;
        }

        void setReqAfterSquash(bool req_after_squash)
        {
            reqAfterSquash = req_after_squash;
        }

        bool isEverPrefetched() const { return everPrefetched; }

        void setEverPrefetched(bool prefetched) { everPrefetched = prefetched; }

        bool isPfHit() const { return pfHit; }

        void setPfHit(bool hit) { pfHit = hit; }

        bool isPfFirstHit() const { return pfFirstHit; }

        void setPfFirstHit(bool hit) { pfFirstHit = hit; }

        void setStorePftrain(bool s) { storePFTrain = s; }

        /**
         * Constructs a PrefetchInfo using a PacketPtr.
         * @param pkt PacketPtr used to generate the PrefetchInfo
         * @param addr the address value of the new object, this address is
         *        used to train the prefetcher
         * @param miss whether this event comes from a cache miss
         */
        PrefetchInfo(PacketPtr pkt, Addr addr, bool miss);

        PrefetchInfo(PacketPtr pkt, Addr addr, bool miss, Request::XsMetadata xsMeta);

        /**
         * Constructs a PrefetchInfo using a new address value and
         * another PrefetchInfo as a reference.
         * @param pfi PrefetchInfo used to generate this new object
         * @param addr the address value of the new object
         */
        PrefetchInfo(PrefetchInfo const &pfi, Addr addr);
        PrefetchInfo(PrefetchInfo_old const &pfi);

        ~PrefetchInfo()
        {
            delete[] data;
        }

        bool lastPfLate{false};
        mutable PFtriggerInfo trigger_info{};
        void setTriggerInfo(const PacketPtr &pkt) const {
            trigger_info = PFtriggerInfo(pkt, *this);
        }
        void setTriggerInfo_PFsrc(const PrefetchSourceType pfSource) const {
            trigger_info.pfSourceType = pfSource;
        }
    };
    /**
     * Class containing the information needed by the prefetch to train and
     * generate new prefetch requests. this is only used by PFtriggerInfo
     */
    class PrefetchInfo_old
    {
        friend class PrefetchInfo;
        /** The address used to train and generate prefetches */
        Addr address;
        /** The program counter that generated this address. */
        Addr pc;
        /** The requestor ID that generated this address. */
        RequestorID requestorId;
        /** The thread context that generated this address. */
        ContextID _contextId;
        /** Whether the thread context is valid. */
        bool validContextId;
        /** Validity bit for the PC of this address. */
        bool validPC;
        /** Whether this address targets the secure memory space. */
        bool secure;
        /** Size in bytes of the request triggering this event */
        unsigned int size;
        /** Whether this event comes from a write request */
        bool write;
        /** Physical address, needed because address can be virtual */
        Addr paddress;
        /** Whether this event comes from a cache miss */
        bool cacheMiss;
        /** Pointer to the associated request data */
        uint8_t *data;
        /** XiangShan metadata of the block*/
        Request::XsMetadata xsMetadata;

        bool reqAfterSquash{false};

        bool everPrefetched{false};

        bool pfFirstHit{false};

        bool pfHit{false};

        bool storePFTrain{ false };

        uint64_t *data_ptr;

      public:
        uint64_t * getDataPtr()const{
            return data_ptr;
        }
        /**
         * Obtains the address value of this Prefetcher address.
         * @return the addres value.
         */
        Addr getAddr() const
        {
            return address;
        }

        /**
         * Returns true if the address targets the secure memory space.
         * @return true if the address targets the secure memory space.
         */
        bool isSecure() const
        {
            return secure;
        }

        /**
         * Returns the program counter that generated this request.
         * @return the pc value
         */
        Addr getPC() const
        {
            assert(hasPC());
            return pc;
        }

        /**
         * Returns true if the associated program counter is valid
         * @return true if the program counter has a valid value
         */
        bool hasPC() const
        {
            return validPC;
        }

        /**
         * Gets the requestor ID that generated this address
         * @return the requestor ID that generated this address
         */
        RequestorID getRequestorId() const
        {
            return requestorId;
        }

        bool hasContextId() const
        {
            return validContextId;
        }

        ContextID contextId() const
        {
            assert(hasContextId());
            return _contextId;
        }

        /**
         * Gets the size of the request triggering this event
         * @return the size in bytes of the request triggering this event
         */
        unsigned int getSize() const
        {
            return size;
        }

        /**
         * Checks if the request that caused this prefetch event was a write
         * request come from committed store inst
         * @return true if the request causing this event is a write request
         */
        bool isWrite() const
        {
            return write;
        }

        // is come from store prefetch train trigger
        bool isStore() const
        {
            return storePFTrain;
        }

        /**
         * Gets the physical address of the request
         * @return physical address of the request
         */
        Addr getPaddr() const
        {
            return paddress;
        }

        /**
         * Check if this event comes from a cache miss
         * @result true if this event comes from a cache miss
         */
        bool isCacheMiss() const
        {
            return cacheMiss;
        }

        /**
         * Gets the associated data of the request triggering the event
         * @param Byte ordering of the stored data
         * @return the data
         */
        template <typename T>
        inline T
        get(ByteOrder endian) const
        {
            if (data == nullptr) {
                panic("PrefetchInfo::get called with a request with no data.");
            }
            switch (endian) {
                case ByteOrder::big:
                    return betoh(*(T*)data);

                case ByteOrder::little:
                    return letoh(*(T*)data);

                default:
                    panic("Illegal byte order in PrefetchInfo::get()\n");
            };
        }

        /**
         * Check for equality
         * @param pfi PrefetchInfo to compare against
         * @return True if this object and the provided one are equal
         */
        bool sameAddr(PrefetchInfo_old const &pfi) const
        {
            return this->getAddr() == pfi.getAddr() &&
                this->isSecure() == pfi.isSecure() &&
                this->sameContext(pfi);
        }

        bool sameContext(PrefetchInfo_old const &pfi) const
        {
            if (hasContextId() != pfi.hasContextId()) {
                return false;
            }
            return !hasContextId() || _contextId == pfi.contextId();
        }

        bool sameAddr(Addr addr, bool isSecure) const
        {
            return this->getAddr() == addr &&
                this->isSecure() == isSecure;
        }

        Request::XsMetadata getXsMetadata() const
        {
            return xsMetadata;
        }

        void setXsMetadata(const Request::XsMetadata &xs_metadata)
        {
            this->xsMetadata = xs_metadata;
        }

        bool isReqAfterSquash() const
        {
            return reqAfterSquash;
        }

        void setReqAfterSquash(bool req_after_squash)
        {
            reqAfterSquash = req_after_squash;
        }

        bool isEverPrefetched() const { return everPrefetched; }

        void setEverPrefetched(bool prefetched) { everPrefetched = prefetched; }

        bool isPfHit() const { return pfHit; }

        void setPfHit(bool hit) { pfHit = hit; }

        bool isPfFirstHit() const { return pfFirstHit; }

        void setPfFirstHit(bool hit) { pfFirstHit = hit; }

        void setStorePftrain(bool s) { storePFTrain = s; }

        /**
         * Constructs a PrefetchInfo using a PacketPtr.
         * @param pkt PacketPtr used to generate the PrefetchInfo
         * @param addr the address value of the new object, this address is
         *        used to train the prefetcher
         * @param miss whether this event comes from a cache miss
         */
        PrefetchInfo_old(PacketPtr pkt, Addr addr, bool miss);

        PrefetchInfo_old(PacketPtr pkt, Addr addr, bool miss, Request::XsMetadata xsMeta);

        /**
         * Constructs a PrefetchInfo using a new address value and
         * another PrefetchInfo as a reference.
         * @param pfi PrefetchInfo used to generate this new object
         * @param addr the address value of the new object
         */
        PrefetchInfo_old(PrefetchInfo_old const &pfi, Addr addr);

        PrefetchInfo_old(PrefetchInfo_old const &other);

        PrefetchInfo_old(PrefetchInfo const &pfi);
        
        ~PrefetchInfo_old()
        {
            delete[] data;
        }

        bool lastPfLate{false};
    };
  protected:
    /**
     * TrainFilter: ROB-order training request filtering and reordering
     *
     * The TrainFilter collects training requests within a cycle, reorders them
     * by ROB sequence number (Load first, then Store), filters duplicates, and
     * feeds them into a FIFO training buffer at a rate of one per cycle.
     */
    struct TrainingRequest
    {
        RequestPtr req;
        MemCmd cmd;
        PacketDataPtr dataCopy;         // Deep copy of packet data
        unsigned dataSize;              // Size of data copied

        Addr addr;                      // Training address
        bool miss;
        Request::XsMetadata xsMetadata;
        bool everPrefetched;
        bool pfFirstHit;
        bool pfHit;
        bool squashMark;                // Request after squash

        // TrainFilter fields for ROB-order training
        InstSeqNum seqNum;              // ROB sequence number for ordering
        Addr blockAddr;                 // Cache block address for filtering
        bool isLoad;

        // Constructor: Extract copies from PacketPtr
        TrainingRequest(PacketPtr pkt, Addr _addr, bool _miss,
                       const Request::XsMetadata &_xsMetadata,
                       bool _everPrefetched, bool _pfFirstHit,
                       bool _pfHit, bool _squashMark,
                       InstSeqNum _seqNum, Addr _blockAddr, bool _isLoad)
            : req(pkt->req),
              cmd(pkt->cmd),
              dataCopy(nullptr),
              dataSize(pkt->getSize()),
              addr(_addr),
              miss(_miss),
              xsMetadata(_xsMetadata),
              everPrefetched(_everPrefetched),
              pfFirstHit(_pfFirstHit),
              pfHit(_pfHit),
              squashMark(_squashMark),
              seqNum(_seqNum),
              blockAddr(_blockAddr),
              isLoad(_isLoad)
        {
            // Deep copy packet data if present
            if (pkt->flags.isSet(Packet::STATIC_DATA | Packet::DYNAMIC_DATA)) {
                dataCopy = new uint8_t[dataSize];
                std::memcpy(dataCopy, pkt->getConstPtr<uint8_t>(), dataSize);
            }
        }

        // Destructor: Free our owned data copy
        ~TrainingRequest() {
            if (dataCopy) {
                delete[] dataCopy;
                dataCopy = nullptr;
            }
        }

        // Move constructor: Transfer ownership of dataCopy
        TrainingRequest(TrainingRequest&& other) noexcept
            : req(std::move(other.req)),
              cmd(other.cmd),
              dataCopy(other.dataCopy),
              dataSize(other.dataSize),
              addr(other.addr),
              miss(other.miss),
              xsMetadata(other.xsMetadata),
              everPrefetched(other.everPrefetched),
              pfFirstHit(other.pfFirstHit),
              pfHit(other.pfHit),
              squashMark(other.squashMark),
              seqNum(other.seqNum),
              blockAddr(other.blockAddr),
              isLoad(other.isLoad)
        {
            other.dataCopy = nullptr;  // Transfer ownership
        }

        // Move assignment: Transfer ownership of dataCopy
        TrainingRequest& operator=(TrainingRequest&& other) noexcept {
            if (this != &other) {
                // Free our old data
                if (dataCopy) delete[] dataCopy;

                // Transfer everything from other
                req = std::move(other.req);
                cmd = other.cmd;
                dataCopy = other.dataCopy;
                dataSize = other.dataSize;
                addr = other.addr;
                miss = other.miss;
                xsMetadata = other.xsMetadata;
                everPrefetched = other.everPrefetched;
                pfFirstHit = other.pfFirstHit;
                pfHit = other.pfHit;
                squashMark = other.squashMark;
                seqNum = other.seqNum;
                blockAddr = other.blockAddr;
                isLoad = other.isLoad;

                other.dataCopy = nullptr;
            }
            return *this;
        }

        // Disable copy constructor/assignment
        TrainingRequest(const TrainingRequest&) = delete;
        TrainingRequest& operator=(const TrainingRequest&) = delete;
    };

    std::vector<TrainingRequest> currentCycleLoads;
    std::vector<TrainingRequest> currentCycleStores;
    std::deque<TrainingRequest> trainingBuffer;

    std::unordered_set<Addr> trainingBufferBlockAddrs;

    /** Maximum size of the training buffer */
    const unsigned trainingBufferSize;


    /**
     * Periodic event that fires every cycle
     * Handles: 1) flush previous cycle requests, 2) train one request
     * This ensures training progresses even when there are no cache accesses
     */
    EventFunctionWrapper cycleEvent;

    void processCycle();

    void flushCurrentCycleRequests();

    /**
     * Train one request from the front of trainingBuffer
     * Dequeues one request and calls notify() to train the prefetcher
     */
    void processTraining();

    /** Whether to use training buffer (can be overridden by subclasses) */
    virtual bool useTrainingBuffer() const { return false; }

    Addr getBlockAddr(Addr addr) const { return blockAddress(addr); }

    InstSeqNum getSeqNum(const PacketPtr &pkt) const;

    bool isLoadRequest(const PacketPtr &pkt) const;

    bool isSubPrefetcher;

    ArchDBer* archDBer;

    // PARAMETERS

    /** Pointr to the parent cache. */
    CacheAccessor* cache = nullptr;

    /** Pointer to the parent system. */
    System* system = nullptr;

    /** Pointer to the parent cache's probe manager. */
    ProbeManager *probeManager = nullptr;

    /** The block size of the parent cache. */
    unsigned blkSize;

    /** log_2(block size of the parent cache). */
    unsigned lBlkSize;

    /** Only consult prefetcher on cache misses? */
    const bool onMiss;

    /** Consult prefetcher on reads? */
    const bool onRead;

    /** Consult prefetcher on reads? */
    const bool onWrite;

    /** Consult prefetcher on data accesses? */
    const bool onData;

    /** Consult prefetcher on instruction accesses? */
    const bool onInst;

    /** Request id for prefetches */
    const RequestorID requestorId;

    const Addr pageBytes;

    /** Allow upstream PF req train low level Prefetcher */
    const bool prefetchTrain;

    /** Prefetch on every access, not just misses */
    const bool prefetchOnAccess;

    /** Prefetch on hit on prefetched lines */
    const bool prefetchOnPfHit;

    /** Use Virtual Addresses for prefetching */
    const bool useVirtualAddresses;

    /**
     * Determine if this access should be observed
     * @param pkt The memory request causing the event
     * @param miss whether this event comes from a cache miss
     */
    virtual bool observeAccess(const PacketPtr &pkt, bool miss) const;

    /** Determine if address is in cache */
    bool inCache(Addr addr, bool is_secure) const;

    /** Determine if address is in cache miss queue */
    bool inMissQueue(Addr addr, bool is_secure) const;

    bool hasBeenPrefetched(Addr addr, bool is_secure) const;
    bool hasEverBeenPrefetched(Addr addr, bool is_secure) const;

    /** Determine if addresses are on the same page */
    bool samePage(Addr a, Addr b) const;
    /** Determine the address of the block in which a lays */
    Addr blockAddress(Addr a) const;
    /** Determine the address of a at block granularity */
    Addr blockIndex(Addr a) const;
    /** Determine the address of the page in which a lays */
    Addr pageAddress(Addr a) const;
    /** Determine the page-offset of a  */
    Addr pageOffset(Addr a) const;
    /** Build the address of the i-th block inside the page */
    Addr pageIthBlockAddress(Addr page, uint32_t i) const;
    struct StatGroup : public statistics::Group
    {
        StatGroup(statistics::Group *parent);
        statistics::Scalar demandMshrMisses;
        statistics::Scalar pfIssued;
        statistics::Vector pfIssued_srcs;

        statistics::Scalar pfOffloaded;
        statistics::Scalar pfaheadOffloaded;
        statistics::Scalar pfaheadProcess;

        /** The number of times a HW-prefetched block is evicted w/o
         * reference. */
        statistics::Scalar pfUnused;
        statistics::Vector pfUnused_srcs;
        /** The number of cache miss requests hitting the PFBad table. */
        statistics::Scalar pfBad;
        statistics::Vector pfBad_srcs;
        /** The number of times a HW-prefetch is useful. */
        statistics::Scalar pfUseful;

        statistics::Vector pfUseful_srcs;
        statistics::Vector pfHitInCache_srcs;
        statistics::Vector pfHitInMSHR_srcs;
        statistics::Vector pfHitInWB_srcs;
        statistics::Vector late_srcs;
        /** The number of times there is a hit on prefetch but cache block
         * is not in an usable state */
        statistics::Scalar pfUsefulButMiss;
        statistics::Formula accuracy;
        statistics::Formula coverage;

        /** The number of times a HW-prefetch hits in cache. */
        statistics::Scalar pfHitInCache;

        /** The number of times a HW-prefetch hits in a MSHR. */
        statistics::Scalar pfHitInMSHR;

        /** The number of times a HW-prefetch hits
         * in the Write Buffer (WB). */
        statistics::Scalar pfHitInWB;

        /** The number of prefetch requests generated by prefetcher. */
        statistics::Scalar pfGenerated;

        /** The number of prefetch requests filtered before issuing. */
        statistics::Scalar pfFiltered;

        /** The number of times a HW-prefetch is late
         * (hit in cache, MSHR, WB). */
        statistics::Formula pfLate;
    } prefetchStats;

    /** Total prefetches issued */
    uint64_t issuedPrefetches;
    /** Total prefetches that has been useful */
    uint64_t usefulPrefetches;

    uint64_t streamlatenum;

    /** Registered tlb for address translations */
    BaseTLB * tlb;

  public:
    Base(const BasePrefetcherParams &p);
    virtual ~Base() = default;

    virtual void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor* _cache, unsigned blk_size);

    /**
     * Notify prefetcher of cache access (may be any access or just
     * misses, depending on cache parameters.)
     */
    virtual void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) = 0;

    /** Notify prefetcher of cache fill */
    virtual void notifyFill(const PacketPtr &pkt)
    {}

    virtual PacketPtr getPacket() = 0;

    virtual bool hasPendingPacket() = 0;

    virtual Tick nextPrefetchReadyTime() const = 0;

    virtual void recvPrefetchFromCache(const PacketPtr &pkt) {}

    virtual bool admitIncomingPrefetchPacket(const PacketPtr &pkt)
    {
        return true;
    }

    virtual bool ownsPrefetchRequest(const PacketPtr &pkt) const
    {
        return pkt && pkt->req && pkt->req->requestorId() == requestorId;
    }

    virtual void recordIssuedPrefetch(PrefetchSourceType source)
    {
        const int source_idx = int(source);
        if (source_idx < 0 || source_idx >= NUM_PF_SOURCES) {
            source = PrefetchSourceType::PF_NONE;
        }
        prefetchStats.pfIssued++;
        prefetchStats.pfIssued_srcs[source]++;
        issuedPrefetches += 1;
    }

    virtual void recordIssuedPrefetch(const PacketPtr &pkt)
    {
        PrefetchSourceType source = PrefetchSourceType::PF_NONE;
        if (pkt && pkt->req) {
            if (pkt->req->hasXsMetadata()) {
                source = pkt->req->getXsMetadata().prefetchSource;
            } else {
                source = pkt->getPFSource();
            }
        }
        recordIssuedPrefetch(source);
    }

    virtual void
    prefetchUnused(PrefetchSourceType pfSource)
    {
        prefetchStats.pfUnused++;
        prefetchStats.pfUnused_srcs[pfSource]++;
    }

    virtual void prefetchUnused(Addr paddr, PrefetchSourceType pfSource) { prefetchUnused(pfSource); }

    virtual void recordPfBadHit(PrefetchSourceType source)
    {
        const int source_idx = int(source);
        if (source_idx < 0 || source_idx >= NUM_PF_SOURCES) {
            source = PrefetchSourceType::PF_NONE;
        }
        prefetchStats.pfBad++;
        prefetchStats.pfBad_srcs[source]++;
    }

    virtual void
    incrDemandMhsrMisses()
    {
        prefetchStats.demandMshrMisses++;
    }

    virtual void notifyDemandMshrMiss(Addr paddr, bool is_secure) {}

    virtual void notifyDemandAccess(Addr paddr, bool is_secure, bool miss) {}

    virtual void notifyCacheMissRequest(Addr paddr, bool is_secure) {}

    virtual void notifyPrefetchUseful(PrefetchSourceType source) {}

    virtual void notifyPrefetchEvictsDemand(
        Addr victim_paddr, bool is_secure, PrefetchSourceType evictor_source)
    {}

    virtual void notifyCachelineRefill(Addr paddr, bool is_secure) {}

    virtual void
    pfHitInCache(PrefetchSourceType pf_type)
    {
        prefetchStats.pfHitInCache++;
        prefetchStats.pfHitInCache_srcs[pf_type]++;
        prefetchStats.late_srcs[pf_type]++;
    }

    virtual void
    pfHitInMSHR(PrefetchSourceType pf_type)
    {
        prefetchStats.pfHitInMSHR++;
        prefetchStats.pfHitInMSHR_srcs[pf_type]++;
        prefetchStats.late_srcs[pf_type]++;
    }

    virtual void
    pfHitInWB(PrefetchSourceType pf_type)
    {
        prefetchStats.pfHitInWB++;
        prefetchStats.pfHitInWB_srcs[pf_type]++;
        prefetchStats.late_srcs[pf_type]++;
    }
    void streamPflate() { streamlatenum++; }

    /**
     * Register probe points for this object.
     */
    void regProbeListeners() override;

    /**
     * Process a notification event from the ProbeListener.
     * @param pkt The memory request causing the event
     * @param miss whether this event comes from a cache miss
     */
    virtual void probeNotify(const PacketPtr& pkt, bool miss);

    virtual void coreDirectAddrNotify(const PacketPtr& pkt);

    /**
     * Add a SimObject and a probe name to listen events from
     * @param obj The SimObject pointer to listen from
     * @param name The probe name
     */
    void addEventProbe(SimObject *obj, const char *name);

    /**
     * Add a BaseTLB object to be used whenever a translation is needed.
     * This is generally required when the prefetcher is allowed to generate
     * page crossing references and/or uses virtual addresses for training.
     * @param tlb pointer to the BaseTLB object to add
     */
    virtual void addTLB(BaseTLB *tlb, bool functional);

  protected:
    Base *hintDownStream{nullptr};

    bool squashMark{false};

    bool functionalTLB{false};

  public:
    virtual void addHintDownStream(Base* down_stream)
    {
        hintDownStream = down_stream;
    }
    virtual void rxHint(BaseMMU::Translation *dpp) = 0;

    virtual void notifyIns(int ins_num){}

    virtual std::pair<long, long> rxMembusRatio(RequestorID requestorId) {return std::pair<long, long>(0,0);};

    virtual void nofityHitToDownStream(const PacketPtr &pkt);

    virtual void pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt) = 0;

    virtual bool hasHintDownStream() const
    {
        return hintDownStream != nullptr;
    }

    virtual void sendCustomInfoToDownStream()
    {
        // construct the custom info
        // just send prefetch coverage of this level for now.
        float coverage = 1;
        if (prefetchStats.demandMshrMisses.value() > 0) {
            coverage = (prefetchStats.pfUseful.value() * 1.0) /
                        (prefetchStats.pfUseful.value() + prefetchStats.demandMshrMisses.value());
        }
        CustomPfInfo info{coverage};
        if (hasHintDownStream()) {
            hintDownStream->recvCustomInfoFrmUpStream(info);
        }
    }

    virtual void recvCustomInfoFrmUpStream(CustomPfInfo& info) {}

    virtual void offloadToDownStream() { panic("offloadToDownStream() not implemented"); }

    virtual bool hasHintsWaiting() { return false; }
};

} // namespace prefetch
} // namespace gem5

#endif //__MEM_CACHE_PREFETCH_BASE_HH__
