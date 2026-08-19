/*
 * Copyright (c) 2012-2014,2017-2018,2020-2021 ARM Limited
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
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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

#ifndef __CPU_O3_LSQ_UNIT_HH__
#define __CPU_O3_LSQ_UNIT_HH__

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <queue>
#include <vector>

#include <base/logging.hh>
#include <boost/circular_buffer.hpp>

#include "arch/generic/debugfaults.hh"
#include "arch/generic/vec_reg.hh"
#include "base/circular_queue.hh"
#include "config/the_isa.hh"
#include "cpu/base.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/replay_events.hh"
#include "cpu/timebuf.hh"
#include "debug/HtmCpu.hh"
#include "debug/LSQUnit.hh"
#include "debug/LoadPipeline.hh"
#include "mem/packet.hh"
#include "mem/port.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

enum class SplitStoreStatus
{
    AddressReady,
    DataReady,
    StaPipeFinish,
    StdPipeFinish
};

class IEW;

/**
 * Class that implements the actual LQ and SQ for each specific
 * thread.  Both are circular queues; load entries are freed upon
 * committing, while store entries are freed once they writeback. The
 * LSQUnit tracks if there are memory ordering violations, and also
 * detects partial load to store forwarding cases (a store only has
 * part of a load's data) that requires the load to wait until the
 * store writes back. In the former case it holds onto the instruction
 * until the dependence unit looks at it, and in the latter it stalls
 * the LSQ until the store writes back. At that point the load is
 * replayed.
 */
class LSQUnit
{
  public:
    static constexpr auto MaxDataBytes = MaxVecRegLenInBytes;

    using LSQRequest = LSQ::LSQRequest;
  private:
    class LSQEntry
    {
      private:
        /** The instruction. */
        DynInstPtr _inst;
        /** The request. */
        LSQRequest* _request = nullptr;
        /** The size of the operation. */
        uint32_t _size = 0;
        /** Valid entry. */
        bool _valid = false;

      public:
        ~LSQEntry()
        {
            if (_request != nullptr) {
                _request->freeLSQEntry();
                _request = nullptr;
            }
        }

        void
        clear()
        {
            _inst = nullptr;
            if (_request != nullptr) {
                _request->freeLSQEntry();
            }
            _request = nullptr;
            _valid = false;
            _size = 0;
        }

        void
        set(const DynInstPtr& new_inst)
        {
            assert(!_valid);
            _inst = new_inst;
            _valid = true;
            _size = 0;
        }

        LSQRequest* request() { return _request; }
        const LSQRequest* request() const { return _request; }
        void setRequest(LSQRequest* r) { _request = r; }
        bool hasRequest() { return _request != nullptr; }
        /** Member accessors. */
        /** @{ */
        bool valid() const { return _valid; }
        uint32_t& size() { return _size; }
        const uint32_t& size() const { return _size; }
        const DynInstPtr& instruction() const { return _inst; }
        /** @} */
    };

    class SQEntry : public LSQEntry
    {
      private:
        /** The store data. */
        char _data[MaxDataBytes];
        /** Whether or not the store can writeback. */
        bool _canWB = false;
        /** Whether or not the store is committed. */
        bool _committed = false;
        /** Whether or not the store is completed. */
        bool _completed = false;
        /** Does this request write all zeros and thus doesn't
         * have any data attached to it. Used for cache block zero
         * style instructs (ARM DC ZVA; ALPHA WH64)
         */
        bool _isAllZeros = false;

        bool _addrReady = false;

        bool _dataReady = false;

        bool _addrOrDataReadyCounted = false;

        bool _staFinish = false;

        bool _stdFinish = false;

      public:
        static constexpr size_t DataSize = sizeof(_data);
        /** Constructs an empty store queue entry. */
        SQEntry()
        {
            std::memset(_data, 0, DataSize);
        }

        void set(const DynInstPtr& inst) { LSQEntry::set(inst); }

        void
        clear()
        {
            LSQEntry::clear();
            _canWB = _completed = _committed = _isAllZeros = false;
            _addrReady = _dataReady = _staFinish = _stdFinish = false;
            _addrOrDataReadyCounted = false;
        }

        void setStatus(SplitStoreStatus status);

