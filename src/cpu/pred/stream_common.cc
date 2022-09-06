#include "cpu/pred/stream_common.hh"

namespace gem5 {

namespace branch_prediction {

unsigned streamChunkSize{0x40};

Addr
computeLastChunkStart(Addr taken_control_pc, Addr stream_start_pc)
{
    Addr chunk_start = taken_control_pc / streamChunkSize * streamChunkSize;
    if (chunk_start >= stream_start_pc) {
        return chunk_start;
    } else {
        return stream_start_pc;
    }
}


}  // namespace branch_prediction

}  // namespace gem5