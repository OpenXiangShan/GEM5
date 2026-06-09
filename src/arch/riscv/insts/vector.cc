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

#include "arch/riscv/insts/vector.hh"

#include <algorithm>
#include <sstream>
#include <string>

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/utility.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

namespace
{

inline std::string
vecGroupName(RegIndex first, int8_t vlmul)
{
    std::stringstream ss;
    const uint8_t regs = vlmul < 0 ? 1 : (1u << vlmul);
    ss << "v" << first;
    if (regs > 1) {
        ss << "-v" << (first + regs - 1);
    }
    return ss.str();
}

inline std::string
vecRegRangeName(RegIndex first, uint32_t regs)
{
    std::stringstream ss;
    ss << "v" << first;
    if (regs > 1) {
        ss << "-v" << (first + regs - 1);
    }
    return ss.str();
}

inline bool
isSegmentMemMnemonic(const char *mnem)
{
    return mnem && (strstr(mnem, "seg") != nullptr);
}

inline uint32_t
segmentGroupRegs(int8_t vlmul, uint32_t nf)
{
    const uint32_t regs_per_field = vlmul < 0 ? 1 : (1u << vlmul);
    return regs_per_field * nf;
}

inline bool
hasSuffix(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline std::string
segMemMnemonicForDisplay(const char *mnem, const ExtMachInst &machInst)
{
    std::string m = mnem ? mnem : "";
    const uint32_t nf = machInst.nf + 1;
    if (nf <= 1 || m.empty()) {
        return m;
    }

    bool is_micro = false;
    if (hasSuffix(m, "_micro")) {
        is_micro = true;
        m.resize(m.size() - 6);
    }

    if (!hasSuffix(m, "_v")) {
        return is_micro ? (m + "_micro") : m;
    }
    std::string core = m.substr(0, m.size() - 2);

    auto rewriteWithWidth = [&core, nf](const std::string &from,
                                        const std::string &to_prefix,
                                        const std::string &to_mid) -> bool {
        if (core.rfind(from, 0) != 0) {
            return false;
        }
        std::string tail = core.substr(from.size());
        core = to_prefix + std::to_string(nf) + to_mid + tail;
        return true;
    };

    bool rewritten =
        rewriteWithWidth("vluxei", "vluxseg", "ei") ||
        rewriteWithWidth("vloxei", "vloxseg", "ei") ||
        rewriteWithWidth("vsuxei", "vsuxseg", "ei") ||
        rewriteWithWidth("vsoxei", "vsoxseg", "ei") ||
        rewriteWithWidth("vlse", "vlsseg", "e") ||
        rewriteWithWidth("vsse", "vssseg", "e") ||
        rewriteWithWidth("vle", "vlseg", "e") ||
        rewriteWithWidth("vse", "vsseg", "e");

    std::string out = rewritten ? (core + "_v") : m;
    if (is_micro) {
        out += "_micro";
    }
    return out;
}

inline std::string
vecMicroSrcGroupName(const VectorMicroInst *inst)
{
    if (!inst || inst->vregsrcIdx < 0 || inst->vregsrcNum <= 0) {
        return "";
    }

    std::stringstream ss;
    ss << registerName(inst->srcRegIdx(inst->vregsrcIdx));
    if (inst->vregsrcNum > 1) {
        ss << "-" << registerName(
            inst->srcRegIdx(inst->vregsrcIdx + inst->vregsrcNum - 1));
    }
    return ss.str();
}

inline std::string
intRegName(RegIndex idx)
{
    if (idx >= int_reg::NumArchRegs) {
        std::stringstream ss;
        ss << "?? (x" << idx << ")";
        return ss.str();
    }
    return int_reg::RegNames[idx];
}

inline std::string
floatRegName(RegIndex idx)
{
    if (idx >= float_reg::NumRegs) {
        std::stringstream ss;
        ss << "?? (f" << idx << ")";
        return ss.str();
    }
    return float_reg::RegNames[idx];
}

} // anonymous namespace

std::string
VConfOp::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", ";
    if (bit31 && bit30 == 0) {
        ss << registerName(srcRegIdx(0)) << ", " << registerName(srcRegIdx(1));
    } else if (bit31 && bit30) {
        ss << uimm << ", " << generateZimmDisassembly();
    } else {
        ss << registerName(srcRegIdx(0)) << ", " << generateZimmDisassembly();
    }
    return ss.str();
}

std::string
VConfOp::generateZimmDisassembly() const
{
    std::stringstream s;

    // VSETIVLI uses ZIMM10 and VSETVLI uses ZIMM11
    uint64_t zimm = (bit31 && bit30) ? zimm10 : zimm11;
    const auto vsew_bits = bits(zimm, 5, 3);
    const auto vlmul_bits = bits(zimm, 2, 0);
    const bool frac_lmul = bits(zimm, 2);
    const bool invalid_vsew = vsew_bits > 0x3;
    const bool invalid_vlmul = vlmul_bits == 0x4;
    auto vta = bits(zimm, 6) == 1 ? "ta" : "tu";
    auto vma = bits(zimm, 7) == 1 ? "ma" : "mu";

    if (invalid_vsew || invalid_vlmul) {
        s << "invalid"
          << "(zimm=" << std::showbase << std::hex << zimm << std::dec
          << ", vsew=" << vsew_bits
          << ", vlmul=" << vlmul_bits
          << ", " << vta << ", " << vma << ")";
        return s.str();
    }

    int sew = 1 << (vsew_bits + 3);
    int lmul = bits(zimm, 1, 0);
    s << "e" << sew;
    if (frac_lmul) {
        std::string lmul_str = "";
        switch(lmul){
        case 3:
            lmul_str = "f2";
            break;
        case 2:
            lmul_str = "f4";
            break;
        case 1:
            lmul_str = "f8";
            break;
        default:
            panic("Unexpected fractional LMUL encoding in vector "
                  "configuration disassembly: zimm=%#x lmul_bits=%d",
                  zimm, lmul);
        }
        s << ", m" << lmul_str;
    } else {
        s << ", m" << (1 << lmul);
    }
    s << ", " << vta << ", " << vma;
    return s.str();
}

