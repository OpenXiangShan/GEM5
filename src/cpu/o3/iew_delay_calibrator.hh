#ifndef __CPU_O3_INST_DELAY_MATRIX_HH__
#define __CPU_O3_INST_DELAY_MATRIX_HH__

#include <list>
#include <map>
#include <queue>
#include <vector>


#include "base/types.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/op_class.hh"
#include "params/DelayCalibrator.hh"
#include "params/ScheduleDelayMatrixmap.hh"


namespace gem5
{

namespace o3
{
class CPU;
class ScheduleDelayMatrixmap : public SimObject
{
  public:
    OpClass dep_opclass;
    OpClass completed_opclass;
    uint32_t delay_tick;
    ScheduleDelayMatrixmap(const ScheduleDelayMatrixmapParams& params)
        : SimObject(params),
          dep_opclass(params.dep_opclass),
          completed_opclass(params.completed_opclass),
          delay_tick(params.delay_tick)
    {
    }
};

class DelayCalibrator : public SimObject
{

    std::map<std::pair<OpClass, OpClass>, uint32_t> matrix;

  public:
    DelayCalibrator(const DelayCalibratorParams& params);
    uint32_t lookupDelayMatrix(std::pair<OpClass, OpClass> ops);
    bool execLatencyCheck(CPU* cpu, DynInstPtr inst, Cycles& op_latency);
};


}
}


#endif