        bool addrOrDataReadyCounted() const
        {
            return _addrOrDataReadyCounted;
        }
        void addrOrDataReadyCounted(bool counted)
        {
            _addrOrDataReadyCounted = counted;
        }

        bool addrReady() const { return _addrReady; }
        bool dataReady() const { return _dataReady; }
        bool staFinish() const { return _staFinish; }
        bool stdFinish() const { return _stdFinish; }
        bool canForwardToLoad() const { return _addrReady && _dataReady; }
        bool splitStoreFinish() const { return _staFinish && _stdFinish; }

        /** Member accessors. */
        /** @{ */
        bool& canWB() { return _canWB; }
        const bool& canWB() const { return _canWB; }
        bool& completed() { return _completed; }
        const bool& completed() const { return _completed; }
        bool& committed() { return _committed; }
        const bool& committed() const { return _committed; }
        bool& isAllZeros() { return _isAllZeros; }
        const bool& isAllZeros() const { return _isAllZeros; }
        char* data() { return _data; }
        const char* data() const { return _data; }
        /** @} */
    };
    using LQEntry = LSQEntry;

  public:
    // storeQue -> shared storeBuffer -> cache
    const int sqFullBufferSize = 4;

    // loadpipe
    const int loadPipeStages = 4;
    const int loadWhenToReplay = 2;
    // stapipe
    const int storePipeStages = 4;
    const int storeWhenToReplay = 2;

    int sqFullUpperLimit = 0;

    uint64_t numSBufferRequest = 0;
    uint64_t numSingleRequest = 0;
    uint64_t numSplitRequest = 0;

    /** Coverage of one address range with another */
    enum class AddrRangeCoverage
    {
        PartialAddrRangeCoverage, /* Two ranges partly overlap */
        FullAddrRangeCoverage, /* One range fully covers another */
        NoAddrRangeCoverage /* Two ranges are disjoint */
    };

  public:
    using LoadQueue = CircularQueue<LQEntry>;
    using StoreQueue = CircularQueue<SQEntry>;

    std::vector<LSQRequest*> inflightLoads;

  public:
    /** Constructs an LSQ unit. init() must be called prior to use. */
    LSQUnit(uint32_t lqEntries, uint32_t sqEntries,
      uint32_t physicalSqEntries,
      uint32_t ldPipeStages, uint32_t stPipeStages, uint32_t maxRARQEntries, uint32_t maxRAWQEntries,
      unsigned rarDequeuePerCycle, unsigned rawDequeuePerCycle,
      unsigned loadCompletionWidth, unsigned storeCompletionWidth,
      unsigned loadPipeCount, unsigned storePipeCount);

    /** We cannot copy LSQUnit because it has stats for which copy
     * contructor is deleted explicitly. However, STL vector requires
     * a valid copy constructor for the base type at compile time.
     */
    LSQUnit(const LSQUnit &l) : loadPipeCount(0), storePipeCount(0),
        physicalSQEntries(0),
        virtualSQEnabled(false), addrOrDataReadyNums(0),
        maxRARQEntries(0), maxRAWQEntries(0),
        rarDequeuePerCycle(0), rawDequeuePerCycle(0), loadCompletionWidth(0),
        storeCompletionWidth(0),
        stats(nullptr, 1, 1)
    {
        panic("LSQUnit is not copy-able");
    }

    /** Initializes the LSQ unit with the specified number of entries. */
    void init(CPU *cpu_ptr, IEW *iew_ptr, const BaseO3CPUParams &params,
            LSQ *lsq_ptr, unsigned id);

    /** Returns the name of the LSQ unit. */
    std::string name() const;

    /** Sets the pointer to the dcache port. */
    void setDcachePort(RequestPort *dcache_port);

    /** Perform sanity checks after a drain. */
    void drainSanityCheck() const;

    /** Takes over from another CPU's thread. */
    void takeOverFrom();

    /** Inserts an instruction. */
    void insert(const DynInstPtr &inst);
    /** Inserts a load instruction. */
    void insertLoad(const DynInstPtr &load_inst);
    /** Inserts a store instruction. */
    void insertStore(const DynInstPtr &store_inst);
    bool splitStoreAddrSquashed(const DynInstPtr &inst);

