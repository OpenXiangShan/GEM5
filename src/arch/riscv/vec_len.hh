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

#ifndef __ARCH_RISCV_VEC_LEN_HH__
#define __ARCH_RISCV_VEC_LEN_HH__

#include "arch/riscv/types.hh"
#include "cpu/exec_context.hh"

namespace gem5
{

class BaseISA;

namespace RiscvISA
{

class ISA;

/**
 * Decoder publishes the hart's configured VLEN while constructing StaticInsts.
 * Vector Macro/Micro constructors capture this value into a per-inst member so
 * later execute() paths do not need to re-query the ISA object, and so we avoid
 * rewriting every `new Foo(machInst)` call site in the ISA parser templates.
 *
 * Constraint: all harts in one gem5 process should share the same VLEN (true
 * for current XiangShan configs). Mixing VLEN values with a shared decode cache
 * would be incorrect.
 */
inline thread_local uint32_t decodeVecLenInBits = DefaultVecLenInBits;

inline void
setDecodeVecLenInBits(uint32_t bits)
{
    decodeVecLenInBits = bits;
}

inline uint32_t
getDecodeVecLenInBits()
{
    return decodeVecLenInBits;
}

inline uint32_t
getDecodeVecLenInBytes()
{
    return decodeVecLenInBits >> 3;
}

/** Resolve architectural VLEN from an ExecContext (preferred in execute()). */
uint32_t curVecLenInBits(ExecContext *xc);
uint32_t curVecLenInBytes(ExecContext *xc);

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_VEC_LEN_HH__
