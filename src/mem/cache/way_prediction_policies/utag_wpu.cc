#include "mem/cache/way_prediction_policies/utag_wpu.hh"

#include <cstdint>

#include "base/types.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"

namespace gem5
{

namespace way_prediction_policy
{

UTagWpu::UTagWpu(const UTagWpuParams &params)
    : BaseWpu(params), utag_bits(params.utag_bits)
{
    utag_table.resize(set_mask + 1, std::vector<UTagEntry>(assoc));
}

Addr
UTagWpu::hash(Addr addr) const
{
    Addr tag = getWpuTag(addr);
    uint64_t utag_mask = (1 << utag_bits) - 1;
    return ((tag >> utag_bits) & utag_mask) ^ (tag & utag_mask);
}

uint8_t
UTagWpu::getPredictedWay(const PacketPtr pkt)
{
    Addr addr = getWpuAddr(pkt);
    Addr set = getWpuSet(addr);
    Addr utag = hash(addr);
    for (uint64_t i = 0; i < assoc; i++) {
        if (utag_table[set][i].valid && utag_table[set][i].utag == utag) {
            return i;
        }
    }
    // Not Found, return an invalid way
    return std::numeric_limits<uint8_t>::max();
}

void
UTagWpu::update(const PacketPtr pkt, const uint8_t way)
{
    Addr addr = getWpuAddr(pkt);
    Addr set = getWpuSet(addr);
    Addr utag = hash(addr);
    utag_table[set][way].valid = true;
    utag_table[set][way].utag = utag;
}

void
UTagWpu::invalidate(const PacketPtr pkt, const uint8_t way)
{
    Addr addr = getWpuAddr(pkt);
    Addr set = getWpuSet(addr);
    utag_table[set][way].valid = false;
}

void
UTagWpu::tryUpdateWpu(const PacketPtr pkt, const uint8_t predicted_way,
                      const uint8_t actual_way, const bool hit)
{
    bool utag_hit = predicted_way != std::numeric_limits<uint8_t>::max();
    // if utag table miss, but cache hit, update the utag table
    if (!utag_hit && hit) {
        update(pkt, actual_way);
    }

    // if utag table hit, but predicted way is not the actual way,
    // invalidate the predicted way
    if (utag_hit && (predicted_way != actual_way)) {
        invalidate(pkt, predicted_way);
    }

    // if utag table hit & cache hit, but predicted way is not the actual way,
    // update the utag table
    if (utag_hit && hit && (predicted_way != actual_way)) {
        update(pkt, actual_way);
    }
}

} // namespace way_prediction_policy
} // namespace gem5
