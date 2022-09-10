#ifndef __CPU_PRED_STREAM_COMMON_HH__
#define __CPU_PRED_STREAM_COMMON_HH__

#include "base/types.hh"
#include "cpu/inst_seq.hh"

namespace gem5 {

namespace branch_prediction {

extern unsigned streamChunkSize;

Addr computeLastChunkStart(Addr taken_control_pc, Addr stream_start_pc);

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_STREAM_COMMON_HH__