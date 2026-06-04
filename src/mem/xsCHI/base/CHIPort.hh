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
#include "sim/stats.hh"

namespace gem5
{
namespace xsCHI
{

class CHIPort: public ClockedObject
{

  public:
    static constexpr size_t NumChannels =
        static_cast<size_t>(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_NUM);

    enum class CreditReturnDirection
    {
        Up,
        Down,
        Internal,
    };

    enum class CreditReleasePolicy
    {
        OnAccept,
        OnDownstreamRelease,
    };

  protected:
    struct PendingCreditReturn
    {
        Tick releaseTick;
        Tick grantTick;
    };

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
    bool blocked;


    CHIPort* connected_port;          // 连接的对端Port
    SimObject* owner_module;             // 所属模块指针
    // std::queue<std::unique_ptr<FlitPtr>> buffer; // 传输缓冲区
    // uint32_t max_buffer_size;         // 最大缓冲条目数
    // uint32_t bandwidth;               // 传输带宽(GB/s)
    std::function<bool(FlitPtr&)> receive_callback;
    // Called when a sender channel regains credit and can retry next cycle.
    std::function<void(Flit::CHI_CHN_TYPE)> credit_unblock_callback;

    // Recv side
    std::queue<FlitPtr> req_buffer;
    // EventFunctionWrapper req_handle_event;

    std::queue<FlitPtr> snp_buffer;
    // EventFunctionWrapper snp_handle_event;

    std::queue<FlitPtr> dat_buffer;
    // EventFunctionWrapper dat_handle_event;

    std::queue<FlitPtr> rsp_buffer;
    // EventFunctionWrapper rsp_handle_event;

    // cmn700_rtl receiver-side RXBUF queues. The existing per-channel buffers
    // above are the staging/skid queues consumed by receive_callback.
    std::queue<FlitPtr> req_rxbuf;
    std::queue<FlitPtr> snp_rxbuf;
    std::queue<FlitPtr> dat_rxbuf;
    std::queue<FlitPtr> rsp_rxbuf;

    // use to simulate every cycle's tick event, bridge can schedule process order at it own will.
    EventFunctionWrapper global_handle_event;

    EventFunctionWrapper req_credit_grant_event;
    EventFunctionWrapper snp_credit_grant_event;
    EventFunctionWrapper dat_credit_grant_event;
    EventFunctionWrapper rsp_credit_grant_event;

    std::queue<PendingCreditReturn> req_credit_grant_queue;
    std::queue<PendingCreditReturn> snp_credit_grant_queue;
    std::queue<PendingCreditReturn> dat_credit_grant_queue;
    std::queue<PendingCreditReturn> rsp_credit_grant_queue;

    // send side
    uint32_t req_credit;
    Cycles req_last_send_time;

    uint32_t snp_credit;
    Cycles snp_last_send_time;

    uint32_t dat_credit;
    Cycles dat_last_send_time;

    uint32_t rsp_credit;
    Cycles rsp_last_send_time;

    // Per-channel blocked state due to credit exhaustion.
    std::array<bool, NumChannels> channel_blocked_by_credit{};
    std::array<bool, NumChannels> credit_block_start_valid{};
    std::array<Cycles, NumChannels> credit_block_start{};
    std::array<bool, NumChannels> no_credit_cycle_valid{};
    std::array<Cycles, NumChannels> last_no_credit_cycle{};


    int rxbufNum = 8;
    int skidDepth = 8;
    int initialCreditCount = 8;
    const bool delayedCreditReturn;
    const bool rtlCreditModel;
    const CreditReturnDirection creditReturnDirection;
    const CreditReleasePolicy creditReleasePolicy;
    const Cycles upCrdLatInt;
    const Cycles upCrdLatExt;
    const Cycles dnCrdLatInt;
    const Cycles dnCrdLatExt;
    const Cycles internalCrdLat;
    std::array<uint32_t, NumChannels> rxbufOutstanding{};

    struct CHIPortStats : public statistics::Group
    {
        explicit CHIPortStats(CHIPort *parent);

