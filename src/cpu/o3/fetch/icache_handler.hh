#ifndef __CPU_O3_FETCH_ICACHE_HANDLER_HH__
#define __CPU_O3_FETCH_ICACHE_HANDLER_HH__

#include <functional>
#include <map>
#include <vector>

#include "arch/generic/mmu.hh"
#include "cpu/o3/limits.hh"
#include "cpu/translation.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "sim/eventq.hh"

namespace gem5 {
namespace o3 {

// Forward declarations
class CPU;
class Fetch;

// Cache request status for individual cache access requests
// Defined here to avoid circular dependency with fetch.hh
enum CacheRequestStatus
{
    CacheIdle,              // No active request
    TlbWait,               // Waiting for TLB translation completion
    CacheWaitResponse,     // Waiting for cache data return
    CacheWaitRetry,        // Waiting for cache retry opportunity
    AccessComplete,        // Access completed, data available
    AccessFailed,          // Access failed (invalid address etc.)
    Cancelled,             // Request cancelled (squash etc.)
    NumCacheRequestStatus
};

// 1. Define unified callback structure for cache operations
struct FetchCallbackData
{
    PacketPtr pkt = nullptr; // Packet containing fetched data
    Fault fault = NoFault;
    RequestPtr req = nullptr; // Original request
    unsigned ftqIndex = 0;    // FTQ index identifying request source
    uint8_t* mergedData = nullptr; // Merged data from multiple cache lines
    unsigned dataSize = 0;    // Size of merged data
};

// 2. Define callback function type using CacheRequestStatus directly
using FetchCallback = std::function<void(CacheRequestStatus status, const FetchCallbackData& data)>;

// 3. Define ICacheHandler class
class ICacheHandler
{
public:
    // Forward declarations for internal classes
    class IcachePort;
    class FetchTranslation;
    class FinishTranslationEvent;

    ICacheHandler(CPU* cpu);
    ~ICacheHandler();

    // Public interface
    void fetch(Addr vaddr, Addr pc, unsigned size, ThreadID tid,
               unsigned ftqIndex, FetchCallback callback);

    void recvReqRetry();
    void cancelRequests(ThreadID tid); // Used for Squash

    // Get the instruction port for external access
    RequestPort& getInstPort() { return icachePort; }

    // Status query interface for Fetch class delegation
    CacheRequestStatus getOverallCacheStatus(ThreadID tid) const;
    bool allActiveFTQCompleted(ThreadID tid) const;
    bool hasPendingCacheRequests(ThreadID tid) const;

    /**
     * IcachePort class for instruction fetch.
     * Moved from Fetch class to ICacheHandler for encapsulation.
     */
    class IcachePort : public RequestPort
    {
      protected:
        /** Pointer to ICacheHandler. */
        ICacheHandler *handler;

      public:
        /** Default constructor. */
        IcachePort(ICacheHandler *_handler, CPU *_cpu);

      protected:
        /** Timing version of receive. Handles cache completion. */
        virtual bool recvTimingResp(PacketPtr pkt);

        /** Handles doing a retry of a failed fetch. */
        virtual void recvReqRetry();
    };

    /**
     * FetchTranslation class for handling MMU translation completion.
     * Moved from Fetch class to ICacheHandler for encapsulation.
     */
    class FetchTranslation : public BaseMMU::Translation
    {
      protected:
        ICacheHandler *handler;
        unsigned ftqIndex;

      public:
        FetchTranslation(ICacheHandler *_handler, unsigned _ftqIndex = 0)
            : handler(_handler), ftqIndex(_ftqIndex) {}

        void markDelayed() {}

        void
        finish(const Fault &fault, const RequestPtr &req,
            gem5::ThreadContext *tc, BaseMMU::Mode mode)
        {
            assert(mode == BaseMMU::Execute);
            handler->finishTranslation(fault, req, ftqIndex);
            delete this;
        }
    };

    /**
     * Event to delay delivery of a fetch translation result in case of
     * a fault and the nop to carry the fault cannot be generated immediately.
     * Moved from Fetch class to ICacheHandler for encapsulation.
     */
    class FinishTranslationEvent : public Event
    {
      private:
        ICacheHandler *handler;
        Fault fault;
        RequestPtr req;
        unsigned ftqIndex;

      public:
        FinishTranslationEvent(ICacheHandler *_handler)
            : handler(_handler), req(nullptr), ftqIndex(0)
        {}

        void setFault(Fault _fault) { fault = _fault; }
        void setReq(const RequestPtr &_req) { req = _req; }
        void setFtqIndex(unsigned _ftqIndex) { ftqIndex = _ftqIndex; }
        RequestPtr getReq() { return req; }

        /** Process the delayed finish translation */
        void process();

