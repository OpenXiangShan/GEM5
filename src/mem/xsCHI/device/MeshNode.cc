#include "mem/xsCHI/device/MeshNode.hh"

#include <cassert>
#include <limits>
#include <string>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/CHIMeshNode.hh"
#include "mem/xsCHI/base/FlitOpType.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace xsCHI
{

namespace
{
// Deterministic channel traversal order. This is not a shared-egress priority:
// each CHI channel has an independent send opportunity in the same cycle.
constexpr std::array<Flit::CHI_CHN_TYPE, 4> kChannelServiceOrder = {
    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP,
    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP,
    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA,
    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ,
};

constexpr size_t kHopHistogramBuckets = 32;
constexpr size_t kE2eLatencyHistogramBuckets = 1024;
}

MeshNode::MeshNodeStats::MeshNodeStats(MeshNode *parent)
    : statistics::Group(parent, "network"),
      ADD_STAT(msg_count_control, statistics::units::Count::get(),
               "Total number of control flits sent by this mesh node"),
      ADD_STAT(msg_count_data, statistics::units::Count::get(),
               "Total number of data flits sent by this mesh node"),
      ADD_STAT(msg_byte_data, statistics::units::Byte::get(),
               "Total number of payload bytes sent on data channel"),
      ADD_STAT(ingress_flits_by_channel, statistics::units::Count::get(),
               "Accepted ingress flits by CHI channel"),
      ADD_STAT(egress_flits_by_channel, statistics::units::Count::get(),
               "Successful egress flits by CHI channel"),
      ADD_STAT(dir_egress_flits, statistics::units::Count::get(),
               "Successful egress flits by mesh direction"),
      ADD_STAT(dir_active_cycles, statistics::units::Cycle::get(),
               "Cycles in which each mesh direction sent at least one flit"),
      ADD_STAT(egress_flits_by_dir_channel, statistics::units::Count::get(),
               "Successful egress flits grouped by mesh direction and CHI channel"),
      ADD_STAT(egress_channel_active_cycles, statistics::units::Cycle::get(),
               "Cycles in which each mesh direction/channel sent a flit"),
      ADD_STAT(egress_parallel_channel_cycles_by_dir,
               statistics::units::Cycle::get(),
               "Scheduler cycles grouped by direction and number of CHI channels "
               "that sent in parallel"),
      ADD_STAT(send_event_cycles, statistics::units::Cycle::get(),
               "Number of scheduler cycles processed by this mesh node"),
      ADD_STAT(dir_link_util, statistics::units::Ratio::get(),
               "Directional link utilization in percent"),
      ADD_STAT(voq_full_events, statistics::units::Count::get(),
               "Number of times ingress was backpressured by a full VOQ"),
      ADD_STAT(voq_full_events_by_egress, statistics::units::Count::get(),
               "VOQ full events grouped by routed egress port"),
      ADD_STAT(voq_backpressure_events_by_channel,
               statistics::units::Count::get(),
               "VOQ full/backpressure events grouped by CHI channel"),
      ADD_STAT(voq_depth_accum_by_egress, statistics::units::Count::get(),
               "Accumulated VOQ depth sampled once per scheduler cycle"),
      ADD_STAT(voq_avg_depth_by_egress, statistics::units::Rate<
                    statistics::units::Count,
                    statistics::units::Cycle>::get(),
               "Average VOQ depth per egress port"),
      ADD_STAT(ib_full_events_by_channel, statistics::units::Count::get(),
               "CMN RTL IB full events grouped by CHI channel"),
      ADD_STAT(ib_occupancy_accum_by_channel, statistics::units::Count::get(),
               "Accumulated CMN RTL IB occupancy sampled at scheduler cycles"),
      ADD_STAT(ib_avg_occupancy_by_channel, statistics::units::Rate<
                    statistics::units::Count,
                    statistics::units::Cycle>::get(),
               "Average CMN RTL IB occupancy per CHI channel"),
      ADD_STAT(egress_stall_cycles_by_dir, statistics::units::Cycle::get(),
               "Directional cycles with pending flits but no successful send"),
      ADD_STAT(egress_bw_sat_cycles_by_dir, statistics::units::Cycle::get(),
               "Directional cycles that sent flits and still had backlog"),
      ADD_STAT(egress_credit_blocked_cycles_by_channel,
               statistics::units::Cycle::get(),
               "Scheduler cycles where egress send failed due to credit"),
      ADD_STAT(hop_count_hist_snp, statistics::units::Count::get(),
               "Hop-count distribution at local delivery for SNP channel"),
      ADD_STAT(hop_count_hist_req, statistics::units::Count::get(),
               "Hop-count distribution at local delivery for REQ channel"),
      ADD_STAT(hop_count_hist_rsp, statistics::units::Count::get(),
               "Hop-count distribution at local delivery for RSP channel"),
      ADD_STAT(hop_count_hist_dat, statistics::units::Count::get(),
               "Hop-count distribution at local delivery for DAT channel"),
      ADD_STAT(e2e_latency_hist_snp, statistics::units::Tick::get(),
               "End-to-end latency distribution at local delivery for SNP"),
      ADD_STAT(e2e_latency_hist_req, statistics::units::Tick::get(),
               "End-to-end latency distribution at local delivery for REQ"),
      ADD_STAT(e2e_latency_hist_rsp, statistics::units::Tick::get(),
               "End-to-end latency distribution at local delivery for RSP"),
      ADD_STAT(e2e_latency_hist_dat, statistics::units::Tick::get(),
               "End-to-end latency distribution at local delivery for DAT")
{
    using namespace statistics;

    msg_count_control.flags(nozero);
    msg_count_data.flags(nozero);
    msg_byte_data.flags(nozero);
    voq_full_events.flags(nozero);

    ingress_flits_by_channel
        .init(MeshNode::NumChannels)
        .flags(nozero);
    egress_flits_by_channel
        .init(MeshNode::NumChannels)
        .flags(nozero);
    for (size_t c = 0; c < MeshNode::NumChannels; ++c) {
        const auto ch = static_cast<Flit::CHI_CHN_TYPE>(c);
        const std::string label = MeshNode::channelName(ch);
        ingress_flits_by_channel.subname(c, label);
        egress_flits_by_channel.subname(c, label);
    }

    dir_egress_flits
        .init(MeshNode::NumDirs)
        .flags(nozero);
    dir_active_cycles
        .init(MeshNode::NumDirs)
        .flags(nozero);
    send_event_cycles.flags(nozero);
    for (size_t d = 0; d < MeshNode::NumDirs; ++d) {
        const std::string label = MeshNode::directionName(d);
        dir_egress_flits.subname(d, label);
        dir_active_cycles.subname(d, label);
    }

    egress_flits_by_dir_channel
        .init(MeshNode::NumDirs, MeshNode::NumChannels)
        .flags(nozero);
    egress_channel_active_cycles
        .init(MeshNode::NumDirs, MeshNode::NumChannels)
        .flags(nozero);
    egress_parallel_channel_cycles_by_dir
        .init(MeshNode::NumDirs, MeshNode::NumChannels + 1)
        .flags(nozero);
    for (size_t d = 0; d < MeshNode::NumDirs; ++d) {
        const std::string dirLabel = MeshNode::directionName(d);
        egress_flits_by_dir_channel.subname(d, dirLabel);
        egress_channel_active_cycles.subname(d, dirLabel);
        egress_parallel_channel_cycles_by_dir.subname(d, dirLabel);
    }
    for (size_t c = 0; c < MeshNode::NumChannels; ++c) {
        const auto ch = static_cast<Flit::CHI_CHN_TYPE>(c);
        const std::string chLabel = MeshNode::channelName(ch);
        egress_flits_by_dir_channel.ysubname(c, chLabel);
        egress_channel_active_cycles.ysubname(c, chLabel);
    }
    for (size_t count = 0; count <= MeshNode::NumChannels; ++count) {
        egress_parallel_channel_cycles_by_dir.ysubname(
            count, std::to_string(count));
    }

    voq_full_events_by_egress
        .init(MeshNode::NumPorts)
        .flags(nozero);
    for (size_t p = 0; p < MeshNode::NumPorts; ++p) {
        const std::string label = MeshNode::portName(static_cast<PortIndex>(p));
        voq_full_events_by_egress.subname(p, label);
    }

    voq_backpressure_events_by_channel
        .init(MeshNode::NumChannels)
        .flags(nozero);
    for (size_t c = 0; c < MeshNode::NumChannels; ++c) {
        const auto ch = static_cast<Flit::CHI_CHN_TYPE>(c);
        voq_backpressure_events_by_channel.subname(c, MeshNode::channelName(ch));
    }

    voq_depth_accum_by_egress
        .init(MeshNode::NumPorts)
        .flags(nozero);
    for (size_t p = 0; p < MeshNode::NumPorts; ++p) {
        voq_depth_accum_by_egress.subname(
            p, MeshNode::portName(static_cast<PortIndex>(p)));
    }

    ib_full_events_by_channel
        .init(MeshNode::NumChannels)
        .flags(nozero);
    ib_occupancy_accum_by_channel
        .init(MeshNode::NumChannels)
        .flags(nozero);
    for (size_t c = 0; c < MeshNode::NumChannels; ++c) {
        const auto ch = static_cast<Flit::CHI_CHN_TYPE>(c);
        const std::string label = MeshNode::channelName(ch);
        ib_full_events_by_channel.subname(c, label);
        ib_occupancy_accum_by_channel.subname(c, label);
    }

    egress_stall_cycles_by_dir
        .init(MeshNode::NumDirs)
        .flags(nozero);
    egress_bw_sat_cycles_by_dir
        .init(MeshNode::NumDirs)
        .flags(nozero);
    for (size_t d = 0; d < MeshNode::NumDirs; ++d) {
        const std::string label = MeshNode::directionName(d);
        egress_stall_cycles_by_dir.subname(d, label);
        egress_bw_sat_cycles_by_dir.subname(d, label);
    }

    egress_credit_blocked_cycles_by_channel
        .init(MeshNode::NumChannels)
        .flags(nozero);
    for (size_t c = 0; c < MeshNode::NumChannels; ++c) {
        const auto ch = static_cast<Flit::CHI_CHN_TYPE>(c);
        egress_credit_blocked_cycles_by_channel.subname(c,
                                                        MeshNode::channelName(ch));
    }

    hop_count_hist_snp
        .init(kHopHistogramBuckets)
        .flags(nozero | nonan);
    hop_count_hist_req
        .init(kHopHistogramBuckets)
        .flags(nozero | nonan);
    hop_count_hist_rsp
        .init(kHopHistogramBuckets)
        .flags(nozero | nonan);
    hop_count_hist_dat
        .init(kHopHistogramBuckets)
        .flags(nozero | nonan);

    e2e_latency_hist_snp
        .init(kE2eLatencyHistogramBuckets)
        .flags(nozero | nonan);
    e2e_latency_hist_req
        .init(kE2eLatencyHistogramBuckets)
        .flags(nozero | nonan);
    e2e_latency_hist_rsp
        .init(kE2eLatencyHistogramBuckets)
        .flags(nozero | nonan);
    e2e_latency_hist_dat
        .init(kE2eLatencyHistogramBuckets)
        .flags(nozero | nonan);

    dir_link_util
        .flags(nozero | nonan)
        .precision(6);
    dir_link_util = 100 * dir_active_cycles / send_event_cycles;

    voq_avg_depth_by_egress
        .flags(nozero | nonan)
        .precision(6);
    voq_avg_depth_by_egress = voq_depth_accum_by_egress / send_event_cycles;

    ib_avg_occupancy_by_channel
        .flags(nozero | nonan)
        .precision(6);
    ib_avg_occupancy_by_channel =
        ib_occupancy_accum_by_channel / send_event_cycles;
}

MeshNode::MeshNode(const Params &p)
    : ClockedObject(p),
      ports{p.port_local0, p.port_local1, p.port_east, p.port_west,
            p.port_north, p.port_south},
      nodeX(p.node_x),
      nodeY(p.node_y),
      voqDepth(p.voq_depth == 0 ? 1 : p.voq_depth),
      ibDepth(p.ib_depth == 0 ? (p.voq_depth == 0 ? 2 : p.voq_depth)
                              : p.ib_depth),
      voqDepthPerIngress(p.voq_depth_per_ingress),
      outVoq(),
      rrCursor(),
      stats(this),
      sendEvent([this] { onSendEvent(); }, name()),
      retryOnNextCycle(false)
{
    registerCallbacks();
    panic_if(ibDepth == 0, "MeshNode %s has invalid ib_depth=0", name());
}

void
MeshNode::init()
{
    panic_if(ports[portToIndex(PortIndex::Local0)] == nullptr,
             "MeshNode %s must have a non-null local0 port", name());
}

CHIPort*
MeshNode::getLocal0Port() const
{
    return ports[portToIndex(PortIndex::Local0)];
}

CHIPort*
MeshNode::getLocal1Port() const
{
    return ports[portToIndex(PortIndex::Local1)];
}

CHIPort*
MeshNode::getEastPort() const
{
    return ports[portToIndex(PortIndex::East)];
}

CHIPort*
MeshNode::getWestPort() const
{
    return ports[portToIndex(PortIndex::West)];
}

CHIPort*
MeshNode::getNorthPort() const
{
    return ports[portToIndex(PortIndex::North)];
}

CHIPort*
MeshNode::getSouthPort() const
{
    return ports[portToIndex(PortIndex::South)];
}

uint32_t
MeshNode::getNodeX() const
{
    return nodeX;
}

uint32_t
MeshNode::getNodeY() const
{
    return nodeY;
}

bool
MeshNode::handleIngress(PortIndex ingress, FlitPtr &flit)
{
    assert(flit != nullptr);

    const PortIndex egress = routeFor(flit);
    const Flit::CHI_CHN_TYPE channel = flit->get_Flit_Channel_Type();

    if (isLocalPort(ingress) && !flit->getMeshStatsValid()) {
        flit->setMeshStatsValid(true);
        flit->setMeshInjectTick(curTick());
        flit->setMeshHopCount(0);
    }

    panic_if(!isEgressUsable(egress),
             "MeshNode %s routes flit(op=%s, tgt=%u) to unusable egress %s",
             name(),
             CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()).c_str(),
             flit->getTgtId(), portName(egress));

    const size_t egressIdx = portToIndex(egress);
    const size_t ingressIdx = portToIndex(ingress);
    const size_t channelIdx = channelToIndex(channel);
    const bool rtlIbMode =
        ports[ingressIdx]->releasesCreditOnDownstreamRelease();
    const size_t ingressDepth = outVoq[egressIdx][channelIdx][ingressIdx].size();
    const size_t aggregateDepth = getQueueDepth(egress, channel);
    const size_t selectedDepth = rtlIbMode ?
        getIngressIbDepth(ingress, channel) :
        selectBackpressureDepthImpl(
            ingressDepth, aggregateDepth, voqDepthPerIngress);
    const size_t depthLimit = rtlIbMode ? ibDepth : voqDepth;

    // Backpressure point: when VOQ/IB is full, keep flit at upstream CHIPort.
    if (shouldBackpressureImpl(selectedDepth, depthLimit)) {
        stats.voq_full_events++;
        stats.voq_full_events_by_egress[egressIdx]++;
        stats.voq_backpressure_events_by_channel[channelIdx]++;
        if (rtlIbMode) {
            stats.ib_full_events_by_channel[channelIdx]++;
        }
        DPRINTF(CHIMeshNode,
                "%s ingress=%s egress=%s channel=%d %s full mode=%s "
                "selected_depth=%u ingress_depth=%u aggregate_depth=%u "
                "limit=%u op=%s tgt=%u txn=%llu\n",
                name(), portName(ingress), portName(egress),
                static_cast<int>(channel),
                rtlIbMode ? "IB" : "VOQ",
                voqDepthPerIngress ? "per_ingress" : "aggregate",
                static_cast<unsigned>(selectedDepth),
                static_cast<unsigned>(ingressDepth),
                static_cast<unsigned>(aggregateDepth),
                static_cast<unsigned>(depthLimit),
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()).c_str(),
                flit->getTgtId(),
                static_cast<unsigned long long>(flit->getTxnId()));
        return false;
    }

    stats.ingress_flits_by_channel[channelIdx]++;
    outVoq[egressIdx][channelIdx][ingressIdx].push(std::move(flit));
    DPRINTF(CHIMeshNode,
            "%s enqueue ingress=%s egress=%s channel=%d op=%s tgt=%u\n",
            name(), portName(ingress), portName(egress),
            static_cast<int>(channel),
            CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(
                outVoq[egressIdx][channelIdx][ingressIdx].front()->getOpcode())
                .c_str(),
            outVoq[egressIdx][channelIdx][ingressIdx].front()->getTgtId());

    scheduleSendEvent();
    return true;
}

