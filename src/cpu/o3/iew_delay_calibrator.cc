#include <list>
#include <map>
#include <queue>
#include <vector>


#include "base/types.hh"
#include "cpu/o3/iew_delay_calibrator.hh"
#include "cpu/op_class.hh"



namespace gem5
{

namespace o3
{


DelayCalibrator::DelayCalibrator(const DelayCalibratorParams& params)
    : SimObject(params)
{
    for (auto it = params.matrix.begin(); it != params.matrix.end(); ++it) {
        matrix[std::make_pair((*it)->dep_opclass, (*it)->completed_opclass)] =
            (*it)->delay_tick;
    }
}

uint32_t
DelayCalibrator::lookupDelayMatrix(std::pair<OpClass, OpClass> ops)
{
    auto find_it = matrix.find(ops);
    if (find_it != matrix.end()) {
        return find_it->second;
    }
    return 0;
}

bool
DelayCalibrator::execLatencyCheck(CPU* cpu, DynInstPtr inst,
                                  Cycles& op_latency)
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