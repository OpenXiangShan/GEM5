/*
 * Copyright (c) 2018 TU Dresden
 * Copyright (c) 2020 Barkhausen Institut
 * All rights reserved
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

#include "arch/riscv/bare_metal/fs_workload.hh"

#include "arch/riscv/faults.hh"
#include "base/loader/object_file.hh"
#include "debug/MemoryAccess.hh"
#include "sim/system.hh"
#include "sim/workload.hh"

namespace gem5
{

namespace RiscvISA
{

BareMetal::BareMetal(const Params &p) : Workload(p),
    _isBareMetal(p.bare_metal), _resetVect(p.reset_vect),
    raw_binary(p.raw_bootloader)
{
    if (!p.xiangshan_cpt) {
        bootloader = loader::createObjectFile(p.bootloader, raw_binary);
        fatal_if(!bootloader, "Could not load bootloader file %s.",
                 p.bootloader);
        _resetVect = raw_binary ? p.reset_vect: bootloader->entryPoint();
        bootloaderSymtab = bootloader->symtab();
        inform("Using %s of bootloader or BareMetal workload, reset to %#lx\n",
               raw_binary? "bin": "elf", _resetVect);
    } else {
        bootloader = nullptr;
        assert(p.bootloader.empty());
        _resetVect = p.reset_vect;
        inform("No bootload provided, because using XS GCPT, reset to %#lx\n",
               _resetVect);
    }
}

BareMetal::~BareMetal()
{
    if (bootloader) {
        delete bootloader;
    }
}

void
BareMetal::initState()
{
    Workload::initState();

    for (auto *tc: system->threads) {
        RiscvISA::Reset().invoke(tc);
        tc->activate();
    }

    if (bootloader) {
        if (!raw_binary) {
            warn_if(!bootloader->buildImage().write(system->physProxy),
                    "Could not load sections to memory.");
        } else {
            warn("Using raw cpt binary and mmap to it, no bootloader loaded.");
        }
    }

    for (auto *tc: system->threads) {
        RiscvISA::Reset().invoke(tc);
        tc->activate();
    }
}

} // namespace RiscvISA
} // namespace gem5
