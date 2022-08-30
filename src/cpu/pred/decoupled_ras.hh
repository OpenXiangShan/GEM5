#ifndef __DECOUPLED_RAS_HH
#define __DECOUPLED_RAS_HH

#include <vector>
#include <queue>
#include <utility> 

#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/fetch_target_queue.hh"
#include "cpu/pred/stream_struct.hh"
#include "debug/DecoupleBP.hh"
#include "params/DecoupledBPU.hh"

namespace gem5 {

namespace branch_prediction {

class StreamRas {
    std::vector<Addr> ras;
    std::vector<uint32_t> rasCnt;
    int32_t head_ptr;
    uint32_t maxSize;

public:
    StreamRas(uint32_t size);
    //push callpc + 4
    void push(Addr nextPC);
    //return the ret pc and idx in the ras
    std::pair<Addr, uint32_t> pop();

    void restore(Addr top, uint32_t topIdx);

};
}
}


#endif