#include "mem/cache/way_prediction_policies/index_mru_wpu.hh"

namespace gem5
{
namespace way_prediction_policy
{

IndexMRUWpu::IndexMRUWpu(const IndexMRUWpuParams &p)
    : BaseWpu(p),
      indexWayPreTable(p.way_entries, p.way_entries, p.way_indexing_policy,
      p.way_replacement_policy, waypreEntry())
{
}

uint8_t
IndexMRUWpu::getPredictedWay(const PacketPtr pkt)
{
    Addr addr = getWpuAddr(pkt);
    Addr index = getWpuSet(addr);
    Addr tag = getWpuTag(addr);
    waypreEntry *entry = indexWayPreTable.findEntry(index, true);
    if (entry) {
        indexWayPreTable.accessEntry(entry);
        return entry->way;
    } else {
        return std::numeric_limits<uint8_t>::max();
    }
}

void
IndexMRUWpu::update(const PacketPtr pkt, const uint8_t way)
{
    Addr addr = getWpuAddr(pkt);
    Addr index = getWpuSet(addr);
    Addr tag = getWpuTag(addr);
    waypreEntry *entry = indexWayPreTable.findEntry(index, true);
    if (entry) {
        indexWayPreTable.accessEntry(entry);
        entry->way = way;
    } else {
        entry = indexWayPreTable.findVictim(0);
        entry->_setSecure(true);
        entry->index = index;
        entry->way = way;
        indexWayPreTable.insertEntry(index, false, entry);
    }
}

} // namespace way_prediction_policy
} // namespace gem5
