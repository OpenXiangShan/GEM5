

/**
 * @file
 * DDRWrapper
 */
#pragma once


#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>

#include <boost/heap/priority_queue.hpp>

#include "mem/xsCHI/base/Network/NodeID.hh"
#include "mem/xsCHI/base/flit.hh"

// #include "mem/xsCHI/base/module.hh"
#include "mem/abstract_mem.hh"
#include "mem/dramsim3_wrapper.hh"
#include "mem/packet.hh"
#include "mem/xsCHI/base/CHIPort.hh"
#include "mem/xsCHI/base/Network/SystemAddressMap.hh"
#include "mem/xsCHI/base/Network/TxnManager.hh"
#include "mem/xsCHI/base/request.hh"
#include "params/DDRWrapper.hh"
#include "sim/clocked_object.hh"

// #include "params/L2ToDramSys.hh"
namespace gem5
{

namespace xsCHI
{

class DDRWrapper :  public memory::AbstractMemory
{
  private:
    uint32_t _NodeID;
    bool useDMT;
    const Cycles readResponsePaddingCycles;
    // SystemAddressMapRN *sam;

    CHIPort* port;

    /**
     * Callback functions for dramsim3
     */
    std::function<void(uint64_t)> read_cb;
    std::function<void(uint64_t)> write_cb;

    /**
     * The actual DRAMsim3 wrapper
     */
    memory::DRAMsim3Wrapper wrapper;

    /**
     * Is the connected port waiting for a retry from us
     */
    bool retryReq;

    /**
     * Are we waiting for a retry for sending a response.
     */
    bool retryResp;

    /**
     * Keep track of when the wrapper is started.
     */
    Tick startTick;

    /**
     * Keep track of what packets are outstanding per
     * address, and do so separately for reads and writes. This is
     * done so that we can return the right packet on completion from
     * DRAMSim.
     */
    std::unordered_map<Addr, std::queue<std::shared_ptr<Packet>> > outstandingReads;
    std::unordered_map<Addr, std::queue<std::shared_ptr<Packet>> > outstandingWrites;

    /**
     * Count the number of outstanding transactions so that we can
     * block any further requests until there is space in DRAMsim3 and
     * the sending queue we need to buffer the response packets.
     */
    unsigned int nbrOutstandingReads;
    unsigned int nbrOutstandingWrites;

    /**
     * Queue to hold response packets until we can send them
     * back. This is needed as DRAMsim3 unconditionally passes
     * responses back without any flow control.
     */

    struct sort_policy
    {
        bool operator()(const std::pair<std::shared_ptr<Packet>, Tick> a, std::pair<std::shared_ptr<Packet>, Tick> b) const {
          return a.second > b.second;
        }
    };

    boost::heap::priority_queue<std::pair<std::shared_ptr<Packet>, Tick>, boost::heap::compare<sort_policy>> responseQueue;

    unsigned int nbrOutstanding() const;

    /**
     * When a packet is ready, use the "access()" method in
     * AbstractMemory to actually create the response packet, and send
     * it back to the outside world requestor.
     *
     * @param pkt The packet from the outside world
     */
    void accessAndRespond(std::shared_ptr<Packet> pkt);

    void sendResponse();
    void scheduleSendResponseRetry();
    void handleCreditUnblock(Flit::CHI_CHN_TYPE channel);
    void dumpOutstandingReadState(const char *reason, Addr focusAddr = 0) const;
    void scanAgedReadTracks(const char *where);

    /**
     * Event to schedule sending of responses
     */
    EventFunctionWrapper sendResponseEvent;

    /**
     * Progress the controller one clock cycle.
     */
    void tick();

    /**
     * Event to schedule clock ticks
     */
    EventFunctionWrapper tickEvent;

    /**
     * Upstream caches need this packet until true is returned, so
     * hold it for deletion until a subsequent call
     */
    std::unique_ptr<Packet> pendingDelete;

  public:

    typedef DDRWrapperParams Params;
    DDRWrapper(const Params &p);
    // DDRWrapper(const Params &p, NodeID nodeID, SystemAddressMapRN *sam);
    DDRWrapper();

    /**
     * Read completion callback.
     *
     * @param id Channel id of the responder
     * @param addr Address of the request
     * @param cycle Internal cycle count of DRAMsim3
     */
    void readComplete(unsigned id, uint64_t addr);

    /**
     * Write completion callback.
     *
     * @param id Channel id of the responder
     * @param addr Address of the request
     * @param cycle Internal cycle count of DRAMsim3
     */
    void writeComplete(unsigned id, uint64_t addr);

    DrainState drain() override;

    virtual Port& getPort(const std::string& if_name,
                          PortID idx = InvalidPortID) override;

    void init() override;
    void startup() override;

    void resetStats() override;
    CHIPort* getCHIPort(){return port;}

    void setNodeID(uint32_t _ID);
    // void setSAM(SystemAddressMapRN *sam);
    // std::string name() const override{ return "DDRWrapper"; }

  protected:

    // Tick recvAtomic(std::shared_ptr<Packet> pkt);
    // void recvFunctional(std::shared_ptr<Packet> pkt);
    bool recvTimingReq(std::shared_ptr<Packet> pkt);
    void recvRespRetry();

    bool handlePortReceive(FlitPtr &flit);

    std::unordered_map<uint64_t,ReqPtr> outstandingReadTransferMap;// for read request

    TxnIDManager TXN_Manager;
    std::unordered_map<int, ReqPtr> outstanding_requests; // 存储由本节点产生的、未完成的请求, SN only for write Transfer
    void saveOutstandingRequest(ReqPtr &req, uint32_t txn_id);
    
};

} // namespace xsCHI
} // namespace gem5
