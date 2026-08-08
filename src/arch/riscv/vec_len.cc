/*
 * Copyright (c) 2026 OpenXiangShan
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

#include "arch/riscv/vec_len.hh"

#include "arch/riscv/isa.hh"
#include "base/logging.hh"
#include "cpu/thread_context.hh"

namespace gem5
{
namespace RiscvISA
{

thread_local uint32_t decodeVecLenInBits = DefaultVecLenInBits;

namespace
{

// 0 means "unset". Shared decode cache keys only on ExtMachInst, so a second
// architectural VLEN in the same process would reuse StaticInsts built for the
// first VLEN. Latch once and reject mismatches.
uint32_t processVecLenInBits = 0;

void
latch_process_vlen(uint32_t bits)
{
    if (processVecLenInBits == 0) {
        processVecLenInBits = bits;
        return;
    }
    fatal_if(processVecLenInBits != bits,
        "Mixed RVV VLEN in one gem5 process is unsupported with the shared "
        "decode cache (process VLEN=%u, new VLEN=%u). Use a single "
        "--rvv-vlen for all harts, or clear/key the decode cache by VLEN.",
        processVecLenInBits, bits);
}

} // namespace

void
registerProcessVecLenInBits(uint32_t bits)
{
    latch_process_vlen(bits);
}

void
setDecodeVecLenInBits(uint32_t bits)
{
    latch_process_vlen(bits);
    decodeVecLenInBits = bits;
}

uint32_t
curVecLenInBits(ExecContext *xc)
{
    auto *isa = dynamic_cast<ISA *>(xc->tcBase()->getIsaPtr());
    panic_if(!isa, "Failed to resolve RiscvISA for VLEN lookup");
    return isa->getVecLenInBits();
}

uint32_t
curVecLenInBytes(ExecContext *xc)
{
    return curVecLenInBits(xc) >> 3;
}

} // namespace RiscvISA
} // namespace gem5
