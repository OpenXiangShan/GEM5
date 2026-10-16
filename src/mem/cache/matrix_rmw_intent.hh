#ifndef __MEM_CACHE_MATRIX_RMW_INTENT_HH__
#define __MEM_CACHE_MATRIX_RMW_INTENT_HH__

#include "mem/packet.hh"

namespace gem5::matrix
{

/** A POU-protected masked write, including RMW reuse, invalidates snoops. */
inline bool
isProtectedStoreSnoop(const PacketPtr pkt)
{
    return pkt->cmd == MemCmd::WriteReq && pkt->req->hasXsMetadata() &&
           pkt->req->getXsMetadata().matrixStoreProtected;
}

/** Only the original data target, never a Cache-generated coherence packet. */
inline bool
isRmwCRead(const PacketPtr pkt)
{
    if (pkt->cmd != MemCmd::ReadReq || pkt->fromCache() ||
        !pkt->req->hasXsMetadata()) {
        return false;
    }
    const auto metadata = pkt->req->getXsMetadata();
    return metadata.matrixTask() && metadata.matrixModify() &&
           metadata.matrixRmwAcquire;
}

} // namespace gem5::matrix

#endif // __MEM_CACHE_MATRIX_RMW_INTENT_HH__
