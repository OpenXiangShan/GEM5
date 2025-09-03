#ifndef __ARCH_RISCV_FUSION_INST_HH__
#define __ARCH_RISCV_FUSION_INST_HH__

#include <string>
#include <typeindex>
#include <variant>

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/utility.hh"
#include "cpu/exec_context.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

class FusionInst : public RiscvStaticInst
{
  protected:
    RegId destRegIdxArr[2];
    RegId srcRegIdxArr[4];

    const o3::DynInstPtr first, second;
    o3::DynInst *fused = nullptr;  // do not use DynInstPtr here, as it will cause a circular dependency
  public:
    FusionInst(const char *name, OpClass op, const o3::DynInstPtr &first, const o3::DynInstPtr &second)
        : RiscvStaticInst(name, 0, op), first(first), second(second)
    {
        setRegIdxArrays(reinterpret_cast<RegIdArrayPtr>(&std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
                        reinterpret_cast<RegIdArrayPtr>(&std::remove_pointer_t<decltype(this)>::destRegIdxArr));
    }

    void setFusedInst(o3::DynInstPtr &inst)
    {
        flags[IsFusion] = true;
        fused = inst.get();
    }

    // true if aligned
    virtual bool correctMisalign(Addr addr) const { return false; }

    Addr getSecondPC() const;

    std::string generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const override {
        std::string str = std::string(mnemonic);
        for (int i=0; i < _numDestRegs; i++) {
            if (i == 0)
                str += " (" + registerName(destRegIdxArr[i]);
            else
                str += "," + registerName(destRegIdxArr[i]);
        }
        str += ")";
        for (int i=0; i < _numSrcRegs; i++) {
            if (i == 0)
                str += " (" + registerName(srcRegIdxArr[i]);
            else
                str += "," + registerName(srcRegIdxArr[i]);
        }
        str += ")";
        return str;
    }
};

struct FusionKey
{
    std::type_index type;
    bool ignore_imm;
    int imm;
    FusionKey() : type(typeid(void)), ignore_imm(true), imm(0) {}
    FusionKey(std::type_index t) : type(t), ignore_imm(true), imm(0) {}
    FusionKey(std::type_index t, int i) : type(t), ignore_imm(false), imm(i) {}

    // hash
    std::size_t operator()(const FusionKey& t) const {
        return (std::size_t)t.type.name();
    }

    bool operator==(const FusionKey& other) const {
        // other is the key in map
        return type == other.type && (imm == other.imm || other.ignore_imm);
    }
};

class FusionTag;

typedef StaticInstPtr (*FuseCreator)(const std::vector<o3::DynInstPtr> &subInsts);

using FusionVal = std::variant<FuseCreator, FusionTag*>;
class FusionTag: public std::unordered_map<FusionKey, FusionVal, FusionKey>
{
public:
    FusionTag(std::initializer_list<std::pair<const FusionKey, FusionVal>> init)
        : std::unordered_map<FusionKey, FusionVal, FusionKey>(init) {}
};

extern const std::unordered_map<std::type_index, std::type_index> deCompressMap;
extern const FusionTag fusionMap;

}

}

#endif
