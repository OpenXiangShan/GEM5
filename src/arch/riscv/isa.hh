/*
 * Copyright (c) 2009 The Regents of The University of Michigan
 * Copyright (c) 2009 The University of Edinburgh
 * Copyright (c) 2014 Sven Karlsson
 * Copyright (c) 2016 RISC-V Foundation
 * Copyright (c) 2016 The University of Virginia
 * Copyright (c) 2020 Barkhausen Institut
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __ARCH_RISCV_ISA_HH__
#define __ARCH_RISCV_ISA_HH__

#include <cstdint>
#include <vector>

#include "arch/generic/isa.hh"
#include "arch/riscv/pcstate.hh"
#include "arch/riscv/types.hh"
#include "base/types.hh"
#include "matrix/CUTEParameters.hh"

namespace gem5
{

struct RiscvISAParams;
class Checkpoint;

namespace RiscvISA
{

enum PrivilegeMode
{
    PRV_U = 0,
    PRV_S = 1,
    PRV_HS = 2,
    PRV_M = 3
};

enum FPUStatus
{
    OFF = 0,
    INITIAL = 1,
    CLEAN = 2,
    DIRTY = 3,
};

enum class VPUStatus
{
    OFF = 0,
    INITIAL = 1,
    CLEAN = 2,
    DIRTY = 3,
};

class ISA : public BaseISA
{
  public:
    static constexpr RegVal MatrixTokenCount = 32;
    static constexpr RegVal MatrixRowNum = 128;
    static constexpr RegVal MatrixTrLenE8Max = 64;
    static constexpr RegVal MatrixTrLenE16Max = 32;
    static constexpr RegVal MatrixAccE32Max = 128;
    static constexpr RegVal MatrixSewE8 = 0;
    static constexpr RegVal MatrixSewE16 = 1;
    static constexpr RegVal MatrixSewE32 = 2;

    static constexpr RegVal matrixMd(RegVal rd) { return rd & 0x7; }
    static constexpr RegVal matrixMemWidth(RegVal rd)
    {
        return (rd >> 3) & 0x3;
    }
    static constexpr bool matrixAbWidthSupported(RegVal width)
    {
        return width == MatrixSewE8 || width == MatrixSewE16;
    }
    static constexpr RegVal matrixTrLenMax(RegVal width)
    {
        return width == MatrixSewE8 ? MatrixTrLenE8Max : MatrixTrLenE16Max;
    }
    static matrix::MatrixElemType matrixAbElemType(RegVal width)
    {
        return width == MatrixSewE8 ? matrix::MatrixElemType::Int8 :
                                      matrix::MatrixElemType::Fp16;
    }

    struct MatrixLsuStubRequest
    {
        bool valid = false;
        bool isLoad = false;
        bool transpose = false;
        bool isAcc = false;
        bool isA = false;
        bool isB = false;
        RegVal op = 0;
        RegVal ms = 0;
        RegVal baseAddr = 0;
        RegVal stride = 0;
        RegVal row = 0;
        RegVal column = 0;
        RegVal widths = 0;
        matrix::MatrixElemType elemType = matrix::MatrixElemType::Int8;
    };

    static MatrixLsuStubRequest makeMatrixLsuStubRequest(
        bool is_load, bool transpose, bool is_acc, bool is_a, bool is_b,
        RegVal op, RegVal md, RegVal base_addr, RegVal stride, RegVal row,
        RegVal column, RegVal width, matrix::MatrixElemType elem_type)
    {
        MatrixLsuStubRequest req;
        req.valid = true;
        req.isLoad = is_load;
        req.transpose = transpose;
        req.isAcc = is_acc;
        req.isA = is_a;
        req.isB = is_b;
        req.op = op;
        req.ms = md;
        req.baseAddr = base_addr;
        req.stride = stride;
        req.row = row;
        req.column = column;
        req.widths = width;
        req.elemType = elem_type;
        return req;
    }

    struct MatrixMmaStubRequest
    {
        bool valid = false;
        bool isFp = false;
        RegVal op = 0;
        RegVal md = 0;
        RegVal ms1 = 0;
        RegVal ms2 = 0;
        RegVal mtilem = 0;
        RegVal mtilen = 0;
        RegVal mtilek = 0;
        RegVal rm = 0;
        RegVal frm = 0;
        RegVal types1 = 0;
        RegVal types2 = 0;
        RegVal typed = 0;
        matrix::MatrixElemType lhsElemType = matrix::MatrixElemType::Int8;
        matrix::MatrixElemType rhsElemType = matrix::MatrixElemType::Int8;
        matrix::MatrixElemType dstElemType = matrix::MatrixElemType::Int32;
        RegVal sat = 0;
    };

    struct MatrixArithStubRequest
    {
        bool valid = false;
        RegVal op = 0;
        RegVal md = 0;
    };

  protected:
    std::vector<RegVal> miscRegFile;
    RegVal matrixXmxrm = 0;
    RegVal matrixXmsat = 0;
    RegVal matrixXmfflags = 0;
    RegVal matrixXmfrm = 0;
    RegVal matrixXmsaten = 0;
    RegVal matrixTileM = 0;
    RegVal matrixTileK = 0;
    RegVal matrixTileN = 0;
    std::vector<RegVal> matrixTokens;
    MatrixLsuStubRequest matrixLastLsuReq;
    MatrixMmaStubRequest matrixLastMmaReq;
    MatrixArithStubRequest matrixLastArithReq;
    bool readMatrixMiscReg(int misc_reg, RegVal &val) const;

  public:
    using Params = RiscvISAParams;

    void clear();
    void clearMatrixStubRequests();
    void recordMatrixLsuRequest(const MatrixLsuStubRequest &req);
    void recordMatrixMmaRequest(const MatrixMmaStubRequest &req);
    void recordMatrixArithRequest(const MatrixArithStubRequest &req);
    void resetMatrixToken(RegVal token_idx);
    void releaseMatrixToken(RegVal token_idx);
    bool matrixTokenReady(RegVal token_idx, RegVal threshold) const;
    const MatrixLsuStubRequest &lastMatrixLsuRequest() const
    { return matrixLastLsuReq; }
    const MatrixMmaStubRequest &lastMatrixMmaRequest() const
    { return matrixLastMmaReq; }
    const MatrixArithStubRequest &lastMatrixArithRequest() const
    { return matrixLastArithReq; }

    PCStateBase *
    newPCState(Addr new_inst_addr=0) const override
    {
        return new PCState(new_inst_addr);
    }

  public:
    bool hpmCounterEnabled(int counter) const;
    RegVal readMiscRegNoEffect(int misc_reg) const;
    RegVal readMiscReg(int misc_reg);
    void setMiscRegNoEffect(int misc_reg, RegVal val);
    void setMiscReg(int misc_reg, RegVal val);

    RegId flattenRegId(const RegId &regId) const { return regId; }
    int flattenIntIndex(int reg) const { return reg; }
    int flattenFloatIndex(int reg) const { return reg; }
    int flattenVecIndex(int reg) const { return reg; }
    int flattenVecElemIndex(int reg) const { return reg; }
    int flattenVecPredIndex(int reg) const { return reg; }
    int flattenCCIndex(int reg) const { return reg; }
    int flattenMiscIndex(int reg) const { return reg; }

    bool inUserMode() const override;
    void copyRegsFrom(ThreadContext *src) override;

    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

    ISA(const Params &p);
    void resetMatrixState();

    void handleLockedRead(const RequestPtr &req) override;

    bool handleLockedWrite(const RequestPtr &req,
            Addr cacheBlockMask) override;

    void handleLockedSnoop(PacketPtr pkt, Addr cacheBlockMask) override;

    void globalClearExclusive() override;
};

} // namespace RiscvISA
} // namespace gem5

std::ostream &operator<<(std::ostream &os, gem5::RiscvISA::PrivilegeMode pm);

#endif // __ARCH_RISCV_ISA_HH__
