/*
 * Copyright (c) 2022 PLCT Lab
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

#ifndef __ARCH_RISCV_MACROOP_HH__
#define __ARCH_RISCV_MACROOP_HH__

#include <bitset>

#include "arch/generic/memhelpers.hh"
#include "arch/riscv/faults.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/regs/vector.hh"
#include "arch/riscv/utility.hh"
#include "cpu/exec_context.hh"
#include "cpu/static_inst.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{

namespace RiscvISA
{

constexpr uint32_t cache_line_size = 64;

inline uint32_t width2sew(uint64_t width) {
    switch (bits(width, 2, 0)) {
        case 0b000: return 8;
        case 0b101: return 16;
        case 0b110: return 32;
        case 0b111: return 64;
        default: panic("width: %x not supported", bits(width, 2, 0));
    }
}

inline uint8_t checked_vtype(bool vill, uint8_t vtype) {
    panic_if(vill, "vill has been set");
    const uint8_t vsew = bits(vtype, 5, 3);
    panic_if(vsew >= 0b100, "vsew: %#x not supported", vsew);
    const uint8_t vlmul = bits(vtype, 2, 0);
    panic_if(vlmul == 0b100, "vlmul: %#x not supported", vlmul);
    return vtype;
}

class VectorMacroInst : public RiscvMacroInst
{
protected:
    uint32_t vl;
    uint8_t vtype;
    VectorMacroInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : RiscvMacroInst(mnem, _machInst, __opClass),
        vl(_machInst.vl),
        vtype(checked_vtype(_machInst.vill, _machInst.vtype8))
    {
        this->flags[IsVector] = true;
    }
};

class VectorMicroInst : public RiscvMicroInst
{
protected:
    uint8_t micro_vl;
    uint8_t vtype;
    VectorMicroInst(const char *mnem, ExtMachInst extMachInst,
            OpClass __opClass, uint8_t _micro_vl)
        : RiscvMicroInst(mnem, extMachInst, __opClass),
        micro_vl(_micro_vl),
        vtype(extMachInst.vtype8)
    {
        this->flags[IsVector] = true;
    }

    uint8_t vsew() const { return bits(this->vtype, 5, 3); }

    virtual uint32_t sew() const = 0;
};

class VectorArithMicroInst : public VectorMicroInst
{
protected:
    VectorArithMicroInst(const char *mnem, ExtMachInst extMachInst,
            OpClass __opClass, uint8_t _micro_vl)
        : VectorMicroInst(mnem, extMachInst, __opClass, _micro_vl)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;

    uint32_t sew() const override { return 8 << this->vsew(); }
};

class VectorArithMacroInst : public VectorMacroInst
{
protected:
    VectorArithMacroInst(const char* mnem, ExtMachInst _extMachInst,
                   OpClass __opClass)
        : VectorMacroInst(mnem, _extMachInst, __opClass)
    {
        this->flags[IsVector] = true;
    }

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorMemMicroInst : public VectorMicroInst
{
    uint32_t _sew;
protected:
    VectorMemMicroInst(const char* mnem, ExtMachInst _extMachInst,
                   OpClass __opClass, uint8_t _micro_vl)
        : VectorMicroInst(mnem, _extMachInst, __opClass, _micro_vl),
        _sew(width2sew(_extMachInst.width))
    {}

    uint32_t sew() const override { return _sew; }
};

class VectorMemMacroInst : public VectorMacroInst
{
    uint32_t _sew;
protected:
    VectorMemMacroInst(const char* mnem, ExtMachInst _extMachInst,
                   OpClass __opClass)
        : VectorMacroInst(mnem, _extMachInst, __opClass),
        _sew(width2sew(_extMachInst.width)) // sew set by mem inst not vconfig
    {}

    virtual uint32_t numElemPerMemAcc() {
        return cache_line_size / this->_sew;
    }

    uint32_t numMemAcc() {
        const uint32_t elemNumPerMemAcc = this->numElemPerMemAcc();
        return (vl + elemNumPerMemAcc - 1) / elemNumPerMemAcc;
    }

    constexpr uint32_t numMemAccPerVReg() {
        return RiscvISA::VLEN / cache_line_size;
    }
};

class VleMacroInst : public VectorMemMacroInst
{
protected:
    VleMacroInst(const char* mnem, ExtMachInst _extMachInst,
                   OpClass __opClass)
        : VectorMemMacroInst(mnem, _extMachInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VseMacroInst : public VectorMemMacroInst
{
protected:
    VseMacroInst(const char* mnem, ExtMachInst _extMachInst,
                   OpClass __opClass)
        : VectorMemMacroInst(mnem, _extMachInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VleMicroInst : public VectorMemMicroInst
{
protected:
    uint32_t offset; // base addr in rs1
    uint8_t dst_reg;
    Request::Flags memAccessFlags;

    VleMicroInst(const char *mnem, ExtMachInst extMachInst,
            OpClass __opClass, uint32_t _offset, uint8_t _dst_reg,
            uint8_t _micro_vl)
        : VectorMemMicroInst(mnem, extMachInst, __opClass, _micro_vl),
        offset(_offset), dst_reg(_dst_reg),
        memAccessFlags(0)
    {}

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VseMicroInst : public VectorMemMicroInst
{
protected:
    uint32_t offset; // base addr in rs1
    uint8_t src_reg;
    Request::Flags memAccessFlags;

    VseMicroInst(const char *mnem, ExtMachInst extMachInst,
            OpClass __opClass, uint32_t _offset, uint8_t _src_reg,
            uint8_t _micro_vl)
        : VectorMemMicroInst(mnem, extMachInst, __opClass, _micro_vl),
        offset(_offset), src_reg(_src_reg),
        memAccessFlags(0)
    {}

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VldMvMicroInst : public VectorArithMicroInst
{
private:
    RegId srcRegIdxArr[NumVecMemInternalRegs];
    RegId destRegIdxArr[1];
    uint8_t src_num;
public:
    VldMvMicroInst(ExtMachInst extMachInst, uint8_t _dst_reg, uint8_t _src_num)
        : VectorArithMicroInst("vl_mv_micro", extMachInst, VectorDummyOp, 0)
    {
        src_num = _src_num;
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        _numSrcRegs = 0;
        _numDestRegs = 0;
        _numFPDestRegs = 0;
        _numVecDestRegs = 0;
        _numVecElemDestRegs = 0;
        _numVecPredDestRegs = 0;
        _numIntDestRegs = 0;
        _numCCDestRegs = 0;

        setDestRegIdx(_numDestRegs++, RegId(VecRegClass, _dst_reg));
        _numVecDestRegs++;
        for (uint8_t i=0; i<_src_num; i++) {
            setSrcRegIdx(_numSrcRegs++,
                        RegId(VecRegClass, VecMemInternalReg0 + i));
        }
        this->flags[IsVector] = true;
    }
    Fault execute(ExecContext *, Trace::InstRecord *) const override;
    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VstMvMicroInst : public VectorArithMicroInst
{
private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[NumVecMemInternalRegs];
    uint8_t dst_num;
public:
    VstMvMicroInst(ExtMachInst extMachInst, uint8_t _src_reg, uint8_t _dst_num)
        : VectorArithMicroInst("vs_mv_micro", extMachInst, VectorDummyOp, 0)
    {
        dst_num = _dst_num;
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        _numSrcRegs = 0;
        _numDestRegs = 0;
        _numFPDestRegs = 0;
        _numVecDestRegs = 0;
        _numVecElemDestRegs = 0;
        _numVecPredDestRegs = 0;
        _numIntDestRegs = 0;
        _numCCDestRegs = 0;

        for (uint8_t i=0; i<_dst_num; i++) {
            setDestRegIdx(_numDestRegs++,
                        RegId(VecRegClass, VecMemInternalReg0 + i));
            _numVecDestRegs++;
        }
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, _src_reg));
        this->flags[IsVector] = true;
    }
    Fault execute(ExecContext *, Trace::InstRecord *) const override;
    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorIntMacroOp : public VectorArithMacroInst
{
public:
    VectorIntMacroOp(const char* mnem, ExtMachInst _extMachInst,
            OpClass __opClass)
        : VectorArithMacroInst(mnem, _extMachInst, __opClass)
    {}
    using VectorArithMacroInst::generateDisassembly;
};

class VectorIntMicroOp : public VectorArithMicroInst
{
public:
    uint8_t micro_idx;
    VectorIntMicroOp(const char *mnem, ExtMachInst extMachInst,
            OpClass __opClass, uint8_t _micro_vl, uint8_t _micro_idx)
        : VectorArithMicroInst(mnem, extMachInst, __opClass, _micro_vl),
        micro_idx(_micro_idx)
    {}
    using VectorArithMicroInst::generateDisassembly;
};

class VectorFloatMacroOp : public VectorArithMacroInst
{
public:
    VectorFloatMacroOp(const char* mnem, ExtMachInst _extMachInst,
            OpClass __opClass)
        : VectorArithMacroInst(mnem, _extMachInst, __opClass)
    {}
    using VectorArithMacroInst::generateDisassembly;
};

class VectorFloatMicroOp : public VectorArithMicroInst
{
public:
    uint8_t micro_idx;
    VectorFloatMicroOp(const char *mnem, ExtMachInst extMachInst,
            OpClass __opClass, uint8_t _micro_vl, uint8_t _micro_idx)
        : VectorArithMicroInst(mnem, extMachInst, __opClass, _micro_vl),
        micro_idx(_micro_idx)
    {}
    using VectorArithMicroInst::generateDisassembly;
};

class VectorIntMaskMacroOp : public VectorArithMacroInst
{
public:
    VectorIntMaskMacroOp(const char* mnem, ExtMachInst _extMachInst,
            OpClass __opClass)
        : VectorArithMacroInst(mnem, _extMachInst, __opClass)
    {}
    using VectorArithMacroInst::generateDisassembly;
};

class VectorIntMaskMicroOp : public VectorArithMicroInst
{
public:
    uint8_t micro_idx;
    VectorIntMaskMicroOp(const char *mnem, ExtMachInst extMachInst,
            OpClass __opClass, uint8_t _micro_vl, uint8_t _micro_idx)
        : VectorArithMicroInst(mnem, extMachInst, __opClass, _micro_vl),
        micro_idx(_micro_idx)
    {}
    using VectorArithMicroInst::generateDisassembly;
};

class VectorFloatMaskMacroOp : public VectorArithMacroInst
{
public:
    VectorFloatMaskMacroOp(const char* mnem, ExtMachInst _extMachInst,
            OpClass __opClass)
        : VectorArithMacroInst(mnem, _extMachInst, __opClass)
    {}
    using VectorArithMacroInst::generateDisassembly;
};

class VectorFloatMaskMicroOp : public VectorArithMicroInst
{
public:
    uint8_t micro_idx;
    VectorFloatMaskMicroOp(const char *mnem, ExtMachInst extMachInst,
            OpClass __opClass, uint8_t _micro_vl, uint8_t _micro_idx)
        : VectorArithMicroInst(mnem, extMachInst, __opClass, _micro_vl),
        micro_idx(_micro_idx)
    {}
    using VectorArithMicroInst::generateDisassembly;
};

}
}
#endif
