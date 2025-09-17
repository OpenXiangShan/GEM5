# Copyright (c) 2012-2013, 2015-2016 ARM Limited
# Copyright (c) 2020 Barkhausen Institut
# All rights reserved
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
# Copyright (c) 2010 Advanced Micro Devices, Inc.
# All rights reserved.
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

# Configure the M5 cache hierarchy config in one place
#

import math
import m5
from m5.objects import *
from common.Caches import *
from common import ObjectList
from common.PrefetcherConfig import *

def _get_hwp(hwp_option):
    if hwp_option == None:
        return NULL

    hwpClass = ObjectList.hwp_list.get(hwp_option)
    return hwpClass()

def _get_cache_opts(cpu, level, options):
    opts = {}

    size_attr = '{}_size'.format(level)
    if hasattr(options, size_attr):
        opts['size'] = getattr(options, size_attr)

    assoc_attr = '{}_assoc'.format(level)
    if hasattr(options, assoc_attr):
        opts['assoc'] = getattr(options, assoc_attr)

    prefetcher_attr = '{}_hwp_type'.format(level)
    if hasattr(options, prefetcher_attr) and (not options.no_pf):
        opts['prefetcher'] = create_prefetcher(cpu, level, options)

    return opts

def config_classic_l2(options, system, l2_cache_class):
    # When using classic L2 cache, The prefetcher is inside the l2cache, instead of l2Wrapper
    # So we need to move the prefetcher from l2Wrapper to l2cache
    if options.l2_hwp_type == 'PrefetcherForwarder' and options.l2_wrapper_hwp_type:
        options.l2_hwp_type = options.l2_wrapper_hwp_type
        options.l2_wrapper_hwp_type = None
    # Provide a clock for the L2 and the L1-to-L2 bus here as they
    # are not connected using addTwoLevelCacheHierarchy. Use the
    # same clock as the CPUs.
    system.l2_caches = [l2_cache_class(clk_domain=system.cpu_clk_domain,
                                    **_get_cache_opts(system.cpu[i], 'l2', options)) for i in range(options.num_cpus)]
    system.tol2bus_list = [L1ToL2Bus(
            clk_domain=system.cpu_clk_domain) for i in range(options.num_cpus)]
    for i in range(options.num_cpus):
        # system.l2_caches.append(l2_cache_class(clk_domain=system.cpu_clk_domain,
        #                        **_get_cache_opts('l2', options)))

        # system.tol2bus_list.append(L2XBar(clk_domain = system.cpu_clk_domain, width=256))
        system.l2_caches[i].cpu_side = system.tol2bus_list[i].mem_side_ports
        system.tol2bus_list[i].snoop_filter.max_capacity = "16MB"
        system.l2_caches[i].do_fast_writeline = not options.kmh_align
        if options.ideal_cache:
            system.l2_caches[i].response_latency = 0
            system.l2_caches[i].tag_latency = 1
            system.l2_caches[i].data_latency = 1
            system.l2_caches[i].sequential_access = False
            system.l2_caches[i].writeback_clean = False
            system.l2_caches[i].mshrs = 64

        if options.xiangshan_ecore:
            system.l2_caches[i].response_latency = 66
            system.l2_caches[i].writeback_clean = False

