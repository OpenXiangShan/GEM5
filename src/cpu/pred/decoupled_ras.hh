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
    int32_t head_ptr;
    uint32_t maxSize;

public:
    inline void set(uint32_t idx,Addr pc){
        assert(idx < maxSize);
        ras[idx] = pc;
    }
    StreamRas(uint32_t size);
    //return the old ras head
    std::pair<Addr, uint32_t> push(Addr nextPC);
    //return the old ras head
    std::pair<Addr, uint32_t> pop();

    void restore(Addr top, uint32_t topIdx);
    void callrestore(uint32_t topIdx);

};
}
}


#endif