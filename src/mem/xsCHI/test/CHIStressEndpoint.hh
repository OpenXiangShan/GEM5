#pragma once

#include <cstdint>

#include "mem/xsCHI/base/CHIPort.hh"
#include "mem/xsCHI/base/flit.hh"
#include "params/CHIStressEndpoint.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/stats.hh"

namespace gem5
{
namespace xsCHI
{

class CHIStressEndpoint : public ClockedObject
{
  public:
    using Params = CHIStressEndpointParams;
    explicit CHIStressEndpoint(const Params &p);

    void startup() override;
    CHIPort *getNetworkPort() { return networkPort; }

  private:
    CHIPort *networkPort;

    const bool enableSender;
    const uint64_t totalFlits;
    const uint32_t injectPerCycle;
    const uint32_t srcId;
    const uint32_t tgtId;
    const uint64_t baseAddr;
    const uint64_t addrStride;
    const uint32_t payloadSize;
    const uint32_t receiverBlockPeriod;
    const uint32_t receiverBlockCycles;

    uint64_t generatedFlits;
    uint64_t nextAddrSeq;

    EventFunctionWrapper tickEvent;

    struct StressStats : public statistics::Group
    {
        explicit StressStats(CHIStressEndpoint *parent);

        statistics::Scalar tx_attempts;
        statistics::Scalar tx_sent;
        statistics::Scalar tx_send_fail;
        statistics::Scalar tx_wakeup_events;

        statistics::Scalar rx_callbacks;
        statistics::Scalar rx_accepted;
        statistics::Scalar rx_blocked_periodic;
        statistics::Scalar rx_target_mismatch;
    } stats;

    void onTick();
    bool handleReceive(FlitPtr &flit);
    void handleCreditUnblock(Flit::CHI_CHN_TYPE channel);

    bool receiverShouldBlock() const;
    bool hasPendingToSend() const;
    void scheduleTickNextCycle();
    void scheduleSenderRetry();
    FlitPtr buildReqFlit();
};

} // namespace xsCHI
} // namespace gem5