        const char *
        description() const
        {
            return "ICacheHandler FinishTranslation";
        }
    };

private:
    CPU* cpu;
    IcachePort icachePort;
    FinishTranslationEvent finishTranslationEvent;

    // Internal state for tracking pending requests and their callbacks
    struct PendingRequest
    {
        FetchCallback callback;
        ThreadID tid;
        Addr vaddr;
        Addr pc;
        unsigned size;
        unsigned ftqIndex;
        // Additional tracking information may be needed
    };

    // Use (tid, ftqIndex) as combined key for pending requests
    std::map<std::pair<ThreadID, unsigned>, PendingRequest> pendingRequests;

    // Internal processing functions
    void finishTranslation(const Fault &fault, const RequestPtr &req, unsigned ftqIndex);
    void processCacheCompletion(PacketPtr pkt);

    // Variables for retry handling
    std::vector<PacketPtr> retryPkt;
    bool cacheBlocked;
    ThreadID retryTid;

    // Internal helper functions
    bool validateTranslationRequest(ThreadID tid, const RequestPtr &mem_req, unsigned ftqIndex);
    void handleSuccessfulTranslation(ThreadID tid, const RequestPtr &mem_req, Addr fetchPC, unsigned ftqIndex);
    void handleTranslationFault(ThreadID tid, const RequestPtr &mem_req, const Fault &fault, unsigned ftqIndex);
    bool handleMultiCacheLineFetch(Addr vaddr, ThreadID tid, Addr pc, unsigned ftqIndex);
    bool processMultiCacheLineCompletion(ThreadID tid, PacketPtr pkt, unsigned ftqIndex);
    void handleRetryPkt(ThreadID tid, PacketPtr pkt);
    unsigned determineFTQIndex(ThreadID tid, PacketPtr pkt);

    // Cache configuration
    unsigned cacheBlkSize;
    unsigned fetchBufferSize;

    // Cache request management structures
    struct CacheRequest
    {
        std::vector<PacketPtr> packets;
        std::vector<RequestPtr> requests;
        std::vector<CacheRequestStatus> requestStatus;
        Addr baseAddr;
        unsigned totalSize;
        unsigned completedPackets;

        CacheRequest() : baseAddr(0), totalSize(0), completedPackets(0) {}

        bool allCompleted() const {
            return completedPackets >= packets.size() && packets.size() > 0;
        }

        CacheRequestStatus getOverallStatus() const {
            if (requestStatus.empty()) return CacheIdle;

            // Priority: Failed > Retry > TlbWait > CacheWaitResponse > Complete > Idle
            bool hasRetry = false;
            bool hasTlbWait = false;
            bool hasCacheWait = false;
            bool hasComplete = false;

            for (const auto& status : requestStatus) {
                switch (status) {
                    case AccessFailed:
                        return AccessFailed;  // Highest priority
                    case CacheWaitRetry:
                        hasRetry = true;
                        break;
                    case TlbWait:
                        hasTlbWait = true;
                        break;
                    case CacheWaitResponse:
                        hasCacheWait = true;
                        break;
                    case AccessComplete:
                        hasComplete = true;
                        break;
                    default:
                        break;
                }
            }

            // Return status by priority
            if (hasRetry) return CacheWaitRetry;
            if (hasTlbWait) return TlbWait;
            if (hasCacheWait) return CacheWaitResponse;
            if (hasComplete) return AccessComplete;

            return CacheIdle;
        }

        void reset() {
            packets.clear();
            requests.clear();
            requestStatus.clear();
            baseAddr = 0;
            totalSize = 0;
            completedPackets = 0;
        }

        void addRequest(RequestPtr req) {
            requests.push_back(req);
            packets.push_back(nullptr);
            requestStatus.push_back(TlbWait); // Initially waiting for translation
        }

        bool markCompletedAndStorePacket(PacketPtr pkt) {
            for (size_t i = 0; i < requests.size(); ++i) {
                if (requests[i] == pkt->req) {
                    if (packets[i] == nullptr) {
                        packets[i] = pkt;
                        completedPackets++;
                        requestStatus[i] = AccessComplete; // Mark as completed
                        return true;
                    }
                }
            }
            return false;
        }
    };

    // Cache request tracking for each (ThreadID, ftqIndex) pair
    std::map<std::pair<ThreadID, unsigned>, CacheRequest> cacheRequests;

    // Helper methods for cache request management
    CacheRequest& getCacheReq(ThreadID tid, unsigned ftqIndex);
    void resetCacheReq(ThreadID tid, unsigned ftqIndex);
    void updateRequestStatus(ThreadID tid, unsigned ftqIndex, const RequestPtr& req, CacheRequestStatus status);
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_FETCH_ICACHE_HANDLER_HH__
