#ifndef __CPU_VALUEPRED_EXAMPLE_VALUE_PREDICTOR_METADATA_HH__
#define __CPU_VALUEPRED_EXAMPLE_VALUE_PREDICTOR_METADATA_HH__

#include "base/types.hh"
#include "cpu/op_class.hh"
#include "cpu/valuepred/valuepred_metadata.hh"

namespace gem5
{

namespace valuepred
{

class ExamplePredictRequestExt : public VPPredictRequestExtension
{
  public:
    ExamplePredictRequestExt(Tick predict_tick, OpClass op_class)
        : predictTick(predict_tick), opClass(op_class)
    {
    }

    // Example of fetch-time information that is not part of the stable
    // VPPredictRequest core fields.
    Tick predictTick = 0;
    OpClass opClass = No_OpClass;
};

class ExampleUpdateInfoExt : public VPUpdateInfoExtension
{
  public:
    ExampleUpdateInfoExt(bool has_memory_addr, Addr virtual_addr,
            Addr physical_addr)
        : hasMemoryAddr(has_memory_addr),
          virtualAddr(virtual_addr),
          physicalAddr(physical_addr)
    {
    }

    // Example of commit-time information that is only needed by a specific
    // predictor implementation.
    bool hasMemoryAddr = false;
    Addr virtualAddr = 0;
    Addr physicalAddr = 0;
};

} // namespace valuepred

} // namespace gem5

#endif // __CPU_VALUEPRED_EXAMPLE_VALUE_PREDICTOR_METADATA_HH__
