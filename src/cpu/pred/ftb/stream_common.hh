#ifndef __CPU_PRED_FTB_STREAM_COMMON_HH__
#define __CPU_PRED_FTB_STREAM_COMMON_HH__

#include "base/types.hh"
#include "cpu/inst_seq.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred {

extern unsigned streamChunkSize;

extern unsigned fetchTargetSize;
extern unsigned fetchTargetMask;

Addr computeLastChunkStart(Addr taken_control_pc, Addr stream_start_pc);

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_FTB_STREAM_COMMON_HH__
