#ifndef __MEM_CACHE_XS_L2_L2_CACHE_WRAPPER_HH__
#define __MEM_CACHE_XS_L2_L2_CACHE_WRAPPER_HH__

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "base/types.hh"
#include "mem/cache/base.hh"
#include "mem/cache/xs_l2/L2CacheSlice.hh"
#include "mem/cache/xs_l2/SlicedCacheAccessor.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/L2CacheWrapper.hh"
#include "sim/clocked_object.hh"
#include "sim/port.hh"
#include "sim/system.hh"

namespace gem5
{

/**
 * @brief This L2CacheWrapper acts as a router/dispatcher to a set of L2 cache
 * slices. It is responsible for hashing requests to the correct slice.
 * The memory-side arbitration and aggregation is handled by an internal
 * CoherentXBar, which is configured in the Python scripts.
 *
 *                +----------------------------------------------------------------------+
 *                | L2CacheWrapper                                                       |
 *                |                                                                      |
 *                |   +-------------+           +--------------------------+             |
 *           L1 ----> | cpu_side    |---addr--->|         Router           |             |
 *                |   | (Response)  |           |      (by address)        |             |
 *                |   +-------------+           +--------------------------+             |
 *                |                                          |                           |
 *                |        +---------------------------------v-----------------------+   |
 *                |        |            slice_cpuside_ports (VectorRequest)          |   |
 *                |        +---------------------------------------------------------+   |
 *                |          |               |                            |              |
 *                |   +------v------+ +------v------+             +------v------+        |
 *                |   | L2Slice[0]  | | L2Slice[1]  |    ...      | L2Slice[N-1]|        |
 *                |   | (contains   | | (contains   |             | (contains   |        |
 *                |   | inner cache)| | inner cache)|             | inner cache)|        |
 *                |   |-------------| |-------------|             |-------------|        |
 *                |   | mem_side    | | mem_side    |             | mem_side    |        |
 *                |   +------^------+ +------^------+             +------^------+        |
 *                |          |               |                            |              |
 *                |        +-----------------^----------------------------------------+  |
 *                |        |        (Connected to internal CoherentXBar)             |   |
 *                +--------|-------------------------------------------------------------+
 *                         |
 *                         v
 *                    L3 / MemBus
 */
class L2CacheWrapper : public ClockedObject
{
  protected:
    void processSendPrefetchEvent();
    EventFunctionWrapper sendPrefetchEvent;

    bool prefetch_blocked = false;
    PacketPtr outstanding_prefetch = nullptr;

    void scheduleSendPrefetch();
    bool needPrefetch();

  public:
    L2CacheWrapper(const L2CacheWrapperParams &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    void addCacheAccessor(BaseCache* accessor)
    {
        cache_accessors.push_back(accessor);
    }

    void addSliceAccessor(L2CacheSlice* slice)
    {
        slice_accessors.push_back(slice);
        slice->setPipeDirWriteStage(pipe_dir_write_stage);
        slice->setDirReadBypass(dirReadBypass);
        slice->setGetSetIdxFunc([this](Addr addr) -> Addr {
            Addr slice_bits = popCount(sliceMask);
            Addr set_idx = (addr >> (block_bits + slice_bits)) & setMask;
            return set_idx;
        });
        // Set the function to calculate DataSram bank index for an address
        slice->setGetDataBankIdxFunc([this](Addr addr) -> Addr {
            Addr slice_bits = popCount(sliceMask);
            Addr set_idx = (addr >> (block_bits + slice_bits)) & setMask;
            // Extract lower bits of set index based on number of banks
            return set_idx & (dataSramBanks - 1);
        });
        // Set the function to calculate DirSram bank index for an address
        slice->setGetDirBankIdxFunc([this](Addr addr) -> Addr {
            Addr slice_bits = popCount(sliceMask);
            Addr set_idx = (addr >> (block_bits + slice_bits)) & setMask;
            // Extract lower bits of set index based on number of banks
            return set_idx & (dirSramBanks - 1);
        });
    }

  protected:
    class CPUSidePort : public ResponsePort
    {
      private:
        L2CacheWrapper &owner;
      public:
        CPUSidePort(const std::string &name, L2CacheWrapper &owner);

      protected:
        bool recvTimingReq(PacketPtr pkt) override;
        bool recvTimingSnoopResp(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvRespRetry() override;
        AddrRangeList getAddrRanges() const override;
    };

    class SliceCPUSidePort : public RequestPort
    {
      private:
        L2CacheWrapper &owner;
        const PortID id;
      public:
        SliceCPUSidePort(const std::string& name, L2CacheWrapper &owner, PortID id);
      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
        void recvRangeChange() override;
        void recvTimingSnoopReq(PacketPtr pkt) override;
        bool isSnooping() const override { return true;}
    };

    CPUSidePort cpu_side_port;
    std::vector<SliceCPUSidePort> slice_cpuside_ports;
    std::vector<CacheAccessor*> cache_accessors;
    std::vector<L2CacheSlice*> slice_accessors;

  private:
    friend class CPUSidePort;
    friend class SliceCPUSidePort;
    friend class SlicedCacheAccessor;

    const Addr sliceMask;
    const Addr setMask;
    const Addr block_bits;
    const uint64_t pipe_dir_write_stage;
    const bool dirReadBypass;
    const uint64_t dataSramBanks;
    const uint64_t dirSramBanks;
    SlicedCacheAccessor sliced_cache_accessor;
    prefetch::Base *prefetcher;

    System* system;

    inline PortID getSliceId(Addr addr) const {
        return ((addr >> block_bits) & sliceMask);
    }

    // Return the last tick of next cycle
    Tick nextCycleLastTick() {
      return clockEdge(Cycles(2)) - 1;
    }

    bool upper_resp_blocked = false;
    bool upper_req_blocked = false;
    std::unordered_set<PortID> resp_waiting_slice;
};

} // namespace gem5

#endif // __MEM_CACHE_XS_L2_L2_CACHE_WRAPPER_HH__