void
MeshNode::onSendEvent()
{
    stats.send_event_cycles++;
    retryOnNextCycle = false;
    bool sentAny = false;
    for (size_t c = 0; c < NumChannels; ++c) {
        size_t channelDepth = 0;
        for (size_t e = 0; e < NumPorts; ++e) {
            for (size_t s = 0; s < NumPorts; ++s) {
                channelDepth += outVoq[e][c][s].size();
            }
        }
        stats.ib_occupancy_accum_by_channel[c] += channelDepth;
    }
    // Try every output each cycle; each output has independent arbitration.
    for (size_t i = 0; i < NumPorts; ++i) {
        const PortIndex egress = static_cast<PortIndex>(i);
        const size_t pendingBefore = getQueueDepthAllChannels(egress);
        stats.voq_depth_accum_by_egress[i] += pendingBefore;

        const size_t sentChannels = trySendForOutput(egress);
        const bool sentOnEgress = sentChannels > 0;
        sentAny |= sentOnEgress;
        const size_t pendingAfter = getQueueDepthAllChannels(egress);

        const int dirIdx = directionToIndex(egress);
        if (dirIdx >= 0 && isEgressUsable(egress)) {
            stats.egress_parallel_channel_cycles_by_dir[
                static_cast<size_t>(dirIdx)][sentChannels]++;
        }
        if (sentOnEgress && dirIdx >= 0) {
            stats.dir_active_cycles[static_cast<size_t>(dirIdx)]++;
        }

        if (dirIdx >= 0 && pendingBefore > 0 && !sentOnEgress) {
            stats.egress_stall_cycles_by_dir[static_cast<size_t>(dirIdx)]++;
        }

        if (dirIdx >= 0 && sentOnEgress && pendingAfter > 0) {
            stats.egress_bw_sat_cycles_by_dir[static_cast<size_t>(dirIdx)]++;
        }
    }

    const bool pending = hasPendingFlits();
    // Avoid blind per-cycle polling under full downstream backpressure.
    // Retry will be re-armed by ingress enqueue or credit-unblock callback.
    // If send failed only because a just-returned credit cannot be consumed in
    // the same cycle, no unblock callback will fire, so retry explicitly.
    if (pending && (sentAny || retryOnNextCycle)) {
        scheduleSendEventAtNextCycle();
    }

    DPRINTF(CHIMeshNode, "%s scheduler tick sentAny=%d pending=%d\n", name(),
            sentAny ? 1 : 0, pending ? 1 : 0);
}

