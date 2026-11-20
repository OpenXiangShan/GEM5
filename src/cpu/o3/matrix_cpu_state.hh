#ifndef __CPU_O3_MATRIX_CPU_STATE_HH__
#define __CPU_O3_MATRIX_CPU_STATE_HH__

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>

#include "arch/riscv/matrix.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "matrix/MatrixTypes.hh"

namespace gem5::matrix
{
class DetailedCuteBackend;
}

namespace gem5::o3
{

/** Optional CPU-owned asynchronous state, separate from speculative MLS/AMU. */
struct MatrixCpuState
{
    MatrixCpuState();
    ~MatrixCpuState();

    struct Owner
    {
        ThreadID tid;
        matrix::CuteRequest request;
        std::optional<matrix::CuteCompletion> completion = {};
    };
    std::unique_ptr<matrix::DetailedCuteBackend> backend;
    std::array<InstSeqNum, RiscvISA::MatrixTokenCount> resetSeqs{};
    std::unordered_map<InstSeqNum, Owner> owners;
    std::deque<InstSeqNum> checkOrder;
};

} // namespace gem5::o3

#endif // __CPU_O3_MATRIX_CPU_STATE_HH__
