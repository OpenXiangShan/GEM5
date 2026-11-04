
#pragma once
#include <array>

namespace gem5 { namespace o3 {
struct ROB
{
    std::array<bool, 2> empty = {false, false}, done = {true, true};
    bool isEmpty(unsigned tid) const { return empty.at(tid); }
    bool isDoneSquashing(unsigned tid) const { return done.at(tid); }
};
}}
