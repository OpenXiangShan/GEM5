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
    // dep_inst completed_inst
    // actually the order of dep_inst and completed_inst doesn't have much
    // effect on the cycle delay????
    {{OpClass::IntAlu, OpClass::IntAlu}, 0},
    {{OpClass::IntMult, OpClass::IntAlu}, 1},
    {{OpClass::IntDiv, OpClass::IntDiv}, 1},
    {{OpClass::IntDiv, OpClass::IntMult}, 1},
    {{OpClass::IntDiv, OpClass::IntAlu}, 2},

    {{OpClass::FloatCvt, OpClass::IntAlu}, 3},
    {{OpClass::FloatCvt, OpClass::IntMult}, 3},
    {{OpClass::FloatCvt, OpClass::IntDiv}, 3},

    {{OpClass::FloatCvt, OpClass::FloatAdd}, 2},
    {{OpClass::FloatCvt, OpClass::FloatMult}, 2},
    {{OpClass::FloatCvt, OpClass::FloatDiv}, 2},
    {{OpClass::FloatCvt, OpClass::FloatCvt}, 2},

};



// check the opclass and replace the latency
bool
execLatencyCheck(CPU* cpu, DynInstPtr inst, Cycles& op_latency)
{
    auto lzc = [](RegVal val) {
        for (int i = 0; i < 64; i++) {
            if (val & (0x1lu << 63)) {
                return i;
            }
            val <<= 1;
        }
        return 64;
    };
    RegVal rs1;
    RegVal rs2;
    int delay_;
    switch (inst->opClass()) {
        case OpClass::IntDiv:
            rs1 = cpu->readArchIntReg(inst->srcRegIdx(0).index(),
                                      inst->threadNumber);
            rs2 = cpu->readArchIntReg(inst->srcRegIdx(1).index(),
                                      inst->threadNumber);
            // rs1 / rs2 : 0x80/0x8 ,delay_ = 4
            delay_ = std::max(lzc(std::labs(rs2)) - lzc(std::labs(rs1)), 0);
            if (rs2 == 1) {
                op_latency = Cycles(5);
            } else if (rs1 == rs2) {
                op_latency = Cycles(7);
            } else if (lzc(std::labs(rs2)) - lzc(std::labs(rs1)) < 0) {
                op_latency = Cycles(5);
            } else {
                op_latency = Cycles(7 + delay_ / 4);
            }
            return true;
        case OpClass::FloatAdd:
            op_latency = Cycles(3);
            return true;
        case OpClass::FloatMult:
            op_latency = Cycles(3);
            return true;
        case OpClass::FloatDiv:
            op_latency = Cycles(19);
            return true;
        case OpClass::FloatCvt:
            op_latency = Cycles(3);
            return true;

        default:
            return false;
    }
    return false;
}



}
}


#endif