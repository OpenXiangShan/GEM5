#pragma once
#include <cstdint>

#include "debug/CHIL2Wrapper.hh"
#include "mem/abstract_mem.hh"
#include "mem/packet.hh"
#include "mem/qport.hh"
#include "mem/xsCHI/base/CHIPort.hh"
#include "mem/xsCHI/base/module.hh"
#include "mem/xsCHI/base/request.hh"
#include "mem/xsCHI/device/CHIBridge.hh"
#include "params/ClockedObject.hh"
#include "params/L2ToDramSys.hh"
#include "params/L2Wrapper.hh"
#include "params/SimObject.hh"

namespace gem5 {
namespace xsCHI {
    class L2Wrapper : public ClockedObject
    {
      // L2Wrapper's job : when recv a pkt from Gem5Cache,
      // it will convert the pkt to a xsCHI request, and send it to CHIBridge port.
      // when recv a xsCHI request from CHIBridge port, the request can only be a snoop request,
      // it will convert the xsCHI request to a pkt,
      // and send it to back Gem5Cache.(which currently is not considered, we do not support snoop yet.)
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

        CacheResponsePort(const std::string &_name, L2Wrapper *wrapper,
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

        // // a pointer to our specific cache implementation
        // Module *cache;
        L2Wrapper *wrapper;

      protected:
        virtual bool recvTimingSnoopResp(PacketPtr pkt) override;

        virtual bool tryTiming(PacketPtr pkt) override;

        virtual bool recvTimingReq(PacketPtr pkt) override;

        virtual Tick recvAtomic(PacketPtr pkt) override;

        virtual void recvFunctional(PacketPtr pkt) override;

        virtual AddrRangeList getAddrRanges() const override;

      public:

        CpuSidePort(const std::string &_name, L2Wrapper *wrapper,
                    const std::string &_label);

    };

    CpuSidePort cpuSidePort;//for recv origin request,and convert it to xsCHI request

    // extract command , data , address from pkt, and create a xsCHI request,other fields are ignored.
    // maybe we need to cache these pkts in case we need to send them back to Gem5Cache.
    ReqPtr  CreateRequest(PacketPtr pkt);

    CHIBridge* bridge; // the bridge to xsCHI network
    void recvReadResp(ReqPtr &req);

    std::unordered_map<uint64_t, PacketPtr> outstanding_pkts;
    public:
    gem5::Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;

    // std::string name() const override{ return "L2Wrapper"; }

    typedef L2WrapperParams Params;
    L2Wrapper(const Params &p);
    // L2Wrapper(const Params &p,NodeID id,SystemAddressMap* sam);
    L2Wrapper();
    // ~L2Wrapper() = default;
    CHIBridge* getBridge();
    CHIPort* getCHIPort();
    void setNodeID(NodeID id){getBridge()->setNodeID(id);}
    void setSAM(SystemAddressMapRN* sam){getBridge()->setSAM(sam);}
  };
}
}