std::string
VectorNonSplitInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0));
    for (int i = 0; i < _numSrcRegs; ++i) {
        // Skip implicit operands used for execution bookkeeping.
        const bool is_vm_src = (vmsrcIdx >= 0) &&
            (i >= vmsrcIdx && i < vmsrcIdx + (int)VregBanks);
        if (i == oldDstIdx || i == vlsrcIdx || i == vtypesrcIdx || is_vm_src) {
            continue;
        }
        ss << ", " << registerName(srcRegIdx(i));
    }
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorArithMicroInst::generateDisassembly(Addr pc,
        const Loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", ";
    if (machInst.funct3 == 0x3) {
        // OPIVI
      ss  << registerName(srcRegIdx(0)) << ", "
          << static_cast<int64_t>(machInst.vecimm);
    } else {
      ss  << registerName(srcRegIdx(1)) << ", " << registerName(srcRegIdx(0));
    }
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorArithMacroInst::generateDisassembly(Addr pc,
        const Loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const char *mnem = mnemonic;
    const bool is_vi = (strstr(mnem, "_vi") != nullptr);
    const bool is_vx = (strstr(mnem, "_vx") != nullptr);
    const bool is_vf = (strstr(mnem, "_vf") != nullptr);
    const bool is_reduction =
        (strstr(mnem, "vred") == mnem) ||
        (strstr(mnem, "vfred") == mnem) ||
        (strstr(mnem, "vwred") == mnem) ||
        (strstr(mnem, "vfwred") == mnem);

    ss << mnemonic << ' ';
    if (is_reduction) {
        ss << "v" << static_cast<uint32_t>(machInst.vd);
    } else {
        ss << vecGroupName(machInst.vd, vlmul);
    }
    ss << ", "
       << vecGroupName(machInst.vs2, vlmul);

    if (is_vi) {
        ss << ", " << static_cast<int64_t>(machInst.vecimm);
    } else if (is_vx) {
        ss << ", " << intRegName(machInst.rs1);
    } else if (is_vf) {
        ss << ", " << floatRegName(machInst.rs1);
    } else {
        if (is_reduction) {
            ss << ", v" << static_cast<uint32_t>(machInst.vs1);
        } else {
            ss << ", " << vecGroupName(machInst.vs1, vlmul);
        }
    }
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorVMUNARY0MicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0));
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorVMUNARY0MacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << vecGroupName(machInst.vd, vlmul);
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorSlideMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) <<  ", ";
    if (machInst.funct3 == 0x3) {
      ss  << registerName(srcRegIdx(0)) << "|" << registerName(srcRegIdx(1))
          << ", " << static_cast<int64_t>(machInst.vecimm);
    } else {
      ss  << registerName(srcRegIdx(1)) << "|" << registerName(srcRegIdx(2)) << ", " << registerName(srcRegIdx(0));
    }
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorSlideMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const char *mnem = mnemonic;
    const bool is_vi = (strstr(mnem, "_vi") != nullptr);
    const bool is_vx = (strstr(mnem, "_vx") != nullptr);
    const bool is_vf = (strstr(mnem, "_vf") != nullptr);

    ss << mnemonic << ' ' << vecGroupName(machInst.vd, vlmul) << ", "
       << vecGroupName(machInst.vs2, vlmul);
    if (is_vi) {
        ss << ", " << static_cast<int64_t>(machInst.vecimm);
    } else if (is_vx) {
        ss << ", " << intRegName(machInst.rs1);
    } else if (is_vf) {
        ss << ", " << floatRegName(machInst.rs1);
    } else {
        ss << ", " << vecGroupName(machInst.vs1, vlmul);
    }
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VleMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const uint32_t eew_bits = width_EEW(machInst.width);
    const uint32_t eew_bytes = eew_bits / 8;
    const uint32_t elems_per_vbuf_bank = DPLEN / eew_bits;
    const uint32_t nf = machInst.nf + 1;
    const uint32_t token_start = vmi.fn;
    const uint32_t max_tokens_per_micro =
        std::max(1u, static_cast<uint32_t>(DPLENB / eew_bytes));
    const uint32_t token_count = std::min<uint32_t>(
        max_tokens_per_micro,
        static_cast<uint32_t>(vmi.re - vmi.rs) * nf > token_start ?
            static_cast<uint32_t>(vmi.re - vmi.rs) * nf - token_start : 0);
    const RegIndex dst_base = numDestRegs() > 0 ? destRegIdx(0).index() : 0;
    auto fieldByteOffset = [&](uint32_t field) {
        for (uint32_t t = 0; t < token_count; ++t) {
            const uint32_t g = token_start + t;
            if ((g % nf) == field) {
                const uint32_t elem_off = g / nf;
                return ((vmi.rs % elems_per_vbuf_bank) + elem_off) * eew_bytes;
            }
        }
        return (vmi.rs % elems_per_vbuf_bank) * eew_bytes;
    };
    auto formatDst = [&](int idx, bool force_show) {
        const RegId &dst = destRegIdx(idx);
        const uint32_t field = static_cast<uint32_t>(dst.index() - dst_base);
        if (!force_show && nf > 1 && token_count > 0) {
            bool touched = false;
            for (uint32_t t = 0; t < token_count; ++t) {
                if (((token_start + t) % nf) == field) {
                    touched = true;
                    break;
                }
            }
            if (!touched) {
                return std::string();
            }
        }
        std::stringstream item;
        item << registerName(dst);
        if (dst.is(VecBufRegClass)) {
            const uint32_t byte_off = fieldByteOffset(field);
            item << "@0x" << std::hex << byte_off << std::dec;
        }
        return item.str();
    };
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' ';
    if (numDestRegs() > 1) {
        ss << '{';
        bool any = false;
        for (int i = 0; i < numDestRegs(); ++i) {
            const std::string item = formatDst(i, false);
            if (item.empty()) {
                continue;
            }
            if (any) {
                ss << ", ";
            }
            ss << item;
            any = true;
        }
        if (!any) {
            ss << formatDst(0, true);
        }
        ss << '}';
    } else {
        ss << formatDst(0, true);
    }
    ss << ", " << vmi.offset << '(' << registerName(srcRegIdx(0)) << ')';
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VleffMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const uint32_t eew_bits = width_EEW(machInst.width);
    const uint32_t eew_bytes = eew_bits / 8;
    const uint32_t elems_per_vbuf_bank = DPLEN / eew_bits;
    const uint32_t nf = machInst.nf + 1;
    const uint32_t token_start = vmi.fn;
    const uint32_t max_tokens_per_micro =
        std::max(1u, static_cast<uint32_t>(DPLENB / eew_bytes));
    const uint32_t token_count = std::min<uint32_t>(
        max_tokens_per_micro,
        static_cast<uint32_t>(vmi.re - vmi.rs) * nf > token_start ?
            static_cast<uint32_t>(vmi.re - vmi.rs) * nf - token_start : 0);
    const uint32_t data_dests = std::min<uint32_t>(nf, numDestRegs());
    const RegIndex dst_base = data_dests > 0 ? destRegIdx(0).index() : 0;
    auto fieldByteOffset = [&](uint32_t field) {
        for (uint32_t t = 0; t < token_count; ++t) {
            const uint32_t g = token_start + t;
            if ((g % nf) == field) {
                const uint32_t elem_off = g / nf;
                return ((vmi.rs % elems_per_vbuf_bank) + elem_off) * eew_bytes;
            }
        }
        return (vmi.rs % elems_per_vbuf_bank) * eew_bytes;
    };
    auto formatDst = [&](int idx, bool force_show) {
        const RegId &dst = destRegIdx(idx);
        const uint32_t field = static_cast<uint32_t>(dst.index() - dst_base);
        if (!force_show && nf > 1 && token_count > 0) {
            bool touched = false;
            for (uint32_t t = 0; t < token_count; ++t) {
                if (((token_start + t) % nf) == field) {
                    touched = true;
                    break;
                }
            }
            if (!touched) {
                return std::string();
            }
        }
        std::stringstream item;
        item << registerName(dst);
        if (dst.is(VecBufRegClass)) {
            const uint32_t byte_off = fieldByteOffset(field);
            item << "@0x" << std::hex << byte_off << std::dec;
        }
        return item.str();
    };
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' ';
    if (data_dests > 1) {
        ss << '{';
        bool any = false;
        for (uint32_t i = 0; i < data_dests; ++i) {
            const std::string item = formatDst(i, false);
            if (item.empty()) {
                continue;
            }
            if (any) {
                ss << ", ";
            }
            ss << item;
            any = true;
        }
        if (!any) {
            ss << formatDst(0, true);
        }
        ss << '}';
    } else {
        ss << formatDst(0, true);
    }
    ss << ", " << vmi.offset << '(' << registerName(srcRegIdx(0)) << ')';
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VlWholeMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
       << DPLENB * microIdx << '(' << registerName(srcRegIdx(0)) << ')'
       << ", rs:" << vmi.rs << ", " << vmi.re;
    return ss.str();
}

