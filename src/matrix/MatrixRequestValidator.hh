#ifndef __MATRIX_REQUEST_VALIDATOR_HH__
#define __MATRIX_REQUEST_VALIDATOR_HH__

#include <cstdint>

#include "matrix/CUTEParameters.hh"

namespace gem5::matrix
{

enum class MatrixRequestError : uint8_t
{
    None,
    UnsupportedMma,
    UnsupportedLsu,
    UnsupportedArith,
    UnsupportedRelease,
};

inline bool
supportedInt8MmaShape(uint32_t m, uint32_t n, uint32_t k)
{
    return m >= 8 && m <= 128 && m % 8 == 0 &&
           n == 128 && (k == 32 || k == 64);
}

inline MatrixRequestError
validateMatrixRequest(const CuteRequest &req)
{
    switch (req.kind) {
      case CuteRequestKind::Mma:
      {
        const auto &mma = req.mma;
        if (!supportedInt8MmaShape(mma.mtilem, mma.mtilen, mma.mtilek) ||
            mma.isFp || mma.sat || mma.lhsElemType != MatrixElemType::Int8 ||
            mma.rhsElemType != MatrixElemType::Int8 ||
            mma.dstElemType != MatrixElemType::Int32 ||
            (mma.types1 != 0 && mma.types1 != 0x4) ||
            (mma.types2 != 0 && mma.types2 != 0x4) ||
            (mma.typed != 0 && mma.typed != 0x2)) {
            return MatrixRequestError::UnsupportedMma;
        }
        return MatrixRequestError::None;
      }
      case CuteRequestKind::Lsu:
      {
        const auto &lsu = req.lsu;
        const bool ab = lsu.isA != lsu.isB && !lsu.isAcc;
        const bool acc = lsu.isAcc && !lsu.isA && !lsu.isB;
        const bool ab_width =
            (lsu.widths == 0 && lsu.elemType == MatrixElemType::Int8) ||
            (lsu.widths == 1 && lsu.elemType == MatrixElemType::Fp16);
        const bool acc_width = lsu.widths == 2 &&
            lsu.elemType == MatrixElemType::Int32;
        if (lsu.row == 0 || lsu.column == 0 ||
            lsu.row > 128 || lsu.column > 128 ||
            (!(ab && ab_width) && !(acc && acc_width)) ||
            (lsu.isStore && !acc)) {
            return MatrixRequestError::UnsupportedLsu;
        }
        return MatrixRequestError::None;
      }
      case CuteRequestKind::Arith:
        return req.op == 0x06 &&
               req.arith.op == MatrixArithOpcode::Zero &&
               req.arith.reg < 8 &&
               req.arith.bank == MatrixBankKind::C &&
               req.arith.rows <= 128 && req.arith.cols <= 128 &&
               req.arith.elemType == MatrixElemType::Int32 ?
            MatrixRequestError::None : MatrixRequestError::UnsupportedArith;
      case CuteRequestKind::Release:
        return req.release.tokenIndex < 32 ?
            MatrixRequestError::None :
            MatrixRequestError::UnsupportedRelease;
    }
    return MatrixRequestError::UnsupportedMma;
}

} // namespace gem5::matrix

#endif // __MATRIX_REQUEST_VALIDATOR_HH__