def config_aligned_l2(options, system, l2_cache_class):
    # Provide a clock for the L2 and the L1-to-L2 bus here as they
    # are not connected using addTwoLevelCacheHierarchy. Use the
    # same clock as the CPUs.
    num_l2_slices = options.l2_slices
    # Create the L2 cache system for each CPU core, which includes a
    # wrapper, an internal crossbar, and multiple slices.
    system.l2_wrappers = [L2CacheWrapper(clk_domain=system.cpu_clk_domain,
                                            num_slices=num_l2_slices,
                                            cache_size=options.l2_size,
                                            cache_assoc=options.l2_assoc,
                                            block_bits=int(math.log2(system.cache_line_size)))
                                            for _ in range(options.num_cpus)]
    for i in range(options.num_cpus):
        # Create an internal L2 crossbar for the slices
        system.l2_wrappers[i].xbar = CoherentXBar(clk_domain = system.cpu_clk_domain,
                                                    width = 512,
                                                    frontend_latency = 0,
                                                    forward_latency = 0,
                                                    response_latency = 0,
                                                    header_latency = 0,
                                                    snoop_response_latency = 0,
                                                    snoop_filter = SnoopFilter(lookup_latency = 0),
                                                    point_of_unification = True)
        # Create the L2 cache slice, which contains the pipeline logic
        system.l2_wrappers[i].slices = [L2CacheSlice(clk_domain=system.cpu_clk_domain)
                                        for _ in range(num_l2_slices)]
        # Create the actual classic L2 cache that stores data
        for j in range(num_l2_slices):
            system.l2_wrappers[i].slices[j].inner_cache = l2_cache_class(clk_domain=system.cpu_clk_domain,
                                                            **_get_cache_opts(system.cpu[i], 'l2', options))

    system.tol2bus_list = [L1ToL2Bus(
        clk_domain=system.cpu_clk_domain) for i in range(options.num_cpus)]

    for i in range(options.num_cpus):
        l2_wrapper = system.l2_wrappers[i]
        xbar = l2_wrapper.xbar
        if not options.no_pf:
            l2_wrapper.prefetcher = create_prefetcher(system.cpu[i], 'l2_wrapper', options)
        for j in range(num_l2_slices):
            # Apply original per-L2-cache configurations to each slice's inner cache
            cache_slice = l2_wrapper.slices[j]
            inner_cache = cache_slice.inner_cache

            l2_wrapper.addCacheAccessor(inner_cache)
            l2_wrapper.addSliceAccessor(cache_slice)

            cache_slice.setCacheAccessor(inner_cache)
            if not options.no_pf and options.l2_hwp_type == 'PrefetcherForwarder':
                inner_cache.prefetcher.setRealPrefetcher(l2_wrapper.prefetcher)

            # Cut off the resources in inner_cache according to slice num
            assert(int(inner_cache.mshrs) % num_l2_slices == 0)
            inner_cache.mshrs = int(inner_cache.mshrs) // num_l2_slices


            inner_cache.do_fast_writeline = not options.kmh_align
            if options.ideal_cache:
                inner_cache.response_latency = 0
                inner_cache.tag_latency = 1
                inner_cache.data_latency = 1
                inner_cache.sequential_access = False
                inner_cache.writeback_clean = False
                inner_cache.mshrs = 64
            if options.xiangshan_ecore:
                inner_cache.response_latency = 66
                inner_cache.writeback_clean = False

            # Connect the slice's inner ports to the actual cache
            cache_slice.inner_cpu_port = inner_cache.cpu_side
            inner_cache.mem_side = cache_slice.inner_mem_port

            # Connect slice to the wrapper's cpu-side input and the internal xbar's cpu-side input
            cache_slice.cpu_side = l2_wrapper.slice_cpuside_ports
            xbar.cpu_side_ports = cache_slice.mem_side

        # Connect the wrapper to the L1-L2 bus
        l2_wrapper.cpu_side = system.tol2bus_list[i].mem_side_ports