    /** Check for ordering violations in the LSQ. For a store squash if we
     * ever find a conflicting load. For a load, only squash if we
     * an external snoop invalidate has been seen for that load address
     * @param load_idx index to start checking at
     * @param inst the instruction to check
     */
    Fault checkViolations(typename LoadQueue::iterator& loadIt,
            const DynInstPtr& inst);

    /** A load replay helper function
     * this function will clear state of inst (the original request, tlb state etc)
     * insert to CacheMissReplayQ or replayQ and set as Replayed in pipeline
     * @param cacheMiss insert to CacheMissReplayQ
     * @param fastReplay insert to replayQ
     * @param dropReqNow call request->discard() now
     */
    void loadSetReplay(DynInstPtr inst, LSQRequest* request, bool dropReqNow);

    /** Drop a completed store translation before a physical-SQ replay. */
    void storeSetReplay(const DynInstPtr& inst, LSQRequest* request);

    /** Check if an incoming invalidate hits in the lsq on a load
     * that might have issued out of order wrt another load beacuse
     * of the intermediate invalidate.
     */
    void checkSnoop(PacketPtr pkt);
    void checkLocalStoreVisible(Addr store_paddr,
                                const std::vector<bool> &store_byte_enable);

    /** Iq issues a load to load pipeline. */
    void issueToLoadPipe(const DynInstPtr &inst);

    bool triggerStorePFTrain(int sq_idx);

    /** Executes an amo instruction. */
    Fault executeAmo(const DynInstPtr& inst);

    /** Iq issues a store to store pipeline. */
    void issueToStorePipe(const DynInstPtr &inst);

    /** physical SQ window check based on monotonic queue indices. */
    bool storeQueueWriteReady(const DynInstPtr &inst) const;

    /** Record the first address/data-ready transition for an SQ entry. */
    void recordAddrOrDataReady(const DynInstPtr &inst);

    /** Decrement the address/data-ready count when an SQ entry is removed. */
    void recordAddrOrDataDequeue(const DynInstPtr &inst);

    /** Account for a physical-SQ-full replay waiting for its window. */
    void recordPhysicalSQReplayBlocked(const DynInstPtr &inst);

    /** Account for a post-issue physical SQ replay. */
    void recordStoreQueueReplay(const DynInstPtr &inst);

    /** Commits the head load. */
    void commitLoad();
    /** Commits loads older than a specific sequence number. */
    void commitLoads(InstSeqNum &youngest_inst);

    /** Commits stores older than a specific sequence number. */
    void commitStores(InstSeqNum &youngest_inst);

    bool directStoreToCache();

    uint32_t countStoreBufferOffloadableEntries(uint32_t max_entries) const;

    /** Writes back stores. */
    void offloadToStoreBuffer(uint32_t max_entries, std::vector<bool>& offload_fail);

    bool insertStoreBuffer(Addr vaddr, Addr paddr, uint8_t* datas,
                           uint64_t size, const std::vector<bool>& mask,
                           InstSeqNum store_seq);

    bool storeBufferEmpty() { return lsq->storeBufferEmpty(); }
    bool storeBufferEmpty(ThreadID tid) { return lsq->storeBufferEmpty(tid); }
    bool storeBufferSQWillFull() const
    {
        return storeQueue.size() > sqFullUpperLimit;
    }
    void recordStoreBufferBlockedByCache() { ++stats.blockedByCache; }

    /** Completes the data access that has been returned from the
     * memory system. */
    void completeDataAccess(PacketPtr pkt);

    /** Squashes all instructions younger than a specific sequence number. */
    void squash(const InstSeqNum &squashed_num);

    /** Returns if there is a memory ordering violation. Value is reset upon
     * call to getMemDepViolator().
     */
    bool violation() { return memDepViolator; }

    /** Returns the memory ordering violator. */
    DynInstPtr getMemDepViolator();

    /** Check if there exists raw nuke between load and store. */
    bool pipeLineNukeCheck(const DynInstPtr &load_inst, const DynInstPtr &store_inst);

    /** Returns the current request attached to an active LQ entry. */
    LSQRequest *currentLoadRequest(const DynInstPtr &inst);

    /** Returns the current request attached to an active SQ entry. */
    LSQRequest *currentStoreRequest(const DynInstPtr &inst);

