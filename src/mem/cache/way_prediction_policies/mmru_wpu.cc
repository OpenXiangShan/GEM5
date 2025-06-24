#include "mem/cache/way_prediction_policies/mmru_wpu.hh"

namespace gem5
{

namespace way_prediction_policy
{

MMRUWpu::MMRUWpu(const MMRUWpuParams &params)
    : BaseWpu(params), n_tags(params.n_tags)
{
    mmru_ways.resize(set_mask + 1, std::vector<uint8_t>(n_tags, 0));
}

uint8_t
MMRUWpu::getPredictedWay(const PacketPtr pkt)
{
    Addr addr = getWpuAddr(pkt);
    Addr set = getWpuSet(addr);
    Addr tag = getWpuTag(addr);
    assert(set < mmru_ways.size());
    return mmru_ways[set][tag % n_tags];
}

void
MMRUWpu::update(const PacketPtr pkt, const uint8_t way)
{
    Addr addr = getWpuAddr(pkt);
    Addr set = getWpuSet(addr);
    Addr tag = getWpuTag(addr);
    assert(set < mmru_ways.size());
    mmru_ways[set][tag % n_tags] = way;
}

} // namespace way_prediction_policy
} // namespace gem5
