/*
 * Copyright (c) 2012 Google
 * Copyright (c) 2017 The University of Virginia
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

#ifndef __ARCH_RISCV_DECODER_HH__
#define __ARCH_RISCV_DECODER_HH__

#include "arch/generic/decode_cache.hh"
#include "arch/generic/decoder.hh"
#include "arch/riscv/insts/vector.hh"
#include "arch/riscv/types.hh"
#include "base/bitfield.hh"
#include "base/types.hh"
#include "cpu/static_inst.hh"
#include "params/RiscvDecoder.hh"

namespace gem5
{

namespace RiscvISA
{

class ISA;

enum class PartialInstResult
{
    NeedMoreBytes,
    ReadyCompressed,
    ReadyFullWidth
};

struct PartialInstBuffer
{
    static constexpr uint8_t LowHalfMask = 0x3;
    static constexpr uint8_t FullMask = 0xf;

    uint32_t instBits = 0;
    Addr instPC = MaxAddr;
    unsigned assembledBytes = 0;
    uint8_t validMask = 0;

    static unsigned
    countValidBytes(uint8_t mask)
    {
        return __builtin_popcount(mask);
    }

    void
    reset()
    {
        instBits = 0;
        instPC = MaxAddr;
        assembledBytes = 0;
        validMask = 0;
    }

    bool
    hasBytes() const
    {
        return validMask != 0;
    }

    PartialInstResult
    pushChunk(Addr currentInstPC, Addr fetchPC, uint32_t chunk,
              size_t validBytes)
    {
        assert(validBytes > 0);
        assert(validBytes <= sizeof(instBits));
        assert(fetchPC >= currentInstPC);

        const size_t offset = fetchPC - currentInstPC;
        assert(offset + validBytes <= sizeof(instBits));

        if (instPC != currentInstPC) {
            reset();
            instPC = currentInstPC;
        }

        for (size_t index = 0; index < validBytes; ++index) {
            const size_t destByte = offset + index;
            const uint32_t byte = (chunk >> (index * 8)) & 0xffu;
            const uint32_t shift = destByte * 8;
            instBits &= ~(0xffu << shift);
            instBits |= byte << shift;
            validMask |= static_cast<uint8_t>(1u << destByte);
        }

        assembledBytes = countValidBytes(validMask);

        if ((validMask & LowHalfMask) == LowHalfMask &&
            (bits(instBits, 15, 0) & 0x3) < 0x3) {
            return PartialInstResult::ReadyCompressed;
        }

        if ((validMask & FullMask) == FullMask) {
            return PartialInstResult::ReadyFullWidth;
        }

        return PartialInstResult::NeedMoreBytes;
    }

    uint16_t
    compressedBits() const
    {
        return bits(instBits, 15, 0);
    }

    uint32_t
    fullBits() const
    {
        return instBits;
    }
};

class Decoder : public InstDecoder
{
  protected:
    //The extended machine instruction being generated
    ExtMachInst emi;
    uint32_t machInst;
    PartialInstBuffer partialInst;

    bool vtypeReady = true;
    VTYPE machVtype;

    /// A cache of decoded instruction objects.
    static GenericISA::BasicDecodeCache<Decoder, ExtMachInst> defaultCache;
    friend class GenericISA::BasicDecodeCache<Decoder, ExtMachInst>;

    StaticInstPtr decodeInst(ExtMachInst mach_inst);

    /// Decode a machine instruction.
    /// @param mach_inst The binary instruction to decode.
    /// @retval A pointer to the corresponding StaticInst object.
    StaticInstPtr decode(ExtMachInst mach_inst, Addr addr);

  public:
    Decoder(const RiscvDecoderParams &p) : InstDecoder(p, &machInst)
    {
        reset();
    }

    void reset() override;

    inline bool compressed(ExtMachInst inst) { return (inst & 0x3) < 0x3; }
    static bool legacyNeedMoreBytes(ExtMachInst inst)
    {
        return (inst & 0x3) == 0x3;
    }
    inline bool vconf(ExtMachInst inst) {
      return inst.opcode7 == 0b1010111u && inst.width == 0b111u;
    }

    //Use this to give data to the decoder. This should be used
    //when there is control flow.
    void moreBytes(const PCStateBase &pc, Addr fetchPC) override;
    void moreBytes(const PCStateBase &pc, Addr fetchPC,
                   size_t validBytes) override;

    StaticInstPtr decode(PCStateBase &nextPC) override;
    bool hasPartialInst() const override
    {
        return partialInst.hasBytes() && !instDone;
    }


    void setPCStateWithInstDesc(const bool &inst,
                                  PCStateBase &pc) override;

    void setVtype(VTYPE vtype);

    void clearVtype();

    bool stall() override;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_DECODER_HH__
