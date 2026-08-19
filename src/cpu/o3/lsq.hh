/*
 * Copyright (c) 2011-2012, 2014, 2018-2019, 2021 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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

#ifndef __CPU_O3_LSQ_HH__
#define __CPU_O3_LSQ_HH__

#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/circular_buffer.hpp>
#include <boost/compute/detail/lru_cache.hpp>

#include "arch/generic/mmu.hh"
#include "arch/generic/tlb.hh"
#include "base/flags.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/dyn_inst_xsmeta.hh"
#include "cpu/o3/limits.hh"
#include "cpu/utils.hh"
#include "enums/SMTLSQMode.hh"
#include "enums/SMTQueuePolicy.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "sim/sim_object.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;
class IEW;
class LSQUnit;


class LSQ
{
  public:
    class LSQRequest;
    class SbufferRequest;
    class StoreBufferEntry;
    class StoreBuffer;
    enum class StoreBufferEvictCause
    {
        Flush,
        Full,
        SQFull,
        Timeout
    };

    enum class StoreBufferBlockCause
    {
        None,
        MainPipe,
        CachePort
    };

    enum class DcacheMainPipeS2Result : unsigned
    {
        Blocked,
        ExitPipe,
        GoToS3
    };

    /**
     * DcachePort class for the load/store queue.
     */
    class DcachePort : public RequestPort
    {
      protected:

        /** Pointer to LSQ. */
        LSQ *lsq;
        CPU *cpu;

      public:
        /** Default constructor. */
        DcachePort(LSQ *_lsq, CPU *_cpu);

      protected:

        /** Timing version of receive.  Handles writing back and
         * completing the load or store that has returned from
         * memory. */
        virtual bool recvTimingResp(PacketPtr pkt);
        virtual void recvTimingSnoopReq(PacketPtr pkt);
        virtual void recvFunctionalCustomSignal(PacketPtr pkt, int sig);
        virtual void* recvGetCPUPtr();

        virtual void
        recvFunctionalSnoop(PacketPtr pkt)
        {
            // @todo: Is there a need for potential invalidation here?
        }

        /** Handles doing a retry of the previous send. */
        virtual void recvReqRetry();

        /**
         * As this CPU requires snooping to maintain the load store queue
         * change the behaviour from the base CPU port.
         *
         * @return true since we have to snoop
         */
        virtual bool isSnooping() const { return true; }
    };

    class StoreBufferEntry
    {
      public:
        const int index;
        ThreadID tid;
        InstSeqNum seqNum = 0;
        Addr blockVaddr;
        Addr blockPaddr;
        std::vector<uint8_t> blockDatas;
        std::vector<bool> validMask;
        bool sending;
        bool inDcacheMainPipe;
        // Blocked at fake MainPipe S2 and waits to re-enter from S0.
        bool replayQueued;
        // the another same addr entry when sending
        // another cannot sending until self sending finished
        StoreBufferEntry *vice = nullptr;
        // merged request
        SbufferRequest *request = nullptr;

        StoreBufferEntry(int size, int index)
            : index(index), sending(false), inDcacheMainPipe(false),
              replayQueued(false)
        {
            blockDatas.resize(size, 0);
            validMask.resize(size, false);
        }

        void reset(ThreadID tid, InstSeqNum seq_num, uint64_t block_vaddr,
                   uint64_t block_paddr, uint64_t offset, uint8_t *datas,
                   uint64_t size, const std::vector<bool> &mask);

        void merge(uint64_t offset, uint8_t *datas, uint64_t size,
                   const std::vector<bool> &mask);

        bool recordForward(RequestPtr req, LSQRequest *lsqreq,
                           ThreadID load_tid, InstSeqNum load_seq);

        // The eviction packet has been built or sent; younger same-line stores
        // must go to a vice entry instead of mutating this entry's payload.
        bool evictionInProgress() const
        {
            return sending || inDcacheMainPipe || replayQueued;
        }
    };

    class StoreBuffer
    {
        using mapIter =
            typename std::unordered_map<uint64_t, StoreBufferEntry *>::iterator;

        // key = (paddr & cacheblockmask) plus tid
        uint64_t _size = 0;
        int max_size = 0;
        int max_thread = 0;
        std::unordered_map<uint64_t, StoreBufferEntry *> data_map;
        std::vector<mapIter> crossRef;
        boost::circular_buffer<int> lru_index;
        boost::circular_buffer<int> free_list;
        std::vector<StoreBufferEntry *> data_vec;
        std::vector<bool> data_vld;
        std::vector<int> vld_cnt_vec;

        uint64_t hashKey(ThreadID tid, Addr block_paddr) const
        {
            // block_paddr[5:0] is 0, so we can use it to store tid
            return (block_paddr | tid);
        }

      public:
        void setData(std::vector<StoreBufferEntry *> &data_vec);
        void setMaxThread(ThreadID max_thread);
	bool full() const;
        bool full(ThreadID tid) const;
	uint64_t size() const;
        uint64_t size(ThreadID tid) const;
        uint64_t size(ThreadID tid, InstSeqNum seq_num) const;
        uint64_t unsentSize() const;
        const std::vector<StoreBufferEntry *> &entries() const { return data_vec; }
        bool valid(size_t index) const { return data_vld.at(index); }
        StoreBufferEntry *getEmpty();
        void insert(StoreBufferEntry *entry);
        StoreBufferEntry *get(ThreadID tid, uint64_t addr) const;
        void update(int index);
        StoreBufferEntry *getEvict();
        StoreBufferEntry *getEvict(const bool *eligible_tids,
                                   const InstSeqNum *eligible_seq,
                                   size_t num_threads);
        StoreBufferEntry *createVice(StoreBufferEntry *entry);
        void release(StoreBufferEntry *entry);
    };

    /** Memory operation metadata.
     * This class holds the information about a memory operation. It lives
     * from initiateAcc to resource deallocation at commit or squash.
     * LSQRequest objects are owned by the LQ/SQ Entry in the LSQUnit that
     * holds the operation. In addition, the LSQRequest is a TranslationState,
     * therefore, upon squash, there must be a defined ownership transferal
     * in case the LSQ resources are deallocated before the TLB is done using
     * the TranslationState.
     * If that happens, the LSQRequest will be self-owned, and responsible to
     * detect that its services are no longer required and self-destruct.
     *
     * Lifetime of a LSQRequest:
     *                 +--------------------+
     *                 |LSQ creates and owns|
     *                 +--------------------+
     *                           |
     *                 +--------------------+
     *                 | Initate translation|
     *                 +--------------------+
     *                           |
     *                        ___^___
     *                    ___/       \___
     *             ______/   Squashed?   \
     *            |      \___         ___/
     *            |          \___ ___/
     *            |              v
     *            |              |
     *            |    +--------------------+
     *            |    |  Translation done  |
     *            |    +--------------------+
     *            |              |
     *            |    +--------------------+
     *            |    |     Send packet    |<------+
     *            |    +--------------------+       |
     *            |              |                  |
     *            |           ___^___               |
     *            |       ___/       \___           |
     *            |  ____/   Squashed?   \          |
     *            | |    \___         ___/          |
     *            | |        \___ ___/              |
     *            | |            v                  |
     *            | |            |                  |
     *            | |         ___^___               |
     *            | |     ___/       \___           |
     *            | |    /     Done?     \__________|
     *            | |    \___         ___/
     *            | |        \___ ___/
     *            | |            v
     *            | |            |
     *            | |  +--------------------+
     *            | |  |    Manage stuff    |
     *            | |  |   Free resources   |
     *            | |  +--------------------+
     *            | |
     *            | |  +--------------------+
     *            | |  |    self owned      |
     *            | +->|  on recvTimingResp |
     *            |    |   free resources   |
     *            |    +--------------------+
     *            |
     *            |   +----------------------+
     *            |   |  self owned (Trans)  |
     *            +-->| on TranslationFinish |
     *                |    free resources    |
     *                +----------------------+
     *
     *
     */
    class LSQRequest : public BaseMMU::Translation, public Packet::SenderState
    {
      protected:
        typedef uint32_t FlagsStorage;
        typedef Flags<FlagsStorage> FlagsType;

        enum Flag : FlagsStorage
        {
            IsLoad              = 0x00000001,
            /** True if this request needs to writeBack to register.
              * Will be set in case of load or a store/atomic
              * that writes registers (SC)
              */
            WriteBackToRegister = 0x00000002,
            Delayed             = 0x00000004,
            IsSplit             = 0x00000008,
            /** True if any translation has been sent to TLB. */
            TranslationStarted  = 0x00000010,
            /** True if there are un-replied outbound translations.. */
            TranslationFinished = 0x00000020,
            Sent                = 0x00000040,
            Retry               = 0x00000080,
            Complete            = 0x00000100,
            /** Ownership tracking flags. */
            /** Translation squashed. */
            TranslationSquashed = 0x00000200,
            /** Request discarded */
            Discarded           = 0x00000400,
            /** LSQ resources freed. */
            LSQEntryFreed       = 0x00000800,
            /** Store written back. */
            WritebackScheduled  = 0x00001000,
            WritebackDone       = 0x00002000,
            /** True if this is an atomic request */
            IsAtomic            = 0x00004000,
            IsHInst            = 0x00008000
        };
        FlagsType flags;

        enum class State
        {
            NotIssued,
            Translation,
            Request,
            Fault,
            PartialFault,
        };
        State _state = State::NotIssued;
        void setState(const State& newState) { _state = newState; }

        uint32_t numTranslatedFragments;
        uint32_t numInTranslationFragments;


        void markDelayed() override { flags.set(Flag::Delayed); }
        bool isDelayed() { return flags.isSet(Flag::Delayed); }

      public:
        LSQUnit& _port;
        const DynInstPtr _inst;
        uint32_t _taskId;
        // Only can use in pushrequest except for the storebuffer
        PacketDataPtr _data;
        std::vector<PacketPtr> _packets;
        std::vector<RequestPtr> _reqs;
        std::vector<Fault> _fault;
        uint64_t* _res;
        const Addr _addr;
        const uint32_t _size;
        const Request::Flags _flags;
        std::vector<bool> _byteEnable;
        uint32_t _numOutstandingPackets;
        AtomicOpFunctorPtr _amo_op;
        bool _hasStaleTranslation;
        bool _sbufferBypass;
        bool _goldenSnapshotCaptured = false;

        struct FWDPacket
        {
            int idx;
            uint8_t byte;
        };
        std::vector<FWDPacket> SBforwardPackets, SQforwardPackets;

      protected:
        LSQUnit* lsqUnit() { return &_port; }
        LSQRequest(LSQUnit* port, const DynInstPtr& inst, bool isLoad);
        LSQRequest(LSQUnit* port, const DynInstPtr& inst, bool isLoad,
                const Addr& addr, const uint32_t& size,
                const Request::Flags& flags_, PacketDataPtr data=nullptr,
                uint64_t* res=nullptr, AtomicOpFunctorPtr amo_op=nullptr,
                bool stale_translation=false);

        /** Install the request in the LQ/SQ. */
        void install();

        /** If the request is still installed in the current LQ/SQ slot,
         * detach that slot so later scans do not observe a discarded or
         * deleted request through the queue entry. */
        void detachLSQEntry();

        /** Remove the request from the in-flight load tracker if present. */
        void detachInflightLoad();

        bool squashed() const override;


        /** Release the LSQRequest.
         * Notify the sender state that the request it points to is not valid
         * anymore. Understand if the request is orphan (self-managed) and if
         * so, mark it as freed, else destroy it, as this means
         * the end of its life cycle.
         * An LSQRequest is orphan when its resources are released
         * but there is any in-flight translation request to the TLB or access
         * request to the memory.
         */
        void
        release(Flag reason)
        {
            assert(reason == Flag::LSQEntryFreed || reason == Flag::Discarded);
            if (!isAnyOutstandingRequest()) {
                delete this;
            } else {
                flags.set(reason);
            }
        }

        /** Helper function used to add a (sub)request, given its address
         * `addr`, size `size` and byte-enable mask `byteEnable`.
         *
         * The request is only added if there is at least one active
         * element in the mask.
         */
        void addReq(Addr addr, unsigned size,
                const std::vector<bool>& byte_enable);

        /** Destructor.
         * The LSQRequest owns the request. If the packet has already been
         * sent, the sender state will be deleted upon receiving the reply.
         */
        virtual ~LSQRequest();

      public:

        bool
        isLoad() const
        {
            return flags.isSet(Flag::IsLoad);
        }

        bool
        isHInst() const
        {
            return flags.isSet(Flag::IsHInst);
        }

        bool
        isAtomic() const
        {
            return flags.isSet(Flag::IsAtomic);
        }

        void forward();

        /** Convenience getters/setters. */
        /** @{ */
        /** Set up Context numbers. */
        void
        setContext(const ContextID& context_id)
        {
            req()->setContext(context_id);
        }

        const DynInstPtr& instruction() { return _inst; }

        bool hasStaleTranslation() const { return _hasStaleTranslation; }

        virtual void markAsStaleTranslation() = 0;

        /** Set up virtual request.
         * For a previously allocated Request objects.
         */
        void
        setVirt(Addr vaddr, unsigned size, Request::Flags flags_,
                RequestorID requestor_id, Addr pc)
        {
            req()->setVirt(vaddr, size, flags_, requestor_id, pc);
        }

        ContextID contextId() const;

        void
        taskId(const uint32_t& v)
        {
            _taskId = v;
            for (auto& r: _reqs)
                r->taskId(v);
        }

        uint32_t taskId() const { return _taskId; }

        void
        setXsMetadata(const Request::XsMetadata &v)
        {
            for (auto& r: _reqs)
                r->setXsMetadata(v);
        }

        RequestPtr req(int idx = 0) { return _reqs.at(idx); }
        const RequestPtr req(int idx = 0) const { return _reqs.at(idx); }
        size_t numReqs() const { return _reqs.size(); }

        Addr getVaddr(int idx = 0) const { return req(idx)->getVaddr(); }
        virtual void initiateTranslation() = 0;

        PacketPtr packet(int idx = 0) { return _packets.at(idx); }

        virtual PacketPtr
        mainPacket()
        {
            assert (_packets.size() == 1);
            return packet();
        }

        virtual RequestPtr
        mainReq()
        {
            assert (_reqs.size() == 1);
            return req();
        }

        virtual RequestPtr
        mainReq() const
        {
            assert (_reqs.size() == 1);
            return req();
        }

        /**
         * Test if there is any in-flight translation or mem access request
         */
        bool
        isAnyOutstandingRequest()
        {
            return numInTranslationFragments > 0 ||
                _numOutstandingPackets > 0 ||
                (flags.isSet(Flag::WritebackScheduled) &&
                 !flags.isSet(Flag::WritebackDone));
        }

        /**
         * Test if the LSQRequest has been released, i.e. self-owned.
         * An LSQRequest manages itself when the resources on the LSQ are freed
         * but the translation is still going on and the LSQEntry was freed.
         */
        bool
        isReleased()
        {
            return flags.isSet(Flag::LSQEntryFreed) ||
                flags.isSet(Flag::Discarded);
        }

        bool
        isSplit() const
        {
            return flags.isSet(Flag::IsSplit);
        }

        bool
        needWBToRegister() const
        {
            return flags.isSet(Flag::WriteBackToRegister);
        }
        /** @} */
        virtual void recvFunctionalCustomSignal(PacketPtr pkt) = 0;
        virtual bool recvTimingResp(PacketPtr pkt) = 0;
        virtual bool sendPacketToCache() = 0;
        virtual void buildPackets() = 0;

        /**
         * Memory mapped IPR accesses
         */
        virtual Cycles handleLocalAccess(
                gem5::ThreadContext *thread, PacketPtr pkt) = 0;

        /**
         * Test if the request accesses a particular cache line.
         */
        virtual bool isCacheBlockHit(Addr blockAddr, Addr cacheBlockMask) = 0;

        virtual void assemblePackets() { panic("assemblePackets not implemented!\n"); }

        /** Update the status to reflect that a packet was sent. */
        void
        packetSent()
        {
            flags.set(Flag::Sent);
        }
        /** Update the status to reflect that a packet was not sent.
         * When a packet fails to be sent, we mark the request as needing a
         * retry. Note that Retry flag is sticky.
         */
        void
        packetNotSent()
        {
            flags.set(Flag::Retry);
            flags.clear(Flag::Sent);
        }

        void sendFragmentToTranslation(int i);
        bool
        isComplete()
        {
            return flags.isSet(Flag::Complete);
        }

        bool
        isInTranslation()
        {
            return _state == State::Translation;
        }

        bool
        isTranslationComplete()
        {
            return flags.isSet(Flag::TranslationStarted) &&
                   !isInTranslation();
        }

        bool
        isTranslationBlocked()
        {
            return _state == State::Translation &&
                flags.isSet(Flag::TranslationStarted) &&
                !flags.isSet(Flag::TranslationFinished);
        }

        bool
        isSent()
        {
            return flags.isSet(Flag::Sent);
        }

        virtual bool
        hasCachePacketProgress() const
        {
            return _numOutstandingPackets > 0;
        }

        bool
        isPartialFault()
        {
            return _state == State::PartialFault;
        }

        bool
        isMemAccessRequired()
        {
            return (_state == State::Request ||
                    (isPartialFault() && isLoad()));
        }

        void
        setStateToFault()
        {
            setState(State::Fault);
        }

        /**
         * The LSQ entry is cleared
         */
        void
        freeLSQEntry()
        {
            release(Flag::LSQEntryFreed);
        }

        /**
         * The request is discarded (e.g. partial store-load forwarding)
         */
        void
        discard()
        {
            detachLSQEntry();
            detachInflightLoad();
            release(Flag::Discarded);
        }

        void
        packetReplied()
        {
            assert(_numOutstandingPackets > 0);
            _numOutstandingPackets--;
            if (_numOutstandingPackets == 0 && isReleased())
                delete this;
        }

        void
        writebackScheduled()
        {
            assert(!flags.isSet(Flag::WritebackScheduled));
            flags.set(Flag::WritebackScheduled);
        }

        void
        writebackDone()
        {
            flags.set(Flag::WritebackDone);
            /* If the lsq resources are already free */
            if (isReleased()) {
                delete this;
            }
        }

        void
        squashTranslation()
        {
            assert(numInTranslationFragments == 0);
            flags.set(Flag::TranslationSquashed);
            /* If we are on our own, self-destruct. */
            if (isReleased()) {
                delete this;
            }
        }

        void
        complete()
        {
            flags.set(Flag::Complete);
        }

        /* Load instrutcion which is not LR or MMIO type of Load. */
        bool
        isNormalLd()
        {
            return isLoad() && !isSplit() && !mainReq()->isLLSC() && !mainReq()->isStrictlyOrdered() && !mainReq()->isUncacheable();
        }

        virtual std::string name() const { return "LSQRequest"; }
    };

    class SingleDataRequest : public LSQRequest
    {
      public:
        static std::list<SingleDataRequest*> singleList;
        SingleDataRequest(LSQUnit* port, const DynInstPtr& inst,
                bool isLoad, const Addr& addr, const uint32_t& size,
                const Request::Flags& flags_, PacketDataPtr data=nullptr,
                uint64_t* res=nullptr, AtomicOpFunctorPtr amo_op=nullptr);

        virtual ~SingleDataRequest();
        virtual void markAsStaleTranslation();
        virtual void initiateTranslation();
        virtual void finish(const Fault &fault, const RequestPtr &req,
                gem5::ThreadContext* tc, BaseMMU::Mode mode);
        virtual bool recvTimingResp(PacketPtr pkt);
        virtual void recvFunctionalCustomSignal(PacketPtr pkt);
        virtual void assemblePackets();
        virtual bool sendPacketToCache();
        virtual void buildPackets();
        virtual Cycles handleLocalAccess(
                gem5::ThreadContext *thread, PacketPtr pkt);
        virtual bool isCacheBlockHit(Addr blockAddr, Addr cacheBlockMask);
        virtual std::string name() const { return "SingleDataRequest"; }
    };

    // This class extends SingleDataRequest for the purpose
    // of allowing special requests (eg Hardware transactional memory, TLB
    // shootdowns) to bypass irrelevant system elements like translation &
    // squashing.
    class UnsquashableDirectRequest : public SingleDataRequest
    {
      public:
        UnsquashableDirectRequest(LSQUnit* port, const DynInstPtr& inst,
                const Request::Flags& flags_);
        inline virtual ~UnsquashableDirectRequest() {}
        virtual void initiateTranslation();
        virtual void markAsStaleTranslation();
        virtual void finish(const Fault &fault, const RequestPtr &req,
                gem5::ThreadContext* tc, BaseMMU::Mode mode);
        virtual std::string
        name() const
        {
            return "UnsquashableDirectRequest";
        }
    };

    class SplitDataRequest : public LSQRequest
    {
      protected:
        uint32_t numFragments;
        uint32_t numReceivedPackets;
        RequestPtr _mainReq;
        PacketPtr _mainPacket;

      public:
        SplitDataRequest(LSQUnit* port, const DynInstPtr& inst,
                bool isLoad, const Addr& addr, const uint32_t& size,
                const Request::Flags & flags_, PacketDataPtr data=nullptr,
                uint64_t* res=nullptr);
        ~SplitDataRequest() override;
        void markAsStaleTranslation() override;
        void finish(const Fault &fault, const RequestPtr &req,
                gem5::ThreadContext* tc, BaseMMU::Mode mode) override;
        bool recvTimingResp(PacketPtr pkt) override;
        void recvFunctionalCustomSignal(PacketPtr pkt) override;
        void assemblePackets() override;
        void initiateTranslation() override;
        bool sendPacketToCache() override;
        void buildPackets() override;
        bool
        hasCachePacketProgress() const override
        {
            return numReceivedPackets > 0 || _numOutstandingPackets > 0;
        }

        Cycles handleLocalAccess(
                gem5::ThreadContext *thread, PacketPtr pkt) override;
        bool isCacheBlockHit(Addr blockAddr, Addr cacheBlockMask) override;

        RequestPtr mainReq() override;
        RequestPtr mainReq() const override;
        PacketPtr mainPacket() override;
        std::string name() const override { return "SplitDataRequest"; }
    };

    class SbufferRequest : public LSQRequest
    {
        CPU* cpu;
        LSQ* lsq;
      public:
        StoreBufferEntry* sbuffer_entry=nullptr;
        SbufferRequest(CPU* cpu, LSQUnit* port, Addr blockpaddr, uint8_t* data);
        virtual ~SbufferRequest();

        void addReq(Addr blockVaddr, Addr blockPaddr, const std::vector<bool> byteEnable);

        // do not translate
        void markAsStaleTranslation() override {}
        void initiateTranslation() override {}
        void finish(const Fault &fault, const RequestPtr &req,
                gem5::ThreadContext* tc, BaseMMU::Mode mode) override {}
        bool recvTimingResp(PacketPtr pkt) override;
        void recvFunctionalCustomSignal(PacketPtr pkt) override;
        bool sendPacketToCache() override;
        void buildPackets() override;
        Cycles handleLocalAccess(
                gem5::ThreadContext *thread, PacketPtr pkt) override { return Cycles(0);};
        bool isCacheBlockHit(Addr blockAddr, Addr cacheBlockMask) override { return false;};
        std::string name() const override { return "SbufferRequest"; }

    };

    /** Constructs an LSQ with the given parameters. */
    LSQ(CPU *cpu_ptr, IEW *iew_ptr, const BaseO3CPUParams &params);

    /** Returns the name of the LSQ. */
    std::string name() const;

    /** Sets the pointer to the list of active threads. */
    void setActiveThreads(std::list<ThreadID> *at_ptr);

    /** Perform sanity checks after a drain. */
    void drainSanityCheck() const;
    /** Has the LSQ drained? */
    bool isDrained() const;
    /** Takes over execution from another CPU's thread. */
    void takeOverFrom();

    /** Number of entries needed for the given amount of threads.*/
    int entryAmount(ThreadID num_threads);

    /** Ticks the LSQ. */
    void tick();

    /** Inserts a load into the LSQ. */
    void insertLoad(const DynInstPtr &load_inst);
    /** Inserts a store into the LSQ. */
    void insertStore(const DynInstPtr &store_inst);
    bool splitStoreAddrSquashed(const DynInstPtr &inst);

    /** Executes an amo inst. */
    Fault executeAmo(const DynInstPtr &inst);

    /** Iq issues a load to load pipeline. */
    void issueToLoadPipe(const DynInstPtr &inst);

    /** Iq issues a store to store pipeline. */
    void issueToStorePipe(const DynInstPtr &inst);

    /** Whether a store uop may update its physical SQ window. */
    bool storeQueueWriteReady(const DynInstPtr &inst) const;

    /** Record the first address/data-ready transition for an SQ entry. */
    void recordAddrOrDataReady(const DynInstPtr &inst);

    /** Decrement the address/data-ready count when an SQ entry is removed. */
    void recordAddrOrDataDequeue(const DynInstPtr &inst);

    /** Whether a physical-SQ-full replay may enter its IQ replayQ. */
    bool phySQFullReplayReady(const DynInstPtr &inst);

    /** Record a post-issue replay caused by the physical SQ window. */
    void recordStoreQueueReplay(const DynInstPtr &inst);

    /** Process instructions in each load/store pipeline stages. */
    void executePipeSx();

    /**
     * Commits loads up until the given sequence number for a specific thread.
     */
    void commitLoads(InstSeqNum &youngest_inst, ThreadID tid);

    /**
     * Commits stores up until the given sequence number for a specific thread.
     */
    void commitStores(InstSeqNum &youngest_inst, ThreadID tid);

    /**
     * Attempts to write back stores until all cache ports are used or the
     * interface becomes blocked.
     */
    void processWriteback();

    void storeBufferWriteback();

    /**
     * Squash instructions from a thread until the specified sequence number.
     */
    void squash(const InstSeqNum &squashed_num, ThreadID tid);

    /** Returns whether or not there was a memory ordering violation. */
    bool violation();

    /**
     * Returns whether or not there was a memory ordering violation for a
     * specific thread.
     */
    bool violation(ThreadID tid);

    /** Gets the instruction that caused the memory ordering violation. */
    DynInstPtr getMemDepViolator(ThreadID tid);

    /** Returns the head index of the load queue for a specific thread. */
    int getLoadHead(ThreadID tid);

    /** Returns the sequence number of the head of the load queue. */
    InstSeqNum getLoadHeadSeqNum(ThreadID tid);

    /** Returns the head index of the store queue. */
    int getStoreHead(ThreadID tid);

    /** Returns the sequence number of the head of the store queue. */
    InstSeqNum getStoreHeadSeqNum(ThreadID tid);

    /** Returns the number of instructions in all of the queues. */
    int getCount();
    /** Returns the number of instructions in the queues of one thread. */
    int getCount(ThreadID tid);

    /** Returns the total number of loads in the load queue. */
    int numLoads() const;
    /** Returns the total number of loads for a single thread. */
    int numLoads(ThreadID tid) const;

    int anyInflightLoadsNotComplete();

    bool anyStoreNotExecute();

    /** Returns the total number of stores in the store queue. */
    int numStores() const;
    /** Returns the total number of stores for a single thread. */
    int numStores(ThreadID tid) const;

    /** Returns the total number of entries in the RAR queue. */
    int numRAREntries() const;
    /** Returns the total number of RAR queue entries for a single thread. */
    int numRAREntries(ThreadID tid) const;

    /** Returns the total number of entries in the RAW queue. */
    int numRAWEntries() const;
    /** Returns the total number of RAW queue entries for a single thread. */
    int numRAWEntries(ThreadID tid) const;


    // hardware transactional memory

    int numHtmStarts(ThreadID tid) const;
    int numHtmStops(ThreadID tid) const;
    void resetHtmStartsStops(ThreadID tid);
    uint64_t getLatestHtmUid(ThreadID tid) const;
    void setLastRetiredHtmUid(ThreadID tid, uint64_t htmUid);

    /** Returns the number of free load entries. */
    unsigned numFreeLoadEntries();

    /** Returns the number of free store entries. */
    unsigned numFreeStoreEntries();

    /** Returns the number of free entries for a specific thread. */
    unsigned numFreeEntries(ThreadID tid);

    /** Returns the number of free entries in the LQ for a specific thread. */
    unsigned numFreeLoadEntries(ThreadID tid);

    /** Returns the number of free entries in the SQ for a specific thread. */
    unsigned numFreeStoreEntries(ThreadID tid);

    /** Returns if the LSQ is full (either LQ or SQ is full). */
    bool isFull();
    /**
     * Returns if the LSQ is full for a specific thread (either LQ or SQ is
     * full).
     */
    bool isFull(ThreadID tid);

    /** Returns if the LSQ is empty (both LQ and SQ are empty). */
    bool isEmpty() const;
    /** Returns if all of the LQs are empty. */
    bool lqEmpty() const;
    /** Returns if the LQ of a given thread is empty. */
    bool lqEmpty(ThreadID tid) const;
    /** Returns if all of the SQs are empty. */
    bool sqEmpty() const;
    /** Returns if the SQ of a given thread is empty. */
    bool sqEmpty(ThreadID tid) const;

    /** Returns if any of the LQs are full. */
    bool lqFull();
    /** Returns if the LQ of a given thread is full. */
    bool lqFull(ThreadID tid);

    /** Returns if any of the SQs are full. */
    bool sqFull();
    /** Returns if the SQ of a given thread is full. */
    bool sqFull(ThreadID tid);

    /** Returns whether the head instruction of sq has completed*/
    const DynInstPtr& getLSQHeadInst(ThreadID tid, bool isLoad);

    int getLoadPFSource(const DynInstPtr &inst) const;

    /**
     * Returns if the LSQ is stalled due to a memory operation that must be
     * replayed.
     */
    bool isStalled();
    /**
     * Returns if the LSQ of a specific thread is stalled due to a memory
     * operation that must be replayed.
     */
    bool isStalled(ThreadID tid);

    /** Returns whether or not there are any stores to write back to memory. */
    bool hasStoresToWB();

    /** Returns whether or not a specific thread has any stores to write back
     * to memory.
     */
    bool hasStoresToWB(ThreadID tid);
    bool hasStoresToWBBefore(ThreadID tid, InstSeqNum seq_num);

    // true if all stores are flushed
    bool flushStores(ThreadID tid);
    bool flushStores(ThreadID tid, InstSeqNum seq_num);
    StoreBufferEntry *findForwardingStoreBufferEntry(Addr block_paddr,
                                                     ThreadID load_tid,
                                                     InstSeqNum load_seq) const;
    void notifyOtherThreadsStoreVisible(ThreadID tid, Addr store_paddr,
                                        const std::vector<bool> &byte_enable);

    /** Returns the number of stores a specific thread has to write back. */
    int numStoresToSbuffer(ThreadID tid);

    /** Returns if the LSQ will write back to memory this cycle. */
    bool willWB();
    /** Returns if the LSQ of a specific thread will write back to memory this
     * cycle.
     */
    bool willWB(ThreadID tid);

    /** Debugging function to print out all instructions. */
    void dumpInsts() const;
    /** Debugging function to print out instructions from a specific thread. */
    void dumpInsts(ThreadID tid) const;
    /** Debugging function to print store-buffer flush state for a thread. */
    void dumpStoreBufferState(ThreadID tid, InstSeqNum seq_num) const;
    /** Debugging function to print store-buffer entries for a thread. */
    void dumpStoreBuffer(ThreadID tid) const;

    bool isMisaligned(const DynInstPtr& inst, Addr vaddr, int size);

    /** Executes a read operation, using the load specified at the load
     * index.
     */
    Fault read(LSQRequest* request, ssize_t load_idx);

    /** Executes a store operation, using the store specified at the store
     * index.
     */
    Fault write(LSQRequest* request, uint8_t *data, ssize_t store_idx);

    /** Checks if queues have any marked operations left,
     * and sends the appropriate Sync Completion message if not.
     */
    void checkStaleTranslations();

    /**
     * Retry the previous send that failed.
     */
    void recvReqRetry();

    /**
     * Handles writing back and completing the load or store that has
     * returned from memory.
     *
     * @param pkt Response packet from the memory sub-system
     */
    bool recvTimingResp(PacketPtr pkt);

    void recvTimingSnoopReq(PacketPtr pkt);

    void recvFunctionalCustomSignal(PacketPtr pkt, int sig);

    void* getCPUPtr();

    Fault pushRequest(const DynInstPtr& inst, bool isLoad, uint8_t *data,
                      unsigned int size, Addr addr, Request::Flags flags,
                      uint64_t *res, AtomicOpFunctorPtr amo_op,
                      const std::vector<bool>& byte_enable);

    /** The CPU pointer. */
    CPU *cpu;

    /** contains all the insts which can forward data from bus <seqNum, Paddr> */
    std::unordered_map<uint64_t, Addr> bus;

    /** The IEW stage pointer. */
    IEW *iewStage;

    void clearAddresses();

    void advanceDcacheMainPipe();

    unsigned
    getDcacheDiv(Addr vaddr) const;

    uint64_t
    getDcacheSetKey(Addr vaddr) const;

    uint64_t
    getDcacheBankSetKey(Addr vaddr) const;

    uint64_t
    getDcacheDivBankSetKey(Addr vaddr) const;

    unsigned bankNum(Addr a) const
    {
        return (a >> dcacheBankOffsetBits) & (numBank - 1);
    }

    bool loadBankConflictedCheck(Addr vaddr, unsigned size);

    void setDcacheWriteStall(bool t) { dcacheWriteStall = t; }
    bool getDcacheWriteStall() { return dcacheWriteStall; }
    StoreBuffer &getStoreBuffer() { return storeBuffer; }
    bool storeBufferEmpty() const { return storeBuffer.size() == 0; }
    bool storeBufferEmpty(ThreadID tid) const
    {
        return storeBuffer.size(tid) == 0;
    }
    bool storeBufferEmpty(ThreadID tid, InstSeqNum seq_num) const
    {
        return storeBuffer.size(tid, seq_num) == 0;
    }
    bool storeBufferFlushing(ThreadID tid) const { return _storeBufferFlushing[tid]; }
    bool storeBufferFlushing() const
    {
        for (auto tid : *activeThreads) {
            if (_storeBufferFlushing[tid])
                return true;
        }
        return false;
    }
    void clearStoreBufferFlushing(ThreadID tid)
    {
        _storeBufferFlushing[tid] = false;
        _storeBufferFlushBeforeSeq[tid] = static_cast<InstSeqNum>(-1);
    }
    void clearStoreBufferFlushing() {
        for (auto tid : *activeThreads) {
            _storeBufferFlushing[tid] = false;
            _storeBufferFlushBeforeSeq[tid] = static_cast<InstSeqNum>(-1);
        }
    }
    uint32_t getSbufferEvictThreshold() const { return sbufferEvictThreshold; }
    uint32_t getSbufferEntries() const { return sbufferEntries; }
    uint64_t getStoreBufferInactiveCycles() const
    {
        return storeBufferWritebackInactive;
    }
    uint64_t getStoreBufferInactiveThreshold() const
    {
        return storeBufferInactiveThreshold;
    }
    void resetStoreBufferInactiveCycles() { storeBufferWritebackInactive = 0; }
    void incStoreBufferInactiveCycles() { ++storeBufferWritebackInactive; }
    bool storeBufferBlocked() const
    {
        // A replayed eviction is still owned by the StoreBuffer and has priority
        // over SQ offload or new evictions.
        return blockedSbufferEntry != nullptr ||
            !sbufferMainPipeReplayQ.empty();
    }
    void setBlockedStoreBufferEntry(StoreBufferEntry *entry,
                                    StoreBufferBlockCause cause =
                                        StoreBufferBlockCause::CachePort)
    {
        blockedSbufferEntry = entry;
        blockedSbufferCause = cause;
    }
    void clearBlockedStoreBufferEntry()
    {
        blockedSbufferEntry = nullptr;
        blockedSbufferCause = StoreBufferBlockCause::None;
    }
    bool storeBufferBlockedByMainPipe() const
    {
        return blockedSbufferEntry != nullptr &&
            blockedSbufferCause == StoreBufferBlockCause::MainPipe;
    }
    // Retry StoreBuffer evictions that exited the fake MainPipe at S2. The
    // return value is false only when the replay queue is empty; true means a
    // replay consumed or blocked this writeback opportunity.
    bool retryReplayStoreBuffer();

    // Retry a StoreBuffer eviction that was blocked before fake MainPipe
    // admission.
    bool retryBlockedStoreBuffer();

    // Admit a StoreBuffer packet into the fake DCache MainPipe. This does not
    // issue the packet to the real classic cache.
    bool sbufferEnterDcacheMainPipe(PacketPtr data_pkt);

    DcacheMainPipeS2Result issueSbufferPacketFromDcacheMainPipe(
        PacketPtr data_pkt, Tick issue_tick);
    void completeSbufferEvict(PacketPtr pkt);

    unsigned getLQEntries() const { return LQEntries; }

    unsigned getFreeLQEntries(ThreadID tid);
    unsigned getAndResetLastLQPopEntries(ThreadID tid);

    unsigned getFreeSQEntries(ThreadID tid);
    unsigned getAndResetLastSQPopEntries(ThreadID tid);

    bool sharedLSQMode() const;
    unsigned activeLSQThreads() const;
    unsigned sharedLSQAllocation(unsigned entries) const;
    unsigned logicalMaxLoadEntries(ThreadID tid) const;
    unsigned logicalMaxStoreEntries(ThreadID tid) const;
    unsigned logicalFreeLoadEntries(ThreadID tid) const;
    unsigned logicalFreeStoreEntries(ThreadID tid) const;
    unsigned logicalMaxRAREntries(ThreadID tid) const;
    unsigned logicalMaxRAWEntries(ThreadID tid) const;
    unsigned logicalFreeRAREntries(ThreadID tid) const;
    unsigned logicalFreeRAWEntries(ThreadID tid) const;

    enum class DcacheBlockSource : uint8_t
    {
        None,
        LoadSendRetry,
        StoreSendRetry,
        StoreBufferSendRetry,
        NumSources
    };

    /** Is the shared D-cache RequestPort waiting for a hard retry? */
    bool cacheBlocked() const;
    DcacheBlockSource cacheBlockedSource() const { return _cacheBlockedSource; }
    ThreadID cacheBlockedOwner() const { return _cacheBlockedOwner; }
    void setDcacheBlocked(ThreadID tid, DcacheBlockSource source);
    void clearDcacheBlocked();
    /** Is any store port available to use? */
    bool cachePortAvailable(bool is_load) const;
    /** Another store port is in use */
    void cachePortBusy(bool is_load);

    void recordDcacheReqAttempt(ThreadID tid, bool attempted, bool success,
                                bool cache_blocked, bool gate_blocked,
                                bool port_quota_blocked, bool bank_conflict,
                                bool tag_read_fail, bool mshr_used,
                                bool mshr_alias_fail, bool hit_in_write_buffer);

    RequestPort &getDataPort() { return dcachePort; }

    bool enableLdMissReplay() const { return _enableLdMissReplay; }
    bool enablePipeNukeCheck() const { return _enablePipeNukeCheck; }
    bool enableReplayBasedMDP() const { return _enableReplayBasedMDP; }
    int storeWbStage() const { return _storeWbStage; }

  public:
    using DcacheBankMask = std::vector<bool>;
    using DcacheMainPipeCompleteCallback = std::function<void(Tick)>;
    using DcacheMainPipeS2Callback =
        std::function<DcacheMainPipeS2Result(Tick)>;

    enum class DcacheMainPipeSource : unsigned
    {
        Refill,
        StoreBuffer
    };

    enum class DcacheMainPipeStage : unsigned
    {
        S0TagReadEntry = 0,
        S1DataRead,
        S2DataResp,
        S3TagWrite,
        S4DataWrite,
    };

    struct DcacheMainPipeRequest
    {
        DcacheMainPipeSource source = DcacheMainPipeSource::Refill;
        Addr addr = 0;
        unsigned div = 0;
        uint64_t setKey = 0;
        bool needDataRead = false;
        bool needTagWrite = false;
        bool needDataWrite = false;
        bool needWritebackPort = false;
        DcacheBankMask readBanks = {};
        DcacheBankMask writeBanks = {};
        DcacheMainPipeCompleteCallback onComplete;
        DcacheMainPipeS2Callback onS2Issue;

        bool isRefill() const
        {
            return source == DcacheMainPipeSource::Refill;
        }

        bool isStoreBuffer() const
        {
            return source == DcacheMainPipeSource::StoreBuffer;
        }
    };

    struct DcacheMainPipeSlot
    {
        bool valid = false;
        DcacheMainPipeRequest req;
    };

    // S0 happens at admission time. The stored array keeps the buffered
    // pipeline slots from S1 to S4.
    static constexpr unsigned FirstBufferedDcacheMainPipeStage =
        static_cast<unsigned>(DcacheMainPipeStage::S1DataRead);
    static constexpr unsigned LastBufferedDcacheMainPipeStage =
        static_cast<unsigned>(DcacheMainPipeStage::S4DataWrite);
    static constexpr unsigned NumBufferedDcacheMainPipeStages =
        LastBufferedDcacheMainPipeStage -
        FirstBufferedDcacheMainPipeStage + 1;

    using DcacheMainPipeBufferedPipe =
        std::array<DcacheMainPipeSlot, NumBufferedDcacheMainPipeStages>;

    static constexpr unsigned
    dcacheMainPipeIndex(DcacheMainPipeStage stage)
    {
        return static_cast<unsigned>(stage) -
            FirstBufferedDcacheMainPipeStage;
    }

    DcacheMainPipeSlot &
    dcacheMainPipeStage(DcacheMainPipeStage stage);

    const DcacheMainPipeSlot &
    dcacheMainPipeStage(DcacheMainPipeStage stage) const;

    DcacheBankMask fullDcacheBankMask() const;
    DcacheBankMask storeMaskToDcacheBanks(
        Addr block_addr, const std::vector<bool> &mask) const;

    DcacheMainPipeRequest makeDcacheRefillMainPipeRequest(
        Addr addr, bool need_data_read,
        DcacheMainPipeCompleteCallback on_complete = {}) const;
    DcacheMainPipeRequest makeStoreBufferMainPipeRequest(
        const StoreBufferEntry &entry,
        DcacheMainPipeS2Callback on_s2_issue = {}) const;

    void markDcacheMainPipeBusyBanks();

    bool dcacheBankMaskAny(const DcacheBankMask &mask) const;
    bool dcacheBankMaskOverlap(const DcacheBankMask &lhs,
                               const DcacheBankMask &rhs) const;

    bool hasDcacheMainPipeDataArrayConflict() const;
    bool isDcacheMainPipeSetBlocked(uint64_t set_key) const;
    bool canEnterDcacheMainPipe(
        const DcacheMainPipeRequest &request,
        const DcacheMainPipeBufferedPipe &next_pipe);
    bool canEnterStoreBufferDcacheMainPipe(const StoreBufferEntry &entry);

    // Put a StoreBuffer request into fake S1 and attach its fake S2 issue hook.
    void enterStoreBufferDcacheMainPipe(StoreBufferEntry &entry,
                                        PacketPtr data_pkt);

    struct NullStruct {};
    boost::compute::detail::lru_cache<uint64_t, NullStruct> recentlyloadAddr;
    std::vector<std::vector<bool>> bankOccupied;

    void notifyDcacheRefill(
        Addr addr, bool need_data_read = true,
        DcacheMainPipeCompleteCallback on_complete = {});

    std::queue<DcacheMainPipeRequest> dcacheMainPipeRefillQ;
    DcacheMainPipeBufferedPipe dcacheMainPipe = {};

    bool isDcacheRefillTagWrite() const
    {
        const auto &stage =
            dcacheMainPipeStage(DcacheMainPipeStage::S3TagWrite);
        return stage.valid && stage.req.isRefill() && stage.req.needTagWrite;
    }

    bool willDcacheRefillTagWriteNextCycle() const;

  protected:
    /** Hard retry state for the shared physical D-cache RequestPort. */
    DcacheBlockSource _cacheBlockedSource;
    ThreadID _cacheBlockedOwner;
    /** The number of cache ports available each cycle (stores only). */
    int cacheStorePorts;
    /** The number of used cache ports in this cycle by stores. */
    int usedStorePorts;
    /** The number of cache ports available each cycle (loads only). */
    int cacheLoadPorts;
    /** The number of used cache ports in this cycle by loads. */
    int usedLoadPorts;

    const unsigned numBank;
    bool dcacheWriteStall = false;
    const uint32_t sbufferEvictThreshold;
    const uint32_t sbufferEntries;
    const uint64_t storeBufferInactiveThreshold;
    const uint32_t maxStoreBufferEntriesAcceptedFromSQPerCycle = 2;
    StoreBuffer storeBuffer;
    bool _storeBufferFlushing[MaxThreads] = {false};
    InstSeqNum _storeBufferFlushBeforeSeq[MaxThreads] = {
        static_cast<InstSeqNum>(-1)
    };
    uint64_t storeBufferWritebackInactive = 0;
    StoreBufferEntry *blockedSbufferEntry = nullptr;
    StoreBufferBlockCause blockedSbufferCause = StoreBufferBlockCause::None;
    bool lastSbufferSendBlockedByMainPipe = false;
    std::deque<StoreBufferEntry *> sbufferMainPipeReplayQ;
    ThreadID nextStoreBufferOffloadTid = InvalidThreadID;
    ThreadID nextStoreBufferInsertTid  = 0;

    bool enableBankConflictCheck;
    bool sbufferBankWriteAccurately;

    const unsigned dcacheSetBits;
    const unsigned dcacheSetDivNum;
    const unsigned dcacheLineBits;
    const unsigned dcacheBankBytes;
    const unsigned dcacheBankOffsetBits;
    const unsigned dcacheBankIndexBits;
    const unsigned dcacheSetBankBits;

    bool _enableLdMissReplay;
    bool _enablePipeNukeCheck;
    bool _enableReplayBasedMDP;

    int _storeWbStage;

    /** If the LSQ is currently waiting for stale translations */
    bool waitingForStaleTranslation;
    /** The ID if the transaction that made translations stale */
    Addr staleTranslationWaitTxnId;

    /** The LSQ policy for SMT mode. */
    SMTLSQMode lsqMode;

    /** The LSQ allocation policy used in shared mode. */
    SMTQueuePolicy lsqPolicy;

    /** The per-thread threshold used in shared threshold mode. */
    unsigned smtLSQThreshold;

    struct LSQStats : public statistics::Group
    {
        LSQStats(statistics::Group *parent, unsigned num_threads);

        /** Per-cycle occupancy samples for the aggregated LSQ structures. */
        statistics::Average lqAvgEntryNum;
        statistics::Average sqAvgEntryNum;
        statistics::Average sbufferAvgEntryNum;
        /** Per-thread full signals based on whether an enqueue bundle fits. */
        statistics::Vector lqFullCycles;
        statistics::Vector sqFullCycles;
        statistics::Vector lsqFullCycles;
        statistics::Vector sbufferFullCycles;
        statistics::Scalar dcachePortRetryCallbacks;
        statistics::Scalar dcachePortRetryCallbacksNoBlockedStore;
        statistics::Scalar dcachePortRetryStoreWakeups;
        statistics::Scalar dcachePortRetryStoreSuccess;
        statistics::Vector dcachePortRetryStoreWakeupsByThread;
        statistics::Vector dcachePortRetryStoreSuccessByThread;
        statistics::Vector dcachePortRetryStoreFailByThread;
        statistics::Scalar cacheBlockedSetEvents;
        statistics::Scalar cacheBlockedClearEvents;
        statistics::Scalar cacheBlockedCycles;
        statistics::Vector dcacheHardRetrySetsBySource;
        statistics::Vector dcacheHardRetryCyclesBySource;
        statistics::Vector dcacheHardRetryGateBlockedCrossThread;
        statistics::Vector dcacheHardRetryGateBlockedSameThread;
        statistics::Vector dcacheSoftTagReadFailByThread;
        statistics::Vector dcacheSoftMshrArbFailByThread;
        statistics::Vector dcacheSoftMshrAliasFailByThread;
        statistics::Vector dcacheSoftWriteBufferFailByThread;
        statistics::Scalar cacheLoadPortFullCycles;
        statistics::Scalar cacheStorePortFullCycles;
        statistics::Vector dcacheReqAttemptsByThread;
        statistics::Vector dcacheReqGrantsByThread;
        statistics::Vector dcacheReqSendFailsByThread;
        statistics::Vector dcacheReqCacheBlockedByThread;
        statistics::Vector dcacheReqGateBlockedByThread;
        statistics::Vector dcacheReqPortQuotaBlockedByThread;
        statistics::Vector dcacheReqBankConflictByThread;
        statistics::Scalar sbufferEvictDuetoFlush;
        statistics::Scalar sbufferEvictDuetoFull;
        statistics::Scalar sbufferEvictDuetoSQFull;
        statistics::Scalar sbufferEvictDuetoTimeout;
        /** Handshake-level sbuffer to dcache request outcomes. */
        statistics::Scalar sbufferDcacheReqFire;
        statistics::Scalar sbufferDcacheReqBlocked;
        statistics::Scalar sbufferDcacheReqBlockedByMainPipe;
        statistics::Scalar dcacheMainPipeRefillEnter;
        statistics::Scalar dcacheMainPipeStoreEnter;
        statistics::Scalar dcacheMainPipeStoreBlockedByRefill;
        statistics::Scalar dcacheMainPipeStoreBlockedBySet;
        statistics::Scalar dcacheMainPipeBlockedByS1Backpressure;
        statistics::Scalar dcacheMainPipeStoreBlockedByS1Backpressure;
        statistics::Scalar dcacheMainPipeRefillBlockedByS1Backpressure;
        statistics::Scalar dcacheMainPipeStoreBlockedByTagWrite;
        statistics::Scalar dcacheMainPipeRefillBlocked;
        statistics::Scalar dcacheMainPipeRefillBlockedByPipeResource;
        statistics::Scalar dcacheMainPipeBlockedByDataConflict;
        statistics::Scalar dcacheMainPipeStoreS2IssueBlocked;
        statistics::Scalar dcacheMainPipeStoreS2MissExit;
    } stats;

    void recordStoreBufferEviction(StoreBufferEvictCause cause);

    /** List of Active Threads in System. */
    std::list<ThreadID> *activeThreads;

    /** Total Size of LQ Entries. */
    unsigned LQEntries;
    /** Number of physical SQ entries available to STA/STD. */
    unsigned physicalSQEntries;
    /** Virtual-to-physical SQ capacity multiplier. */
    unsigned storeQueueMultiple;
    /** Whether a physical-SQ-full replay waits for physical SQ space. */
    bool phySQFullCheckAtReplay;
    /** Total number of virtual SQ entries. */
    unsigned SQEntries;

    /** Max number of memory instructions that may enter LSQ in one cycle. */
    const unsigned enqueueWidth;

    /** Total Size of RARQ Entries. */
    unsigned RARQEntries;
    /** Total Size of RAWQ Entries. */
    unsigned RAWQEntries;

    /** Data port. */
    DcachePort dcachePort;

    /** The LSQ units for individual threads. */
    std::vector<LSQUnit> thread;

    /** Number of Threads. */
    ThreadID numThreads;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_LSQ_HH__
