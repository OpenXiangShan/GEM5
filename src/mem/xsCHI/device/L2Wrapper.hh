#pragma once
#include "../base/module.hh"
#include "../base/request.hh"
#include "mem/abstract_mem.hh"
#include "mem/packet.hh"
#include "mem/qport.hh"

namespace gem5 {
namespace xsCHI {
    class L2Wrapper : public Module , public memory::AbstractMemory
    {
    protected:
     /**
     * A cache response port is used for the CPU-side port of the cache,
     * and it is basically a simple timing port that uses a transmit
     * list for responses to the CPU (or connected requestor). In
     * addition, it has the functionality to block the port for
     * incoming requests. If blocked, the port will issue a retry once
     * unblocked.
     */
    class CacheResponsePort : public QueuedResponsePort
    {

      public:

        /** Do not accept any new requests. */
        void setBlocked();

        /** Return to normal operation and accept new requests. */
        void clearBlocked();

        bool isBlocked() const { return blocked; }

      protected:

        CacheResponsePort(const std::string &_name, Module *_cache,
                       const std::string &_label);

        /** A normal packet queue used to store responses. */
        RespPacketQueue queue;

        bool blocked;

        bool mustSendRetry;

        void processSendRetry();

        EventFunctionWrapper sendRetryEvent;

    };
     /**
     * The CPU-side port extends the base cache response port with access
     * functions for functional, atomic and timing requests.
     */
    class CpuSidePort : public CacheResponsePort
    {
      private:

        // a pointer to our specific cache implementation
        Module *cache;

      protected:
        virtual bool recvTimingSnoopResp(PacketPtr pkt) override;

        virtual bool tryTiming(PacketPtr pkt) override;

        virtual bool recvTimingReq(PacketPtr pkt) override;

        virtual Tick recvAtomic(PacketPtr pkt) override;

        virtual void recvFunctional(PacketPtr pkt) override;

        virtual AddrRangeList getAddrRanges() const override;

      public:

        CpuSidePort(const std::string &_name, Module *_cache,
                    const std::string &_label);

    };

    CpuSidePort cpuSidePort;//for recv origin request,and convert it to xsCHI request

    ReqPtr CreateRequest(PacketPtr pkt);

    };
}
}