size_t
MeshNode::trySendForOutput(PortIndex egress)
{
    if (!isEgressUsable(egress)) {
        return 0;
    }

    size_t sentChannels = 0;
    for (const auto channel : kChannelServiceOrder) {
        if (trySendForOutputAndChannel(egress, channel)) {
            sentChannels++;
        }
    }
    return sentChannels;
}

bool
MeshNode::trySendForOutputAndChannel(PortIndex egress,
                                     Flit::CHI_CHN_TYPE channel)
{
    if (!isEgressUsable(egress)) {
        return false;
    }

    const size_t egressIdx = portToIndex(egress);
    const size_t channelIdx = channelToIndex(channel);
    auto &cursor = rrCursor[egressIdx][channelIdx];

    CHIPort* const egressPort = ports[egressIdx];

    std::array<size_t, NumPorts> pending{};
    for (size_t i = 0; i < NumPorts; ++i) {
        pending[i] = outVoq[egressIdx][channelIdx][i].size();
    }

    // Round-robin over ingress sources that target the same egress/channel.
    const int selected = selectIngressRRImpl(pending, cursor);
    if (selected >= 0) {
        const size_t srcIdx = static_cast<size_t>(selected);
        auto &q = outVoq[egressIdx][channelIdx][srcIdx];
        FlitPtr &head = q.front();
        assert(head != nullptr);
        const CHI_OP_TYPE op = head->getOpcode();
        const uint32_t tgt = head->getTgtId();
        const uint64_t txn = head->getTxnId();
        const uint32_t dataBytes = head->getSize();
        const bool meshStatsValid = head->getMeshStatsValid();
        const Counter hopCountAtEgress = head->getMeshHopCount();
        const Tick injectTick = head->getMeshInjectTick();
        const int dirIdx = directionToIndex(egress);
        const bool forwardToDirection = dirIdx >= 0 && meshStatsValid;
        const uint16_t oldHopCount = head->getMeshHopCount();

        if (forwardToDirection) {
            if (oldHopCount < std::numeric_limits<uint16_t>::max()) {
                head->setMeshHopCount(oldHopCount + 1);
            }
        }

        if (egressPort->send(head)) {
            stats.egress_flits_by_channel[channelIdx]++;
            if (channel == Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA) {
                stats.msg_count_data++;
                stats.msg_byte_data += dataBytes;
            } else {
                stats.msg_count_control++;
            }

            if (dirIdx >= 0) {
                stats.dir_egress_flits[static_cast<size_t>(dirIdx)]++;
                stats.egress_flits_by_dir_channel[
                    static_cast<size_t>(dirIdx)][channelIdx]++;
                stats.egress_channel_active_cycles[
                    static_cast<size_t>(dirIdx)][channelIdx]++;
            } else if (meshStatsValid) {
                sampleHopCountByChannel(channel, hopCountAtEgress);
                const Tick e2eLatency = curTick() >= injectTick ?
                    (curTick() - injectTick) : 0;
                sampleE2eLatencyByChannel(channel, e2eLatency);
            }

            DPRINTF(CHIMeshNode,
                    "%s send egress=%s from ingress=%s channel=%d op=%s "
                    "tgt=%u txn=%llu\n",
                    name(), portName(egress),
                    portName(static_cast<PortIndex>(srcIdx)),
                    static_cast<int>(channel),
                    CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op).c_str(), tgt,
                    static_cast<unsigned long long>(txn));
            q.pop();
            CHIPort* const ingressPort = ports[srcIdx];
            if (ingressPort->releasesCreditOnDownstreamRelease()) {
                ingressPort->releaseRxbufEntry(channel, curTick());
            }
            cursor = (srcIdx + 1) % NumPorts;
            return true;
        }

        const bool creditBlocked = egressPort->isChannelBlockedByCredit(channel);
        if (creditBlocked) {
            stats.egress_credit_blocked_cycles_by_channel[channelIdx]++;
        }
        if (!creditBlocked) {
            retryOnNextCycle = true;
        }
        DPRINTF(CHIMeshNode,
                "%s blocked egress=%s channel=%d ingress=%s op=%s tgt=%u "
                "credit_blocked=%d retry_next=%d\n",
                name(), portName(egress), static_cast<int>(channel),
                portName(static_cast<PortIndex>(srcIdx)),
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op).c_str(), tgt,
                creditBlocked ? 1 : 0, retryOnNextCycle ? 1 : 0);
        if (forwardToDirection) {
            head->setMeshHopCount(oldHopCount);
        }
        return false;
    }
    return false;
}

