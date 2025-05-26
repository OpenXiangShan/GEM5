/*
 * Copyright (c) 2016-2018,2019 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder. You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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

#include "cpu/o3/rename_map.hh"

#include <vector>

#include "arch/vecregs.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/reg_class.hh"
#include "debug/Rename.hh"

namespace gem5
{

namespace o3
{

SimpleRenameMap::SimpleRenameMap() : freeList(NULL)
{
}


void
SimpleRenameMap::init(const RegClass &reg_class, SimpleFreeList *_freeList)
{
    assert(freeList == NULL);
    assert(map.empty());

    map.resize(reg_class.numRegs());
    freeList = _freeList;
}

SimpleRenameMap::RenameInfo
SimpleRenameMap::rename(const RegId &arch_reg, const PhysRegIdPtr provided_dest,
                        const int64_t displacement)
{
    RenameEntry renamed_reg = RenameEntry();
    // Record the current physical register that is renamed to the
    // requested architected register.
    RenameEntry prev_reg = map[arch_reg.index()];

    if (arch_reg.is(InvalidRegClass)) {
        assert(prev_reg.PhyReg()->is(InvalidRegClass));
        renamed_reg = prev_reg;
    } else if (provided_dest != nullptr) {

        // Move elimination & Constant folding
        renamed_reg.setPhyReg(provided_dest);
        renamed_reg.setDisplacement(displacement);

        // New mapping
        map[arch_reg.index()] = renamed_reg;

        if (prev_reg.PhyReg() != provided_dest) {
            // A new archReg map to the same physReg
            renamed_reg.PhyReg()->incRef();
            DPRINTF(Rename, "Increment the refcnt of p%i to %i\n",
                    renamed_reg.PhyReg()->flatIndex(), renamed_reg.PhyReg()->getRef());
        } else {
            DPRINTF(Rename,
                    "Provided destination is the same as the previous one, "
                    "leave ref counter untouched\n");
        }
    } else if (prev_reg.PhyReg()->getNumPinnedWrites() > 0) {
        // Do not rename if the register is pinned
        assert(arch_reg.getNumPinnedWrites() == 0);  // Prevent pinning the
                                                     // same register twice
        DPRINTF(Rename, "Renaming pinned reg, numPinnedWrites %d\n",
                prev_reg.PhyReg()->getNumPinnedWrites());
        renamed_reg = prev_reg;
        renamed_reg.PhyReg()->decrNumPinnedWrites();
    } else {
        renamed_reg.setPhyReg(freeList->getReg());
        DPRINTF(Rename, "Get free reg p%i\n", renamed_reg.PhyReg()->flatIndex());
        map[arch_reg.index()] = renamed_reg;
        renamed_reg.PhyReg()->setNumPinnedWrites(arch_reg.getNumPinnedWrites());
        renamed_reg.PhyReg()->setNumPinnedWritesToComplete(
            arch_reg.getNumPinnedWrites() + 1);
        DPRINTF(Rename, "set refcnt of p%i to %i\n",
                renamed_reg.PhyReg()->flatIndex(), renamed_reg.PhyReg()->getRef());
    }

    DPRINTF(Rename, "Renamed reg %d to physical reg %d (%d) old mapping was"
            " %d (%d)\n",
            arch_reg, renamed_reg.PhyReg()->flatIndex(), renamed_reg.PhyReg()->flatIndex(),
            prev_reg.PhyReg()->flatIndex(), prev_reg.PhyReg()->flatIndex());

    return RenameInfo(renamed_reg, prev_reg);
}


/**** UnifiedRenameMap methods ****/

void
UnifiedRenameMap::init(const BaseISA::RegClasses &regClasses,
        PhysRegFile *_regFile, UnifiedFreeList *freeList)
{
    regFile = _regFile;

    for (int i = 0; i < renameMaps.size(); i++)
        renameMaps[i].init(regClasses.at(i), &(freeList->freeLists[i]));
}

bool
UnifiedRenameMap::canRename(DynInstPtr inst) const
{
    for (int i = 0; i < renameMaps.size(); i++) {
        if (inst->numDestRegs((RegClassType)i) >
                renameMaps[i].numFreeEntries()) {
            return false;
        }
    }
    return true;
}

} // namespace o3
} // namespace gem5
