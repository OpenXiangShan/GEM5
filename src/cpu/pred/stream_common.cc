#include "cpu/pred/stream_common.hh"

namespace gem5 {

namespace branch_prediction {

unsigned streamChunkSize{0x80};

unsigned fetchTargetSize{0x80};
unsigned fetchTargetMask = fetchTargetSize - 1;

Addr
computeLastChunkStart(Addr taken_control_pc, Addr stream_start_pc)
{
    while (stream_start_pc < taken_control_pc) {
        stream_start_pc += streamChunkSize;
    }

    if (stream_start_pc > taken_control_pc) {
        stream_start_pc -= streamChunkSize;
    }

    return stream_start_pc;
}


}  // namespace branch_prediction

}  // namespace gem5