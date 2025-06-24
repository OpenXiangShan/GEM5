#include "mem/cache/way_prediction_policies/base_wpu.hh"

#include <cstdint>

#include "base/intmath.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"

namespace gem5
{

namespace way_prediction_policy
{

BaseWpu::BaseWpu(const BaseWpuParams &params)
    : SimObject(params), use_virtual(params.use_virtual),
      miss_train(params.miss_train), assoc(params.assoc),
      cycle_reduction(params.cycle_reduction)
{
    // Calculate the shift and mask values based on the associativity
    uint64_t blk_offset = floorLog2(params.blk_size);
    uint64_t set_num = params.size / params.blk_size / params.assoc;
    // Maximum PA & VA bits
    uint64_t addr_bits = params.max_addr_bits;
    set_shift = blk_offset;
    set_mask = set_num - 1;
    tag_shift = blk_offset + floorLog2(set_num);
    tag_mask = (1 << (addr_bits - tag_shift)) - 1;
}

Addr
BaseWpu::getWpuAddr(const PacketPtr pkt) const
{
    if (use_virtual && pkt->req->hasVaddr()) {
        return pkt->req->getVaddr();
    } else {
        return pkt->req->getPaddr();
    }
}

Addr
BaseWpu::getWpuSet(const Addr addr) const
{
    return (addr >> set_shift) & set_mask;
}

Addr
BaseWpu::getWpuTag(const Addr addr) const
{
    return (addr >> tag_shift) & tag_mask;
}

} // namespace way_prediction_policy
} // namespace gem5
