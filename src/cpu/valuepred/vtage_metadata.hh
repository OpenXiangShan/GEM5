/*
 * VTAGE is largely based on the open-source implementation of the
 * 1st-place Championship Value Prediction (CVP-1) submission.
 *
 * The version in this codebase is adapted and modified for our environment.
 * It is not intended to be a bit-exact copy of the original code.
 *
 * For detailed background and reference material:
 * Paper: https://microarch.org/cvp1/papers/Seznec.pdf
 * Open-source implementation: https://www.microarch.org/cvp1/code/Seznec.tar.gz
 * Official website: https://www.microarch.org/cvp1/
 */

#ifndef __CPU_VALUEPRED_VTAGE_METADATA_HH__
#define __CPU_VALUEPRED_VTAGE_METADATA_HH__

#include <cstdint>

#include "base/types.hh"
#include "cpu/op_class.hh"
#include "cpu/valuepred/valuepred_metadata.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

struct FetchTarget;

} // namespace btb_pred

} // namespace branch_prediction

namespace valuepred
{

class VPHistoryRequestExt : public VPPredictRequestExtension
{
  public:
    explicit VPHistoryRequestExt(
            const branch_prediction::btb_pred::FetchTarget *fetch_target)
        : fetchTarget(fetch_target)
    {
    }

    const branch_prediction::btb_pred::FetchTarget *fetchTarget = nullptr;
};

class LoadTrainInfoExt : public VPUpdateInfoExtension
{
  public:
    LoadTrainInfoExt(bool cache_hit, uint32_t observed_latency_cycles,
            bool latency_valid, uint8_t num_src_regs, OpClass op_class,
            bool critical_load)
        : cacheHit(cache_hit),
          observedLatencyCycles(observed_latency_cycles),
          latencyValid(latency_valid),
          numSrcRegs(num_src_regs),
          opClass(op_class),
          criticalLoad(critical_load)
    {
    }

    bool cacheHit = false;
    uint32_t observedLatencyCycles = 0;
    bool latencyValid = false;
    uint8_t numSrcRegs = 0;
    OpClass opClass = No_OpClass;
    bool criticalLoad = false;
};

} // namespace valuepred

} // namespace gem5

#endif // __CPU_VALUEPRED_VTAGE_METADATA_HH__
