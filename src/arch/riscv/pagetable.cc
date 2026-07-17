/*
 * Copyright (c) 2002-2005 The Regents of The University of Michigan
 * Copyright (c) 2007 MIPS Technologies, Inc.
 * Copyright (c) 2020 Barkhausen Institut
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

#include "arch/riscv/pagetable.hh"

#include "sim/serialize.hh"

namespace gem5
{

namespace RiscvISA
{

void
TlbEntry::serialize(CheckpointOut &cp) const
{
    SERIALIZE_SCALAR(paddr);
    SERIALIZE_SCALAR(vaddr);
    SERIALIZE_SCALAR(logBytes);
    SERIALIZE_SCALAR(translateMode);
    SERIALIZE_SCALAR(asid);
    SERIALIZE_SCALAR(pte);
    SERIALIZE_SCALAR(isCompressed);
    SERIALIZE_SCALAR(l1CompressedNarrow);
    SERIALIZE_SCALAR(validIdx);
    SERIALIZE_SCALAR(pteIdx);
    SERIALIZE_CONTAINER(ppnLow);
    SERIALIZE_SCALAR(lruSeq);
    SERIALIZE_SCALAR(level);
#if MPT_ENABLED
    SERIALIZE_SCALAR(mptInfo.raw);
    SERIALIZE_SCALAR(mptInfo.mptlevel);
    SERIALIZE_SCALAR(mptInfo.valid);
#endif
}

void
TlbEntry::unserialize(CheckpointIn &cp)
{
    UNSERIALIZE_SCALAR(paddr);
    UNSERIALIZE_SCALAR(vaddr);
    UNSERIALIZE_SCALAR(logBytes);
    UNSERIALIZE_SCALAR(translateMode);
    UNSERIALIZE_SCALAR(asid);
    UNSERIALIZE_SCALAR(pte);
    UNSERIALIZE_SCALAR(isCompressed);
    UNSERIALIZE_SCALAR(l1CompressedNarrow);
    UNSERIALIZE_SCALAR(validIdx);
    UNSERIALIZE_SCALAR(pteIdx);
    arrayParamIn(cp, "ppnLow", ppnLow.data(), ppnLow.size());
    UNSERIALIZE_SCALAR(lruSeq);
    UNSERIALIZE_SCALAR(level);
#if MPT_ENABLED
    UNSERIALIZE_SCALAR(mptInfo.raw);
    UNSERIALIZE_SCALAR(mptInfo.mptlevel);
    UNSERIALIZE_SCALAR(mptInfo.valid);
#endif
}


uint64_t getPageSizeForLevel(int level) {
    switch (level) {
        case 0: return MPT_LEAF_L0_PAGE_SIZE;
        case 1: return MPT_LEAF_L1_PAGE_SIZE;
        case 2: return MPT_LEAF_L2_PAGE_SIZE;
        case 3: return MPT_LEAF_L3_PAGE_SIZE;
        default: return 0;//or panic
    }
}

// Retrieve the MPTE region size for the current level (16 pages).
uint64_t getRegionSizeForLevel(int level) {
    return MPT_NUM_PERMS * getPageSizeForLevel(level);
}

uint8_t log2floor(uint64_t x) {
    uint8_t r = 0;
    while (x >>= 1) ++r;
    return r;
}



gem5::RiscvISA::MPTE52::MPTE52() : raw(0) {}

MPTE52::MPTE52(uint64_t val) : raw(val) {}

bool MPTE52::isValid() const { return raw & 0x1; }

bool MPTE52::isLeaf() const { return raw & 0x2; }

bool MPTE52::getN() const { return (raw >> 63) & 0x1; } // N （bit 63）

// Physical page number of the next-level page table (used when not a leaf).
Addr MPTE52::nextLevelPPN() const {
    return (raw >> 10) & 0x000FFFFFFFFFFFFF; // bits 10~61
}

// Physical address of the next-level page table (aligned to 4KB pages).
Addr MPTE52::nextLevelPAddr() const {
    return nextLevelPPN() << 12;   //2^12=4KB
}

// Retrieve the pi-th （pi ∈ [0, 15]）
uint8_t MPTE52::perms(uint8_t pi) const {
    // If napot is enabled, return unified permissions (using `perms[0]`).


    // Otherwise, return the permissions of the `pi`-th entry.
    if (pi >= MPT_NUM_PERMS) return 0;
    return (raw >> (10 + pi * MPT_PERM_BITS_PER_ENTRY)) & MPT_PERM_MASK;
    //2 is because the last two bits represent `valid` and `leaf`.
}






//Namespace-level utility function, not inside a struct.
bool checkMPTEPermissions(const MPTE52 &mpte, BaseMMU::Mode mode, Addr range_offset, int level)
{
    if (!mpte.isValid() || !mpte.isLeaf())
        return false;

    // The page size of the current level.
    uint64_t pageSize = getPageSizeForLevel(level);         // e.g. 2MB for level=1
    uint8_t pi = (range_offset / pageSize) & 0xF;            // Select the permissions of the specified page.

    uint8_t perm = mpte.perms(pi);

    return mptPermAllowsAccess(perm, mode);
}





} // namespace RiscvISA
} // namespace gem5