        statistics::Vector credit_stall_events_by_channel;
        statistics::Vector credit_stall_cycles_by_channel;
        statistics::Vector no_credit_bubble_cycles_by_channel;
        statistics::Vector receive_callback_reject_events_by_channel;

        statistics::Histogram credit_return_latency_hist_req;
        statistics::Histogram credit_return_latency_hist_snp;
        statistics::Histogram credit_return_latency_hist_dat;
        statistics::Histogram credit_return_latency_hist_rsp;

        statistics::Histogram rxbuf_occupancy_hist_req;
        statistics::Histogram rxbuf_occupancy_hist_snp;
        statistics::Histogram rxbuf_occupancy_hist_dat;
        statistics::Histogram rxbuf_occupancy_hist_rsp;

        statistics::Histogram skid_occupancy_hist_req;
        statistics::Histogram skid_occupancy_hist_snp;
        statistics::Histogram skid_occupancy_hist_dat;
        statistics::Histogram skid_occupancy_hist_rsp;

        statistics::Histogram rxbuf_outstanding_hist_req;
        statistics::Histogram rxbuf_outstanding_hist_snp;
        statistics::Histogram rxbuf_outstanding_hist_dat;
        statistics::Histogram rxbuf_outstanding_hist_rsp;

        statistics::Vector rxbuf_release_events_by_channel;
        statistics::Vector deferred_credit_release_events_by_channel;
    } stats;

    Cycles creditReturnLatency() const;
    void returnCreditToPeer(Flit::CHI_CHN_TYPE channel, Tick releaseTick);
    void enqueueCreditGrant(Flit::CHI_CHN_TYPE channel, Cycles latency,
                            Tick releaseTick);
    void processCreditGrant(Flit::CHI_CHN_TYPE channel);
    std::queue<PendingCreditReturn>&
    creditGrantQueue(Flit::CHI_CHN_TYPE channel);
    EventFunctionWrapper&
    creditGrantEvent(Flit::CHI_CHN_TYPE channel);
    void grantCredit(Flit::CHI_CHN_TYPE channel);
    void recordCreditBlocked(Flit::CHI_CHN_TYPE channel);
    void recordRxbufOccupancy(Flit::CHI_CHN_TYPE channel, size_t occupancy);
    void recordSkidOccupancy(Flit::CHI_CHN_TYPE channel, size_t occupancy);
    void recordRxbufOutstanding(Flit::CHI_CHN_TYPE channel, size_t occupancy);
    void recordReceiveQueueOccupancies(Flit::CHI_CHN_TYPE channel);
    void sampleCreditReturnLatency(Flit::CHI_CHN_TYPE channel,
                                   Tick releaseTick);
    std::queue<FlitPtr>& receiveBuffer(Flit::CHI_CHN_TYPE channel);
    const std::queue<FlitPtr>& receiveBuffer(Flit::CHI_CHN_TYPE channel) const;
    std::queue<FlitPtr>& rxbufQueue(Flit::CHI_CHN_TYPE channel);
    const std::queue<FlitPtr>& rxbufQueue(Flit::CHI_CHN_TYPE channel) const;
    bool useRtlRxbufStaging() const;
    void pumpRxbufToStaging(Flit::CHI_CHN_TYPE channel);
    void pumpRxbufToStaging();
    bool hasReceiveWork() const;
    void noteRxbufReceive(Flit::CHI_CHN_TYPE channel);
    bool releaseCreditOnAccept() const;

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
    void setCreditUnblockCallback(
        std::function<void(Flit::CHI_CHN_TYPE)> callback)
    {
        credit_unblock_callback = callback;
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
    bool usesCmn700RtlCreditModel() const { return rtlCreditModel; }
    bool releasesCreditOnDownstreamRelease() const
    {
        return creditReleasePolicy == CreditReleasePolicy::OnDownstreamRelease;
    }
    void releaseRxbufEntry(Flit::CHI_CHN_TYPE channel, Tick releaseTick);

    void setBlocked();
    void setUnblocked();
    bool isChannelBlockedByCredit(Flit::CHI_CHN_TYPE channel) const;

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
