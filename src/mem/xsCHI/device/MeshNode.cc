#include "mem/xsCHI/device/MeshNode.hh"

#include <cassert>

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
// Keep channel service order aligned with CHIPort receiver order so the
// behavior is predictable end-to-end.
constexpr std::array<Flit::CHI_CHN_TYPE, 4> kChannelPriority = {
    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP,
    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP,
    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA,
    Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ,
};
}

MeshNode::MeshNode(const Params &p)
    : ClockedObject(p),
      ports{p.port_local0, p.port_local1, p.port_east, p.port_west,
            p.port_north, p.port_south},
      nodeX(p.node_x),
      nodeY(p.node_y),
      voqDepth(p.voq_depth == 0 ? 1 : p.voq_depth),
      outVoq(),
      rrCursor(),
      sendEvent([this] { onSendEvent(); }, name())
{
    registerCallbacks();
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

    panic_if(!isEgressUsable(egress),
             "MeshNode %s routes flit(op=%s, tgt=%u) to unusable egress %s",
             name(),
             CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()).c_str(),
             flit->getTgtId(), portName(egress));

    const size_t egressIdx = portToIndex(egress);
    const size_t ingressIdx = portToIndex(ingress);
    const size_t channelIdx = channelToIndex(channel);

    // Backpressure point: when VOQ is full, keep flit at upstream CHIPort.
    if (shouldBackpressureImpl(getQueueDepth(egress, channel), voqDepth)) {
        DPRINTF(CHIMeshNode,
                "%s ingress=%s egress=%s channel=%d VOQ full depth=%u op=%s "
                "tgt=%u txn=%llu\n",
                name(), portName(ingress), portName(egress),
                static_cast<int>(channel), static_cast<unsigned>(voqDepth),
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()).c_str(),
                flit->getTgtId(),
                static_cast<unsigned long long>(flit->getTxnId()));
        return false;
    }

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
    bool sentAny = false;
    // Try every output each cycle; each output has independent arbitration.
    for (size_t i = 0; i < NumPorts; ++i) {
        sentAny |= trySendForOutput(static_cast<PortIndex>(i));
    }

    if (hasPendingFlits()) {
        schedule(sendEvent, curTick() + clockPeriod());
    }

    DPRINTF(CHIMeshNode, "%s scheduler tick sentAny=%d pending=%d\n", name(),
            sentAny ? 1 : 0, hasPendingFlits() ? 1 : 0);
}

bool
MeshNode::trySendForOutput(PortIndex egress)
{
    if (!isEgressUsable(egress)) {
        return false;
    }

    bool sentAny = false;
    for (const auto channel : kChannelPriority) {
        sentAny |= trySendForOutputAndChannel(egress, channel);
    }
    return sentAny;
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

        if (egressPort->send(head)) {
            DPRINTF(CHIMeshNode,
                    "%s send egress=%s from ingress=%s channel=%d op=%s "
                    "tgt=%u txn=%llu\n",
                    name(), portName(egress),
                    portName(static_cast<PortIndex>(srcIdx)),
                    static_cast<int>(channel),
                    CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op).c_str(), tgt,
                    static_cast<unsigned long long>(txn));
            q.pop();
            cursor = (srcIdx + 1) % NumPorts;
            return true;
        }

        DPRINTF(CHIMeshNode,
                "%s blocked egress=%s channel=%d ingress=%s op=%s tgt=%u\n",
                name(), portName(egress), static_cast<int>(channel),
                portName(static_cast<PortIndex>(srcIdx)),
                CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op).c_str(), tgt);
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
        schedule(sendEvent, curTick() + clockPeriod());
    }
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

} // namespace xsCHI
} // namespace gem5