std::string VseMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const std::string src_vec = vecMicroSrcGroupName(this);
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' '
       << (src_vec.empty() ? registerName(srcRegIdx(1)) : src_vec) << ", "
       << vmi.offset  << '(' << registerName(srcRegIdx(0)) << ')';
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VsWholeMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(srcRegIdx(1)) << ", "
       << DPLENB * microIdx << '(' << registerName(srcRegIdx(0)) << ')'
       << ", rs:" << vmi.rs << ", " << vmi.re;
    return ss.str();
}

std::string VleMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const bool seg = isSegmentMemMnemonic(mnemonic);
    const uint32_t dst_regs = seg ?
        segmentGroupRegs(vlmul, machInst.nf + 1) :
        (vlmul < 0 ? 1 : (1u << vlmul));
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' '
       << vecRegRangeName(machInst.vd, dst_regs) << ", "
       << '(' << intRegName(machInst.rs1) << ')';
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VlWholeMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << vecGroupName(machInst.vd, vlmul) << ", "
       << '(' << intRegName(machInst.rs1) << ')';
    return ss.str();
}

std::string VseMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const bool seg = isSegmentMemMnemonic(mnemonic);
    const uint32_t src_regs = seg ?
        segmentGroupRegs(vlmul, machInst.nf + 1) :
        (vlmul < 0 ? 1 : (1u << vlmul));
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' '
       << vecRegRangeName(machInst.vs3, src_regs) << ", "
       << '(' << intRegName(machInst.rs1) << ')';
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VsWholeMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << vecGroupName(machInst.vs3, vlmul) << ", "
       << '(' << intRegName(machInst.rs1) << ')';
    return ss.str();
}

