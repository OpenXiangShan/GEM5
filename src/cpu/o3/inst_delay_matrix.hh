#ifndef __CPU_O3_INST_DELAY_MATRIX_HH__
#define __CPU_O3_INST_DELAY_MATRIX_HH__

#include <list>
#include <map>
#include <queue>
#include <vector>

#include "base/types.hh"
#include "cpu/op_class.hh"

namespace gem5
{

namespace o3
{

std::map<std::pair<OpClass, OpClass>, uint32_t> scheduleDelayMatrix = {
    {{OpClass::IntAlu, OpClass::IntAlu}, 0},
};


std::map<std::pair<OpClass, OpClass>, uint32_t> iewExecuteDelayMatrix = {

};
}
}


#endif