    /** Returns the number of free LQ entries. */
    unsigned numFreeLoadEntries();

    /** Returns the number of free SQ entries. */
    unsigned numFreeStoreEntries();

    /** Returns the number of Poped LQ entries in LAST CLOCK. */
    unsigned getAndResetLastClockLQPopEntries();

    /** Returns the number of Poped SQ entries in LAST CLOCK. */
    unsigned getAndResetLastClockSQPopEntries();

    /** Returns the number of loads in the LQ. */
    int numLoads() const { return loadQueue.size(); }

    /** Returns the number of stores in the SQ. */
    int numStores() const { return storeQueue.size(); }

    /** Returns the number of entries in the per-thread RAR queue. */
    int numRAREntries() const { return RARQueue.size(); }

    /** Returns the number of entries in the per-thread RAW queue. */
    int numRAWEntries() const { return RAWQueue.size(); }

    // hardware transactional memory
    int numHtmStarts() const { return htmStarts; }
    int numHtmStops() const { return htmStops; }
    void resetHtmStartsStops() { htmStarts = htmStops = 0; }
    uint64_t getLatestHtmUid() const;
    void
    setLastRetiredHtmUid(uint64_t htm_uid)
    {
        assert(htm_uid >= lastRetiredHtmUid);
        lastRetiredHtmUid = htm_uid;
    }

    // Stale translation checks
    void startStaleTranslationFlush();
    bool checkStaleTranslations() const;

    /** Returns if either the LQ or SQ is full. */
    bool isFull() { return lqFull() || sqFull(); }

    /** Returns if both the LQ and SQ are empty. */
    bool isEmpty() const { return lqEmpty() && sqEmpty(); }

    /** Returns if the LQ is full. */
    bool lqFull() { return loadQueue.full(); }

    /** Returns if the SQ is full. */
    bool sqFull() { return storeQueue.full(); }

    /** Returns if the LQ is empty. */
    bool lqEmpty() const { return loadQueue.size() == 0; }

    /** Returns if the SQ is empty. */
    bool sqEmpty() const { return storeQueue.size() == 0; }

    /** Returns the number of instructions in the LSQ. */
    unsigned getCount() { return loadQueue.size() + storeQueue.size(); }

    /** Returns if there are any stores to writeback. */
    bool hasStoresToWB() { return storesToWB > 0; }

    /** Returns if there are older stores/atomics still pending writeback. */
    bool hasStoresToWBBefore(InstSeqNum seq_num) const;

    /** Returns the number of stores to writeback. */
    int numStoresToSbuffer() const { return storesToWB; }

    /** Update loadCompletedIdx and storeCompletedIdx */
    void updateCompletedIdx();

    LSQ* getLsq() { return lsq; }

    /** Returns if the LSQ unit will writeback on this cycle. */
    bool
    willWB()
    {
        bool t = storeWBIt.dereferenceable() &&
                        storeWBIt->valid() &&
                        storeWBIt->canWB() &&
                        !storeWBIt->completed() &&
                        !isStoreBlocked;
        return t;
    }

    /** Returns whether this LSQ unit is waiting for a blocked store retry. */
    bool hasBlockedStore() const { return isStoreBlocked; }

    /** Handles doing the retry and returns whether the store was sent. */
    bool recvRetry();

    unsigned int cacheLineSize();

    PacketPtr makeFullFwdPkt(DynInstPtr load_inst, LSQRequest *request);
  private:
    /** Reset the LSQ state */
    void resetState();

    /** Writes back the instruction, sending it to IEW. */
    void writebackReg(const DynInstPtr &inst, PacketPtr pkt);

    /** Completes the store at the specified index. */
    void completeStore(typename StoreQueue::iterator store_idx, bool from_sbuffer = false);

    /** Handles completing the send of a store to memory. */
    void storePostSend();

  public:
    /** Try to finish a previously blocked write back attempt */
    bool writebackBlockedStore();

    /** Attempts to send a packet to the cache.
     * Check if there are ports available. Return true if
     * there are, false if there are not.
     */
    void bankConflictReplaySchedule();

    void tagReadFailReplaySchedule();

