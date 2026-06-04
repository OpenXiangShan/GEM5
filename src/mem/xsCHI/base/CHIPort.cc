#include "mem/xsCHI/base/CHIPort.hh"

#include <cassert>
#include <limits>
#include <utility>

#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/CHIPort.hh"
#include "mem/xsCHI/base/flit.hh"
#include "mem/xsCHI/base/params.hh"
#include "params/ClockedObject.hh"
#include "sim/cur_tick.hh"
#include "sim/port.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace xsCHI
{
    namespace
    {
    inline size_t
    channelIndex(Flit::CHI_CHN_TYPE channel)
    {
        return static_cast<size_t>(channel);
    }

    const char*
    channelName(Flit::CHI_CHN_TYPE channel)
    {
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            return "snp";
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            return "req";
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            return "rsp";
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            return "dat";
          default:
            return "invalid";
        }
    }

    bool
    isCmn700CreditModel(const std::string &model)
    {
        return model == "cmn700";
    }

    bool
    isCmn700RtlCreditModel(const std::string &model)
    {
        return model == "cmn700_rtl";
    }

    bool
    isDelayedCreditModel(const std::string &model)
    {
        return isCmn700CreditModel(model) || isCmn700RtlCreditModel(model);
    }

    unsigned
    selectBufferSize(const CHIPort::Params &p)
    {
        if (p.rxbuf_num != 0) {
            return p.rxbuf_num;
        }
        if (p.recv_buffer_size != 0) {
            return p.recv_buffer_size;
        }
        return isDelayedCreditModel(p.credit_model) ? 3 : 4;
    }

    unsigned
    selectSkidDepth(const CHIPort::Params &p, unsigned rxbuf_num)
    {
        if (p.skid_depth != 0) {
            return p.skid_depth;
        }
        if (isCmn700RtlCreditModel(p.credit_model)) {
            return 1;
        }
        return rxbuf_num;
    }

    unsigned
    selectInitialCreditCount(const CHIPort::Params &p, unsigned rxbuf_num)
    {
        if (p.initial_credit_count != 0) {
            return p.initial_credit_count;
        }
        return rxbuf_num;
    }

    CHIPort::CreditReturnDirection
    parseCreditReturnDirection(const std::string &direction)
    {
        if (direction == "up") {
            return CHIPort::CreditReturnDirection::Up;
        }
        if (direction == "down") {
            return CHIPort::CreditReturnDirection::Down;
        }
        if (direction == "internal") {
            return CHIPort::CreditReturnDirection::Internal;
        }
        panic("Invalid CHIPort credit_return_direction '%s'; expected up, "
              "down, or internal", direction.c_str());
    }

    CHIPort::CreditReleasePolicy
    parseCreditReleasePolicy(const std::string &policy)
    {
        if (policy == "on_accept") {
            return CHIPort::CreditReleasePolicy::OnAccept;
        }
        if (policy == "on_downstream_release") {
            return CHIPort::CreditReleasePolicy::OnDownstreamRelease;
        }
        panic("Invalid CHIPort credit_release_policy '%s'; expected on_accept "
              "or on_downstream_release", policy.c_str());
    }
    }

    CHIPort::CHIPortStats::CHIPortStats(CHIPort *parent)
        : statistics::Group(parent, "credit"),
          ADD_STAT(credit_stall_events_by_channel,
                   statistics::units::Count::get(),
                   "Credit-empty send failures grouped by CHI channel"),
          ADD_STAT(credit_stall_cycles_by_channel,
                   statistics::units::Cycle::get(),
                   "Cycles spent waiting for credit grouped by CHI channel"),
          ADD_STAT(no_credit_bubble_cycles_by_channel,
                   statistics::units::Cycle::get(),
                   "Cycles with a send attempt blocked by no credit"),
          ADD_STAT(receive_callback_reject_events_by_channel,
                   statistics::units::Count::get(),
                   "Receive callback reject/backpressure events by channel"),
          ADD_STAT(credit_return_latency_hist_req,
                   statistics::units::Cycle::get(),
                   "REQ credit return latency after RX buffer release"),
          ADD_STAT(credit_return_latency_hist_snp,
                   statistics::units::Cycle::get(),
                   "SNP credit return latency after RX buffer release"),
          ADD_STAT(credit_return_latency_hist_dat,
                   statistics::units::Cycle::get(),
                   "DAT credit return latency after RX buffer release"),
          ADD_STAT(credit_return_latency_hist_rsp,
                   statistics::units::Cycle::get(),
                   "RSP credit return latency after RX buffer release"),
          ADD_STAT(rxbuf_occupancy_hist_req, statistics::units::Count::get(),
                   "REQ receive buffer occupancy samples"),
          ADD_STAT(rxbuf_occupancy_hist_snp, statistics::units::Count::get(),
                   "SNP receive buffer occupancy samples"),
          ADD_STAT(rxbuf_occupancy_hist_dat, statistics::units::Count::get(),
                   "DAT receive buffer occupancy samples"),
          ADD_STAT(rxbuf_occupancy_hist_rsp, statistics::units::Count::get(),
                   "RSP receive buffer occupancy samples"),
          ADD_STAT(skid_occupancy_hist_req, statistics::units::Count::get(),
                   "REQ skid/staging buffer occupancy samples"),
          ADD_STAT(skid_occupancy_hist_snp, statistics::units::Count::get(),
                   "SNP skid/staging buffer occupancy samples"),
          ADD_STAT(skid_occupancy_hist_dat, statistics::units::Count::get(),
                   "DAT skid/staging buffer occupancy samples"),
          ADD_STAT(skid_occupancy_hist_rsp, statistics::units::Count::get(),
                   "RSP skid/staging buffer occupancy samples"),
          ADD_STAT(rxbuf_outstanding_hist_req, statistics::units::Count::get(),
                   "REQ combined RXBUF outstanding occupancy samples"),
          ADD_STAT(rxbuf_outstanding_hist_snp, statistics::units::Count::get(),
                   "SNP combined RXBUF outstanding occupancy samples"),
          ADD_STAT(rxbuf_outstanding_hist_dat, statistics::units::Count::get(),
                   "DAT combined RXBUF outstanding occupancy samples"),
          ADD_STAT(rxbuf_outstanding_hist_rsp, statistics::units::Count::get(),
                   "RSP combined RXBUF outstanding occupancy samples"),
          ADD_STAT(rxbuf_release_events_by_channel,
                   statistics::units::Count::get(),
                   "Combined RXBUF release events grouped by CHI channel"),
          ADD_STAT(deferred_credit_release_events_by_channel,
                   statistics::units::Count::get(),
                   "Accepted flits whose credit release is deferred to "
                   "downstream IB release")
    {
        credit_stall_events_by_channel
            .init(CHIPort::NumChannels)
            .flags(statistics::nozero);
        credit_stall_cycles_by_channel
            .init(CHIPort::NumChannels)
            .flags(statistics::nozero);
        no_credit_bubble_cycles_by_channel
            .init(CHIPort::NumChannels)
            .flags(statistics::nozero);
        receive_callback_reject_events_by_channel
            .init(CHIPort::NumChannels)
            .flags(statistics::nozero);
        rxbuf_release_events_by_channel
            .init(CHIPort::NumChannels)
            .flags(statistics::nozero);
        deferred_credit_release_events_by_channel
            .init(CHIPort::NumChannels)
            .flags(statistics::nozero);

        for (size_t c = 0; c < CHIPort::NumChannels; ++c) {
            const auto channel = static_cast<Flit::CHI_CHN_TYPE>(c);
            const std::string label = channelName(channel);
            credit_stall_events_by_channel.subname(c, label);
            credit_stall_cycles_by_channel.subname(c, label);
            no_credit_bubble_cycles_by_channel.subname(c, label);
            receive_callback_reject_events_by_channel.subname(c, label);
            rxbuf_release_events_by_channel.subname(c, label);
            deferred_credit_release_events_by_channel.subname(c, label);
        }

        credit_return_latency_hist_req.init(64).flags(statistics::nozero);
        credit_return_latency_hist_snp.init(64).flags(statistics::nozero);
        credit_return_latency_hist_dat.init(64).flags(statistics::nozero);
        credit_return_latency_hist_rsp.init(64).flags(statistics::nozero);

        rxbuf_occupancy_hist_req.init(16).flags(statistics::nozero);
        rxbuf_occupancy_hist_snp.init(16).flags(statistics::nozero);
        rxbuf_occupancy_hist_dat.init(16).flags(statistics::nozero);
        rxbuf_occupancy_hist_rsp.init(16).flags(statistics::nozero);
        skid_occupancy_hist_req.init(16).flags(statistics::nozero);
        skid_occupancy_hist_snp.init(16).flags(statistics::nozero);
        skid_occupancy_hist_dat.init(16).flags(statistics::nozero);
        skid_occupancy_hist_rsp.init(16).flags(statistics::nozero);
        rxbuf_outstanding_hist_req.init(16).flags(statistics::nozero);
        rxbuf_outstanding_hist_snp.init(16).flags(statistics::nozero);
        rxbuf_outstanding_hist_dat.init(16).flags(statistics::nozero);
        rxbuf_outstanding_hist_rsp.init(16).flags(statistics::nozero);
    }

    CHIPort::CHIPort(const Params &p)
    : ClockedObject(p),
        //   id(_id),
          _connected(false),
                    blocked(false),
          connected_port(nullptr),
          owner_module(nullptr),
          receive_callback(nullptr),
          req_buffer(),
        //   req_handle_event([this]{OnHandleEventCallback_REQ(); },name()),
          snp_buffer(),
        //   snp_handle_event([this]{OnHandleEventCallback_SNP(); },name()),
          dat_buffer(),
        //   dat_handle_event([this]{OnHandleEventCallback_DAT(); },name()),
          rsp_buffer(),
        //   rsp_handle_event([this]{OnHandleEventCallback_RSP(); },name()),
          req_rxbuf(),
          snp_rxbuf(),
          dat_rxbuf(),
          rsp_rxbuf(),
          global_handle_event([this]{OnHandleEventCallback(); },name()),
          req_credit_grant_event(
              [this]{processCreditGrant(
                  Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ); }, name()),
          snp_credit_grant_event(
              [this]{processCreditGrant(
                  Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP); }, name()),
          dat_credit_grant_event(
              [this]{processCreditGrant(
                  Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA); }, name()),
          rsp_credit_grant_event(
              [this]{processCreditGrant(
                  Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP); }, name()),
          req_credit_grant_queue(),
          snp_credit_grant_queue(),
          dat_credit_grant_queue(),
          rsp_credit_grant_queue(),
          req_credit(0),
          req_last_send_time(0),
          snp_credit(0),
          snp_last_send_time(0),
          dat_credit(0),
          dat_last_send_time(0),
          rsp_credit(0),
          rsp_last_send_time(0),
          rxbufNum(selectBufferSize(p)),
          skidDepth(selectSkidDepth(p, selectBufferSize(p))),
          initialCreditCount(
              selectInitialCreditCount(p, selectBufferSize(p))),
          delayedCreditReturn(isDelayedCreditModel(p.credit_model)),
          rtlCreditModel(isCmn700RtlCreditModel(p.credit_model)),
          creditReturnDirection(
              parseCreditReturnDirection(p.credit_return_direction)),
          creditReleasePolicy(
              parseCreditReleasePolicy(p.credit_release_policy)),
          upCrdLatInt(p.up_crd_lat_int),
          upCrdLatExt(p.up_crd_lat_ext),
          dnCrdLatInt(p.dn_crd_lat_int),
          dnCrdLatExt(p.dn_crd_lat_ext),
          internalCrdLat(p.internal_crd_lat),
          stats(this)
    {
        panic_if(p.credit_model != "legacy" && p.credit_model != "cmn700" &&
                 p.credit_model != "cmn700_rtl",
                 "Invalid CHIPort credit_model '%s'; expected legacy or "
                 "cmn700 or cmn700_rtl", p.credit_model.c_str());
        panic_if(rxbufNum <= 0, "CHIPort %s has invalid RXBUF size %d",
                 name(), rxbufNum);
        panic_if(skidDepth <= 0, "CHIPort %s has invalid skid depth %d",
                 name(), skidDepth);
        panic_if(initialCreditCount <= 0,
                 "CHIPort %s has invalid initial credit count %d",
                 name(), initialCreditCount);
        panic_if(rtlCreditModel && initialCreditCount > rxbufNum,
                 "CHIPort %s cmn700_rtl initial_credit_count=%d exceeds "
                 "rxbuf_num=%d", name(), initialCreditCount, rxbufNum);
        panic_if(rtlCreditModel && skidDepth > rxbufNum,
                 "CHIPort %s cmn700_rtl skid_depth=%d exceeds rxbuf_num=%d",
                 name(), skidDepth, rxbufNum);
        if (delayedCreditReturn && (rxbufNum < 2 || rxbufNum > 4)) {
            warn("%s uses cmn700 credit model with rxbuf_num=%d; CMN-700 "
                 "device RXBUF_NUM range is 2..4", name().c_str(),
                 rxbufNum);
        }
    }
    // CHIPort::CHIPort(const Params &p,SimObject* module, const std::string& _name, int recv_buffer_size)
    //     : ClockedObject(p),
    //       portName(_name),
    //     //   id(_id),
    //       _connected(false),
    //       connected_port(nullptr),
    //       owner_module(module),
    //       receive_callback(nullptr),
    //       req_buffer(),
    //     //   req_handle_event([this]{OnHandleEventCallback_REQ(); },name()),
    //       snp_buffer(),
    //     //   snp_handle_event([this]{OnHandleEventCallback_SNP(); },name()),
    //       dat_buffer(),
    //     //   dat_handle_event([this]{OnHandleEventCallback_DAT(); },name()),
    //       rsp_buffer(),
    //     //   rsp_handle_event([this]{OnHandleEventCallback_RSP(); },name()),
    //       global_handle_event([this]{OnHandleEventCallback(); },name()),
    //       req_credit(0),
    //       req_last_send_time(0),
    //       snp_credit(0),
    //       snp_last_send_time(0),
    //       dat_credit(0),
    //       dat_last_send_time(0),
    //       rsp_credit(0),
    //       rsp_last_send_time(0),
    //       BUFFER_SIZE(recv_buffer_size)
    // {
    //     //make sure sender_credit==recver_buffer_size
    // }

    Cycles
    CHIPort::creditReturnLatency() const
    {
        if (!delayedCreditReturn) {
            return Cycles(0);
        }

        switch (creditReturnDirection) {
          case CreditReturnDirection::Up:
            return upCrdLatInt + upCrdLatExt;
          case CreditReturnDirection::Down:
            return dnCrdLatInt + dnCrdLatExt;
          case CreditReturnDirection::Internal:
            return internalCrdLat;
        }
        panic("Invalid CHIPort credit return direction");
    }

    std::queue<CHIPort::PendingCreditReturn>&
    CHIPort::creditGrantQueue(Flit::CHI_CHN_TYPE channel)
    {
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            return req_credit_grant_queue;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            return snp_credit_grant_queue;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            return dat_credit_grant_queue;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            return rsp_credit_grant_queue;
          default:
            panic("Invalid channel for credit grant queue");
        }
    }

    EventFunctionWrapper&
    CHIPort::creditGrantEvent(Flit::CHI_CHN_TYPE channel)
    {
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            return req_credit_grant_event;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            return snp_credit_grant_event;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            return dat_credit_grant_event;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            return rsp_credit_grant_event;
          default:
            panic("Invalid channel for credit grant event");
        }
    }

    void
    CHIPort::returnCreditToPeer(Flit::CHI_CHN_TYPE channel, Tick releaseTick)
    {
        const Cycles latency = creditReturnLatency();
        if (!(latency > Cycles(0))) {
            getPeer().sampleCreditReturnLatency(channel, releaseTick);
            getPeer().grantCredit(channel);
            return;
        }
        getPeer().enqueueCreditGrant(channel, latency, releaseTick);
    }

    void
    CHIPort::enqueueCreditGrant(Flit::CHI_CHN_TYPE channel, Cycles latency,
                                Tick releaseTick)
    {
        auto &queue = creditGrantQueue(channel);
        auto &event = creditGrantEvent(channel);
        const Tick grantTick = curTick() + clockPeriod() * latency;
        queue.push({releaseTick, grantTick});
        if (!event.scheduled()) {
            schedule(event, grantTick);
        }
    }

    void
    CHIPort::processCreditGrant(Flit::CHI_CHN_TYPE channel)
    {
        auto &queue = creditGrantQueue(channel);
        auto &event = creditGrantEvent(channel);

        while (!queue.empty() && queue.front().grantTick <= curTick()) {
            const Tick releaseTick = queue.front().releaseTick;
            queue.pop();
            sampleCreditReturnLatency(channel, releaseTick);
            grantCredit(channel);
        }

        if (!queue.empty() && !event.scheduled()) {
            schedule(event, queue.front().grantTick);
        }
    }

    void
    CHIPort::grantCredit(Flit::CHI_CHN_TYPE channel)
    {
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            GrantCredit_REQ();
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            GrantCredit_SNP();
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            GrantCredit_DAT();
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            GrantCredit_RSP();
            break;
          default:
            panic("Invalid channel for credit grant");
        }
    }

    void
    CHIPort::recordCreditBlocked(Flit::CHI_CHN_TYPE channel)
    {
        const size_t idx = channelIndex(channel);
        if (!channel_blocked_by_credit[idx]) {
            stats.credit_stall_events_by_channel[idx]++;
            credit_block_start[idx] = curCycle();
            credit_block_start_valid[idx] = true;
        }
        channel_blocked_by_credit[idx] = true;

        if (!no_credit_cycle_valid[idx] ||
            curCycle() > last_no_credit_cycle[idx]) {
            stats.no_credit_bubble_cycles_by_channel[idx]++;
            last_no_credit_cycle[idx] = curCycle();
            no_credit_cycle_valid[idx] = true;
        }
    }

    void
    CHIPort::recordRxbufOccupancy(Flit::CHI_CHN_TYPE channel, size_t occupancy)
    {
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            stats.rxbuf_occupancy_hist_req.sample(occupancy);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            stats.rxbuf_occupancy_hist_snp.sample(occupancy);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            stats.rxbuf_occupancy_hist_dat.sample(occupancy);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            stats.rxbuf_occupancy_hist_rsp.sample(occupancy);
            break;
          default:
            panic("Invalid channel for RX buffer occupancy sample");
        }
    }

    void
    CHIPort::recordSkidOccupancy(Flit::CHI_CHN_TYPE channel, size_t occupancy)
    {
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            stats.skid_occupancy_hist_req.sample(occupancy);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            stats.skid_occupancy_hist_snp.sample(occupancy);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            stats.skid_occupancy_hist_dat.sample(occupancy);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            stats.skid_occupancy_hist_rsp.sample(occupancy);
            break;
          default:
            panic("Invalid channel for skid occupancy sample");
        }
    }

    void
    CHIPort::recordRxbufOutstanding(Flit::CHI_CHN_TYPE channel,
                                    size_t occupancy)
    {
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            stats.rxbuf_outstanding_hist_req.sample(occupancy);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            stats.rxbuf_outstanding_hist_snp.sample(occupancy);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            stats.rxbuf_outstanding_hist_dat.sample(occupancy);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            stats.rxbuf_outstanding_hist_rsp.sample(occupancy);
            break;
          default:
            panic("Invalid channel for RXBUF outstanding sample");
        }
    }

    void
    CHIPort::recordReceiveQueueOccupancies(Flit::CHI_CHN_TYPE channel)
    {
        const size_t rxbufOccupancy = useRtlRxbufStaging() ?
            rxbufQueue(channel).size() : receiveBuffer(channel).size();
        recordRxbufOccupancy(channel, rxbufOccupancy);
        recordSkidOccupancy(channel, receiveBuffer(channel).size());
    }

    std::queue<FlitPtr>&
    CHIPort::receiveBuffer(Flit::CHI_CHN_TYPE channel)
    {
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            return req_buffer;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            return snp_buffer;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            return dat_buffer;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            return rsp_buffer;
          default:
            panic("Invalid channel for receive buffer");
        }
    }

    const std::queue<FlitPtr>&
    CHIPort::receiveBuffer(Flit::CHI_CHN_TYPE channel) const
    {
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            return req_buffer;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            return snp_buffer;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            return dat_buffer;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            return rsp_buffer;
          default:
            panic("Invalid channel for receive buffer");
        }
    }

    std::queue<FlitPtr>&
    CHIPort::rxbufQueue(Flit::CHI_CHN_TYPE channel)
    {
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            return req_rxbuf;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            return snp_rxbuf;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            return dat_rxbuf;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            return rsp_rxbuf;
          default:
            panic("Invalid channel for RTL RXBUF queue");
        }
    }

    const std::queue<FlitPtr>&
    CHIPort::rxbufQueue(Flit::CHI_CHN_TYPE channel) const
    {
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            return req_rxbuf;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            return snp_rxbuf;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            return dat_rxbuf;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            return rsp_rxbuf;
          default:
            panic("Invalid channel for RTL RXBUF queue");
        }
    }

    bool
    CHIPort::useRtlRxbufStaging() const
    {
        return rtlCreditModel && releasesCreditOnDownstreamRelease();
    }

    void
    CHIPort::pumpRxbufToStaging(Flit::CHI_CHN_TYPE channel)
    {
        if (!useRtlRxbufStaging()) {
            return;
        }

        auto &rxbuf = rxbufQueue(channel);
        auto &staging = receiveBuffer(channel);
        while (!rxbuf.empty() &&
               staging.size() < static_cast<size_t>(skidDepth)) {
            staging.push(std::move(rxbuf.front()));
            rxbuf.pop();
            recordRxbufOccupancy(channel, rxbuf.size());
            recordSkidOccupancy(channel, staging.size());
        }
    }

    void
    CHIPort::pumpRxbufToStaging()
    {
        pumpRxbufToStaging(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP);
        pumpRxbufToStaging(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
        pumpRxbufToStaging(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
        pumpRxbufToStaging(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ);
    }

    bool
    CHIPort::hasReceiveWork() const
    {
        if (!snp_buffer.empty() || !req_buffer.empty() ||
            !dat_buffer.empty() || !rsp_buffer.empty()) {
            return true;
        }

        if (!useRtlRxbufStaging()) {
            return false;
        }

        return !snp_rxbuf.empty() || !req_rxbuf.empty() ||
               !dat_rxbuf.empty() || !rsp_rxbuf.empty();
    }

    bool
    CHIPort::releaseCreditOnAccept() const
    {
        return creditReleasePolicy == CreditReleasePolicy::OnAccept;
    }

    void
    CHIPort::noteRxbufReceive(Flit::CHI_CHN_TYPE channel)
    {
        if (!rtlCreditModel) {
            return;
        }

        const size_t idx = channelIndex(channel);
        panic_if(rxbufOutstanding[idx] >= static_cast<uint32_t>(rxbufNum),
                 "%s channel %s RXBUF overflow: outstanding=%u rxbuf_num=%d",
                 name(), channelName(channel), rxbufOutstanding[idx],
                 rxbufNum);
        rxbufOutstanding[idx]++;
        recordRxbufOutstanding(channel, rxbufOutstanding[idx]);
    }

    void
    CHIPort::releaseRxbufEntry(Flit::CHI_CHN_TYPE channel, Tick releaseTick)
    {
        if (rtlCreditModel) {
            const size_t idx = channelIndex(channel);
            panic_if(rxbufOutstanding[idx] == 0,
                     "%s channel %s releases RXBUF with no outstanding entry",
                     name(), channelName(channel));
            rxbufOutstanding[idx]--;
            recordRxbufOutstanding(channel, rxbufOutstanding[idx]);
        }

        stats.rxbuf_release_events_by_channel[channelIndex(channel)]++;
        returnCreditToPeer(channel, releaseTick);
    }

    void
    CHIPort::sampleCreditReturnLatency(Flit::CHI_CHN_TYPE channel,
                                       Tick releaseTick)
    {
        const uint64_t latencyCycles = ticksToCycles(curTick() - releaseTick);
        switch (channel) {
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
            stats.credit_return_latency_hist_req.sample(latencyCycles);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
            stats.credit_return_latency_hist_snp.sample(latencyCycles);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
            stats.credit_return_latency_hist_dat.sample(latencyCycles);
            break;
          case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
            stats.credit_return_latency_hist_rsp.sample(latencyCycles);
            break;
          default:
            panic("Invalid channel for credit latency sample");
        }
    }

    bool CHIPort::send(FlitPtr& data)
    {
        Flit::CHI_CHN_TYPE channel_type = data->get_Flit_Channel_Type();
        const size_t ch_idx = channelIndex(channel_type);
        //do credit check and check if the port has sent other flit already
        switch (channel_type) {
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
                if (req_credit == 0 || curCycle() <= req_last_send_time) {
                    if (req_credit == 0) {
                        recordCreditBlocked(channel_type);
                    }
                    DPRINTF(CHIPort,"CHIPort %s in module %s has no credit to send request flit :%d\
                        or already sent a request flit in the current cycle\n",
                          owner_module->name(), owner_module->name(), req_credit);
                    return false; // 没有信用，发送失败
                }
                channel_blocked_by_credit[ch_idx] = false;
                req_credit--;
                DPRINTF(CHIPort,"%s:REQ channel send, remain Credit:%d, req_last_send_time:%d, curcycle:%d\n",this->name(),req_credit,req_last_send_time,curCycle());
                req_last_send_time = curCycle();
                break;
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
                if (snp_credit == 0 || curCycle() <= snp_last_send_time) {
                    if (snp_credit == 0) {
                        recordCreditBlocked(channel_type);
                    }
                    DPRINTF(CHIPort,"CHIPort %s in module %s has no credit to send request flit :%d\
                        or already sent a request flit in the current cycle\n",
                          owner_module->name(), owner_module->name(), snp_credit);
                    return false; // 没有信用，发送失败
                }
                channel_blocked_by_credit[ch_idx] = false;
                snp_credit--;
                DPRINTF(CHIPort,"%s:SNP channel send, remain Credit:%d, snp_last_send_time:%d, curcycle:%d\n",this->name(),snp_credit,snp_last_send_time,curCycle());
                snp_last_send_time = curCycle();
                break;
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
                if (dat_credit == 0 || curCycle() <= dat_last_send_time) {
                    if (dat_credit == 0) {
                        recordCreditBlocked(channel_type);
                    }
                    DPRINTF(CHIPort,"CHIPort %s in module %s has no credit to send request flit :%d\
                        or already sent a request flit in the current cycle\n",
                          owner_module->name(), owner_module->name(), dat_credit);
                    return false; // 没有信用，发送失败
                }
                channel_blocked_by_credit[ch_idx] = false;
                dat_credit--;
                DPRINTF(CHIPort,"%s:DAT channel send, remain Credit:%d, dat_last_send_time:%d, curcycle:%d\n",this->name(),dat_credit,dat_last_send_time,curCycle());
                dat_last_send_time = curCycle();
                break;
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
                if (rsp_credit == 0 || curCycle() <= rsp_last_send_time) {
                    if (rsp_credit == 0) {
                        recordCreditBlocked(channel_type);
                    }
                    DPRINTF(CHIPort,"CHIPort %s in module %s has no credit to send request flit :%d\
                        or already sent a request flit in the current cycle\n",
                          owner_module->name(), owner_module->name(), rsp_credit);
                    return false; // 没有信用，发送失败
                }
                channel_blocked_by_credit[ch_idx] = false;
                rsp_credit--;
                DPRINTF(CHIPort,"%s:RSP channel send, remain Credit:%d, rsp_last_send_time:%d, curcycle:%d\n",this->name(),rsp_credit,rsp_last_send_time,curCycle());
                rsp_last_send_time = curCycle();
                break;
            default:
                panic("Invalid channel type for sending flit");
        }
        DPRINTF(CHIPort,"send Flit op:%s, addr: %lx, size:%d,txn_id:%d\n",CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(data->getOpcode()),data->getAddr(),data->getSize(),data->getTxnId());
        // 将数据发送到连接的端口
        if (connected_port != nullptr) {
            connected_port->receive(std::move(data));
            return true;
        } else {
            panic("CHIPort %s in module %s is not connected to any peer port",
                  owner_module->name(), owner_module->name());
            return false; // 连接失败
        }
    }
    void CHIPort::receive(FlitPtr data){
        //todo ::schedule event, call OnEventCallback
        DPRINTF(CHIPort,"Recv Flit op:%s, addr: %lx, size:%d,txn_id:%d\n",CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(data->getOpcode()),data->getAddr(),data->getSize(),data->getTxnId());
        Flit::CHI_CHN_TYPE channel_type = data->get_Flit_Channel_Type();
        if (useRtlRxbufStaging()) {
            noteRxbufReceive(channel_type);
            auto &rxbuf = rxbufQueue(channel_type);
            rxbuf.push(std::move(data));
            recordReceiveQueueOccupancies(channel_type);
        } else {
            auto &buffer = receiveBuffer(channel_type);
            panic_if(buffer.size() >= static_cast<size_t>(rxbufNum),
                     "%s channel %s receive buffer overflow: occupancy=%u "
                     "limit=%d", name(), channelName(channel_type),
                     static_cast<unsigned>(buffer.size()), rxbufNum);
            noteRxbufReceive(channel_type);
            buffer.push(std::move(data));
            recordReceiveQueueOccupancies(channel_type);
        }
        if (!global_handle_event.scheduled()) {
            schedule(global_handle_event, curTick()+clockPeriod()*PortTransferLatency);
        }
    }
    void CHIPort::OnHandleEventCallback() {
        // This function is called every cycle to handle events
        // It can be used to process all buffers in a round-robin manner
        // SNP>RSP>DAT>REQ
        pumpRxbufToStaging();
        if (!snp_buffer.empty()) {
            OnHandleEventCallback_SNP();
        }
        if (!rsp_buffer.empty()) {
            OnHandleEventCallback_RSP();
        }
        if (!dat_buffer.empty()) {
            OnHandleEventCallback_DAT();
        }
        if (!req_buffer.empty()) {
            OnHandleEventCallback_REQ();
        }
        // after handling the head entry, check if the buffer is empty,if not, reschedule the event to next cycle
        if (hasReceiveWork()) {
            if (!global_handle_event.scheduled()) {
                schedule(global_handle_event, curTick()+clockPeriod());
            }
        }
    }
    void CHIPort::OnHandleEventCallback_REQ() {
        // Handle the event callback logic here
        // call receive_callback to handle the head entry of the buffer
        // if hanle successful, pop the head entry, and return the credit to sender
        assert(!req_buffer.empty() && "Request buffer is empty");
        FlitPtr& head_entry = req_buffer.front();
        if (receive_callback) {
            if (receive_callback(head_entry)) {
                // Handle successful reception
                req_buffer.pop();
                recordReceiveQueueOccupancies(
                    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ);
                if (releaseCreditOnAccept()) {
                    releaseRxbufEntry(
                        Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ, curTick());
                } else {
                    stats.deferred_credit_release_events_by_channel[
                        channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)]++;
                }
                DPRINTF(CHIPort,
                        "%s:REQ Recv buffer handle success, "
                        "buffer occupied num:%d, Peer Credit:%d\n",
                        this->name(), req_buffer.size(),
                        getPeer().req_credit);
            } else {
                // Handle failure to process the entry
                //todo, you may want to log or handle this case
                stats.receive_callback_reject_events_by_channel[
                    channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)]++;
                DPRINTF(CHIPort,
                        "%s:REQ Recv buffer handle Failed, "
                        "buffer occupied num:%d, Peer Credit:%d\n",
                        this->name(), req_buffer.size(),
                        getPeer().req_credit);
            }
        } else {
            panic("No receive callback set for CHIPort %s", owner_module->name());
        }
        // after handling the head entry, check if the buffer is empty,if not, reschedule the event to next cycle
        // if (!req_buffer.empty()) {
        //     if (!req_handle_event.scheduled()) {
        //         schedule(req_handle_event, curTick()+clockPeriod());
        //     }
        // }
    }

    void CHIPort::OnHandleEventCallback_SNP() {
        // Handle the event callback logic here
        // call receive_callback to handle the head entry of the buffer
        // if hanle successful, pop the head entry, and return the credit to sender
        assert(!snp_buffer.empty() && "snoop buffer is empty");
        FlitPtr& head_entry = snp_buffer.front();
        if (receive_callback) {
            if (receive_callback(head_entry)) {
                // Handle successful reception
                snp_buffer.pop();
                recordReceiveQueueOccupancies(
                    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP);
                if (releaseCreditOnAccept()) {
                    releaseRxbufEntry(
                        Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP, curTick());
                } else {
                    stats.deferred_credit_release_events_by_channel[
                        channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP)]++;
                }
                DPRINTF(CHIPort,
                        "%s:SNP Recv buffer handle success, "
                        "buffer occupied num:%d, Peer Credit:%d\n",
                        this->name(), snp_buffer.size(),
                        getPeer().snp_credit);
            } else {
                // Handle failure to process the entry
                //todo, you may want to log or handle this case
                stats.receive_callback_reject_events_by_channel[
                    channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP)]++;
                DPRINTF(CHIPort,
                        "%s:SNP Recv buffer handle failed, "
                        "buffer occupied num:%d, Peer Credit:%d\n",
                        this->name(), snp_buffer.size(),
                        getPeer().snp_credit);
            }
        } else {
            panic("No receive callback set for CHIPort %s", owner_module->name());
        }
        // // after handling the head entry, check if the buffer is empty,if not, reschedule the event to next cycle
        // if (!snp_buffer.empty()) {
        //     if (!snp_handle_event.scheduled()) {
        //         schedule(snp_handle_event, curTick()+clockPeriod());
        //     }
        // }
    }

    void CHIPort::OnHandleEventCallback_DAT() {
        // Handle the event callback logic here
        // call receive_callback to handle the head entry of the buffer
        // if hanle successful, pop the head entry, and return the credit to sender
        assert(!dat_buffer.empty() && "data buffer is empty");
        FlitPtr& head_entry = dat_buffer.front();
        if (receive_callback) {
            if (receive_callback(head_entry)) {
                // Handle successful reception
                dat_buffer.pop();
                recordReceiveQueueOccupancies(
                    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
                if (releaseCreditOnAccept()) {
                    releaseRxbufEntry(
                        Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA, curTick());
                } else {
                    stats.deferred_credit_release_events_by_channel[
                        channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA)]++;
                }
                DPRINTF(CHIPort,
                        "%s:DAT Recv buffer handle success, "
                        "buffer occupied num:%d, Peer Credit:%d\n",
                        this->name(), dat_buffer.size(),
                        getPeer().dat_credit);
            } else {
                // Handle failure to process the entry
                //todo, you may want to log or handle this case
                stats.receive_callback_reject_events_by_channel[
                    channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA)]++;
                DPRINTF(CHIPort,
                        "%s:DAT Recv buffer handle failed, "
                        "buffer occupied num:%d, Peer Credit:%d\n",
                        this->name(), dat_buffer.size(),
                        getPeer().dat_credit);
            }
        } else {
            panic("No receive callback set for CHIPort %s", owner_module->name());
        }
        // // after handling the head entry, check if the buffer is empty,if not, reschedule the event to next cycle
        // if (!dat_buffer.empty()) {
        //     if (!dat_handle_event.scheduled()) {
        //         schedule(dat_handle_event, curTick()+clockPeriod());
        //     }
        // }
    }

    void CHIPort::OnHandleEventCallback_RSP() {
        // Handle the event callback logic here
        // call receive_callback to handle the head entry of the buffer
        // if hanle successful, pop the head entry, and return the credit to sender
        assert(!rsp_buffer.empty() && "response buffer is empty");
        FlitPtr& head_entry = rsp_buffer.front();
        if (receive_callback) {
            if (receive_callback(head_entry)) {
                // Handle successful reception
                rsp_buffer.pop();
                recordReceiveQueueOccupancies(
                    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
                if (releaseCreditOnAccept()) {
                    releaseRxbufEntry(
                        Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP, curTick());
                } else {
                    stats.deferred_credit_release_events_by_channel[
                        channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP)]++;
                }
                DPRINTF(CHIPort,
                        "%s:RSP Recv buffer handle success, "
                        "buffer occupied num:%d, Peer Credit:%d\n",
                        this->name(), rsp_buffer.size(),
                        getPeer().rsp_credit);
            } else {
                // Handle failure to process the entry
                //todo, you may want to log or handle this case
                stats.receive_callback_reject_events_by_channel[
                    channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP)]++;
                DPRINTF(CHIPort,
                        "%s:RSP Recv buffer handle failed, "
                        "buffer occupied num:%d, Peer Credit:%d\n",
                        this->name(), rsp_buffer.size(),
                        getPeer().rsp_credit);
            }
        } else {
            panic("No receive callback set for CHIPort %s", owner_module->name());
        }
        // // after handling the head entry, check if the buffer is empty,if not, reschedule the event to next cycle
        // if (!rsp_buffer.empty()) {
        //     if (!rsp_handle_event.scheduled()) {
        //         schedule(rsp_handle_event, curTick()+clockPeriod());
        //     }
        // }
    }
    void CHIPort::GrantCredit_REQ() {
        const bool was_blocked = channel_blocked_by_credit[
            channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)];
        req_credit++;
        // If after granting credit, the channel's credit is 1,
        // it means the port cannot use the credit immediately,
        // so we update the last send time to avoid protential use of it.
        if (req_credit==1) {
            req_last_send_time = curCycle();
        }
        if (was_blocked && credit_block_start_valid[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)]) {
            stats.credit_stall_cycles_by_channel[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)] +=
                static_cast<uint64_t>(
                    curCycle() - credit_block_start[
                        channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)]);
            credit_block_start_valid[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)] = false;
        }
        if (was_blocked && req_credit > 0) {
            channel_blocked_by_credit[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)] = false;
            if (credit_unblock_callback) {
                credit_unblock_callback(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ);
            }
        }
    }
    void CHIPort::GrantCredit_SNP() {
        const bool was_blocked = channel_blocked_by_credit[
            channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP)];
        snp_credit++;
        if (snp_credit==1) {
            snp_last_send_time = curCycle();
        }
        if (was_blocked && credit_block_start_valid[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP)]) {
            stats.credit_stall_cycles_by_channel[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP)] +=
                static_cast<uint64_t>(
                    curCycle() - credit_block_start[
                        channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP)]);
            credit_block_start_valid[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP)] = false;
        }
        if (was_blocked && snp_credit > 0) {
            channel_blocked_by_credit[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP)] = false;
            if (credit_unblock_callback) {
                credit_unblock_callback(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP);
            }
        }
    }
    void CHIPort::GrantCredit_DAT() {
        const bool was_blocked = channel_blocked_by_credit[
            channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA)];
        dat_credit++;
        if (dat_credit==1) {
            dat_last_send_time = curCycle();
        }
        if (was_blocked && credit_block_start_valid[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA)]) {
            stats.credit_stall_cycles_by_channel[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA)] +=
                static_cast<uint64_t>(
                    curCycle() - credit_block_start[
                        channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA)]);
            credit_block_start_valid[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA)] = false;
        }
        if (was_blocked && dat_credit > 0) {
            channel_blocked_by_credit[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA)] = false;
            if (credit_unblock_callback) {
                credit_unblock_callback(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
            }
        }
    }
    void CHIPort::GrantCredit_RSP() {
        const bool was_blocked = channel_blocked_by_credit[
            channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP)];
        rsp_credit++;
        if (rsp_credit==1) {
            rsp_last_send_time = curCycle();
        }
        if (was_blocked && credit_block_start_valid[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP)]) {
            stats.credit_stall_cycles_by_channel[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP)] +=
                static_cast<uint64_t>(
                    curCycle() - credit_block_start[
                        channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP)]);
            credit_block_start_valid[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP)] = false;
        }
        if (was_blocked && rsp_credit > 0) {
            channel_blocked_by_credit[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP)] = false;
            if (credit_unblock_callback) {
                credit_unblock_callback(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
            }
        }
    }

    void CHIPort::setBlocked()
    {
        blocked = true;
    }

    void CHIPort::setUnblocked()
    {
        blocked = false;
    }

    bool CHIPort::isChannelBlockedByCredit(Flit::CHI_CHN_TYPE channel) const
    {
        const size_t idx = static_cast<size_t>(channel);
        panic_if(idx >= channel_blocked_by_credit.size(),
                 "Invalid CHI channel index %zu", idx);
        return channel_blocked_by_credit[idx];
    }
    void CHIPort::initState()
    {
        // Initialize the port state if needed
    }
    void CHIPort::init()
    {
        // Initialize the port, if needed
        // This can be used to set up initial conditions or configurations
        // DPRINTF(CHIPort, "CHIPort %s initialized with ID %d\n", portName, id);
    }
    void
    CHIPort::connect(CHIPort* peer_port)
    {
        if (!_connected) {
            connected_port = peer_port;
            _connected = true;
            req_credit = peer_port->initialCreditCount;
            snp_credit = peer_port->initialCreditCount;
            dat_credit = peer_port->initialCreditCount;
            rsp_credit = peer_port->initialCreditCount;
            DPRINTF(CHIPort,"Port Connect,%s : credit: %d\n",this->name(),req_credit);
        }else {
            assert(connected_port == peer_port);
        }
        if (!peer_port->_connected) {
            peer_port->connected_port = this;
            peer_port->_connected = true;
            peer_port->req_credit = this->initialCreditCount;
            peer_port->snp_credit = this->initialCreditCount;
            peer_port->dat_credit = this->initialCreditCount;
            peer_port->rsp_credit = this->initialCreditCount;
            DPRINTF(CHIPort,"Port Connect,%s : credit: %d\n",peer_port->name(),peer_port->req_credit);
        }else{
            assert(peer_port->connected_port == this);
        }

    }
    // Port &
    // CHIPort::getPort(const std::string &if_name,
    //               PortID idx=InvalidPortID) {
    //                 return Port();
    //               }
}}
