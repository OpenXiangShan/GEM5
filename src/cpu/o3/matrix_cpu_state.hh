#ifndef __CPU_O3_MATRIX_CPU_STATE_HH__
#define __CPU_O3_MATRIX_CPU_STATE_HH__

#include <array>
#include <cassert>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "matrix/MatrixTimingMemoryAdapter.hh"
#include "matrix/TaskController.hh"
#include "mem/port.hh"

namespace gem5::o3
{

class CPU;

class MatrixMemPort : public RequestPort,
                      public matrix::MatrixTimingMemoryAdapter
{
  public:
    MatrixMemPort(const std::string &name, CPU *cpu, bool acquire);

    bool connected() const override { return isConnected(); }
    bool sendTimingRequest(const Request &request) override;

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
    // One entry per live backend source; preparation is not completion.
    std::unordered_map<uint32_t, Tick> sourceStarted;
    void noteSourceCompletion(uint32_t source);
    struct SenderState : public Packet::SenderState
    {
        SenderState(const Request &request, uint64_t id, bool continuation_)
            : transactionId(id), byteMask(request.byteMask),
              paddr(request.paddr), sourceId(request.sourceId),
              packetSize(request.packetSize), contextId(request.contextId),
              matrixKey(request.matrixKey), isStore(request.isStore),
              continuation(continuation_)
        {
            if (isStore && !continuation) {
                storeData = request.data;
            }
        }

        Request originalRequest() const
        {
            assert(!continuation);
            Request request;
            request.isStore = isStore;
            request.paddr = paddr;
            request.packetSize = packetSize;
            request.sourceId = sourceId;
            request.contextId = contextId;
            request.byteMask = byteMask;
            request.matrixKey = matrixKey;
            if (isStore) {
                request.data = storeData;
            }
            return request;
        }

        const uint64_t transactionId;
        const uint64_t byteMask;
        const Addr paddr;
        const uint32_t sourceId;
        const uint32_t packetSize;
        const ContextID contextId;
        const uint8_t matrixKey;
        const bool isStore;
        const bool continuation;
        std::array<uint8_t, 64> storeData;
    };

    PacketPtr buildPacket(const Request &request, uint64_t transaction_id,
                          bool continuation = false);
    void sendOrBlock(PacketPtr pkt);
    void trySendBlocked();

    CPU *cpu = nullptr;
    const bool rmwAcquire;
    std::deque<PacketPtr> blockedPackets;
    bool waitingForRetry = false;
    uint64_t nextTransactionId = 0;
};

/** Optional CPU-owned asynchronous state, separate from speculative MLS/AMU. */
struct MatrixCpuState
{
    struct Owner
    {
        ThreadID tid;
        matrix::CuteRequest request;
    };
    std::unique_ptr<MatrixMemPort> memoryPort;
    std::unique_ptr<matrix::MatrixBackend> backend;
    std::vector<std::vector<RegVal>> shadowTokens;
    std::vector<std::vector<InstSeqNum>> resetSeqs;
    std::unordered_map<InstSeqNum, Owner> owners;
};

} // namespace gem5::o3

#endif // __CPU_O3_MATRIX_CPU_STATE_HH__