    bool trySendPacket(bool isLoad, PacketPtr data_pkt, bool &bank_conflict, bool &tag_read_fail,
                       bool &mshr_used, bool &mshr_alias_fail, bool &hit_in_write_buffer);

    bool forwardFromStoreBuffer(const DynInstPtr &inst);

    bool forwardFromStoreQueue(const DynInstPtr &inst);

    /** Debugging function to dump instructions in the LSQ. */
    void dumpInsts() const;

    /** Ticks
     *  causing load/store pipe to run for one cycle.
     */
    void tick();

    /**
     * Process all load-pipeline stages for this cycle.
     *
     * The stage handlers below only update local DynInst/LSQRequest state.
     * executeLoadPipeSx() is the pipe-exit arbiter: it removes squashed
     * loads, hands replaying loads back to IEW/IQ, and sends completed
     * last-stage loads to readyToFinish().
     */
    void executeLoadPipeSx();

    /**
     * Load pipeline stage contract:
     *
     * - S0 translate: start/finish address translation and record the
     *   translated request.  A delayed translation marks TLBMissReplay; data
     *   and cache replay decisions are left to later stages.
     * - S1 send: run the same-cycle RAW/nuke guard, then let read() perform
     *   SQ/SBuffer forwarding or send the cache request.  A sendable request
     *   may spec-wakeup dependent instructions.
     * - S2 recv/select: reconcile data availability, DCache/SQ forwarding
     *   state, nuke checks, and RAW/RAR tracking capacity into either one
     *   replay reason or a completion path.
     * - S3 writeback: currently a timing placeholder.  The normal finish path
     *   is handled by executeLoadPipeSx() after the last stage.
     */
    Fault loadDoTranslate(const DynInstPtr &inst);
    Fault loadDoSendRequest(const DynInstPtr &inst);
    Fault loadDoRecvData(const DynInstPtr &inst);
    Fault loadDoWriteback(const DynInstPtr &inst);

    /** Process instructions in each store pipeline stages. */
    void executeStorePipeSx();

    /**
     * - stage0: access TLB
     * - stage1: save data to store queue, check load violations, set memDepViolator
     */
    Fault storeDoTranslate(const DynInstPtr &inst);
    Fault storeDoWriteSQ(const DynInstPtr &inst);
    Fault emptyStorePipeSx(const DynInstPtr &inst, uint64_t stage);

    /** Wrap function. */
    void executePipeSx();

    /** Schedule event for the cpu. */
    void schedule(Event& ev, Tick when);

    BaseMMU *getMMUPtr();

  private:
    System *system;

    /** Pointer to the CPU. */
    CPU *cpu;

    /** Pointer to the IEW stage. */
    IEW *iewStage;

    /** Pointer to the LSQ. */
    LSQ *lsq;

    /** Pointer to the dcache port.  Used only for sending. */
    RequestPort *dcachePort;

    /** Writeback event, specifically for when stores forward data to loads. */
    class WritebackRegEvent : public Event
    {
      public:
        /** Constructs a writeback event. */
        WritebackRegEvent(const DynInstPtr &_inst, PacketPtr pkt,
                LSQUnit *lsq_ptr);

        /** Processes the writeback event. */
        void process();

        /** Returns the description of this event. */
        const char *description() const;

      private:
        /** Instruction whose results are being written back. */
        DynInstPtr inst;

        /** Request that owns the delayed writeback lifecycle. */
        LSQRequest *request;

        /** The packet that would have been sent to memory. */
        PacketPtr pkt;

        /** The pointer to the LSQ unit that issued the store. */
        LSQUnit *lsqPtr;
    };
    class bankConflictReplayEvent : public Event
    {
      public:
        /** Constructs a bankConflict event. */
        bankConflictReplayEvent(LSQUnit *lsq_ptr);

        /** Processes the bankConflict event. */
        void process();

        /** Returns the description of this event. */
        const char *description() const;

      private:
        /** The pointer to the LSQ unit that issued the bankConflictReplayEvent. */
        LSQUnit *lsqPtr;
    };
    class tagReadFailReplayEvent : public Event
    {
      public:
        /** Constructs a tagReadFail event. */
        tagReadFailReplayEvent(LSQUnit *lsq_ptr);

        /** Processes the tagReadFail event. */
        void process();

