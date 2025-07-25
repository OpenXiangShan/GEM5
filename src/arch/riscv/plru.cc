// plru.cc
#include "plru.hh"
#include <algorithm>
#include <cassert>

PLRUTreeN::PLRUTreeN(size_t ways)
    : numWays(ways), bits(ways > 1 ? ways - 1 : 0, false)
{
    assert((ways & (ways - 1)) == 0 && "PLRU requires power-of-two number of ways");
}

size_t PLRUTreeN::getVictim() const {
    size_t idx = 0;
    while (idx < bits.size()) {
        idx = bits[idx] ? (2 * idx + 2) : (2 * idx + 1);
    }
    return idx - bits.size();
}

void PLRUTreeN::access(size_t way) {
    size_t idx = way + bits.size();
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        bits[parent] = (idx % 2 == 0); // 右子 = 1，左子 = 0
        idx = parent;
    }
}

void PLRUTreeN::reset() {
    std::fill(bits.begin(), bits.end(), false);
}
