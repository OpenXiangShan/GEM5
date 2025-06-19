

/**
 * @file
 * DDRWrapper
 */
#pragma once


#include <functional>
#include <queue>
#include <unordered_map>

#include <boost/heap/priority_queue.hpp>

#include "../base/Network/NodeID.hh"
#include "../base/flit.hh"
#include "../base/module.hh"
#include "../base/port.hh"
#include "../base/request.hh"
#include "mem/abstract_mem.hh"
#include "mem/dramsim3_wrapper.hh"
#include "params/DRAMsim3.hh"
#include "sim/clocked_object.hh"

namespace gem5
{

namespace xsCHI
{

class DDRWrapper : public Module , public memory::AbstractMemory
{
  private:
    NodeID _NodeID;


    Port<Request> port;

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
    std::unordered_map<Addr, std::queue<ReqPtr> > outstandingReads;
    std::unordered_map<Addr, std::queue<ReqPtr> > outstandingWrites;

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
        bool operator()(const std::pair<ReqPtr, Tick> a, std::pair<ReqPtr, Tick> b) const {
          return a.second > b.second;
        }
    };

    boost::heap::priority_queue<std::pair<ReqPtr, Tick>, boost::heap::compare<sort_policy>> responseQueue;


    unsigned int nbrOutstanding() const;

    /**
     * When a packet is ready, use the "access()" method in
     * AbstractMemory to actually create the response packet, and send
     * it back to the outside world requestor.
     *
     * @param req The packet from the outside world
     */
    void accessAndRespond(ReqPtr req);

    void sendResponse();

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
    std::unique_ptr<Request> pendingDelete;

  public:

    typedef DRAMsim3Params Params;
    DDRWrapper(const Params &p);

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

    // virtual Port& getPort(const std::string& if_name,
    //                       PortID idx = InvalidPortID) override;

    void init() override;
    void startup() override;

    void resetStats() override;

  protected:

    // Tick recvAtomic(ReqPtr req);
    // void recvFunctional(ReqPtr req);
    bool recvTimingReq(ReqPtr req);
    void recvRespRetry();

};

} // namespace xsCHI
} // namespace gem5


