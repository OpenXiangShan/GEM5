#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <queue>

#include "mem/xsCHI/base/CHIPort.hh"
#include "mem/xsCHI/base/flit.hh"
#include "params/MeshNode.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/stats.hh"

namespace gem5
{
namespace xsCHI
{

class MeshNode : public ClockedObject
{
  public:
    enum class RouteDecision : uint8_t
    {
        Local0 = 0,
        Local1 = 1,
        East = 2,
        West = 3,
        North = 4,
        South = 5
    };

    // Dedicated API for unit tests. Runtime code should use internal
    // helpers through MeshNode member functions instead of this API.
    class TestApi
    {
      public:
        static RouteDecision routeDecisionXY(uint32_t nodeX, uint32_t nodeY,
                                             uint32_t tgtId);
        static bool shouldBackpressure(size_t currentDepth,
                                       size_t configuredDepth);
        static size_t selectBackpressureDepth(size_t ingressDepth,
                                              size_t aggregateDepth,
                                              bool perIngressMode);
        static int selectIngressRR(const std::array<size_t, 6>& pendingPerIngress,
                                   size_t cursor);
    };

    typedef MeshNodeParams Params;
    MeshNode(const Params &p);

    void init() override;

    CHIPort* getLocal0Port() const;
    CHIPort* getLocal1Port() const;
    CHIPort* getEastPort() const;
    CHIPort* getWestPort() const;
    CHIPort* getNorthPort() const;
    CHIPort* getSouthPort() const;
    uint32_t getNodeX() const;
    uint32_t getNodeY() const;

  private:
    // Keep port index stable to simplify queue/arbitration array indexing.
    enum class PortIndex : uint8_t
    {
        Local0 = 0,
        Local1 = 1,
        East = 2,
        West = 3,
        North = 4,
        South = 5,
        NumPorts = 6
    };

    static constexpr size_t NumPorts = static_cast<size_t>(PortIndex::NumPorts);
    static constexpr size_t NumChannels =
        static_cast<size_t>(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_NUM);
    static constexpr size_t NumDirs = 4;
    // Must match NodeID encoding assumptions used in xsCHI.
    static constexpr uint32_t MeshCoordBits = 5;

    using FlitQueue = std::queue<FlitPtr>;
    using SourceQueues = std::array<FlitQueue, NumPorts>;
    using ChannelQueues = std::array<SourceQueues, NumChannels>;
    using OutputQueues = std::array<ChannelQueues, NumPorts>;

    std::array<CHIPort*, NumPorts> ports;
    const uint32_t nodeX;
    const uint32_t nodeY;
    const size_t voqDepth;
    const size_t ibDepth;
    const bool voqDepthPerIngress;

    // VOQ indexed by [egress][channel][ingress]. This avoids head-of-line
    // blocking between different output directions.
    OutputQueues outVoq;
    // Round-robin cursor per [egress][channel].
    std::array<std::array<size_t, NumChannels>, NumPorts> rrCursor;

    struct MeshNodeStats : public statistics::Group
    {
        explicit MeshNodeStats(MeshNode *parent);

        statistics::Scalar msg_count_control;
        statistics::Scalar msg_count_data;
        statistics::Scalar msg_byte_data;

        statistics::Vector ingress_flits_by_channel;
        statistics::Vector egress_flits_by_channel;

        statistics::Vector dir_egress_flits;
        statistics::Vector dir_active_cycles;
        statistics::Vector2d egress_flits_by_dir_channel;
        statistics::Vector2d egress_channel_active_cycles;
        statistics::Vector2d egress_parallel_channel_cycles_by_dir;
        statistics::Scalar send_event_cycles;
        statistics::Formula dir_link_util;

        statistics::Scalar voq_full_events;
        statistics::Vector voq_full_events_by_egress;
        statistics::Vector voq_backpressure_events_by_channel;

        statistics::Vector voq_depth_accum_by_egress;
        statistics::Formula voq_avg_depth_by_egress;
        statistics::Vector ib_full_events_by_channel;
        statistics::Vector ib_occupancy_accum_by_channel;
        statistics::Formula ib_avg_occupancy_by_channel;

        statistics::Vector egress_stall_cycles_by_dir;
        statistics::Vector egress_bw_sat_cycles_by_dir;
        statistics::Vector egress_credit_blocked_cycles_by_channel;

        statistics::Histogram hop_count_hist_snp;
        statistics::Histogram hop_count_hist_req;
        statistics::Histogram hop_count_hist_rsp;
        statistics::Histogram hop_count_hist_dat;

        statistics::Histogram e2e_latency_hist_snp;
        statistics::Histogram e2e_latency_hist_req;
        statistics::Histogram e2e_latency_hist_rsp;
        statistics::Histogram e2e_latency_hist_dat;
    } stats;

    // Global retry/scheduling event for all output ports.
    EventFunctionWrapper sendEvent;
    // A send can fail because a just-returned credit is not usable until the
    // next cycle. That case has no future credit-unblock callback, so the
    // scheduler must explicitly retry once.
    bool retryOnNextCycle;

    // Ingress callback registered to each physical port.
    bool handleIngress(PortIndex ingress, FlitPtr &flit);
    // Scheduler entry: pick candidates and try to send one flit per
    // output/channel opportunity.
    void onSendEvent();