std::string VlStrideMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const bool seg = isSegmentMemMnemonic(mnemonic);
    const uint32_t dst_regs = seg ?
        segmentGroupRegs(vlmul, machInst.nf + 1) :
        (vlmul < 0 ? 1 : (1u << vlmul));
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' '
       << vecRegRangeName(machInst.vd, dst_regs) << ", "
       << '(' << intRegName(machInst.rs1) << ')'
       << ", " << intRegName(machInst.rs2);
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VlStrideMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const uint32_t eew_bits = width_EEW(machInst.width);
    const uint32_t eew_bytes = eew_bits / 8;
    const uint32_t elems_per_vbuf_bank = DPLEN / eew_bits;
    const uint32_t vbuf_byte_offset =
        (vmi.rs % elems_per_vbuf_bank) * eew_bytes;
    auto formatDst = [&](int idx) {
        const RegId &dst = destRegIdx(idx);
            ss << registerName(dst);
        if (dst.is(VecBufRegClass)) {
            ss << "@0x" << std::hex << vbuf_byte_offset << std::dec;
        }
    };
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' ';
    if (numDestRegs() > 1) {
        ss << '{';
        for (int i = 0; i < numDestRegs(); ++i) {
            if (i) ss << ", ";
            formatDst(i);
        }
        ss << '}';
    } else {
        formatDst(0);
    }
    ss << ", " << '(' << registerName(srcRegIdx(0)) << ')' <<
        ", "<< registerName(srcRegIdx(1));
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VsStrideMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const bool seg = isSegmentMemMnemonic(mnemonic);
    const uint32_t src_regs = seg ?
        segmentGroupRegs(vlmul, machInst.nf + 1) :
        (vlmul < 0 ? 1 : (1u << vlmul));
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' '
       << vecRegRangeName(machInst.vs3, src_regs) << ", "
       << '(' << intRegName(machInst.rs1) << ')'
       << ", " << intRegName(machInst.rs2);
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VsStrideMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const std::string src_vec = vecMicroSrcGroupName(this);
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' '
       << (src_vec.empty() ? registerName(srcRegIdx(2)) : src_vec) << ", " <<
        '(' << registerName(srcRegIdx(0)) << ')' <<
        ", "<< registerName(srcRegIdx(1));
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VlIndexMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const bool seg = isSegmentMemMnemonic(mnemonic);
    const uint32_t dst_regs = seg ?
        segmentGroupRegs(vlmul, machInst.nf + 1) :
        (vlmul < 0 ? 1 : (1u << vlmul));
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' '
       << vecRegRangeName(machInst.vd, dst_regs) << ", "
       << '(' << intRegName(machInst.rs1) << "),"
       << vecGroupName(machInst.vs2, vlmul);
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VlIndexMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const uint32_t data_bits = getSew(machInst.vtype8.vsew);
    const uint32_t data_bytes = data_bits / 8;
    const uint32_t elems_per_vbuf_bank = DPLEN / data_bits;
    const uint32_t vbuf_elem_idx = vmi.rs % elems_per_vbuf_bank;
    const uint32_t vbuf_byte_offset = vbuf_elem_idx * data_bytes;
    uint32_t vdElemIdx = vmi.rs % (VLEN / data_bits);
    uint32_t vs2ElemIdx = vmi.rs % (VLEN / width_EEW(machInst.width));
    auto formatDst = [&](int idx) {
        const RegId &dst = destRegIdx(idx);
        ss << registerName(dst);
        if (dst.is(VecBufRegClass)) {
            ss << "@0x" << std::hex << vbuf_byte_offset << std::dec;
        } else {
            ss << "[" << uint16_t(vdElemIdx) << "]";
        }
    };
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' ';
    if (numDestRegs() > 1) {
        ss << '{';
        for (int i = 0; i < numDestRegs(); ++i) {
            if (i) ss << ", ";
            formatDst(i);
        }
        ss << "}, ";
    } else {
        formatDst(0);
        ss << ", ";
    }
    ss
        << '(' << registerName(srcRegIdx(0)) << "), "
        << registerName(srcRegIdx(1)) << "[" << uint16_t(vs2ElemIdx) << "]";
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VsIndexMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const bool seg = isSegmentMemMnemonic(mnemonic);
    const uint32_t src_regs = seg ?
        segmentGroupRegs(vlmul, machInst.nf + 1) :
        (vlmul < 0 ? 1 : (1u << vlmul));
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' '
       << vecRegRangeName(machInst.vs3, src_regs) << ", "
       << '(' << intRegName(machInst.rs1) << "),"
       << vecGroupName(machInst.vs2, vlmul);
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VsIndexMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    uint32_t vs3ElemIdx = vmi.rs % (VLEN / getSew(machInst.vtype8.vsew));
    uint32_t vs2ElemIdx = vmi.rs % (VLEN / width_EEW(machInst.width));
    const std::string src_vec = vecMicroSrcGroupName(this);
    ss << segMemMnemonicForDisplay(mnemonic, machInst) << ' '
        << (src_vec.empty() ? registerName(srcRegIdx(2)) : src_vec) <<
        "[" << uint16_t(vs3ElemIdx) << "], "
        << '(' << registerName(srcRegIdx(0)) << "), "
        << registerName(srcRegIdx(1)) << "[" << uint16_t(vs2ElemIdx) << "]";
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string
VMvWholeMacroInst::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << vecGroupName(machInst.vd, vlmul) << ", "
       << vecGroupName(machInst.vs2, vlmul);
    return ss.str();
}

std::string
VMvWholeMicroInst::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        registerName(srcRegIdx(1));
    return ss.str();
}

VleffEndMicroInst::VleffEndMicroInst(ExtMachInst extMachInst, uint8_t _numSrcs,
                                     uint8_t _fofSrcBase,
                                     bool _packedFaultSlots,
                                     uint8_t _packedFaultCount)
    : VectorMicroInst("VleffEnd", extMachInst,
    VectorMisc0Op, 0)
{
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
    _numSrcRegs = 0;
    _numDestRegs = 0;
    setDestRegIdx(_numDestRegs++, VecRenamedVLReg);
    _numTypedDestRegs[RMiscRegClass]++;
    this->fofSrcBase = _fofSrcBase;
    this->packedFaultSlots = _packedFaultSlots;
    this->packedFaultCount = _packedFaultCount;
    if (packedFaultSlots) {
        panic_if(this->fofSrcBase >= NumVecBufRegs,
                 "VleffEnd packed source index %u exceeds vecbuf size %u",
                 this->fofSrcBase, NumVecBufRegs);
        panic_if(this->packedFaultCount == 0,
                 "VleffEnd packedFaultCount must be non-zero");
        panic_if(this->packedFaultCount > DPLENB,
                 "VleffEnd packedFaultCount=%u exceeds DPLENB=%u",
                 this->packedFaultCount, (uint32_t)DPLENB);
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, this->fofSrcBase));
    } else {
        panic_if(static_cast<uint32_t>(this->fofSrcBase) +
                 static_cast<uint32_t>(_numSrcs) > NumVecBufRegs,
                 "VleffEnd source range [%u, %u) exceeds vecbuf size %u",
                 this->fofSrcBase, this->fofSrcBase + _numSrcs, NumVecBufRegs);
        for (uint8_t i = 0; i < _numSrcs; i++) {
            setSrcRegIdx(_numSrcRegs++,
                         RegId(VecBufRegClass, this->fofSrcBase + i));
        }
    }
    this->numSrcs = _numSrcs;
    this->vlsrcIdx = _numSrcRegs;
    setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);

}

