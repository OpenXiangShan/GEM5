#include "cpu/pred/decoupled_ras.hh"



namespace gem5 {

namespace branch_prediction {

StreamRas::StreamRas(uint32_t size) {
    ras.resize(size);
    rasCnt.resize(size);
    for (auto& it : rasCnt) {
        it = 0;
    }
    maxSize = size;
}


void StreamRas::push(Addr nextPC) {
    bool needtopush = true;
    for (int i = 0;i < ras.size();i++) {
        if (ras[i] == nextPC) {
            rasCnt[i]++;
            needtopush = false;
        }
    }
    if (needtopush) {
        head_ptr++;
        if (head_ptr >= maxSize) {
            head_ptr = 0;
        }
        ras[head_ptr] = nextPC;
        rasCnt[head_ptr]=1;
    }
}
    //return the ret pc and idx in the ras
std::pair<Addr, uint32_t> StreamRas::pop() {
    if (rasCnt[head_ptr] > 1) {
        rasCnt[head_ptr]--;
        return std::make_pair(ras[head_ptr], head_ptr);
    }
    uint32_t topIdx = head_ptr;
    Addr topPC = ras[head_ptr];
    rasCnt[head_ptr] = 0;
    head_ptr--;
    if (head_ptr < 0) {
        head_ptr = maxSize - 1;
    }
    return std::make_pair(topPC, topIdx);
}

void StreamRas::restore(Addr top, uint32_t topIdx) {
    head_ptr = topIdx;
    ras[head_ptr] = top;
    rasCnt[head_ptr] = 1;
}


}
}