    size_t trySendForOutput(PortIndex egress);
    bool trySendForOutputAndChannel(PortIndex egress,
                                    Flit::CHI_CHN_TYPE channel);

    // Deterministic XY routing.
    PortIndex routeFor(const FlitPtr &flit) const;

    bool hasPendingFlits() const;
    size_t getQueueDepth(PortIndex egress, Flit::CHI_CHN_TYPE channel) const;
    size_t getQueueDepthAllChannels(PortIndex egress) const;
    size_t getIngressIbDepth(PortIndex ingress,
                             Flit::CHI_CHN_TYPE channel) const;
    bool canAllocateIbSlot(PortIndex ingress,
                           Flit::CHI_CHN_TYPE channel) const;
    bool isEgressUsable(PortIndex egress) const;
    void scheduleSendEvent();
    void scheduleSendEventAtNextCycle();
    void registerCallbacks();
    void handleCreditUnblock(PortIndex egress, Flit::CHI_CHN_TYPE channel);
    bool hasPendingOnEgressChannel(PortIndex egress,
                                   Flit::CHI_CHN_TYPE channel) const;
    void sampleHopCountByChannel(Flit::CHI_CHN_TYPE channel, Counter hops);
    void sampleE2eLatencyByChannel(Flit::CHI_CHN_TYPE channel, Counter latency);

    // Runtime helpers. Do not call these directly from tests.
    static RouteDecision routeDecisionXYImpl(uint32_t nodeX, uint32_t nodeY,
                                             uint32_t tgtId);
    static bool shouldBackpressureImpl(size_t currentDepth,
                                       size_t configuredDepth);
    static size_t selectBackpressureDepthImpl(size_t ingressDepth,
                                              size_t aggregateDepth,
                                              bool perIngressMode);
    static int selectIngressRRImpl(const std::array<size_t, 6>& pendingPerIngress,
                                   size_t cursor);

    static size_t portToIndex(PortIndex idx);
    static size_t channelToIndex(Flit::CHI_CHN_TYPE channel);
    static int directionToIndex(PortIndex idx);
    static bool isLocalPort(PortIndex idx);
    static const char* portName(PortIndex idx);
    static const char* directionName(size_t idx);
    static const char* channelName(Flit::CHI_CHN_TYPE channel);
};

inline MeshNode::RouteDecision
MeshNode::TestApi::routeDecisionXY(uint32_t nodeX, uint32_t nodeY,
                                   uint32_t tgtId)
{
    return MeshNode::routeDecisionXYImpl(nodeX, nodeY, tgtId);
}

inline bool
MeshNode::TestApi::shouldBackpressure(size_t currentDepth,
                                      size_t configuredDepth)
{
    return MeshNode::shouldBackpressureImpl(currentDepth, configuredDepth);
}

inline size_t
MeshNode::TestApi::selectBackpressureDepth(size_t ingressDepth,
                                           size_t aggregateDepth,
                                           bool perIngressMode)
{
    return MeshNode::selectBackpressureDepthImpl(
        ingressDepth, aggregateDepth, perIngressMode);
}

inline int
MeshNode::TestApi::selectIngressRR(const std::array<size_t, 6>& pendingPerIngress,
                                   size_t cursor)
{
    return MeshNode::selectIngressRRImpl(pendingPerIngress, cursor);
}

inline MeshNode::RouteDecision
MeshNode::routeDecisionXYImpl(uint32_t nodeX, uint32_t nodeY, uint32_t tgtId)
{
    const uint32_t coordMask = (1u << MeshCoordBits) - 1;
    const uint32_t encodedCoord = tgtId >> 3;
    const uint32_t tgtX = encodedCoord >> MeshCoordBits;
    const uint32_t tgtY = encodedCoord & coordMask;

    if (tgtX > nodeX) {
        return RouteDecision::East;
    }
    if (tgtX < nodeX) {
        return RouteDecision::West;
    }
    if (tgtY > nodeY) {
        return RouteDecision::North;
    }
    if (tgtY < nodeY) {
        return RouteDecision::South;
    }
    return (tgtId & 0b100) ? RouteDecision::Local1 : RouteDecision::Local0;
}

inline bool
MeshNode::shouldBackpressureImpl(size_t currentDepth, size_t configuredDepth)
{
    const size_t effectiveDepth = configuredDepth == 0 ? 1 : configuredDepth;
    return currentDepth >= effectiveDepth;
}

inline size_t
MeshNode::selectBackpressureDepthImpl(size_t ingressDepth,
                                      size_t aggregateDepth,
                                      bool perIngressMode)
{
    return perIngressMode ? ingressDepth : aggregateDepth;
}

inline int
MeshNode::selectIngressRRImpl(const std::array<size_t, 6>& pendingPerIngress,
                              size_t cursor)
{
    constexpr size_t kPorts = 6;
    for (size_t offset = 0; offset < kPorts; ++offset) {
        const size_t idx = (cursor + offset) % kPorts;
        if (pendingPerIngress[idx] > 0) {
            return static_cast<int>(idx);
        }
    }
    return -1;
}

} // namespace xsCHI
} // namespace gem5