MeshNode::PortIndex
MeshNode::routeFor(const FlitPtr &flit) const
{
    assert(flit != nullptr);
    switch (routeDecisionXYImpl(nodeX, nodeY, flit->getTgtId())) {
      case RouteDecision::Local0:
        return PortIndex::Local0;
      case RouteDecision::Local1:
        return PortIndex::Local1;
      case RouteDecision::East:
        return PortIndex::East;
      case RouteDecision::West:
        return PortIndex::West;
      case RouteDecision::North:
        return PortIndex::North;
      case RouteDecision::South:
        return PortIndex::South;
    }
    panic("MeshNode %s invalid route decision", name());
}

bool
MeshNode::hasPendingFlits() const
{
    for (size_t e = 0; e < NumPorts; ++e) {
        for (size_t c = 0; c < NumChannels; ++c) {
            for (size_t s = 0; s < NumPorts; ++s) {
                if (!outVoq[e][c][s].empty()) {
                    return true;
                }
            }
        }
    }
    return false;
}

size_t
MeshNode::getQueueDepth(PortIndex egress, Flit::CHI_CHN_TYPE channel) const
{
    const size_t egressIdx = portToIndex(egress);
    const size_t channelIdx = channelToIndex(channel);

    size_t depth = 0;
    for (size_t s = 0; s < NumPorts; ++s) {
        depth += outVoq[egressIdx][channelIdx][s].size();
    }
    return depth;
}