        /** Returns the description of this event. */
        const char *description() const;

      private:
        /** The pointer to the LSQ unit that issued the tagReadFailReplayEvent. */
        LSQUnit *lsqPtr;
    };

    bool enableStorePrefetchTrain;

  public:
    /**
     * Handles writing back and completing the load or store that has
     * returned from memory.
     *
     * @param pkt Response packet from the memory sub-system
     */
    bool recvTimingResp(PacketPtr pkt);

    /** The LSQUnit thread id. */
    ThreadID lsqID;
  public:
    /** The store queue. */
    StoreQueue storeQueue;
    /** The load queue. */
    LoadQueue loadQueue;

    /** Points to the last position of continuously completed instructions from the beginning in loadQueue */
    size_t loadCompletedIdx;

    /** Points to the last position of continuously completed instructions from the beginning in storeQueue */
    size_t storeCompletedIdx;

    /** Load pipeline lanes and PMU channels, derived from scheduler ports. */
    const unsigned loadPipeCount;
    /** Store S0 lanes, derived from scheduler STA+STD ports. */
    const unsigned storePipeCount;

    /** Struct that defines the information passed through Load Pipeline. */
    struct LoadPipeStruct
    {
        LoadPipeStruct() : size(0), insts() {}

        // S0 is shared by all load issue sources in the current cycle.
        int size;

        std::vector<DynInstPtr> insts;
    };
    /** Struct that defines the information passed through Store Pipeline. */
    struct StorePipeStruct
    {
        StorePipeStruct() : size(0), insts() {}

        // S0 is shared by all store issue sources in the current cycle.
        int size;

        std::vector<DynInstPtr> insts;
    };


    /** The load pipeline TimeBuffer. */
    TimeBuffer<LoadPipeStruct> loadPipe;
    /** Each stage in load pipeline. loadPipeSx[0] means load pipe S0 */
    std::vector<TimeBuffer<LoadPipeStruct>::wire> loadPipeSx;

    /** The store pipeline TimeBuffer. */
    TimeBuffer<StorePipeStruct> storePipe;
    /** Each stage in store pipeline. storePipeSx[0] means store pipe S0 */
    std::vector<TimeBuffer<StorePipeStruct>::wire> storePipeSx;

  private:
    /** The number of places to shift addresses in the LSQ before checking
     * for dependency violations
     */
    unsigned depCheckShift;

    /** Number of maximum physical SQ entries that can hold address/data. */
    const unsigned physicalSQEntries;
    /** VirtualSQ replay is meaningful only when logical capacity is larger. */
    const bool virtualSQEnabled;
    /** Number of SQ entries with a valid address or data state. */
    unsigned addrOrDataReadyNums;

    /** Should loads be checked for dependency issues */
    bool checkLoads;

    /** The number of store instructions in the SQ waiting to writeback. */
    int storesToWB;

    // hardware transactional memory
    // nesting depth
    int htmStarts;
    int htmStops;
    // sanity checks and debugging
    uint64_t lastRetiredHtmUid;

    /** The index of the first instruction that may be ready to be
     * written back, and has not yet been written back.
     */
    typename StoreQueue::iterator storeWBIt;

    /** Address Mask for a cache block (e.g. ~(cache_block_size-1)) */
    Addr cacheBlockMask;

    /** Wire to read information from the issue stage time queue. */
    typename TimeBuffer<IssueStruct>::wire fromIssue;

    /** Whether or not the LSQ is stalled. */
    bool stalled;
    /** The store that causes the stall due to partial store to load
     * forwarding.
     */
    InstSeqNum stallingStoreIsn;
    /** The index of the above store. */
    ssize_t stallingLoadIdx;

    /** The packet that needs to be retried. */
    PacketPtr retryPkt;

    /** Whehter or not a store is blocked due to the memory system. */
    bool isStoreBlocked;

    bool sbufferStall;

    /** Whether or not a store is in flight. */
    bool storeInFlight;

    /** The oldest load that caused a memory ordering violation. */
    DynInstPtr memDepViolator;

    /** Flag for memory model. */
    bool needsTSO;

    /** Avoid counting the same store-load violation more than once per cycle. */
    bool countedStLdViolationThisCycle = false;

