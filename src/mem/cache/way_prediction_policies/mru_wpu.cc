#include "mem/cache/way_prediction_policies/mru_wpu.hh"

namespace gem5
{

namespace way_prediction_policy
{

MRUWpu::MRUWpu(const MRUWpuParams &params)
    : BaseWpu(params)
{
    mru_ways.resize(set_mask + 1, 0);
}

uint8_t
MRUWpu::getPredictedWay(const PacketPtr pkt)
{
    Addr addr = getWpuAddr(pkt);
    Addr set = getWpuSet(addr);
    assert(set < mru_ways.size());
    return mru_ways[set];
}

void
MRUWpu::update(const PacketPtr pkt, const uint8_t way)
{
    Addr addr = getWpuAddr(pkt);
    Addr set = getWpuSet(addr);
    assert(set < mru_ways.size());
    mru_ways[set] = way;
}

} // namespace way_prediction_policy
} // namespace gem5