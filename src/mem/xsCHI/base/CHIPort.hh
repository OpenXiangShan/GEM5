#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "base/logging.hh"
#include "base/types.hh"

// #include "module.hh"
#include "mem/xsCHI/base/flit.hh"
#include "mem/xsCHI/base/params.hh"
#include "params/CHIPort.hh"
#include "sim/clocked_object.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace xsCHI
{

class CHIPort: public ClockedObject
{

  protected:

    class UnboundPortException {};

    [[noreturn]] void reportUnbound() const;

    /**
    //  * A numeric identifier to distinguish ports in a vector, and set
    //  * to InvalidPortID in case this port is not part of a vector.
    //  */
    // const PortID id;

    /**
     * Whether this port is currently connected to a peer port.
     */
    bool _connected;


    CHIPort* connected_port;          // 连接的对端Port
    SimObject* owner_module;             // 所属模块指针
    // std::queue<std::unique_ptr<FlitPtr>> buffer; // 传输缓冲区
    // uint32_t max_buffer_size;         // 最大缓冲条目数
    // uint32_t bandwidth;               // 传输带宽(GB/s)
    std::function<bool(FlitPtr&)> receive_callback;

    // Recv side
    std::queue<FlitPtr> req_buffer;
    // EventFunctionWrapper req_handle_event;

    std::queue<FlitPtr> snp_buffer;
    // EventFunctionWrapper snp_handle_event;

    std::queue<FlitPtr> dat_buffer;
    // EventFunctionWrapper dat_handle_event;

    std::queue<FlitPtr> rsp_buffer;
    // EventFunctionWrapper rsp_handle_event;

    // use to simulate every cycle's tick event, bridge can schedule process order at it own will.
    EventFunctionWrapper global_handle_event;

    // send side
    uint32_t req_credit;
    Cycles req_last_send_time;

    uint32_t snp_credit;
    Cycles snp_last_send_time;

    uint32_t dat_credit;
    Cycles dat_last_send_time;

    uint32_t rsp_credit;
    Cycles rsp_last_send_time;


    int BUFFER_SIZE = 8; // 可根据需要调整

public:
    typedef CHIPortParams Params;
    CHIPort(const Params &p);
    // CHIPort(const Params &p,SimObject* module, const std::string& _name, int recv_buffer_size);
    // ~CHIPort() =default;

    void connect(CHIPort* peer_port);

    bool send(FlitPtr&  data);

    void receive(FlitPtr  data);
    void setReceiveCallback(std::function<bool(FlitPtr&)> callback) {
        receive_callback = callback;
    }
    /** Return a reference to this port's peer. */
    CHIPort &getPeer() { return *connected_port; }

    // /** Return port name (for DPRINTF). */
    // const std::string name() const { return portName; }

    /** Get the port id. */
    // PortID getId() const { return id; }

    /** Is this port currently connected to a peer? */
    bool isConnected() const { return _connected; }
    void setOwner(SimObject* owner){owner_module = owner;}

    // operator<< will be defined as a friend outside the class
    friend std::ostream& operator<<(std::ostream& os, const CHIPort& port) {
        os << port.name();
        return os;
    }
    void OnHandleEventCallback_REQ();
    void OnHandleEventCallback_SNP();
    void OnHandleEventCallback_DAT();
    void OnHandleEventCallback_RSP();
    void OnHandleEventCallback();

    //called by _connected_port
    void GrantCredit_REQ();
    void GrantCredit_SNP();
    void GrantCredit_DAT();
    void GrantCredit_RSP();


    void initState() override;
    void init() override;

    // Port &getPort(const std::string &if_name,
    //               PortID idx=InvalidPortID) override;
    // DrainState drain() override;

};

} // namespace xsCHI
} // namespace gem5