size_t
MeshNode::getQueueDepthAllChannels(PortIndex egress) const
{
    size_t depth = 0;
    for (size_t c = 0; c < NumChannels; ++c) {
        depth += getQueueDepth(
            egress, static_cast<Flit::CHI_CHN_TYPE>(c));
    }
    return depth;
}

size_t
MeshNode::getIngressIbDepth(PortIndex ingress,
                            Flit::CHI_CHN_TYPE channel) const
{
    const size_t ingressIdx = portToIndex(ingress);
    const size_t channelIdx = channelToIndex(channel);

    size_t depth = 0;
    for (size_t e = 0; e < NumPorts; ++e) {
        depth += outVoq[e][channelIdx][ingressIdx].size();
    }
    return depth;
}

bool
MeshNode::canAllocateIbSlot(PortIndex ingress,
                            Flit::CHI_CHN_TYPE channel) const
{
    return getIngressIbDepth(ingress, channel) < ibDepth;
}

bool
MeshNode::isEgressUsable(PortIndex egress) const
{
    // A null port means boundary edge is not present in this topology.
    CHIPort* const p = ports[portToIndex(egress)];
    return p != nullptr && p->isConnected();
}

void
MeshNode::scheduleSendEvent()
{
    if (!sendEvent.scheduled()) {
        scheduleSendEventAtNextCycle();
    }
}