Fault
VleffEndMicroInst::execute(ExecContext* xc, Trace::InstRecord* traceData) const
{
    vreg_t cnt[NumVecBufRegs];
    const uint8_t fof_src_regs = packedFaultSlots ? 1 : this->numSrcs;
    for (uint8_t i = 0; i < fof_src_regs; i++) {
        xc->getRegOperand(this, i, cnt + i);
    }

    uint64_t new_vl = 0;
    assert(vlsrcIdx >= 0);
    uint64_t old_vl = xc->getRegOperand(this, vlsrcIdx);
    uint64_t final_vl = old_vl;
    if (packedFaultSlots) {
        auto packed = cnt[0].as<uint8_t>();
        const uint8_t slot_count = std::min<uint8_t>(numSrcs, packedFaultCount);
        for (uint8_t i = 0; i < slot_count; i++) {
            new_vl = packed[i];
            if ((new_vl > 0) && (new_vl < old_vl)) {
                final_vl = new_vl;
                break;
            }
        }
    } else {
        for (uint8_t i = 0; i < this->numSrcs; i++) {
            new_vl = cnt[i].as<uint64_t>()[0];
            if ((new_vl > 0) && (new_vl < old_vl)) {
                final_vl = new_vl;
                break;
            }
        }
    }

    xc->setRegOperand(this, 0, final_vl);

    if (traceData) {
        // Always emit the post-vleff VL in trace output.
        traceData->setData(final_vl);
    }

    return NoFault;
}

std::string
VleffEndMicroInst::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic;
    return ss.str();
}

// VregMergeMicroInst implementation
template<typename ElemType>
VregMergeMicroInst<ElemType>::VregMergeMicroInst(
    ExtMachInst extMachInst,
    uint8_t _numSrcs,
    VectorMicroInfo& _vmi,
    bool _maskMerge,
    uint8_t _srcStart,
    bool _maskUseVm)
    : VectorArithMicroInst("vreg_merge_micro", extMachInst,
                          VectorMisc0Op, 0)
{
    // 保存VectorMicroInfo信息
    this->vmi = _vmi;
    this->maskMerge = _maskMerge;
    this->maskUseVm = _maskUseVm;

    // 设置寄存器索引数组
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

    // 初始化寄存器计数
    _numSrcRegs = 0;
    _numDestRegs = 0;

    // 设置目标寄存器（从vmi.microVd获取）
    setDestRegIdx(_numDestRegs++, RegId(VecRegClass, _vmi.microVd));
    _numTypedDestRegs[VecRegClass]++;

    // 设置源寄存器（多个vtemp寄存器）
    for (uint8_t i = 0; i < _numSrcs; i++) {
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, _srcStart + i));
    }

    // 设置oldDst源（用于COPY_OLD_VD）
    this->oldDstIdx = _numSrcRegs;
    setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, _vmi.microVd));

    // 设置VL源（用于获取向量长度）
    this->vlsrcIdx = _numSrcRegs;
    setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
    this->vtypesrcIdx = _numSrcRegs;
    setSrcRegIdx(_numSrcRegs++, VecRenamedVTYPEReg);

    // 设置VSTART源（用于获取向量起始位置）
    this->vstartsrcIdx = _numSrcRegs;
    setSrcRegIdx(_numSrcRegs++, VecRenamedVSTARTReg);

    // 设置VM源（用于VM_REQUIRED）
    if (!this->vm) {
        this->vmsrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 0));
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 1));
    }

    // 最终检查：确保总数没有超出数组大小
    panic_if(_numSrcRegs > MAX_SRC_REGS,
             "VregMergeMicroInst: Total source registers (%d) exceed array size (%d)!",
             _numSrcRegs, MAX_SRC_REGS);
    panic_if(_numDestRegs > MAX_DEST_REGS,
             "VregMergeMicroInst: Total destination registers (%d) exceed array size (%d)!",
             _numDestRegs, MAX_DEST_REGS);
}

