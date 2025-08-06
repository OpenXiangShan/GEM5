# Copyright (c) 2021 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from ruby import CHI_config
import math
import m5
from m5.objects import *

# CustomMesh parameters for a 4x4 mesh. Routers will have the following layout:
#
# 0 --- 1 --- 2 --- 3
# |     |     |     |
# 4 --- 5 --- 6 --- 7
# |     |     |     |
# 8 --- 9 --- 10 --- 11
# |     |     |     |
# 12 --- 13 --- 14 --- 15
# |     |     |     |
# 16 --- 17 --- 18 --- 19
# |     |     |     |
# 20 --- 21 --- 22 --- 23
#
# Default parameter are configs/ruby/CHI_config.py
#
class NoC_Params(CHI_config.NoC_Params):
    num_rows = 4
    num_cols = 6

# Specialization of nodes to define bindings for each CHI node type
# needed by CustomMesh.
# The default types are defined in CHI_Node and their derivatives in
# configs/ruby/CHI_config.py

class CHI_RNF_TrafficGen(CHI_config.CHI_RNF):
    """
    Defines a CHI request node.
    Notice all contollers and sequencers are set as children of the cpus, so
    this object acts more like a proxy for seting things up and has no topology
    significance unless the cpus are set as its children at the top level
    """

    def __init__(
        self,
        cpus,
        ruby_system,
        l1Icache_type,
        l1Dcache_type,
        cache_line_size,
        options
    ):
        CHI_config.CHI_Node.__init__(self, ruby_system)

        assert len(cpus) == 1
        self._block_size_bits = int(math.log(cache_line_size, 2))

        # All sequencers and controllers
        self._seqs = []
        self._cntrls = []

        # Last level controllers in this node, i.e., the ones that will send
        # requests to the home nodes
        self._ll_cntrls = []

        self._cpus = cpus

        # First creates L1 caches and sequencers
        for cpu in self._cpus:
            cpu.data_sequencer = RubySequencer(
                version=1, ruby_system=ruby_system, is_data_sequencer=True
            )

            self._seqs.append(
                cpu.data_sequencer
            )


            l1d_cache = l1Dcache_type(
                start_index_bit=self._block_size_bits, is_icache=False
            )

            # For other protocol, machine type is "L1Cache" so
            # L1Cache_Controller is created by slicc compiler
            # For CHI, all controllers are MachineType:Cache
            # CHI_L1Controller inherits from Cache_Controller
            cpu.l1d = CHI_config.CHI_L1Controller(
                ruby_system, cpu.data_sequencer, l1d_cache, NULL, is_dcache=True, enable_difftest = False
            )
            cpu.data_sequencer.dcache = cpu.l1d.cache

            self._cntrls.append(cpu.l1d)
            self.connectController(cpu.l1d)
            self._ll_cntrls.append(cpu.l1d)

    def getSequencers(self):
        return self._seqs

    def getAllControllers(self):
        return self._cntrls

    def getNetworkSideControllers(self):
        return self._cntrls

    def setDownstream(self, cntrls):
        for c in self._ll_cntrls:
            c.downstream_destinations = cntrls

    def getCpus(self):
        return self._cpus

    def addPrivL2Cache(self, cache_type, options):
        # print("This custom config does not support L2 caches "
        # "No action is done!!")
        pass

    # Adds a private L2 for each cpu
class CHI_RNF(CHI_RNF_TrafficGen):
    class NoC_Params(CHI_config.CHI_RNF.NoC_Params):
        router_list = [i for i in range(4, 20)]
    def getTypeId(self):
        return 1

class CHI_HNF(CHI_config.CHI_HNF):
    class NoC_Params(CHI_config.CHI_HNF.NoC_Params):
        router_list = [i for i in range(4, 20)]
    def getTypeId(self):
        return 2

class CHI_MN(CHI_config.CHI_MN):
    class NoC_Params(CHI_config.CHI_MN.NoC_Params):
        router_list = [0]
    def getTypeId(self):
        return 4

class CHI_SNF_MainMem(CHI_config.CHI_SNF_MainMem):
    class NoC_Params(CHI_config.CHI_SNF_MainMem.NoC_Params):
        router_list = [i for i in range(4)] + [i for i in range(20, 24)]
    def getTypeId(self):
        return 3

class CHI_SNF_BootMem(CHI_config.CHI_SNF_BootMem):
    class NoC_Params(CHI_config.CHI_SNF_BootMem.NoC_Params):
        router_list = []

class CHI_RNI_DMA(CHI_config.CHI_RNI_DMA):
    class NoC_Params(CHI_config.CHI_RNI_DMA.NoC_Params):
        router_list = []

class CHI_RNI_IO(CHI_config.CHI_RNI_IO):
    class NoC_Params(CHI_config.CHI_RNI_IO.NoC_Params):
        router_list = []