    unsigned lastClockSQPopEntries;
    unsigned lastClockLQPopEntries;
    /** Store requests for potential RAR violations */
    std::list<DynInstPtr> RARQueue;
    const int maxRARQEntries;

    /** Store requests for potential RAW violations */
    std::list<DynInstPtr> RAWQueue;
    const int maxRAWQEntries;

    /** Maximum number of instructions to dequeue from RAR queue per cycle */
    const unsigned rarDequeuePerCycle;

    /** Maximum number of instructions to dequeue from RAW queue per cycle */
    const unsigned rawDequeuePerCycle;

    /** Number of loads to complete per cycle */
    const unsigned loadCompletionWidth;

    /** Number of stores to complete per cycle */
    const unsigned storeCompletionWidth;

    /** RARReplayQueue for instructions waiting due to RAR dependency */
    std::list<DynInstPtr> RARReplayQueue;

    /** RAWReplayQueue for instructions waiting due to RAW dependency */
    std::list<DynInstPtr> RAWReplayQueue;

    /** Process instructions in RARReplayQueue and RAWReplayQueue */
    void processReplayQueues();

    /** Add instruction to RARReplayQueue */
    void addToRARReplayQueue(const DynInstPtr &inst);

    /** Add instruction to RAWReplayQueue */
    void addToRAWReplayQueue(const DynInstPtr &inst);

  protected:
    // Will also need how many read/write ports the Dcache has.  Or keep track
    // of that in stage that is one level up, and only call executeLoad/Store
    // the appropriate number of times.
    struct LSQUnitStats : public statistics::Group
    {
        LSQUnitStats(statistics::Group *parent, unsigned loadPipeCount,
                     unsigned storePipeCount);

        /** Total number of loads forwaded from LSQ stores. */
        statistics::Scalar forwLoads;

        /** Total number of squashed loads. */
        statistics::Scalar squashedLoads;

        /** Total number of pipeline detected raw nuke. */
        statistics::Scalar pipeRawNukeReplay;

        /** Total number of responses from the memory system that are
         * ignored due to the instruction already being squashed. */
        statistics::Scalar ignoredResponses;

        /** Tota number of memory ordering violations. */
        statistics::Scalar memOrderViolation;

        /** Total number of load-load violation events. */
        statistics::Scalar ldLdViolation;

        /** Total number of store-load violation events. */
        statistics::Scalar stLdViolation;

        /** RAW memory ordering violations caused by a younger load. */
        statistics::Scalar rawMemOrderViolation;

        /** RAW violations where replay-based MDP had no producer prediction. */
        statistics::Scalar rawViolationMdpNoPred;

        /** RAW violations where replay-based MDP predicted the violating store. */
        statistics::Scalar rawViolationMdpHit;

        /** RAW violations where replay-based MDP predicted other stores only. */
        statistics::Scalar rawViolationMdpMiss;

        /** RAW violations where replay-based MDP used strict wait. */
        statistics::Scalar rawViolationMdpStrict;

        /** Load-load/snoop ordering violations. */
        statistics::Scalar loadOrderViolation;

        /** Tota number of successfully forwarding from bus. */
        statistics::Scalar busForwardSuccess;

        /** Tota number of early cache miss replay. */
        statistics::Scalar cacheMissReplayEarly;

        /** Total number of squashed stores. */
        statistics::Scalar squashedStores;

        /** Number of loads that were rescheduled. */
        statistics::Scalar rescheduledLoads;

        /**Number of bank conflict times**/
        statistics::Scalar bankConflictTimes;

        /** Number of bus append times **/
        statistics::Scalar busAppendTimes;

        /** Number of times the LSQ is blocked due to the cache. */
        statistics::Scalar blockedByCache;

        statistics::Scalar sbufferFull;

        /** Store-buffer line management counters. */
        statistics::Scalar sbufferMerge;
        statistics::Scalar sbufferNewline;
        statistics::Scalar sbufferCreateVice;

        statistics::Scalar sbufferFullForward;
        statistics::Scalar sbufferPartiForward;

        /** Distribution of cycle latency between the first time a load
         * is issued and its completion */
        statistics::Distribution loadToUse;
        statistics::Distribution loadTranslationLat;


