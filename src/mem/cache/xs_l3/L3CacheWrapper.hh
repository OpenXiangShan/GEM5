#ifndef __MEM_CACHE_xs_l3_L3_CACHE_WRAPPER_HH__
#define __MEM_CACHE_xs_l3_L3_CACHE_WRAPPER_HH__

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "base/types.hh"
#include "mem/cache/base.hh"
#include "mem/cache/xs_l3/L3CacheSlice.hh"
#include "mem/cache/xs_l3/L3SlicedCacheAccessor.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/L3CacheWrapper.hh"
#include "sim/clocked_object.hh"
#include "sim/port.hh"
#include "sim/system.hh"

namespace gem5
{

/**
 * @brief This L3CacheWrapper acts as a router/dispatcher to a set of L3 cache
 * slices. It is responsible for hashing requests to the correct slice.
 * The memory-side arbitration and aggregation is handled by an internal
 * CoherentXBar, which is configured in the Python scripts.
 *
 *                +----------------------------------------------------------------------+
 *                | L3CacheWrapper                                                       |
 *                |                                                                      |
 *                |   +-------------+           +--------------------------+             |
 *           L2 ----> | cpu_side    |---addr--->|         Router           |             |
 *                |   | (Response)  |           |      (by address)        |             |
 *                |   +-------------+           +--------------------------+             |
 *                |                                          |                           |
 *                |        +---------------------------------v-----------------------+   |
 *                |        |            slice_cpuside_ports (VectorRequest)          |   |
 *                |        +---------------------------------------------------------+   |
 *                |          |               |                            |              |
 *                |   +------v------+ +------v------+             +------v------+        |
 *                |   | L3Slice[0]  | | L3Slice[1]  |    ...      | L3Slice[N-1]|        |
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
 *                       MemBus
 */
class L3CacheWrapper : public ClockedObject
{
  protected:
    //Prefetch related (commented out for now)
    void processSendPrefetchEvent();
    EventFunctionWrapper sendPrefetchEvent;

    bool prefetch_blocked = false;
    PacketPtr outstanding_prefetch = nullptr;

    void scheduleSendPrefetch();
    bool needPrefetch();

  public:
    L3CacheWrapper(const L3CacheWrapperParams &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    void addCacheAccessor(BaseCache* accessor)
    {
        cache_accessors.push_back(accessor);
    }

    void addSliceAccessor(L3CacheSlice* slice)
    {
        slice_accessors.push_back(slice);

        // Add PipeDataWriteStage and DirReadBypass configurations
        slice->setPipeDataWriteStage(pipe_dir_write_stage);
        slice->setDirReadBypass(dirReadBypass);
        slice->setGetSetIdxFunc([this](Addr addr) -> Addr {
            Addr slice_bits = popCount(sliceMask);
            Addr set_idx = (addr >> (block_bits + slice_bits)) & setMask;
            return set_idx;
        });
    }

  protected:
    class CPUSidePort : public ResponsePort
    {
      private:
        L3CacheWrapper &owner;
      public:
        CPUSidePort(const std::string &name, L3CacheWrapper &owner);

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
        L3CacheWrapper &owner;
        const PortID id;
      public:
        SliceCPUSidePort(const std::string& name, L3CacheWrapper &owner, PortID id);
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
    std::vector<L3CacheSlice*> slice_accessors;

  private:
    friend class CPUSidePort;
    friend class SliceCPUSidePort;
    friend class L3SlicedCacheAccessor;

    const Addr sliceMask;
    const Addr setMask;
    const Addr block_bits;
    const uint64_t pipe_dir_write_stage;
    const bool dirReadBypass;
    L3SlicedCacheAccessor sliced_cache_accessor;
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

#endif // __MEM_CACHE_xs_l3_L3_CACHE_WRAPPER_HH__
