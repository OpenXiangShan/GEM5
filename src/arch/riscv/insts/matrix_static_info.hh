#ifndef __ARCH_RISCV_INSTS_MATRIX_STATIC_INFO_HH__
#define __ARCH_RISCV_INSTS_MATRIX_STATIC_INFO_HH__

#include <array>
#include <cstddef>
#include <cstdint>

#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/types.hh"

namespace gem5
{

namespace RiscvISA
{

enum class MatrixInstClass : uint8_t
{
    None,
    Init,
    Set,
    Csr,
    Lsu,
    Mma,
    Arith,
    Sync
};

enum class MatrixRouteKind : uint8_t
{
    None,
    Init,
    SetTile,
    TileCsr,
    Lsu,
    Mma,
    Arith,
    Release,
    SyncReset,
    Acquire
};

enum class MatrixCommitBoundary : uint8_t
{
    None,
    ArchStateOnly,
    TokenSyncOnly,
    FutureAmuProducer
};

enum class MatrixStateOperand : uint8_t
{
    None,
    Mtilem,
    Mtilen,
    Mtilek,
};

enum class MatrixElemKind : uint8_t
{
    None,
    Int8,
    Fp16,
    Int32,
};

inline const char *
matrixInstClassName(MatrixInstClass inst_class)
{
    switch (inst_class) {
      case MatrixInstClass::Init:
        return "init";
      case MatrixInstClass::Set:
        return "set";
      case MatrixInstClass::Csr:
        return "csr";
      case MatrixInstClass::Lsu:
        return "lsu";
      case MatrixInstClass::Mma:
        return "mma";
      case MatrixInstClass::Arith:
        return "arith";
      case MatrixInstClass::Sync:
        return "sync";
      case MatrixInstClass::None:
        return "unknown";
    }

    return "unknown";
}

inline const char *
matrixRouteName(MatrixRouteKind route)
{
    switch (route) {
      case MatrixRouteKind::Init:
        return "init";
      case MatrixRouteKind::SetTile:
        return "settile";
      case MatrixRouteKind::TileCsr:
        return "tile-csr";
      case MatrixRouteKind::Lsu:
        return "lsu";
      case MatrixRouteKind::Mma:
        return "mma";
      case MatrixRouteKind::Arith:
        return "arith";
      case MatrixRouteKind::Release:
        return "release";
      case MatrixRouteKind::SyncReset:
        return "sync-reset";
      case MatrixRouteKind::Acquire:
        return "acquire";
      case MatrixRouteKind::None:
        return "unknown";
    }

    return "unknown";
}

inline const char *
matrixCommitBoundaryName(MatrixCommitBoundary boundary)
{
    switch (boundary) {
      case MatrixCommitBoundary::ArchStateOnly:
        return "arch-only";
      case MatrixCommitBoundary::TokenSyncOnly:
        return "token-sync";
      case MatrixCommitBoundary::FutureAmuProducer:
        return "future-amu";
      case MatrixCommitBoundary::None:
        return "none";
    }

    return "none";
}

static constexpr size_t MaxMatrixStateOperands = 6;
static constexpr size_t MaxMatrixStateWrites = 2;

struct MatrixStaticInfo
{
    bool valid = false;
    MatrixInstClass instClass = MatrixInstClass::None;
    MatrixRouteKind route = MatrixRouteKind::None;
    MatrixCommitBoundary commitBoundary = MatrixCommitBoundary::None;
    uint16_t csrIndex = 0;
    uint8_t opcode7 = 0;
    uint8_t funct7 = 0;
    uint8_t funct3 = 0;
    uint8_t rd = 0;
    uint8_t rs1 = 0;
    uint8_t rs2 = 0;
    uint8_t tokenIndex = 0;
    bool loadLike = false;
    bool storeLike = false;
    bool tokenLike = false;
    bool usesLsq = false;
    bool needAmuCtrlCandidate = false;
    bool dirtyMs = false;
    std::array<MatrixStateOperand, MaxMatrixStateOperands> stateReads = {
        MatrixStateOperand::None, MatrixStateOperand::None,
        MatrixStateOperand::None, MatrixStateOperand::None,
        MatrixStateOperand::None, MatrixStateOperand::None
    };
    std::array<MatrixStateOperand, MaxMatrixStateWrites> stateWrites = {
        MatrixStateOperand::None, MatrixStateOperand::None
    };

