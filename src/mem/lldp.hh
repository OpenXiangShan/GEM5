// Copyright (c) 2026. All rights reserved.
// Shared, value-only descriptors for the IQ/Cache LLDP interfaces.
#ifndef __MEM_LLDP_HH__
#define __MEM_LLDP_HH__

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace gem5
{
namespace lldp
{
enum class SourceForm : uint8_t { SingleImmediate, DualRegister, Other };

inline SourceForm sourceForm(unsigned registers, bool immediate)
{
    if (registers == 2 && !immediate)
        return SourceForm::DualRegister;
    if (registers == 1 && immediate)
        return SourceForm::SingleImmediate;
    return SourceForm::Other;
}

enum class Op : uint8_t
{
    Other, Add, Sub, Shl, Shr, Sar, And, Or, Xor, Mul, Div, Rem
};

struct Operation
{
    Op op{Op::Other};
    int64_t imm{0};
    bool word{false};
    bool replayable{false};
    bool operator==(const Operation &b) const
    {
        return op == b.op && imm == b.imm && word == b.word &&
            replayable == b.replayable;
    }
    bool operator!=(const Operation &b) const { return !(*this == b); }
};

inline unsigned category(Op op)
{
    switch (op) {
      case Op::Add: case Op::Sub: return 0;
      case Op::Shl: case Op::Shr: case Op::Sar: return 1;
      case Op::And: case Op::Or: case Op::Xor: return 2;
      case Op::Mul: case Op::Div: case Op::Rem: return 3;
      default: return 4;
    }
}

// Decode the scalar RISC-V operations that can be reconstructed from one
// loaded value and an instruction immediate. Register-register operations
// remain observable, but cannot be replayed without the other register value.
inline Operation decodeOperation(std::string name, int64_t imm)
{
    if (name.compare(0, 2, "c_") == 0)
        name = name.substr(2);
    Operation result;
    result.imm = imm;
    result.word = !name.empty() && name.back() == 'w';
    if (name.compare(0, 3, "add") == 0) result.op = Op::Add;
    else if (name.compare(0, 3, "sub") == 0) result.op = Op::Sub;
    else if (name.compare(0, 3, "sll") == 0) result.op = Op::Shl;
    else if (name.compare(0, 3, "srl") == 0) result.op = Op::Shr;
    else if (name.compare(0, 3, "sra") == 0) result.op = Op::Sar;
    else if (name.compare(0, 3, "and") == 0) result.op = Op::And;
    else if (name.compare(0, 2, "or") == 0) result.op = Op::Or;
    else if (name.compare(0, 3, "xor") == 0) result.op = Op::Xor;
    else if (name.compare(0, 3, "mul") == 0) result.op = Op::Mul;
    else if (name.compare(0, 3, "div") == 0) result.op = Op::Div;
    else if (name.compare(0, 3, "rem") == 0) result.op = Op::Rem;
    result.replayable = name == "addi" || name == "addiw" ||
        name == "addi4spn" || name == "addi16sp" || name == "slli" ||
        name == "slliw" || name == "srli" || name == "srliw" ||
        name == "srai" || name == "sraiw" || name == "andi" ||
        name == "ori" || name == "xori";
    return result;
}

inline bool apply(const Operation &op, uint64_t &value)
{
    if (!op.replayable)
        return false;
    if (op.word)
        value = uint32_t(value);
    const unsigned bits = op.word ? 32 : 64;
    const unsigned shift = uint64_t(op.imm) & (bits - 1);
    switch (op.op) {
      case Op::Add: value += uint64_t(op.imm); break;
      case Op::Sub: value -= uint64_t(op.imm); break;
      case Op::Shl: value <<= shift; break;
      case Op::Shr: value >>= shift; break;
      case Op::Sar:
        if (shift) {
            const bool negative = value & (uint64_t(1) << (bits - 1));
            value >>= shift;
            if (negative)
                value |= (~uint64_t(0)) << (bits - shift);
        }
        break;
      case Op::And: value &= uint64_t(op.imm); break;
      case Op::Or: value |= uint64_t(op.imm); break;
      case Op::Xor: value ^= uint64_t(op.imm); break;
      default: return false;
    }
    if (op.word) {
        value = uint32_t(value);
        if (value & (uint64_t(1) << 31))
            value |= 0xffffffff00000000ULL;
    }
    return true;
}

struct Chain
{
    bool valid{false};
    bool replayable{true};
    uint64_t producerPC{0};
    uint32_t length{0};
    std::array<Operation, 2> ops{};
    // Count all operations, including those beyond the two replay slots.
    std::array<uint32_t, 5> categories{};
    uint32_t singleSrcImmediateOps{0};
    uint32_t dualSrcRegisterOps{0};

    static Chain start(uint64_t pc)
    {
        Chain c;
        c.valid = true;
        c.producerPC = pc;
        c.length = 1;
        return c;
    }
    Chain extend(const Operation &op) const
    {
        Chain c = *this;
        if (!valid)
            return c;
        if (length >= 1 && length <= 2)
            c.ops[length - 1] = op;
        if (c.length != std::numeric_limits<uint32_t>::max())
            ++c.length;
        ++c.categories[category(op.op)];
        ++c.singleSrcImmediateOps;
        c.replayable &= op.replayable;
        return c;
    }

    // Do not store op/imm or register values for dependency-only operations.
    // Keep the aggregate source-form count needed for complete-chain stats.
    Chain extendDependencyOnly(SourceForm form) const
    {
        Chain c = *this;
        if (!valid)
            return c;
        if (c.length != std::numeric_limits<uint32_t>::max())
            ++c.length;
        if (form == SourceForm::DualRegister)
            ++c.dualSrcRegisterOps;
        else
            ++c.categories[category(Op::Other)];
        c.replayable = false;
        return c;
    }

    bool trainable() const
    {
        return valid && replayable && dualSrcRegisterOps == 0 &&
            length >= 1 && length <= 3;
    }
};

struct Hint
{
    bool valid{false};
    uint64_t producerPC{0};
    uint64_t generation{0};
    uint32_t offset{0};
    uint8_t size{0};
    bool signExtend{false};
};

// A small binary tree PLRU, also used by the 4-way consumer subtable.
// A bit points at the least recently used subtree.
template <unsigned N> struct PLRU
{
    static_assert(N && !(N & (N - 1)), "PLRU requires power-of-two ways");
    std::array<bool, N - 1> bits{};
    unsigned victim() const
    {
        unsigned node = 0;
        while (node < N - 1)
            node = node * 2 + 1 + unsigned(bits[node]);
        return node - (N - 1);
    }
    void touch(unsigned way)
    {
        unsigned node = way + N - 1;
        while (node) {
            const unsigned parent = (node - 1) / 2;
            bits[parent] = (node == parent * 2 + 1);
            node = parent;
        }
    }
};
} // namespace lldp
} // namespace gem5
#endif // __MEM_LLDP_HH__
