#include "base/logging.hh"
#include "cpu/o3/mls_unit.hh"

namespace gem5
{

namespace o3
{

MlsUnit::MlsUnit(CPU *cpu_) : cpu(cpu_)
{
}

bool
MlsUnit::replayReady(const MlsReplayQueue::ReplayState &state) const
{
    panic("Matrix MLS replay is only supported by the RISC-V ISA");
}

MlsUnit::IssueResult
MlsUnit::issue(const DynInstPtr &inst)
{
    panic("Matrix MLS execution is only supported by the RISC-V ISA");
}

} // namespace o3
} // namespace gem5
