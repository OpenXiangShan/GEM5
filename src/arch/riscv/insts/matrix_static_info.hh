#ifndef __ARCH_RISCV_INSTS_MATRIX_STATIC_INFO_HH__
#define __ARCH_RISCV_INSTS_MATRIX_STATIC_INFO_HH__

#include <cstdint>

#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/types.hh"

namespace gem5
{

namespace RiscvISA
{

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

struct MatrixStaticInfo
{
    bool valid = false;
    uint8_t funct7 = 0;
    uint8_t rd = 0;
    bool loadLike = false;
    bool storeLike = false;
    bool needAmuCtrlCandidate = false;

    bool lsuIsA = false;
    bool lsuIsB = false;
    bool lsuIsAcc = false;
    bool lsuTranspose = false;
    uint8_t lsuEncodedWidth = 0;
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

inline MatrixElemKind
matrixAbElemKind(uint8_t width)
{
    return width == MatrixSewE16 ? MatrixElemKind::Fp16 :
           width == MatrixSewE8 ? MatrixElemKind::Int8 : MatrixElemKind::None;
}

inline uint8_t
matrixAccessSize(uint8_t width)
{
    return width == MatrixSewE16 ? 2 : width == MatrixSewE8 ? 1 : 0;
}

inline bool
lsuEncodingLegal(const MatrixStaticInfo &info)
{
    return ((info.rd & 4) != 0) == info.rdMustSetBit2 &&
           (info.lsuIsAcc ? info.lsuEncodedWidth == MatrixSewE32 :
            (info.lsuEncodedWidth == MatrixSewE8 ||
             info.lsuEncodedWidth == MatrixSewE16));
}

inline bool
isMatrixTileCsr(uint16_t csr_idx)
{
    return csr_idx == CSR_MTILEM ||
           csr_idx == CSR_MTILEN ||
           csr_idx == CSR_MTILEK;
}

inline void
setLsu(MatrixStaticInfo &info, bool load, bool store, bool is_a, bool is_b,
       bool is_acc, bool transpose, uint8_t widths, MatrixElemKind elem_kind,
       uint8_t access_size, MatrixStateOperand tile0,
       MatrixStateOperand tile1, bool rd_must_set_bit2)
{
    info.loadLike = load;
    info.storeLike = store;
    info.needAmuCtrlCandidate = true;
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
}

} // namespace matrix_static_info

inline MatrixStaticInfo
matrixStaticInfoFromMachInst(ExtMachInst mach_inst)
{
    using namespace matrix_static_info;

    MatrixStaticInfo info;
    info.funct7 = mach_inst.funct7;
    info.rd = mach_inst.rd;
    info.lsuEncodedWidth = (mach_inst.rd >> 3) & 0x3;

    const bool is_matrix_opcode = mach_inst.opcode7 == MatrixOpcode7;
    const bool is_csr =
        mach_inst.opcode7 == SystemOpcode7 && mach_inst.funct3 != 0;
    const auto csr_idx = static_cast<uint16_t>(mach_inst.funct12);
    const bool is_matrix_csr = is_csr && isMatrixTileCsr(csr_idx);

    if (is_matrix_csr) {
        info.valid = true;
        return info;
    }

    if (!is_matrix_opcode) {
        return info;
    }

    info.valid = true;
    switch (mach_inst.funct7) {
      case 0x00:
      case 0x08:
      case 0x09:
      case 0x10:
      case 0x11:
      case 0x18:
      case 0x19:
      case 0x40:
      case 0x50:
        break;
      case 0x02:
      {
        const auto width = static_cast<uint8_t>((mach_inst.rd >> 3) & 0x3);
        setLsu(info, true, false, true, false, false, false, width,
               matrixAbElemKind(width), matrixAccessSize(width),
               MatrixStateOperand::Mtilem,
               MatrixStateOperand::Mtilek, false);
        break;
      }
      case 0x0a:
      {
        const auto width = static_cast<uint8_t>((mach_inst.rd >> 3) & 0x3);
        setLsu(info, true, false, false, true, false, true, width,
               matrixAbElemKind(width), matrixAccessSize(width),
               MatrixStateOperand::Mtilek,
               MatrixStateOperand::Mtilen, false);
        break;
      }
      case 0x12:
        setLsu(info, true, false, false, false, true, false,
               info.lsuEncodedWidth,
               info.lsuEncodedWidth == MatrixSewE32 ?
                   MatrixElemKind::Int32 : MatrixElemKind::None,
               info.lsuEncodedWidth == MatrixSewE32 ? 4 : 0,
               MatrixStateOperand::Mtilem,
               MatrixStateOperand::Mtilen, true);
        break;
      case 0x13:
        setLsu(info, false, true, false, false, true, false,
               info.lsuEncodedWidth,
               info.lsuEncodedWidth == MatrixSewE32 ?
                   MatrixElemKind::Int32 : MatrixElemKind::None,
               info.lsuEncodedWidth == MatrixSewE32 ? 4 : 0,
               MatrixStateOperand::Mtilem,
               MatrixStateOperand::Mtilen, true);
        break;
      case 0x04:
      case 0x0c:
      case 0x06:
        info.needAmuCtrlCandidate = true;
        break;
      case 0x48:
        info.needAmuCtrlCandidate = true;
        break;
      default:
        info.valid = false;
        break;
    }

    return info;
}

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_INSTS_MATRIX_STATIC_INFO_HH__
