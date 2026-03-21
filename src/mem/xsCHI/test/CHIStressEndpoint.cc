#include "mem/xsCHI/test/CHIStressEndpoint.hh"

#include <algorithm>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/CHIMeshNode.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace xsCHI
{

CHIStressEndpoint::StressStats::StressStats(CHIStressEndpoint *parent)
    : statistics::Group(parent, "stress"),
      ADD_STAT(tx_attempts, statistics::units::Count::get(),
               "Total send attempts"),
      ADD_STAT(tx_sent, statistics::units::Count::get(),
               "Total flits sent successfully"),
      ADD_STAT(tx_send_fail, statistics::units::Count::get(),
               "Total send attempts that failed"),
      ADD_STAT(tx_wakeup_events, statistics::units::Count::get(),
               "Credit-unblock wakeups observed by sender"),
      ADD_STAT(rx_callbacks, statistics::units::Count::get(),
               "Total receive callback invocations"),
      ADD_STAT(rx_accepted, statistics::units::Count::get(),
               "Total flits accepted by receiver callback"),
      ADD_STAT(rx_blocked_periodic, statistics::units::Count::get(),
               "Total flits periodically blocked by receiver callback"),
      ADD_STAT(rx_target_mismatch, statistics::units::Count::get(),
               "Total accepted flits whose target does not match endpoint")
{
    using namespace statistics;

    tx_attempts.flags(nozero);
    tx_sent.flags(nozero);
    tx_send_fail.flags(nozero);
    tx_wakeup_events.flags(nozero);
    rx_callbacks.flags(nozero);
    rx_accepted.flags(nozero);
    rx_blocked_periodic.flags(nozero);
    rx_target_mismatch.flags(nozero);
}

CHIStressEndpoint::CHIStressEndpoint(const Params &p)
    : ClockedObject(p),
      networkPort(p.networkPort),
      enableSender(p.enable_sender),
      totalFlits(p.total_flits),
      injectPerCycle(std::max<uint32_t>(1, p.inject_per_cycle)),
      srcId(p.src_id),
      tgtId(p.tgt_id),
      baseAddr(p.base_addr),
      addrStride(std::max<uint64_t>(1, p.addr_stride)),
      payloadSize(std::max<uint32_t>(1, p.payload_size)),
      receiverBlockPeriod(p.receiver_block_period),
      receiverBlockCycles(p.receiver_block_cycles),
      generatedFlits(0),
      nextAddrSeq(0),
      tickEvent([this] { onTick(); }, name()),
      stats(this)
{
    panic_if(networkPort == nullptr,
             "CHIStressEndpoint %s requires a valid networkPort", name());

    networkPort->setReceiveCallback(
        [this](FlitPtr &flit) { return handleReceive(flit); });
    networkPort->setCreditUnblockCallback(
        [this](Flit::CHI_CHN_TYPE channel) { handleCreditUnblock(channel); });
    networkPort->setOwner(this);
}

void
CHIStressEndpoint::startup()
{
    ClockedObject::startup();
    if (enableSender && hasPendingToSend()) {
        scheduleSenderRetry();
    }
}

void
CHIStressEndpoint::onTick()
{
    if (!enableSender || !hasPendingToSend()) {
        return;
    }

    for (uint32_t i = 0; i < injectPerCycle && hasPendingToSend(); ++i) {
        FlitPtr flit = buildReqFlit();
        stats.tx_attempts++;
        if (!networkPort->send(flit)) {
            stats.tx_send_fail++;
            scheduleSenderRetry();
            return;
        }

        stats.tx_sent++;
        generatedFlits++;
    }

    if (hasPendingToSend()) {
        scheduleSenderRetry();
    }
}

bool
CHIStressEndpoint::handleReceive(FlitPtr &flit)
{
    stats.rx_callbacks++;
    if (receiverShouldBlock()) {
        stats.rx_blocked_periodic++;
        return false;
    }

    stats.rx_accepted++;
    if (flit->getTgtId() != srcId) {
        stats.rx_target_mismatch++;
    }
    return true;
}

void
CHIStressEndpoint::handleCreditUnblock(Flit::CHI_CHN_TYPE channel)
{
    if (channel != Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ) {
        return;
    }

    if (enableSender && hasPendingToSend()) {
        stats.tx_wakeup_events++;
        scheduleSenderRetry();
    }
}

bool
CHIStressEndpoint::receiverShouldBlock() const
{
    if (receiverBlockPeriod == 0 || receiverBlockCycles == 0) {
        return false;
    }

    const uint64_t cycle = curCycle();
    const uint64_t phase = cycle % receiverBlockPeriod;
    const uint64_t blockSpan = std::min<uint64_t>(receiverBlockCycles,
                                                  receiverBlockPeriod);
    return phase < blockSpan;
}

bool
CHIStressEndpoint::hasPendingToSend() const
{
    return generatedFlits < totalFlits;
}

void
CHIStressEndpoint::scheduleTickNextCycle()
{
    if (!tickEvent.scheduled()) {
        schedule(tickEvent, curTick() + clockPeriod());
    }
}

void
CHIStressEndpoint::scheduleSenderRetry()
{
    if (!enableSender || !hasPendingToSend()) {
        return;
    }
    if (networkPort->isChannelBlockedByCredit(
            Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)) {
        return;
    }
    scheduleTickNextCycle();
}

FlitPtr
CHIStressEndpoint::buildReqFlit()
{
    const uint64_t addr = baseAddr + nextAddrSeq * addrStride;
    nextAddrSeq++;

    FlitPtr flit = std::make_unique<Flit>(
        CHI_OP_TYPE::CHI_REQ_READNOSNP, addr, payloadSize);
    flit->setOpcode(CHI_OP_TYPE::CHI_REQ_READNOSNP);
    flit->setSrcId(srcId);
    flit->setTgtId(tgtId);
    flit->setTxnId(static_cast<uint32_t>(generatedFlits & 0xffffffff));
    flit->setReturnNid(srcId);
    flit->setReturnTxnid(static_cast<uint32_t>(generatedFlits & 0xffffffff));
    return flit;
}

} // namespace xsCHI
} // namespace gem5
