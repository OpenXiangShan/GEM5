/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * This file provides RISC-V-specific request flag bits which live in
 * Request::ARCH_BITS (the low 8 bits of Request::FlagsType).
 */

#ifndef __ARCH_RISCV_MEMFLAGS_HH__
#define __ARCH_RISCV_MEMFLAGS_HH__

#include "mem/request.hh"

namespace gem5
{

namespace RiscvISA
{

namespace XlateFlags
{

// Force the RISC-V MMU to perform (guest) virtual address translation even when
// the current V bit is off (used by H-extension hypervisor load/store).
constexpr Request::FlagsType FORCE_VIRT = 1u << 0;

// For HLVX.* instructions: treat the access as "execute-permission checked"
// without changing the fault type (it is still a load).
constexpr Request::FlagsType HLVX = 1u << 1;

// Mark a load-reserved request (LR.*). This is carried through Request::ARCH_BITS
// for RISC-V-specific translation/permission handling when needed.
constexpr Request::FlagsType LR = 1u << 2;

static_assert((FORCE_VIRT | HLVX | LR) <= Request::ARCH_BITS,
              "RISC-V XlateFlags must fit in Request::ARCH_BITS");

} // namespace XlateFlags

} // namespace RiscvISA

} // namespace gem5

#endif // __ARCH_RISCV_MEMFLAGS_HH__