template<typename ElemType>
Fault VregMergeMicroInst<ElemType>::execute(
    ExecContext* xc,
    Trace::InstRecord* traceData) const
{
    // 1. 获取目标寄存器Vd的可写引用
    auto& tmp_d0 = *(RiscvISA::VecRegContainer *)
                    xc->getWritableRegOperand(this, 0);
    auto Vd = tmp_d0.as<ElemType>();
    auto VdBytes = tmp_d0.as<uint8_t>();

    // 2. 获取向量长度rVl
    assert(vlsrcIdx > 0);
    uint64_t rVl = xc->getRegOperand(this, vlsrcIdx);
    assert(vtypesrcIdx > 0);
    VTYPE rVtype = xc->getRegOperand(this, vtypesrcIdx);

    // 2.5. 获取向量起始位置rVstart
    uint64_t rVstart = 0;
    assert(vstartsrcIdx > 0);
    rVstart = xc->getRegOperand(this, vstartsrcIdx);

    // 3. VM_REQUIRED() - 获取掩码寄存器v0
    uint8_t tmp_v0_storage[VLENB];
    uint8_t *v0 = nullptr;
    if (!this->vm) {
        assert(vmsrcIdx > 0);
        for (uint32_t _b = 0; _b < VregBanks; ++_b) {
            RiscvISA::vreg_t v0_bank;
            xc->getRegOperand(this, vmsrcIdx + _b, &v0_bank);
            memcpy(tmp_v0_storage + _b * DPLENB, &v0_bank, DPLENB);
        }
        v0 = tmp_v0_storage;
    }

    // 4. COPY_OLD_VD() - 复制旧的Vd值（用于tail和inactive元素）
    RiscvISA::vreg_t old_vd;
    decltype(Vd) old_Vd = nullptr;
    assert(oldDstIdx > 0);
    xc->getRegOperand(this, oldDstIdx, &old_vd);
    old_Vd = old_vd.as<ElemType>();
    memcpy(Vd, old_Vd, DPLENB);

    // 6. 计算实际的vtemp源寄存器数量（排除oldDst, vl, vtype, vstart, vm）
    uint8_t num_extra = 4; // oldDst + vl + vtype + vstart
    if (!this->vm) {
        num_extra += 2; // v0_lo + v0_hi
    }
    uint8_t num_vtemp_srcs = this->_numSrcRegs - num_extra;

    // 7. 计算每个vtemp能容纳的元素数量
    constexpr uint32_t elems_per_bank = DPLENB / sizeof(ElemType);

    // 8. 遍历需要合并的元素范围 [vmi.rs, vmi.re)
    RiscvISA::vreg_t tmp_s[8];

    // 预先读取所有vtemp源寄存器
    for (uint8_t src_idx = 0; src_idx < num_vtemp_srcs; src_idx++) {
        xc->getRegOperand(this, src_idx, &tmp_s[src_idx]);
    }
    // 遍历所有需要合并的元素
    const uint32_t nf = getNF();
    constexpr uint32_t elems_per_vreg = DPLENB / sizeof(ElemType);

    for (uint32_t elem_idx = std::max(static_cast<uint32_t>(vmi.rs),
                                      static_cast<uint32_t>(rVstart));
         elem_idx < vmi.re; elem_idx++) {
        const bool in_vl = (elem_idx < rVl);
        const bool lane_enabled = maskMerge ? (in_vl && (!maskUseVm || this->vm || elem_mask(v0, elem_idx)))
            : (in_vl && (this->vm || elem_mask(v0, elem_idx)));
        if (lane_enabled) {
            if (maskMerge) {
                // Mask merge: each vtemp carries one DPLEN-backed chunk of mask bits.
                const uint32_t local_idx = elem_idx - vmi.rs;
                const uint32_t vtemp_idx = local_idx / elems_per_bank;
                if (vtemp_idx < num_vtemp_srcs) {
                    auto src_bits = tmp_s[vtemp_idx].as<uint8_t>();
                    const uint32_t src_bit_idx = local_idx % elems_per_bank;
                    const uint8_t bit = elem_mask(src_bits, src_bit_idx);
                    const uint32_t dst_bit_idx = elem_idx % DPLEN;
                    const uint32_t byte_idx = dst_bit_idx / 8;
                    const uint8_t bit_off = dst_bit_idx % 8;
                    VdBytes[byte_idx] &= ~(1u << bit_off);
                    VdBytes[byte_idx] |= (bit << bit_off);
                }
            } else {
                uint32_t ei_in_vtemp = (elem_idx - vmi.rs) * nf + vmi.fn;
                uint8_t vtemp_idx = ei_in_vtemp / elems_per_bank;
                uint32_t offset_in_vtemp = ei_in_vtemp % elems_per_bank;
                uint32_t vdElemIdx = (elem_idx % elems_per_vreg);
                auto src_data = tmp_s[vtemp_idx].as<ElemType>();
                Vd[vdElemIdx] = src_data[offset_in_vtemp];
            }
        } else if (RVV_AGNOSTIC && rVtype.vta && rVl > 0 && elem_idx >= rVl) {
            if (maskMerge) {
                const uint32_t dst_bit_idx = elem_idx % DPLEN;
                const uint32_t byte_idx = dst_bit_idx / 8;
                const uint8_t bit_off = dst_bit_idx % 8;
                VdBytes[byte_idx] |= (1u << bit_off);
            } else {
                auto &dst = Vd[elem_idx % elems_per_vreg];
                if constexpr (std::is_same_v<ElemType, float32_t> ||
                              std::is_same_v<ElemType, float64_t>) {
                    dst.v = static_cast<decltype(dst.v)>(~0ULL);
                } else {
                    dst = static_cast<ElemType>(-1);
                }
            }
        }
    }

    // 8. 写回结果
    xc->setRegOperand(this, 0, &tmp_d0);
    if (traceData)
        traceData->setData(tmp_d0);

    return NoFault;
}

template<typename ElemType>
std::string VregMergeMicroInst<ElemType>::generateDisassembly(
    Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0));

    uint8_t num_extra = 4;
    if (!this->vm) {
        num_extra += 2;
    }
    uint8_t num_vtemp_srcs = this->_numSrcRegs - num_extra;

    // 输出所有源寄存器（vtemp）
    for (uint8_t i = 0; i < num_vtemp_srcs; i++) {
        ss << ", " << registerName(srcRegIdx(i));
    }

    // 输出VectorMicroInfo信息
    ss << ", rs:" << vmi.rs
       << ", re:" << vmi.re
       << ", elems_per_vreg:" << (DPLENB / sizeof(ElemType));

    return ss.str();
}

// 显式实例化模板，支持常用的元素类型
template class VregMergeMicroInst<uint8_t>;
template class VregMergeMicroInst<uint16_t>;
template class VregMergeMicroInst<uint32_t>;
template class VregMergeMicroInst<uint64_t>;
template class VregMergeMicroInst<int8_t>;
template class VregMergeMicroInst<int16_t>;
template class VregMergeMicroInst<int32_t>;
template class VregMergeMicroInst<int64_t>;
template class VregMergeMicroInst<float32_t>;
template class VregMergeMicroInst<float64_t>;

// ========== BitMaskMergeMicroInst Implementation ==========
BitMaskMergeMicroInst::BitMaskMergeMicroInst(
    ExtMachInst extMachInst,
    VectorMicroInfo& _vmi,
    uint8_t _srcStart,
    bool _maskUseVm,
    bool _useVstart,
    bool _roundUpVlToByte)
    : VectorArithMicroInst("bitmask_merge_micro", extMachInst,
                          VectorMisc0Op, 0),
      maskUseVm(_maskUseVm),
      useVstart(_useVstart),
      roundUpVlToByte(_roundUpVlToByte)
{
    this->vmi = _vmi;

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

    _numSrcRegs = 0;
    _numDestRegs = 0;

    setDestRegIdx(_numDestRegs++, RegId(VecRegClass, _vmi.microVd));
    _numTypedDestRegs[VecRegClass]++;

    setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, _srcStart));

    this->oldDstIdx = _numSrcRegs;
    setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, _vmi.microVd));

    this->vlsrcIdx = _numSrcRegs;
    setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);

    this->vtypesrcIdx = _numSrcRegs;
    setSrcRegIdx(_numSrcRegs++, VecRenamedVTYPEReg);

    if (useVstart) {
        this->vstartsrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, VecRenamedVSTARTReg);
    }

    if (maskUseVm && !this->vm) {
        this->vmsrcIdx = _numSrcRegs;
        for (uint32_t _b = 0; _b < VregBanks; ++_b) {
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, _b));
        }
    }

    panic_if(_numSrcRegs > MAX_SRC_REGS,
             "BitMaskMergeMicroInst: Total source registers (%d) exceed "
             "array size (%d)!",
             _numSrcRegs, MAX_SRC_REGS);
    panic_if(_numDestRegs > MAX_DEST_REGS,
             "BitMaskMergeMicroInst: Total destination registers (%d) exceed "
             "array size (%d)!",
             _numDestRegs, MAX_DEST_REGS);
}

