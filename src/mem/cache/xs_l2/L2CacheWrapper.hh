#ifndef __MEM_CACHE_XS_L2_L2_CACHE_WRAPPER_HH__
#define __MEM_CACHE_XS_L2_L2_CACHE_WRAPPER_HH__

#include <unordered_set>
#include <vector>

#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/L2CacheWrapper.hh"
#include "sim/clocked_object.hh"
#include "sim/port.hh"

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
  public:
    L2CacheWrapper(const L2CacheWrapperParams &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

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

  private:
    friend class CPUSidePort;
    friend class SliceCPUSidePort;

    const uint32_t sliceMask;
    const uint32_t block_bits;

    inline PortID getSliceId(Addr addr) const {
        return ((addr >> block_bits) & sliceMask);
    }

    bool upper_resp_blocked = false;
    std::unordered_set<PortID> resp_waiting_slice;
};

} // namespace gem5

#endif // __MEM_CACHE_XS_L2_L2_CACHE_WRAPPER_HH__
