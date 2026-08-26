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
#include "arch/riscv/insts/matrix.hh"
#include "arch/riscv/pcstate.hh"
#include "arch/riscv/types.hh"
#include "base/types.hh"
#include "cpu/exec_context.hh"

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
  protected:
    std::vector<RegVal> miscRegFile;
    uint32_t matrixTileM = 0;
    uint32_t matrixTileK = 0;
    uint32_t matrixTileN = 0;
    std::vector<int8_t> matrixTileA;
    std::vector<int8_t> matrixTileB;
    std::vector<int32_t> matrixAcc;
    std::vector<RegVal> matrixTokens;

    /** Architectural VLEN/ELEN (bits). Fixed for a simulation run. */
    unsigned vlen;
    unsigned elen;

    RegVal &
    matrixToken(size_t idx);

    const RegVal &
    matrixToken(size_t idx) const;

  public:
    using Params = RiscvISAParams;

    void clear();

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

    /** RVV VLEN in bits / bytes and ELEN in bits (Ch. 2 of the vector spec). */
    unsigned getVecLenInBits() const { return vlen; }
    unsigned getVecLenInBytes() const { return vlen >> 3; }
    unsigned getVecElemLenInBits() const { return elen; }

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
    void matrixSyncReset(uint64_t token_idx);
    void matrixRelease(uint64_t token_idx);
    void matrixAcquire(uint64_t token_idx, uint64_t target);
    void setMatrixTileM(uint64_t value);
    void setMatrixTileK(uint64_t value);
    void setMatrixTileN(uint64_t value);
    uint32_t getMatrixTileM() const { return matrixTileM; }
    uint32_t getMatrixTileK() const { return matrixTileK; }
    uint32_t getMatrixTileN() const { return matrixTileN; }
    Fault matrixLoadA8(ExecContext *xc, Addr base, Addr stride);
    Fault matrixLoadB8(ExecContext *xc, Addr base, Addr stride);
    Fault matrixLoadC32(ExecContext *xc, Addr base, Addr stride);
    Fault matrixStoreC32(ExecContext *xc, Addr base, Addr stride);
    void matrixZeroAcc();
    void matrixMMAccWB();

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