Fault
BitMaskMergeMicroInst::execute(ExecContext* xc,
                               Trace::InstRecord* traceData) const
{
    auto& tmp_d0 = *(RiscvISA::VecRegContainer *)
                    xc->getWritableRegOperand(this, 0);
    auto VdBytes = tmp_d0.as<uint8_t>();

    RiscvISA::vreg_t old_vd;
    xc->getRegOperand(this, oldDstIdx, &old_vd);
    auto old_VdBytes = old_vd.as<uint8_t>();
    memcpy(VdBytes, old_VdBytes, DPLENB);

    uint64_t rVl = xc->getRegOperand(this, vlsrcIdx);
    // vlm.v loads packed mask bytes, so the merge stage must overwrite all bits
    // in the fetched bytes rather than preserving stale high bits in the last
    // partially used byte.
    const uint64_t activeVlBits =
        roundUpVlToByte ? ((rVl + 7) & ~uint64_t(7)) : rVl;
    VTYPE rVtype = xc->getRegOperand(this, vtypesrcIdx);
    uint64_t rVstart = 0;
    if (useVstart) {
        assert(vstartsrcIdx > 0);
        rVstart = xc->getRegOperand(this, vstartsrcIdx);
    }

    uint8_t tmp_v0_storage[VLENB] = {0};
    uint8_t *v0 = nullptr;
    if (maskUseVm && !this->vm) {
        assert(vmsrcIdx > 0);
        for (uint32_t _b = 0; _b < VregBanks; ++_b) {
            RiscvISA::vreg_t v0_bank;
            xc->getRegOperand(this, vmsrcIdx + _b, &v0_bank);
            memcpy(tmp_v0_storage + _b * DPLENB, &v0_bank, DPLENB);
        }
        v0 = tmp_v0_storage;
    }

    RiscvISA::vreg_t src_vtmp;
    xc->getRegOperand(this, 0, &src_vtmp);
    auto src_bits = src_vtmp.as<uint8_t>();

    for (uint32_t bit_idx = 0; bit_idx < DPLEN; bit_idx++) {
        const uint32_t elem_idx = vmi.rs + bit_idx;
        const uint32_t dst_byte_idx = bit_idx / 8;
        const uint8_t dst_bit_off = bit_idx % 8;
        const bool in_vl = (elem_idx < activeVlBits);
        const bool after_vstart = !useVstart || (elem_idx >= rVstart);
        const bool lane_enabled =
            in_vl && after_vstart &&
            (!maskUseVm || this->vm || elem_mask(v0, elem_idx));

        if (lane_enabled) {
            const uint8_t bit = elem_mask(src_bits, bit_idx);
            VdBytes[dst_byte_idx] &= ~(1u << dst_bit_off);
            VdBytes[dst_byte_idx] |= (bit << dst_bit_off);
        } else if (RVV_AGNOSTIC && rVtype.vta && activeVlBits > 0 &&
                   elem_idx >= activeVlBits) {
            VdBytes[dst_byte_idx] |= (1u << dst_bit_off);
        }
    }

    xc->setRegOperand(this, 0, &tmp_d0);
    if (traceData) {
        traceData->setData(tmp_d0);
    }
    return NoFault;
}

std::string
BitMaskMergeMicroInst::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0))
       << ", " << registerName(srcRegIdx(0))
       << ", rs:" << vmi.rs
       << ", re:" << vmi.re;
    return ss.str();
}

// ========== VectorGatherMicroInst Implementation ==========

std::string
VectorGatherMacroInst::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const char *mnemonic_str = mnemonic;
    const bool is_vv = (strstr(mnemonic_str, "_vv") != nullptr);
    const bool is_vx = (strstr(mnemonic_str, "_vx") != nullptr);
    const bool is_vi = (strstr(mnemonic_str, "_vi") != nullptr);
    const bool is_ei16 = (strstr(mnemonic_str, "vrgatherei16") != nullptr);

    auto appendVecGroup = [&ss](RegIndex first, uint8_t num_regs) {
        ss << "v" << first;
        if (num_regs > 1) {
            ss << "-v" << (first + num_regs - 1);
        }
    };

    const uint8_t vs2_vregs = vlmul < 0 ? 1 : (1 << vlmul);
    const uint8_t vd_vregs = vs2_vregs;

    ss << mnemonic << " ";
    appendVecGroup(machInst.vd, vd_vregs);
    ss << ", ";
    appendVecGroup(machInst.vs2, vs2_vregs);

    if (is_vv) {
        int8_t vs1_emul_raw = vlmul;
        if (is_ei16) {
            const uint32_t vd_eewb = sew / 8;
            int8_t eew_ratio_log2 = 0;
            if (2 > vd_eewb) {
                uint32_t ratio = 2 / vd_eewb;
                while (ratio > 1) {
                    eew_ratio_log2++;
                    ratio >>= 1;
                }
            } else if (2 < vd_eewb) {
                uint32_t ratio = vd_eewb / 2;
                while (ratio > 1) {
                    eew_ratio_log2--;
                    ratio >>= 1;
                }
            }
            vs1_emul_raw = vlmul + eew_ratio_log2;
        }
        const int8_t vs1_emul =
            std::max<int8_t>(-3, std::min<int8_t>(3, vs1_emul_raw));
        const uint8_t vs1_vregs = vs1_emul < 0 ? 1 : (1 << vs1_emul);
        ss << ", ";
        appendVecGroup(machInst.vs1, vs1_vregs);
    } else if (is_vx) {
        ss << ", x" << static_cast<int>(machInst.rs1);
    } else if (is_vi) {
        ss << ", " << static_cast<uint64_t>(sext<5>(bits(machInst, 19, 15)));
    }

    if (!machInst.vm) {
        ss << ", v0.t";
    }
    return ss.str();
}