    bool lsuIsA = false;
    bool lsuIsB = false;
    bool lsuIsAcc = false;
    bool lsuTranspose = false;
    uint8_t lsuWidths = 0;
    MatrixElemKind lsuElemKind = MatrixElemKind::None;
    uint8_t lsuAccessSize = 0;
    MatrixStateOperand tile0State = MatrixStateOperand::None;
    MatrixStateOperand tile1State = MatrixStateOperand::None;
    MatrixStateOperand rowState = MatrixStateOperand::None;
    MatrixStateOperand columnState = MatrixStateOperand::None;
    bool rdMustSetBit2 = false;
};

namespace matrix_static_info
{

static constexpr uint8_t MatrixOpcode7 = 0x2b;
static constexpr uint8_t SystemOpcode7 = 0x73;
static constexpr uint8_t MatrixSewE8 = 0;
static constexpr uint8_t MatrixSewE16 = 1;
static constexpr uint8_t MatrixSewE32 = 2;

inline bool
isMatrixTileCsr(uint16_t csr_idx)
{
    return csr_idx == CSR_MTILEM ||
           csr_idx == CSR_MTILEN ||
           csr_idx == CSR_MTILEK;
}

inline MatrixStateOperand
tileCsrToMatrixStateOperand(uint16_t csr_idx)
{
    switch (csr_idx) {
      case CSR_MTILEM:
        return MatrixStateOperand::Mtilem;
      case CSR_MTILEN:
        return MatrixStateOperand::Mtilen;
      case CSR_MTILEK:
        return MatrixStateOperand::Mtilek;
      default:
        return MatrixStateOperand::None;
    }
}

inline bool
csrWritesMatrixState(uint8_t funct3, uint8_t rs1)
{
    switch (funct3) {
      case 0x1:
      case 0x5:
        return true;
      case 0x2:
      case 0x3:
      case 0x6:
      case 0x7:
        return rs1 != 0;
      default:
        return false;
    }
}

inline void
setStateReads(MatrixStaticInfo &info,
              std::array<MatrixStateOperand, MaxMatrixStateOperands> reads)
{
    info.stateReads = reads;
}

inline void
setMatrixRoute(MatrixStaticInfo &info, MatrixInstClass inst_class,
               MatrixRouteKind route, MatrixCommitBoundary boundary)
{
    info.instClass = inst_class;
    info.route = route;
    info.commitBoundary = boundary;
}

inline void
setLsu(MatrixStaticInfo &info, bool load, bool store, bool is_a, bool is_b,
       bool is_acc, bool transpose, uint8_t widths, MatrixElemKind elem_kind,
       uint8_t access_size, MatrixStateOperand tile0,
       MatrixStateOperand tile1, bool rd_must_set_bit2)
{
    setMatrixRoute(info, MatrixInstClass::Lsu, MatrixRouteKind::Lsu,
                   MatrixCommitBoundary::FutureAmuProducer);
    info.loadLike = load;
    info.storeLike = store;
    info.usesLsq = true;
    info.needAmuCtrlCandidate = true;
    info.dirtyMs = load;
    info.lsuIsA = is_a;
    info.lsuIsB = is_b;
    info.lsuIsAcc = is_acc;
    info.lsuTranspose = transpose;
    info.lsuWidths = widths;
    info.lsuElemKind = elem_kind;
    info.lsuAccessSize = access_size;
    info.tile0State = tile0;
    info.tile1State = tile1;
    info.rowState = tile0;
    info.columnState = tile1;
    info.rdMustSetBit2 = rd_must_set_bit2;
    setStateReads(info, {
        tile0, tile1, MatrixStateOperand::None, MatrixStateOperand::None,
        MatrixStateOperand::None, MatrixStateOperand::None
    });
}

} // namespace matrix_static_info

inline MatrixStaticInfo
matrixStaticInfoFromMachInst(ExtMachInst mach_inst)
{
    using namespace matrix_static_info;

    MatrixStaticInfo info;
    info.opcode7 = mach_inst.opcode7;
    info.funct7 = mach_inst.funct7;
    info.funct3 = mach_inst.funct3;
    info.rd = mach_inst.rd;
    info.rs1 = mach_inst.rs1;
    info.rs2 = mach_inst.rs2;

    const bool is_matrix_opcode = mach_inst.opcode7 == MatrixOpcode7;
    const bool is_csr =
        mach_inst.opcode7 == SystemOpcode7 && mach_inst.funct3 != 0;
    const auto csr_idx = static_cast<uint16_t>(mach_inst.funct12);
    const bool is_matrix_csr = is_csr && isMatrixTileCsr(csr_idx);

    if (is_matrix_csr) {
        info.valid = true;
        info.csrIndex = csr_idx;
        setMatrixRoute(info, MatrixInstClass::Csr, MatrixRouteKind::TileCsr,
                       MatrixCommitBoundary::ArchStateOnly);
        const auto operand = tileCsrToMatrixStateOperand(csr_idx);
        info.stateReads[0] = operand;
        if (csrWritesMatrixState(mach_inst.funct3, mach_inst.rs1)) {
            info.stateWrites[0] = operand;
        }
        return info;
    }

    if (!is_matrix_opcode) {
        return info;
    }

    info.valid = true;
    switch (mach_inst.funct7) {
      case 0x00:
        setMatrixRoute(info, MatrixInstClass::Init, MatrixRouteKind::Init,
                       MatrixCommitBoundary::ArchStateOnly);
        info.dirtyMs = true;
        break;
      case 0x08:
      case 0x09:
        setMatrixRoute(info, MatrixInstClass::Set, MatrixRouteKind::SetTile,
                       MatrixCommitBoundary::ArchStateOnly);
        info.dirtyMs = true;
        info.stateWrites[0] = MatrixStateOperand::Mtilek;
        break;
      case 0x10:
      case 0x11:
        setMatrixRoute(info, MatrixInstClass::Set, MatrixRouteKind::SetTile,
                       MatrixCommitBoundary::ArchStateOnly);
        info.dirtyMs = true;
        info.stateWrites[0] = MatrixStateOperand::Mtilem;
        break;
      case 0x18:
      case 0x19:
        setMatrixRoute(info, MatrixInstClass::Set, MatrixRouteKind::SetTile,
                       MatrixCommitBoundary::ArchStateOnly);
        info.dirtyMs = true;
        info.stateWrites[0] = MatrixStateOperand::Mtilen;
        break;
      case 0x02:
        setLsu(info, true, false, true, false, false, false, MatrixSewE8,
               MatrixElemKind::Int8, 1, MatrixStateOperand::Mtilem,
               MatrixStateOperand::Mtilek, false);
        break;
      case 0x0a:
        setLsu(info, true, false, false, true, false, true, MatrixSewE8,
               MatrixElemKind::Int8, 1, MatrixStateOperand::Mtilek,
               MatrixStateOperand::Mtilen, false);
        break;
      case 0x12:
        setLsu(info, true, false, true, false, false, false, MatrixSewE16,
               MatrixElemKind::Fp16, 2, MatrixStateOperand::Mtilem,
               MatrixStateOperand::Mtilek, false);
        break;
      case 0x1a:
        setLsu(info, true, false, false, true, false, true, MatrixSewE16,
               MatrixElemKind::Fp16, 2, MatrixStateOperand::Mtilek,
               MatrixStateOperand::Mtilen, false);
        break;
      case 0x22:
        setLsu(info, true, false, false, false, true, false, MatrixSewE32,
               MatrixElemKind::Int32, 4, MatrixStateOperand::Mtilem,
               MatrixStateOperand::Mtilen, true);
        break;
      case 0x13:
        setLsu(info, false, true, false, false, true, false, MatrixSewE32,
               MatrixElemKind::Int32, 4, MatrixStateOperand::Mtilem,
               MatrixStateOperand::Mtilen, true);
        break;
      case 0x04:
      case 0x0c:
        setMatrixRoute(info, MatrixInstClass::Mma, MatrixRouteKind::Mma,
                       MatrixCommitBoundary::FutureAmuProducer);
        info.needAmuCtrlCandidate = true;
        info.dirtyMs = true;
        setStateReads(info, {
            MatrixStateOperand::Mtilem, MatrixStateOperand::Mtilen,
            MatrixStateOperand::Mtilek, MatrixStateOperand::None,
            MatrixStateOperand::None, MatrixStateOperand::None
        });
        break;
      case 0x06:
        setMatrixRoute(info, MatrixInstClass::Arith, MatrixRouteKind::Arith,
                       MatrixCommitBoundary::FutureAmuProducer);
        info.needAmuCtrlCandidate = true;
        info.dirtyMs = true;
        setStateReads(info, {
            MatrixStateOperand::Mtilem, MatrixStateOperand::Mtilen,
            MatrixStateOperand::None, MatrixStateOperand::None,
            MatrixStateOperand::None, MatrixStateOperand::None
        });
        break;
      case 0x40:
        setMatrixRoute(info, MatrixInstClass::Sync, MatrixRouteKind::SyncReset,
                       MatrixCommitBoundary::TokenSyncOnly);
        info.tokenLike = true;
        info.tokenIndex = mach_inst.rs2;
        break;
      case 0x48:
        setMatrixRoute(info, MatrixInstClass::Sync, MatrixRouteKind::Release,
                       MatrixCommitBoundary::FutureAmuProducer);
        info.tokenLike = true;
        info.needAmuCtrlCandidate = true;
        info.tokenIndex = mach_inst.rs2;
        break;
      case 0x50:
        setMatrixRoute(info, MatrixInstClass::Sync, MatrixRouteKind::Acquire,
                       MatrixCommitBoundary::TokenSyncOnly);
        info.tokenLike = true;
        info.tokenIndex = mach_inst.rs2;
        break;
      default:
        info.valid = false;
        info.instClass = MatrixInstClass::None;
        info.route = MatrixRouteKind::None;
        info.commitBoundary = MatrixCommitBoundary::None;
        break;
    }

    return info;
}

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_INSTS_MATRIX_STATIC_INFO_HH__
