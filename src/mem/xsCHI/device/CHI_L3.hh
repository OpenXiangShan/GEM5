#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/logging.hh"
#include "mem/coherent_xbar.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "mem/xsCHI/base/CHIPort.hh"
#include "mem/xsCHI/base/Network/SystemAddressMap.hh"
#include "mem/xsCHI/base/Network/TxnManager.hh"
#include "mem/xsCHI/base/flit.hh"
#include "mem/xsCHI/base/request.hh"
#include "params/CHI_L3.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{
class L2CacheWrapper;
}

namespace gem5
{
namespace xsCHI
{

/**
 * CHI_L3: CHI HN front-end that wraps a L2CacheWrapper + coherent_xbar.
 *
 * This is an initial skeleton following Design_CacheWrapper_CHI.md. The
 * request/response translation and retry logic are intentionally stubbed
 * for now; the structure (ports, mappings, callbacks) is provided so we can
 * incrementally fill protocol handling without reshaping the class surface.
 */
class CHI_L3 : public ClockedObject
{
  public:
    using Params = CHI_L3Params;
    explicit CHI_L3(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    void init() override;
    void setNodeID(uint32_t id) { _NodeID = id; }
    void setSAM(std::shared_ptr<SystemAddressMapHN> sam) { SAM = sam; }

    CHIPort *getNetworkPort() { return networkPort; }
    // Compatibility aliases for existing topology wiring.
    CHIPort *getCpuSidePort() { return networkPort; }
    CHIPort *getMemSidePort() { return networkPort; }

  private:
    // --- Pseudo ports used to intercept xbar/cache traffic ---
    // InnerCacheReqPort connects to coherent_xbar cpu_side_ports[0] (ResponsePort)
    // and intercepts responses coming back toward CacheWrapper.
    class InnerCacheReqPort : public RequestPort
    {
      public:
        InnerCacheReqPort(const std::string &name, CHI_L3 *owner)
            : RequestPort(name, owner), owner(owner) {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
        void recvRangeChange() override;
        void recvTimingSnoopReq(PacketPtr pkt) override;
        void recvFunctionalSnoop(PacketPtr pkt) override;
        Tick recvAtomicSnoop(PacketPtr pkt) override;

      private:
        CHI_L3 *owner;
    };

    // InnerCacheRespPort connects to coherent_xbar mem_side_ports[0] (RequestPort)
    // and intercepts CacheWrapper misses going downstream toward DDR.
    class InnerCacheRespPort : public ResponsePort
    {
      public:
        InnerCacheRespPort(const std::string &name, CHI_L3 *owner)
            : ResponsePort(name, owner), owner(owner) {}

      protected:
        bool recvTimingReq(PacketPtr pkt) override;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        void recvRespRetry() override;
        AddrRangeList getAddrRanges() const override;

      private:
        CHI_L3 *owner;
    };

    bool handleNetworkFlit(FlitPtr &flit);
    // --- Helpers ---
    bool handleCpuSideFlit(FlitPtr &flit);
    bool handleMemSideFlit(FlitPtr &flit);

    bool handleXBarCpuTimingReq(PacketPtr pkt);
    bool handleCacheMemTimingResp(PacketPtr pkt);
    void drainPendingCacheMemReqQueue();
    void enqueuePendingCacheMemReq(PacketPtr pkt);
    bool sendPktToXbar(PacketPtr pkt);
    bool sendReadToDdr(PacketPtr pkt, uint32_t txnId, CHI_OP_TYPE chiOp);
    bool sendWriteToDdr(PacketPtr pkt, uint32_t txnId, CHI_OP_TYPE chiOp);
    void drainPendingDdrQueue();
    void enqueuePendingDdr(PacketPtr pkt, uint32_t txnId, CHI_OP_TYPE chiOp);
    void drainDataQueue();
    void drainCompRspQueue();
    void drainWriteDataQueue();
    void drainPendingXbarQueue();
    void enqueuePendingXbar(PacketPtr pkt, bool cleanupTxn, uint32_t txnId);
    bool dispatchReadToXbar(PacketPtr pkt, uint32_t txnId);
    bool dispatchWriteToXbar(PacketPtr pkt, uint32_t txnId);
    Addr blockAddr(Addr addr) const;
    void trackPendingRead(Addr addr);
    void completePendingRead(Addr addr);
    bool hasPendingRead(Addr addr) const;
    void trackPendingWrite(Addr addr);
    void completePendingWrite(Addr addr);
    bool hasPendingWrite(Addr addr) const;
    void enqueueBlockedRead(PacketPtr pkt, uint32_t txnId);
    void wakeBlockedReads(Addr addr);
    void enqueueBlockedWrite(PacketPtr pkt, uint32_t txnId);
    void wakeBlockedWrites(Addr addr);

    bool isDdrReadCmd(const PacketPtr pkt) const;
    bool isDdrWriteCmd(const PacketPtr pkt) const;
    void trackDdrReadStart(Addr addr);
    void completeDdrRead(Addr addr);
    bool hasDdrReadInFlight(Addr addr) const;
    void trackDdrWriteStart(Addr addr);
    void completeDdrWrite(Addr addr);
    bool hasDdrWriteInFlight(Addr addr) const;
    void enqueueBlockedDdrRead(PacketPtr pkt, uint32_t txnId, CHI_OP_TYPE chiOp);
    void wakeBlockedDdrReads(Addr addr);

    // opcode→MemCmd mapping helpers
    MemCmd mapChiReqToMemCmd(CHI_OP_TYPE op) const;
    CHI_OP_TYPE mapMemCmdToChiReq(const PacketPtr pkt) const;

    // Txn bookkeeping
    uint32_t allocateTxnId();
    void releaseTxn(uint32_t txnId);

    // TODO: fill mappings per design doc
    struct TxnMeta
    {
      CHI_OP_TYPE opcode{CHI_OP_TYPE::CHI_REQ_OP_START};
      Addr addr{0};
      unsigned size{0};
      uint32_t srcId{0};
      uint32_t txnId{0};
      uint32_t returnNid{0};
      uint32_t returnTxnId{0};
      uint32_t dbid{0};
      PacketPtr pkt;              // pseudo-packet associated with this txn
      std::vector<bool> dataBits; // track COMPDATA fragments
      ReqPtr req;                 // for data slicing / resend
      bool cacheResponding{false};
      bool responderHadWritable{false};
      bool retireAfterXbarSend{false};
    };

    using TxnTable = std::unordered_map<uint32_t, TxnMeta>;
    using PacketMap = std::unordered_map<PacketPtr, uint32_t>;
    using DownstreamMap = std::unordered_map<PacketPtr, uint32_t>;

    TxnTable txnTable;
    PacketMap cacheReqMap;   // xbar/cw pkt -> txnId (hits)
    DownstreamMap downstreamMap; // cw->ddr pkt -> txnId (misses)

    TxnIDManager txnIdMgr{1024};
    uint32_t _NodeID{0};
    std::shared_ptr<SystemAddressMapHN> SAM;

    struct PendingData
    {
      ReqPtr req;
      uint32_t txnId;
      uint32_t srcId;
      uint32_t tgtId;
      uint32_t HomeNid;
      uint32_t returnNid;
      uint32_t returnTxnId;
      uint32_t dbid;
    };
    std::deque<PendingData> dataQ;
    EventFunctionWrapper dataSendEvent;

    std::deque<uint32_t> pendingCompRspQ;
    EventFunctionWrapper compRspSendEvent;

    struct PendingWriteData
    {
      ReqPtr req;
      uint32_t txnId;     // original CHI_REQ_WRITENOSNPFULL txn
      uint32_t ddrDbid;   // DBID returned by DDR
      uint32_t tgtId;
    };
    std::deque<PendingWriteData> writeDataQ;
    EventFunctionWrapper writeDataSendEvent;

    struct PendingXbarReq
    {
      PacketPtr pkt;
      bool cleanupTxn;
      uint32_t txnId;
    };
    std::deque<PendingXbarReq> pendingXbarQ;
    EventFunctionWrapper pendingXbarSendEvent;
    bool xbarRetryPending{false};

    struct PendingReadReq
    {
      PacketPtr pkt;
      uint32_t txnId;
    };
    struct PendingWriteReq
    {
      PacketPtr pkt;
      uint32_t txnId;
    };
    std::unordered_map<Addr, unsigned> pendingReadCount;
    std::unordered_map<Addr, unsigned> pendingWriteCount;
    std::unordered_map<Addr, std::deque<PendingReadReq>> blockedReadByAddr;
    std::unordered_map<Addr, std::deque<PendingWriteReq>> blockedWriteByAddr;

    struct PendingDdrReq
    {
      PacketPtr pkt;
      uint32_t txnId;
      CHI_OP_TYPE chiOp;
    };
    std::unordered_map<Addr, unsigned> ddrReadInFlightCount;
    std::unordered_map<Addr, unsigned> ddrWriteInFlightCount;
    std::unordered_map<Addr, std::deque<PendingDdrReq>> blockedDdrReadByAddr;
    std::deque<PendingDdrReq> pendingDdrQ;
    EventFunctionWrapper pendingDdrSendEvent;

    // Cache/mem-side timing requests accepted by InnerCacheRespPort but
    // deferred due to transient backpressure inside handleCacheMemTimingResp.
    std::deque<PacketPtr> pendingCacheMemReqQ;
    EventFunctionWrapper pendingCacheMemReqSendEvent;

    // Members
    CHIPort *networkPort{nullptr};
    L3CacheWrapper *cacheWrapper{nullptr};
    CoherentXBar *coherentXBar{nullptr};

    // Pseudo ports
    InnerCacheReqPort innerCacheReqPort;
    InnerCacheRespPort innerCacheRespPort;
};

} // namespace xsCHI
} // namespace gem5
