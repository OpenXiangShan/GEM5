#include "cpu/pred/decoupled_ras.hh"



namespace gem5 {

namespace branch_prediction {

StreamRas::StreamRas(uint32_t size) {
    ras.resize(size);
    for (auto& it : ras) {
        it = 0;
    }
    head_ptr=0;
    maxSize = size;
}


std::pair<Addr, uint32_t> StreamRas::push(Addr nextPC) {
    Addr topPC = ras[head_ptr];
    uint32_t topIdx = head_ptr;

    head_ptr++;
    if (head_ptr >= maxSize) {
        head_ptr = 0;
    }
    ras[head_ptr] = nextPC;

    return std::make_pair(topPC,topIdx);

}
    //return the ret pc and idx in the ras
std::pair<Addr, uint32_t> StreamRas::pop() {
    Addr topPC = ras[head_ptr];
    uint32_t topIdx = head_ptr;

    head_ptr--;
    if (head_ptr < 0) {
        head_ptr = maxSize - 1;
    }
    return std::make_pair(topPC, topIdx);
}

void StreamRas::restore(Addr top, uint32_t topIdx) {
    head_ptr = topIdx;
    ras[head_ptr] = top;
}


void StreamRas::callrestore(uint32_t topIdx){
    head_ptr = topIdx;
}

}
}