        statistics::Scalar forwardSTDNotReady;
        statistics::Scalar STAReadyFirst;
        statistics::Scalar STDReadyFirst;

        statistics::Scalar nonUnitStrideCross16Byte;
        statistics::Scalar unitStrideCross16Byte;
        statistics::Scalar unitStrideAligned;

        statistics::Scalar skipRawWhenLoadAtS0;

        /** RAR replay queue related stats */
        statistics::Scalar RARQueueFull;
        statistics::Scalar RARQueueFullCycles;
        statistics::Scalar RARQueueReplay;
        statistics::Distribution RARQueueLatency;
        statistics::Average RARQueueAvgEntryNum;

        /** RAW replay queue related stats */
        statistics::Scalar RAWQueueFull;
        statistics::Scalar RAWQueueFullCycles;
        statistics::Scalar RAWQueueReplay;
        statistics::Distribution RAWQueueLatency;
        statistics::Average RAWQueueAvgEntryNum;

        /** Pipe-entry counters sampled at the actual pipe accept point. */
        statistics::Vector loadPipeAccepted;
        statistics::Vector storePipeAccepted;
        /** Store replay counters recorded at the replay exit. */
        statistics::Scalar storeReplayTotal;
        statistics::Scalar storeReplayTlbMiss;
        statistics::Scalar storeReplayPhysicalSQFull;
        statistics::Scalar storePhysicalSQReplayBlocked;
        statistics::Vector loadPipeReplayAccepted;
        statistics::Vector loadPipeFastReplayAccepted;
        statistics::Vector loadReplayEvents;
        /**
         * Replay causes counted only on the first IssueQueue -> load-pipe
         * attempt, to better align with the RTL counters that count only the
         * first enqueue into LRQ.
         */
        statistics::Vector loadReplayEventsFromIssueQueue;
    } stats;

    void bankConflictReplay();

    void tagReadFailReplay();

    bool squashMark{false};

    /**
     * Helper function to check address range overlap and determine coverage type
     * for store-to-load forwarding.
     *
     * This function handles all combinations of split/non-split loads and stores:
     * - Load not split + Store not split: Basic address range comparison
     * - Load split + Store not split: Check each load sub-request against store
     * - Load not split + Store split: Check load against each store sub-request
     * - Load split + Store split: Check each load sub-request against each store sub-request
     *
     * @param store_it Iterator to store queue entry
     * @param request Load request pointer
     * @param load_inst Load instruction pointer
     * @param load_req_idx Index of load sub-request (-1 if checking entire load)
     * @param store_req_idx Index of store sub-request (-1 if checking entire store)
     * @return Coverage type for this address range comparison
     */
    AddrRangeCoverage checkStoreLoadForwardingRange(
                                                   typename StoreQueue::iterator store_it,
                                                   LSQRequest *request, const DynInstPtr &load_inst,
                                                   int load_req_idx = -1, int store_req_idx = -1);

  public:
    /** Load Forwards data from Data bus. */
    void forwardFromBus(DynInstPtr inst, LSQRequest *request);

    /**
     * Run the S1 LSQ access for a translated load.
     *
     * This is not just a cache-read helper: it owns replay-based MDP address
     * waits, SQ/SBuffer forwarding checks, early data-bus forwarding, and the
     * final cache send/block decision.
     */
    Fault read(LSQRequest *request, ssize_t load_idx);

    /** Executes the store at the given index. */
    Fault write(LSQRequest *requst, uint8_t *data, ssize_t store_idx);

    /** Returns the index of the head load instruction. */
    int getLoadHead() { return loadQueue.head(); }

    /** Returns the sequence number of the head load instruction. */
    InstSeqNum getLoadHeadSeqNum();

    /** Returns the index of the head store instruction. */
    int getStoreHead() { return storeQueue.head(); }
    /** Returns the sequence number of the head store instruction. */
    InstSeqNum getStoreHeadSeqNum();

    /** Returns whether or not the LSQ unit is stalled. */
    bool isStalled()  { return stalled; }

    LSQUnitStats* getStats() { return &stats; }
  public:
    typedef typename CircularQueue<LQEntry>::iterator LQIterator;
    typedef typename CircularQueue<SQEntry>::iterator SQIterator;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_LSQ_UNIT_HH__
