
#pragma once
#include <string>

#include "base/statistics.hh"

namespace gem5 { namespace o3 {
struct CPU : statistics::Group
{
    unsigned numThreads = 2;
    std::string name() const { return "test"; }
};
}}