void
MeshNode::scheduleSendEventAtNextCycle()
{
    panic_if(sendEvent.scheduled(),
             "MeshNode %s sendEvent already scheduled", name());
    schedule(sendEvent, curTick() + clockPeriod());
}

bool
MeshNode::hasPendingOnEgressChannel(PortIndex egress,
                                    Flit::CHI_CHN_TYPE channel) const
{
    return getQueueDepth(egress, channel) > 0;
}

void
MeshNode::handleCreditUnblock(PortIndex egress, Flit::CHI_CHN_TYPE channel)
{
    if (!isEgressUsable(egress) || sendEvent.scheduled()) {
        return;
    }

    // Only wake the scheduler if this egress/channel still has backlog.
    if (!hasPendingOnEgressChannel(egress, channel)) {
        return;
    }

    DPRINTF(CHIMeshNode,
            "%s credit-unblock egress=%s channel=%d schedule retry\n",
            name(), portName(egress), static_cast<int>(channel));
    scheduleSendEventAtNextCycle();
}

void
MeshNode::registerCallbacks()
{
    for (size_t i = 0; i < NumPorts; ++i) {
        CHIPort* const port = ports[i];
        if (port == nullptr) {
            continue;
        }
        const PortIndex ingress = static_cast<PortIndex>(i);
        port->setReceiveCallback(
            [this, ingress](FlitPtr &flit) { return handleIngress(ingress, flit); });
        const PortIndex egress = static_cast<PortIndex>(i);
        port->setCreditUnblockCallback(
            [this, egress](Flit::CHI_CHN_TYPE channel) {
                handleCreditUnblock(egress, channel);
            });
        port->setOwner(this);
    }
}