std::string
VectorGatherMicroInst::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    const char* mnemonic_str = mnemonic;  // mnemonic已经是const char*类型
    bool is_vv = (strstr(mnemonic_str, "_vv") != nullptr);
    bool is_vx = (strstr(mnemonic_str, "_vx") != nullptr);
    bool is_vi = (strstr(mnemonic_str, "_vi") != nullptr);

    // 目标寄存器：使用真实的微操作目的寄存器（区分lo/hi bank）。
    ss << mnemonic << " " << registerName(destRegIdx(0)) << ", ";
    // vs2寄存器组：打印第一个和最后一个寄存器
    ss << registerName(srcRegIdx(vs2srcIdx));

    const uint8_t vs2_vregs = vlmul < 0 ? 1 : 1 << vlmul;
    if (vs2_vregs > 1 && vs2srcIdx >= 0 &&
        srcRegIdx(vs2srcIdx).is(VecRegClass)) {
        // 计算最后一个vs2寄存器
        ss << "-" << registerName(srcRegIdx(vs2srcIdx + vs2_vregs - 1));
    }
    ss << ", ";

    if (is_vv) {
        if (vs1srcIdx >= 0) {
            ss << registerName(srcRegIdx(vs1srcIdx));
        }
    } else if (is_vx) {
        ss << "x" << (int)machInst.rs1;
    } else if (is_vi) {
        ss << (uint64_t)sext<5>(bits(machInst, 19, 15)); // SIMM5
    }

    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}


VBufInsertMicroInst::VBufInsertMicroInst(
        ExtMachInst machInst, RegIndex vbuf_idx,
        RegIndex src_vec_idx, uint32_t _offset, int expected_writes)
    : RiscvMicroInst("vbuf_insert", machInst, VectorMisc0Op),
      offset(_offset % DPLENB)
{
    flags[IsMicroop] = true;
    flags[IsVector] = true;
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

    _numSrcRegs = 0;
    _numDestRegs = 0;

    setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, src_vec_idx));

    const RegIndex slot = vbuf_idx + (_offset / DPLENB);
    panic_if(slot >= NumVecBufRegs,
             "VBufInsert slot %u exceeds NumVecBufRegs %u",
             slot, NumVecBufRegs);
    RegId dst_reg(VecBufRegClass, slot);
    dst_reg.setNumPinnedWrites(std::max(0, expected_writes - 1));
    setDestRegIdx(_numDestRegs++, dst_reg);
    _numTypedDestRegs[VecBufRegClass]++;
}

Fault
VBufInsertMicroInst::execute(ExecContext *xc,
                             Trace::InstRecord *traceData) const
{
    panic_if(offset != 0,
             "VBufInsert expects DPLEN-aligned slot writes, got offset=%u",
             offset);
    VecRegContainer src;
    xc->getRegOperand(this, 0, &src);

    auto *vbuf = static_cast<VecBufRegContainer *>(
        xc->getWritableRegOperand(this, 0));
    auto *vbuf_data = vbuf->as<uint8_t>();
    auto *src_data = src.as<uint8_t>();
    memcpy(vbuf_data + offset, src_data, DPLENB);
    if (traceData) {
        traceData->setData(*vbuf);
    }

    return NoFault;
}

std::string
VBufInsertMicroInst::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << " "
       << registerName(destRegIdx(0)) << ", "
       << registerName(srcRegIdx(0)) << ", offset=" << offset;
    return ss.str();
}

VBufExtractMicroInst::VBufExtractMicroInst(
        ExtMachInst machInst, RegIndex vbuf_idx,
        RegIndex dst_vec_idx, uint32_t _offset, bool merge_old_dest)
    : RiscvMicroInst("vbuf_extract", machInst, VectorMisc0Op),
      offset(_offset % DPLENB),
      bankIdx(_offset / DPLENB),
      mergeOldDest(merge_old_dest)
{
    flags[IsMicroop] = true;
    flags[IsVector] = true;
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

    _numSrcRegs = 0;
    _numDestRegs = 0;

    const RegIndex slot = vbuf_idx + (_offset / DPLENB);
    panic_if(slot >= NumVecBufRegs,
             "VBufExtract slot %u exceeds NumVecBufRegs %u",
             slot, NumVecBufRegs);
    setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, slot));
    if (mergeOldDest) {
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, dst_vec_idx));
        setSrcRegIdx(_numSrcRegs++, VecCompressCntReg);
    }

    setDestRegIdx(_numDestRegs++, RegId(VecRegClass, dst_vec_idx));
    _numTypedDestRegs[VecRegClass]++;
}

Fault
VBufExtractMicroInst::execute(ExecContext *xc,
                              Trace::InstRecord *traceData) const
{
    panic_if(offset != 0,
             "VBufExtract expects DPLEN-aligned slot reads, got offset=%u",
             offset);
    VecBufRegContainer vbuf;
    xc->getRegOperand(this, 0, &vbuf);

    VecRegContainer dst;
    auto *vbuf_data = vbuf.as<uint8_t>();
    auto *dst_data = dst.as<uint8_t>();
    if (!mergeOldDest) {
        memcpy(dst_data, vbuf_data + offset, DPLENB);
    } else {
        VecRegContainer old_dst;
        vreg_t compress_cnt;
        xc->getRegOperand(this, 1, &old_dst);
        xc->getRegOperand(this, 2, &compress_cnt);
        memcpy(dst_data, old_dst.as<uint8_t>(), DPLENB);

        const uint32_t elem_bytes = 1u << machInst.vtype8.vsew;
        const uint32_t elems_per_bank = DPLENB / elem_bytes;
        const uint32_t bank_begin_elem = bankIdx * elems_per_bank;
        const uint32_t total_compressed = compress_cnt.as<uint32_t>()[0];

        uint32_t valid_elems = 0;
        if (total_compressed > bank_begin_elem) {
            valid_elems = std::min(total_compressed - bank_begin_elem,
                                   elems_per_bank);
        }
        memcpy(dst_data, vbuf_data + offset, valid_elems * elem_bytes);
    }

    xc->setRegOperand(this, 0, &dst);
    if (traceData)
        traceData->setData(dst);
    return NoFault;
}

std::string
VBufExtractMicroInst::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << " "
       << registerName(destRegIdx(0)) << ", "
       << registerName(srcRegIdx(0)) << ", offset=" << offset;
    return ss.str();
}

} // namespace RiscvISA
} // namespace gem5
