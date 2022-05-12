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

#ifndef __ARCH_RISCV_INSTS_VECTOR_HH__
#define __ARCH_RISCV_INSTS_VECTOR_HH__

#include <string>

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/utility.hh"
#include "cpu/exec_context.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

/**
 * Base class for Vector Config operations
 */
class VConfOp : public RiscvStaticInst
{
  protected:
    uint64_t bit30;
    uint64_t bit31;
    uint64_t zimm;
    uint64_t uimm;
    VConfOp(const char *mnem, ExtMachInst _extMachInst, OpClass __opClass)
        : RiscvStaticInst(mnem, _extMachInst, __opClass),
          bit30(_extMachInst.bit30), bit31(_extMachInst.bit31),
          zimm(_extMachInst.zimm_vsetivli), uimm(_extMachInst.uimm_vsetivli)
    {}

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

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
    uint8_t microVl;
    uint8_t microIdx;
    uint8_t vtype;
    VectorMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                    uint8_t _microVl, uint8_t _microIdx)
        : RiscvMicroInst(mnem, _machInst, __opClass),
        microVl(_microVl),
        microIdx(_microIdx),
        vtype(_machInst.vtype8)
    {
        this->flags[IsVector] = true;
    }
};

class VectorArithMicroInst : public VectorMicroInst
{
protected:
    VectorArithMicroInst(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass, uint8_t _microVl,
                         uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorArithMacroInst : public VectorMacroInst
{
  protected:
    VectorArithMacroInst(const char* mnem, ExtMachInst _machInst,
                         OpClass __opClass)
        : VectorMacroInst(mnem, _machInst, __opClass)
    {
        this->flags[IsVector] = true;
    }

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorMemMicroInst : public VectorMicroInst
{
  protected:
    VectorMemMicroInst(const char* mnem, ExtMachInst _machInst,
                       OpClass __opClass, uint8_t _microVl, uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
    {}
};

class VectorMemMacroInst : public VectorMacroInst
{
  protected:
    VectorMemMacroInst(const char* mnem, ExtMachInst _machInst,
                       OpClass __opClass)
        : VectorMacroInst(mnem, _machInst, __opClass)
    {}
};

class VleMacroInst : public VectorMemMacroInst
{
  protected:
    VleMacroInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : VectorMemMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VseMacroInst : public VectorMemMacroInst
{
  protected:
    VseMacroInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : VectorMemMacroInst(mnem, _machInst, __opClass)
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

    VleMicroInst(const char *mnem, ExtMachInst _machInst,
            OpClass __opClass, uint32_t _offset, uint8_t _dstReg,
            uint8_t _microVl, uint8_t _microIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
        , offset(_offset), dst_reg(_dstReg), memAccessFlags(0)
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

    VseMicroInst(const char *mnem, ExtMachInst _machInst,
            OpClass __opClass, uint32_t _offset, uint8_t _srcReg,
            uint8_t _microVl, uint8_t _microIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
        , offset(_offset) , src_reg(_srcReg), memAccessFlags(0)
    {}

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VlWholeMacroInst : public VectorMemMacroInst
{
  protected:
    VlWholeMacroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass)
      : VectorMemMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
      Addr pc, const loader::SymbolTable *symtab) const override;
};

class VlWholeMicroInst : public VectorMemMicroInst
{
  protected:
    uint32_t offset;
    Request::Flags memAccessFlags;

    VlWholeMicroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint32_t _offset, uint8_t _microIdx)
      : VectorMemMicroInst(
          mnem, _machInst, __opClass, NumVecMemInternalRegs, _microIdx)
      , offset(_offset), memAccessFlags(0)
    {}

    std::string generateDisassembly(
      Addr pc, const loader::SymbolTable *symtab) const override;
};

class VsWholeMacroInst : public VectorMemMacroInst
{
  protected:
    VsWholeMacroInst(const char *mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : VectorMemMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VsWholeMicroInst : public VectorMemMicroInst
{
  protected:
    uint32_t offset;
    Request::Flags memAccessFlags;

    VsWholeMicroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint32_t _offset, uint8_t _microIdx)
        : VectorMemMicroInst(
            mnem, _machInst, __opClass, NumVecMemInternalRegs, _microIdx)
        , offset(_offset), memAccessFlags(0)
    {}

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VldMvMicroInst : public RiscvMicroInst
{
  private:
    RegId srcRegIdxArr[NumVecMemInternalRegs];
    RegId destRegIdxArr[1];
    uint8_t src_num;
  public:
    VldMvMicroInst(ExtMachInst _machInst, uint8_t _dstReg, uint8_t _src_num)
        : RiscvMicroInst("vl_mv_micro", _machInst, VectorDummyOp)
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

        setDestRegIdx(_numDestRegs++, RegId(VecRegClass, _dstReg));
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

class VstMvMicroInst : public RiscvMicroInst
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[NumVecMemInternalRegs];
    uint8_t dst_num;
  public:
    VstMvMicroInst(ExtMachInst _machInst, uint8_t _srcReg, uint8_t _dst_num)
        : RiscvMicroInst("vs_mv_micro", _machInst, VectorDummyOp)
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
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, _srcReg));
        this->flags[IsVector] = true;
    }
    Fault execute(ExecContext *, Trace::InstRecord *) const override;
    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

} // namespace RiscvISA
} // namespace gem5


#endif // __ARCH_RISCV_INSTS_VECTOR_HH__