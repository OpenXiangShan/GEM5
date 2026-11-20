#ifndef __CPU_O3_MATRIX_CPU_STATE_HH__
#define __CPU_O3_MATRIX_CPU_STATE_HH__

#include <array>
#include <cassert>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "arch/riscv/matrix.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "matrix/MatrixTypes.hh"
#include "mem/port.hh"

namespace gem5::matrix
{
class DetailedCuteBackend;
}

namespace gem5::o3
{

class CPU;

class MatrixMemPort : public RequestPort,
                      public matrix::MatrixTimingMemoryAdapter
{
  public:
    MatrixMemPort(const std::string &name, CPU *cpu);

    bool connected() const override { return isConnected(); }
    bool sendTimingRequest(const Request &request) override;
    bool hasPendingRequests() const
    {
        return !sourceStarted.empty() || !blockedPackets.empty();
    }

  protected:
    bool recvTimingResp(PacketPtr pkt) override;
    void recvReqRetry() override;

  private:
    struct PortStats : public statistics::Group
    {
        explicit PortStats(statistics::Group *parent);
        statistics::Scalar sourcesAccepted;
        statistics::Scalar sourcesCompleted;
        statistics::Scalar sourceTicks;
        statistics::Scalar sourceTicksMax;
        statistics::Scalar sourcesMax;
    } stats;
    // One entry per live backend source until its data or write response.
    std::unordered_map<uint32_t, Tick> sourceStarted;
    void noteSourceCompletion(uint32_t source);
    struct SenderState : public Packet::SenderState
    {
        explicit SenderState(const Request &request_)
            : request(request_)
        {}

        Request request;
    };

    PacketPtr buildPacket(const Request &request);
    void trySendBlocked();

    CPU *cpu = nullptr;
    std::deque<PacketPtr> blockedPackets;
    bool waitingForRetry = false;
};

/** Optional CPU-owned asynchronous state, separate from speculative MLS/AMU. */
struct MatrixCpuState
{
    MatrixCpuState();
    ~MatrixCpuState();

    struct ObservedPacket
    {
        Addr paddr;
        uint32_t packetSize;
        std::array<uint8_t, 64> data;
        uint64_t byteMask;
    };
    struct Owner
    {
        ThreadID tid;
        matrix::CuteRequest request;
        std::optional<matrix::CuteCompletion> completion = {};
        std::vector<ObservedPacket> packets = {};
        bool refReleaseApplied = false;
    };
    std::unique_ptr<MatrixMemPort> memoryPort;
    std::unique_ptr<matrix::DetailedCuteBackend> backend;
    std::array<InstSeqNum, RiscvISA::MatrixTokenCount> resetSeqs{};
    std::unordered_map<InstSeqNum, Owner> owners;
    std::deque<InstSeqNum> checkOrder;
    std::map<std::pair<ThreadID, uint8_t>, std::vector<uint8_t>> verified;
    uint64_t committed = 0, submitted = 0, completed = 0, checked = 0;
    uint64_t mmaChecked = 0, loadChecked = 0, zeroChecked = 0;
    uint64_t storeChecked = 0, releaseChecked = 0, syncChecked = 0;
    uint64_t memorySent = 0, memoryCompleted = 0;
    uint64_t staleReleases = 0, outOfOrder = 0;

};

} // namespace gem5::o3

#endif // __CPU_O3_MATRIX_CPU_STATE_HH__
