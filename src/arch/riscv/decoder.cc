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

void Decoder::reset()
{
    machInst = 0;
    emi = 0;
    instDone = false;
    outOfBytes = true;
}

void
Decoder::moreBytes(const PCStateBase &pc, Addr fetchPC)
{
    // Get the instruction from machInst buffer
    auto inst = letoh(machInst);

    DPRINTF(Decode, "Requesting bytes 0x%08x from address %#x\n", inst,
        fetchPC);

    // We assume fetchPC is always the actual instruction address,
    // so we can directly work with the instruction
    emi.instBits = inst;

    // For compressed instruction, we only need the lower 16 bits
    if (compressed(inst)) {
        constexpr size_t mid_bit = sizeof(machInst) * 4 - 1; // 15 for 32-bit machInst
        emi.instBits = bits(inst, mid_bit, 0);
    }

    // For any instruction (compressed or not), we've received enough data
    instDone = true;    // decoder->instReady() is always true

    // For 32-bit instructions, we still need all 4 bytes
    // For 16-bit instructions, we already have enough bytes
    outOfBytes = !compressed(emi); // not used !!!
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

    if (compressed(emi)) {
        next_pc.npc(next_pc.instAddr() + sizeof(machInst) / 2);
        next_pc.compressed(true);
    } else {
        next_pc.npc(next_pc.instAddr() + sizeof(machInst));
        next_pc.compressed(false);
    }

    emi.vtype8 = this->machVtype & 0xff;
    StaticInstPtr inst = decode(emi, next_pc.instAddr());
    if (inst->isVectorConfig()) {
        auto vset = static_cast<VConfOp*>(inst.get());
        if (vset->vtypeIsImm) {
            this->setVtype(vset->earlyVtype);
            VTYPE new_vtype = vset->earlyVtype;
        }
        else {
            this->clearVtype();
        }
    }

    return inst;
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

void
Decoder::setVtype(VTYPE vtype) {
    vtypeReady = true;
    this->machVtype = vtype;
}

void
Decoder::clearVtype() {
    vtypeReady = false;
}

bool
Decoder::stall() {
    return !vtypeReady;
}

} // namespace RiscvISA
} // namespace gem5