size_t
MeshNode::portToIndex(PortIndex idx)
{
    return static_cast<size_t>(idx);
}

size_t
MeshNode::channelToIndex(Flit::CHI_CHN_TYPE channel)
{
    return static_cast<size_t>(channel);
}

void
MeshNode::sampleHopCountByChannel(Flit::CHI_CHN_TYPE channel, Counter hops)
{
    switch (channel) {
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
        stats.hop_count_hist_snp.sample(hops);
        return;
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
        stats.hop_count_hist_req.sample(hops);
        return;
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
        stats.hop_count_hist_rsp.sample(hops);
        return;
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
        stats.hop_count_hist_dat.sample(hops);
        return;
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_NUM:
        return;
    }
}

void
MeshNode::sampleE2eLatencyByChannel(Flit::CHI_CHN_TYPE channel, Counter latency)
{
    switch (channel) {
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
        stats.e2e_latency_hist_snp.sample(latency);
        return;
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
        stats.e2e_latency_hist_req.sample(latency);
        return;
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
        stats.e2e_latency_hist_rsp.sample(latency);
        return;
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
        stats.e2e_latency_hist_dat.sample(latency);
        return;
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_NUM:
        return;
    }
}

int
MeshNode::directionToIndex(PortIndex idx)
{
    switch (idx) {
      case PortIndex::East:
        return 0;
      case PortIndex::West:
        return 1;
      case PortIndex::North:
        return 2;
      case PortIndex::South:
        return 3;
      case PortIndex::Local0:
      case PortIndex::Local1:
      case PortIndex::NumPorts:
        return -1;
    }
    return -1;
}

bool
MeshNode::isLocalPort(PortIndex idx)
{
    return idx == PortIndex::Local0 || idx == PortIndex::Local1;
}

const char*
MeshNode::portName(PortIndex idx)
{
    switch (idx) {
      case PortIndex::Local0:
        return "local0";
      case PortIndex::Local1:
        return "local1";
      case PortIndex::East:
        return "east";
      case PortIndex::West:
        return "west";
      case PortIndex::North:
        return "north";
      case PortIndex::South:
        return "south";
      case PortIndex::NumPorts:
        return "invalid";
    }
    return "invalid";
}

const char*
MeshNode::directionName(size_t idx)
{
    switch (idx) {
      case 0:
        return "east";
      case 1:
        return "west";
      case 2:
        return "north";
      case 3:
        return "south";
      default:
        return "invalid";
    }
}

const char*
MeshNode::channelName(Flit::CHI_CHN_TYPE channel)
{
    switch (channel) {
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
        return "SNP";
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
        return "REQ";
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
        return "RSP";
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
        return "DAT";
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_NUM:
        return "invalid";
    }
    return "invalid";
}

} // namespace xsCHI
} // namespace gem5