def config_cache(options, system):
    if options.external_memory_system and (options.caches or options.l2cache):
        print("External caches and internal caches are exclusive options.\n")
        sys.exit(1)

    if options.external_memory_system:
        ExternalCache = ExternalCacheFactory(options.external_memory_system)

    if options.cpu_type == "O3_ARM_v7a_3":
        try:
            import cores.arm.O3_ARM_v7a as core
        except:
            print("O3_ARM_v7a_3 is unavailable. Did you compile the O3 model?")
            sys.exit(1)

        dcache_class, icache_class, l2_cache_class, walk_cache_class = \
            core.O3_ARM_v7a_DCache, core.O3_ARM_v7a_ICache, \
            core.O3_ARM_v7aL2, \
            None
    elif options.cpu_type == "HPI":
        try:
            import cores.arm.HPI as core
        except:
            print("HPI is unavailable.")
            sys.exit(1)

        dcache_class, icache_class, l2_cache_class, walk_cache_class = \
            core.HPI_DCache, core.HPI_ICache, core.HPI_L2, None
    else:
        dcache_class, icache_class, l2_cache_class, walk_cache_class = \
            L1_DCache, L1_ICache, L2Cache, None

        if buildEnv['TARGET_ISA'] in ['x86', 'riscv']:
        #if buildEnv['TARGET_ISA'] in ['x86']:
            walk_cache_class = PageTableWalkerCache

    # Set the cache line size of the system
    system.cache_line_size = options.cacheline_size

    # If elastic trace generation is enabled, make sure the memory system is
    # minimal so that compute delays do not include memory access latencies.
    # Configure the compulsory L1 caches for the O3CPU, do not configure
    # any more caches.
    if options.l2cache:
        assert (not hasattr(options, 'elastic_trace_en') or
                not options.elastic_trace_en)

    if options.l2cache:
        if options.classic_l2:
            config_classic_l2(options, system, l2_cache_class)
        else:
            config_aligned_l2(options, system, l2_cache_class)

        for i in range(options.num_cpus):
            system.tol2bus_list[i].snoop_filter.max_capacity = "16MB"
            if options.ideal_cache:
                assert not options.l3cache, \
                    "Ideal caches and L3s are exclusive options."
                assert options.l2cache, "Ideal caches require L2s."
                assert options.mem_type == "SimpleMemory", \
                    "Ideal caches require SimpleMemory."

                system.tol2bus_list[i].frontend_latency = 0
                system.tol2bus_list[i].response_latency = 0
                system.tol2bus_list[i].forward_latency = 0
                system.tol2bus_list[i].header_latency = 0
                system.tol2bus_list[i].snoop_response_latency = 0
                system.tol2bus_list[i].width = 256 # byte per cycle

        if options.l3cache:
            system.l3 = L3Cache(clk_domain=system.cpu_clk_domain,
                                        **_get_cache_opts(NULL, 'l3', options))
            system.tol3bus = L2ToL3Bus(clk_domain=system.cpu_clk_domain)
            if not options.classic_l2:
                # In Aligned L2, an extra 4 cycles are simulated in L2Cache Pipeline, instead of L2ToL3Bus
                # So we need to subtract 4 cycles from the L2ToL3Bus response latency
                assert int(system.tol3bus.response_latency) >= 4
                system.tol3bus.response_latency -= 4
            system.tol3bus.snoop_filter.max_capacity = "32MB"
            system.l3.cpu_side = system.tol3bus.mem_side_ports
            system.l3.mem_side = system.membus.cpu_side_ports

            system.l3.do_fast_writeline = not options.kmh_align

        for i in range(options.num_cpus):
            if options.l3cache:
                # l2 -> tol3bus -> l3
                if options.classic_l2:
                    system.l2_caches[i].mem_side = system.tol3bus.cpu_side_ports
                else:
                    system.l2_wrappers[i].xbar.mem_side_ports = system.tol3bus.cpu_side_ports
                # l3 -> membus
            else:
                if options.classic_l2:
                    system.l2_caches[i].mem_side = system.membus.cpu_side_ports
                else:
                    system.l2_wrappers[i].xbar.mem_side_ports = system.membus.cpu_side_ports

    if options.memchecker:
        system.memchecker = MemChecker()

    for i in range(options.num_cpus):
        if options.caches:
            icache = icache_class(**_get_cache_opts(system.cpu[i], 'l1i', options))
            dcache = dcache_class(**_get_cache_opts(system.cpu[i], 'l1d', options))
            if dcache.prefetcher != NULL and options.cpu_type == 'DerivO3CPU':
                system.cpu[i].add_pf_downstream(dcache.prefetcher)

            if options.ideal_cache:
                icache.response_latency = 0
                dcache.response_latency = 0

            dcache.do_fast_writeline = not options.kmh_align
            l2_prefetcher = system.l2_caches[i].prefetcher if options.classic_l2 else system.l2_wrappers[i].prefetcher
            if (not options.no_pf) and options.l1_to_l2_pf_hint:
                assert dcache.prefetcher != NULL and \
                    l2_prefetcher != NULL
                dcache.prefetcher.add_pf_downstream(l2_prefetcher)

            if (not options.no_pf) and options.l3cache and options.l2_to_l3_pf_hint:
                assert l2_prefetcher != NULL and \
                    system.l3.prefetcher != NULL
                l2_prefetcher.add_pf_downstream(system.l3.prefetcher)

            # If we have a walker cache specified, instantiate two
            # instances here
            if walk_cache_class:
                iwalkcache = walk_cache_class()
                dwalkcache = walk_cache_class()
            else:
                iwalkcache = None
                dwalkcache = None

            if options.memchecker:
                dcache_mon = MemCheckerMonitor(warn_only=True)
                dcache_real = dcache

                # Do not pass the memchecker into the constructor of
                # MemCheckerMonitor, as it would create a copy; we require
                # exactly one MemChecker instance.
                dcache_mon.memchecker = system.memchecker

                # Connect monitor
                dcache_mon.mem_side = dcache.cpu_side

                # Let CPU connect to monitors
                dcache = dcache_mon

            # When connecting the caches, the clock is also inherited
            # from the CPU in question
            system.cpu[i].addPrivateSplitL1Caches(icache, dcache,
                                                  iwalkcache, dwalkcache)

            if options.memchecker:
                # The mem_side ports of the caches haven't been connected yet.
                # Make sure connectAllPorts connects the right objects.
                system.cpu[i].dcache = dcache_real
                system.cpu[i].dcache_mon = dcache_mon

        elif options.external_memory_system:
            # These port names are presented to whatever 'external' system
            # gem5 is connecting to.  Its configuration will likely depend
            # on these names.  For simplicity, we would advise configuring
            # it to use this naming scheme; if this isn't possible, change
            # the names below.
            if buildEnv['TARGET_ISA'] in ['x86', 'arm', 'riscv']:
                system.cpu[i].addPrivateSplitL1Caches(
                        ExternalCache("cpu%d.icache" % i),
                        ExternalCache("cpu%d.dcache" % i),
                        ExternalCache("cpu%d.itb_walker_cache" % i),
                        ExternalCache("cpu%d.dtb_walker_cache" % i))
            else:
                system.cpu[i].addPrivateSplitL1Caches(
                        ExternalCache("cpu%d.icache" % i),
                        ExternalCache("cpu%d.dcache" % i))

        system.cpu[i].createInterruptController()
        if options.l2cache:
            system.cpu[i].connectAllPorts(
                system.tol2bus_list[i].cpu_side_ports,
                system.membus.cpu_side_ports, system.membus.mem_side_ports)
        elif options.external_memory_system:
            system.cpu[i].connectUncachedPorts(
                system.membus.cpu_side_ports, system.membus.mem_side_ports)
        else:
            system.cpu[i].connectBus(system.membus)

    print('Finish memory system configuration')
    return system

# ExternalSlave provides a "port", but when that port connects to a cache,
# the connecting CPU SimObject wants to refer to its "cpu_side".
# The 'ExternalCache' class provides this adaptation by rewriting the name,
# eliminating distracting changes elsewhere in the config code.
class ExternalCache(ExternalSlave):
    def __getattr__(cls, attr):
        if (attr == "cpu_side"):
            attr = "port"
        return super(ExternalSlave, cls).__getattr__(attr)

    def __setattr__(cls, attr, value):
        if (attr == "cpu_side"):
            attr = "port"
        return super(ExternalSlave, cls).__setattr__(attr, value)

def ExternalCacheFactory(port_type):
    def make(name):
        return ExternalCache(port_data=name, port_type=port_type,
                             addr_ranges=[AllMemory])
    return make
