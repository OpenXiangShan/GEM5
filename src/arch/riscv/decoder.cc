/*
 * Copyright (c) 2012 Google
 * Copyright (c) The University of Virginia
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

#include "arch/riscv/decoder.hh"
#include "arch/riscv/types.hh"
#include "base/bitfield.hh"
#include "debug/Decode.hh"

namespace gem5
{

namespace RiscvISA
{

GenericISA::BasicDecodeCache<Decoder, ExtMachInst> Decoder::defaultCache;

bool isVConfig(ExtMachInst extMachInst)
{
    uint64_t opcode = bits(extMachInst, 6, 0);
    uint64_t width = extMachInst.width;
    return opcode == 0b1010111u && width == 0b111u;
}

void Decoder::reset()
{
    aligned = true;
    mid = false;
    vConfigDone = true;
    data = 0;
    emi = 0;
}

void
Decoder::moreBytes(const PCStateBase &pc, Addr fetchPC)
{
    if (GEM5_UNLIKELY(!this->vConfigDone)) {
        DPRINTF(Decode, "Wait VConfig inst execute ...\n");
        instDone = false;
        outOfBytes = false; // stop update pc
        stall = true; // stop fetch
        return;
    }
    stall = false;
    // The MSB of the upper and lower halves of a machine instruction.
    constexpr size_t max_bit = 31;
    constexpr size_t mid_bit = 15;
    constexpr size_t inst_normal_size = 4;
    auto inst = letoh(data);
    DPRINTF(Decode, "Requesting bytes 0x%08x from address %#x\n", inst,
            fetchPC);

    bool aligned = pc.instAddr() % inst_normal_size == 0;
    if (aligned) {
        emi.instBits = inst;
        if (compressed(inst))
            emi.instBits = bits(inst, mid_bit, 0);
        outOfBytes = !compressed(emi);
        instDone = true;
    } else {
        if (mid) {
            assert(bits(emi.instBits, max_bit, mid_bit + 1) == 0);
            replaceBits(emi.instBits, max_bit, mid_bit + 1, inst);
            mid = false;
            outOfBytes = false;
            instDone = true;
        } else {
            emi.instBits = bits(inst, max_bit, mid_bit + 1);
            mid = !compressed(emi);
            outOfBytes = true;
            instDone = compressed(emi);
        }
    }
    if (instDone) {
        emi.vl      = this->vl;
        emi.vtype   = this->vtype;
        emi.vill    = this->vill;
        emi.compressed = compressed(emi);
        if (isVConfig(emi)) {
            this->vConfigDone = false; // set true when vconfig inst execute
        }
        DPRINTF(Decode, "inst:0x%08x, vtype:0x%x, vill:%d, vl:%d, "
            "compressed:%01x, vConfDone:%d\n",
            emi.instBits, emi.vtype, emi.vill, emi.vl, emi.compressed,
            this->vConfigDone);
    }
}

StaticInstPtr
Decoder::decode(ExtMachInst mach_inst, Addr addr)
{
    DPRINTF(Decode, "Decoding instruction 0x%08x at address %#x\n",
            mach_inst.instBits, addr);

    StaticInstPtr si = defaultCache.decode(this, mach_inst, addr);

    DPRINTF(Decode, "Decode: Decoded %s instruction: %#x\n",
            si->getName(), mach_inst);
    return si;
}

StaticInstPtr
Decoder::decode(PCStateBase &_next_pc)
{
    if (!instDone)
        return nullptr;
    instDone = false;

    auto &next_pc = _next_pc.as<PCState>();

    if (emi.compressed) {
        next_pc.npc(next_pc.instAddr() + 2);
        next_pc.compressed(true);
    } else {
        next_pc.npc(next_pc.instAddr() + 4);
        next_pc.compressed(false);
    }

    return decode(emi, next_pc.instAddr());
}

void
Decoder::setPCStateWithInstDesc(const bool &compressed, PCStateBase &_next_pc)
{
    auto &next_pc = _next_pc.as<PCState>();
    if (compressed) {
        next_pc.npc(next_pc.instAddr() + sizeof(machInst) / 2);
        next_pc.compressed(true);
    } else {
        next_pc.npc(next_pc.instAddr() + sizeof(machInst));
        next_pc.compressed(false);
    }
}

Decoder::setVl(uint32_t new_vl)
{
    this->vl = new_vl;
}

void
Decoder::setVtype(uint64_t new_vtype)
{
    this->vill = bits(new_vtype, XLEN-1);
    if (GEM5_UNLIKELY(this->vill)) {
        this->vtype = 0;
    }
    else {
        this->vtype = bits(new_vtype, 7, 0);
    }
}

void
Decoder::setVConfigDone()
{
    this->vConfigDone = true;
}

} // namespace RiscvISA
} // namespace gem5
