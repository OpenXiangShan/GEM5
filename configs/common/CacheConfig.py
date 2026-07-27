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
import os
import sys
import m5
from m5.objects import *
from m5.util.convert import toMemorySize
from common.Caches import *
from common.LSQBankConflict import set_lsq_bank_conflict_cache_params
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


def _parse_csv_list(raw):
    """
    将命令行传入的逗号分隔文本统一解析为列表。
    设计目标：
    1) 兼容 None / list / tuple / str；
    2) 自动去除空白和空 token；
    3) 返回纯净列表，便于后续做“长度必须一致”的严格校验。
    """
    if raw is None:
        return []
    if isinstance(raw, (list, tuple)):
        return list(raw)
    text = str(raw).strip()
    if text == "":
        return []
    return [item.strip() for item in text.split(",") if item.strip()]


def _parse_csv_addr_list(raw, field_name):
    """
    解析地址列表，支持 0x 前缀十六进制与十进制（int(token, 0)）。
    任一 token 非法都立即抛错，避免“部分成功”导致实验配置含混。
    """
    values = []
    for token in _parse_csv_list(raw):
        try:
            values.append(int(token, 0))
        except ValueError as exc:
            raise ValueError(
                f"Invalid {field_name} token '{token}', expected integer literal"
            ) from exc
    return values


def _cli_opt_provided(flag_name):
    """判断某个命令行参数是否被显式传入。"""
    return any(
        arg == flag_name or arg.startswith(flag_name + "=")
        for arg in sys.argv
    )


def _split_l3_resources_for_multi_hn(total_l3_size, total_l3_mshrs,
                                     hn_count, cacheline_size, l3_assoc):
    """
    将“总 L3 资源预算”按 HN 数量做严格等分。

    设计原则：
    1) 单 HN 保持现有行为不变；
    2) 多 HN 时保持总容量、总 MSHR 不变，只改变每个 HN 实例的份额；
    3) 任一资源无法整分都立即失败，避免静默截断污染实验结论。
    """
    if hn_count <= 0:
        raise ValueError(f"hn_count must be > 0, got {hn_count}")

    total_l3_size_bytes = int(toMemorySize(total_l3_size))
    if total_l3_size_bytes % hn_count != 0:
        raise ValueError(
            "L3 size cannot be evenly split across HNs: "
            f"total={total_l3_size} ({total_l3_size_bytes}B), hn_count={hn_count}. "
            "Please choose an --l3-size divisible by --chi-hn-count."
        )

    per_hn_l3_size = total_l3_size_bytes // hn_count
    cacheline_size = int(cacheline_size)
    l3_assoc = int(l3_assoc)
    if per_hn_l3_size % cacheline_size != 0:
        raise ValueError(
            "Per-HN L3 size must align to cache line size: "
            f"per_hn={per_hn_l3_size}B, cacheline={cacheline_size}B"
        )
    if per_hn_l3_size % (cacheline_size * l3_assoc) != 0:
        raise ValueError(
            "Per-HN L3 size must be divisible by cacheline_size * l3_assoc: "
            f"per_hn={per_hn_l3_size}B, cacheline={cacheline_size}B, "
            f"l3_assoc={l3_assoc}"
        )

    total_l3_mshrs = int(total_l3_mshrs)
    if total_l3_mshrs % hn_count != 0:
        raise ValueError(
            "L3 MSHRs cannot be evenly split across HNs: "
            f"total={total_l3_mshrs}, hn_count={hn_count}. "
            "Please use an L3 MSHR count divisible by --chi-hn-count."
        )

    per_hn_l3_mshrs = total_l3_mshrs // hn_count
    if per_hn_l3_mshrs <= 0:
        raise ValueError(
            "Per-HN L3 MSHRs must be > 0 after splitting: "
            f"total={total_l3_mshrs}, hn_count={hn_count}"
        )

    return per_hn_l3_size, per_hn_l3_mshrs


def _build_xschi_sam_intlv_masks(target_count):
    """
    生成与 xsCHI SystemAddressMapHN/SystemAddressMapRN 完全同构的 XOR masks。

    对应 C++ 逻辑：
      select_bits = floor(log2(target_count))
      select[i] = XOR(addr[6 + i], addr[6 + i + select_bits], ...)
    """
    if target_count <= 0:
        raise ValueError(f"target_count must be > 0, got {target_count}")
    if target_count == 1:
        return []
    if target_count & (target_count - 1):
        raise ValueError(
            "xsCHI interleaved ranges require target_count to be a power of 2, "
            f"got {target_count}"
        )

    select_bits = int(math.log2(target_count))
    masks = []
    for i in range(select_bits):
        mask = 0
        for bit in range(6 + i, 52, select_bits):
            mask |= 1 << bit
        masks.append(mask)
    return masks


def _build_xschi_dram_interleaved_ranges(base_range, dram_count):
    """
    按当前 xsCHI HN SAM 的地址哈希规则，为每个 DRAM 生成一条 interleaved range。
    """
    if dram_count <= 0:
        raise ValueError(f"dram_count must be > 0, got {dram_count}")
    if dram_count & (dram_count - 1):
        raise ValueError(
            "xsCHI multi-DRAM topologies require --chi-dram-count to be a "
            f"power of 2 when dram_count > 1, got {dram_count}"
        )

    base_masks = list(getattr(base_range, "masks", []))
    if base_masks:
        raise ValueError(
            "xsCHI multi-DRAM topologies only support a non-interleaved "
            f"base memory range, got {base_range}"
        )

    start = int(base_range.start)
    end = int(base_range.end)
    if end <= start:
        raise ValueError(
            "xsCHI multi-DRAM topologies require a valid base memory range, "
            f"got start={start:#x}, end={end:#x}"
        )

    total_size = end - start
    if total_size % dram_count != 0:
        raise ValueError(
            "xsCHI multi-DRAM topologies require the base memory range size "
            f"to be divisible by --chi-dram-count: size={total_size}B, "
            f"dram_count={dram_count}"
        )

    if dram_count == 1:
        return [base_range]

    masks = _build_xschi_sam_intlv_masks(dram_count)
    return [
        AddrRange(start=start, end=end, masks=masks, intlvMatch=match)
        for match in range(dram_count)
    ]


def _validate_shadow_dst_windows_non_overlapping(dst_bases, window_sizes):
    """
    检查每个 shadow 的目标窗口 [dst, dst+window) 两两不重叠。
    若重叠则直接抛错，避免实验被污染。
    """
    if len(dst_bases) != len(window_sizes):
        raise ValueError(
            "shadow dst/window length mismatch in overlap validation: "
            f"dst={len(dst_bases)} window={len(window_sizes)}"
        )

    intervals = []
    for idx, (dst, window) in enumerate(zip(dst_bases, window_sizes)):
        if dst < 0:
            raise ValueError(f"shadow dst base must be >= 0, got shadow[{idx}]={dst}")
        if window <= 0:
            raise ValueError(
                f"shadow window size must be > 0, got shadow[{idx}]={window}"
            )
        end = dst + window
        intervals.append((dst, end, idx))

    intervals.sort(key=lambda item: item[0])
    for i in range(1, len(intervals)):
        prev_start, prev_end, prev_idx = intervals[i - 1]
        curr_start, curr_end, curr_idx = intervals[i]
        if curr_start < prev_end:
            raise ValueError(
                "shadow dst windows overlap: "
                f"shadow[{prev_idx}]=[{prev_start:#x},{prev_end:#x}) "
                f"shadow[{curr_idx}]=[{curr_start:#x},{curr_end:#x})"
            )

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
            # real cache size is divided by number of slices
            inner_cache.size = inner_cache.size / num_l2_slices
            inner_cache.tags.indexing_policy.num_slices = num_l2_slices
            inner_cache.tags.indexing_policy.slice_idx = j
            if isinstance(inner_cache.replacement_policy, DRRIPRP):
                inner_cache.replacement_policy.num_slices = num_l2_slices
                inner_cache.replacement_policy.num_sets_per_slice = inner_cache.size // (64 * inner_cache.assoc)

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
            if options.CHI:
                # opt_dramsim3_ini = getattr(options, 'dramsim3_ini', None)
                root_dir = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
                chi_topology = getattr(options, 'chi_topology', 'L2ToDramSys')
                chi_voq_depth = getattr(options, 'chi_voq_depth', 2)
                chi_voq_depth_mode = getattr(options, 'chi_voq_depth_mode',
                                             'per_ingress')
                if chi_voq_depth_mode not in ('per_ingress', 'aggregate'):
                    raise ValueError(
                        f"Unsupported --chi-voq-depth-mode: {chi_voq_depth_mode}"
                    )
                chi_voq_depth_per_ingress = (chi_voq_depth_mode == 'per_ingress')
                chi_credit_model = getattr(options, 'chi_credit_model', 'legacy')
                if chi_credit_model not in ('legacy', 'cmn700', 'cmn700_rtl'):
                    raise ValueError(
                        f"Unsupported --chi-credit-model: {chi_credit_model}"
                    )
                chi_rxbuf_num = int(getattr(options, 'chi_rxbuf_num', 0))
                if chi_rxbuf_num < 0:
                    raise ValueError("--chi-rxbuf-num must be >= 0")
                chi_skid_depth = int(getattr(options, 'chi_skid_depth', 0))
                if chi_skid_depth < 0:
                    raise ValueError("--chi-skid-depth must be >= 0")
                chi_ib_depth = int(getattr(options, 'chi_ib_depth', 0))
                if chi_ib_depth < 0:
                    raise ValueError("--chi-ib-depth must be >= 0")
                chi_initial_credit_count = int(
                    getattr(options, 'chi_initial_credit_count', 0))
                if chi_initial_credit_count < 0:
                    raise ValueError("--chi-initial-credit-count must be >= 0")
                if chi_credit_model == 'cmn700_rtl':
                    if chi_rxbuf_num == 0:
                        chi_rxbuf_num = 3
                    if chi_ib_depth == 0 and chi_skid_depth == 0:
                        chi_ib_depth = 2
                        chi_skid_depth = chi_rxbuf_num - chi_ib_depth
                    elif chi_ib_depth == 0:
                        chi_ib_depth = chi_rxbuf_num - chi_skid_depth
                    elif chi_skid_depth == 0:
                        chi_skid_depth = chi_rxbuf_num - chi_ib_depth
                    if chi_rxbuf_num != chi_skid_depth + chi_ib_depth:
                        raise ValueError(
                            "cmn700_rtl requires "
                            "--chi-rxbuf-num == --chi-skid-depth + "
                            "--chi-ib-depth"
                        )
                    if chi_skid_depth <= 0 or chi_ib_depth <= 0:
                        raise ValueError(
                            "cmn700_rtl requires positive skid_depth and "
                            "ib_depth"
                        )
                    if chi_initial_credit_count == 0:
                        chi_initial_credit_count = chi_rxbuf_num
                    if chi_initial_credit_count > chi_rxbuf_num:
                        raise ValueError(
                            "cmn700_rtl requires initial_credit_count <= "
                            "rxbuf_num"
                        )
                chi_ib_depth_effective = (
                    chi_ib_depth if chi_ib_depth != 0 else chi_voq_depth)
                chi_up_crd_lat_int = int(getattr(options, 'chi_up_crd_lat_int', 1))
                chi_up_crd_lat_ext = int(getattr(options, 'chi_up_crd_lat_ext', 2))
                chi_dn_crd_lat_int = int(getattr(options, 'chi_dn_crd_lat_int', 2))
                chi_dn_crd_lat_ext = int(getattr(options, 'chi_dn_crd_lat_ext', 1))
                chi_internal_crd_lat = int(getattr(options, 'chi_internal_crd_lat', 1))
                chi_ddr_read_response_padding_cycles = int(
                    getattr(options, 'chi_ddr_read_response_padding_cycles', 0))
                if chi_ddr_read_response_padding_cycles < 0:
                    raise ValueError(
                        "--chi-ddr-read-response-padding-cycles must be >= 0")
                dramsim3_config_file = getattr(options, 'dramsim3_ini', None)
                if not dramsim3_config_file:
                    dramsim3_config_file = os.path.join(
                        root_dir,
                        'ext/dramsim3/xiangshan_configs/'
                        'xiangshan_DDR4_32Gb_x8_3200_8ch.ini',
                    )
                total_l3_mshrs = getattr(options, 'l3_mshrs', None)
                if total_l3_mshrs is None:
                    total_l3_mshrs = int(L3Cache.mshrs)
                else:
                    total_l3_mshrs = int(total_l3_mshrs)
                    if total_l3_mshrs <= 0:
                        raise ValueError("--l3_mshrs must be > 0")

                def make_chi_port(credit_return_direction='internal',
                                  credit_release_policy='on_accept'):
                    if credit_return_direction not in ('up', 'down', 'internal'):
                        raise ValueError(
                            "Unsupported CHIPort credit_return_direction: "
                            f"{credit_return_direction}"
                        )
                    if credit_release_policy not in (
                            'on_accept', 'on_downstream_release'):
                        raise ValueError(
                            "Unsupported CHIPort credit_release_policy: "
                            f"{credit_release_policy}"
                        )
                    kwargs = dict(
                        credit_model=chi_credit_model,
                        credit_return_direction=credit_return_direction,
                        credit_release_policy=credit_release_policy,
                        up_crd_lat_int=chi_up_crd_lat_int,
                        up_crd_lat_ext=chi_up_crd_lat_ext,
                        dn_crd_lat_int=chi_dn_crd_lat_int,
                        dn_crd_lat_ext=chi_dn_crd_lat_ext,
                        internal_crd_lat=chi_internal_crd_lat,
                    )
                    if chi_rxbuf_num != 0:
                        kwargs["rxbuf_num"] = chi_rxbuf_num
                    if chi_skid_depth != 0:
                        kwargs["skid_depth"] = chi_skid_depth
                    if chi_initial_credit_count != 0:
                        kwargs["initial_credit_count"] = (
                            chi_initial_credit_count)
                    return CHIPort(**kwargs)

                def make_mesh_port(credit_return_direction='internal'):
                    policy = ('on_downstream_release'
                              if chi_credit_model == 'cmn700_rtl'
                              else 'on_accept')
                    return make_chi_port(credit_return_direction, policy)

                def make_chi_l3_internal_xbar():
                    return CoherentXBar(
                        clk_domain=system.cpu_clk_domain,
                        width=512,
                        frontend_latency=0,
                        forward_latency=0,
                        response_latency=0,
                        header_latency=0,
                        snoop_response_latency=0,
                        snoop_filter=SnoopFilter(lookup_latency=0),
                        point_of_unification=True)

                def _build_shadow_l2_config(
                    default_attach_point="mesh3.local0",
                    default_attach_points=None,
                    enable_auto_mapping_defaults=False,
                ):
                    shadow_enable = bool(getattr(options, "shadow_l2_enable", False))
                    shadow_count = int(getattr(options, "shadow_l2_count", 1))
                    if shadow_enable and shadow_count <= 0:
                        raise ValueError(
                            "--shadow-l2-count must be > 0 when --shadow-l2-enable is set"
                        )

                    shadow_bridges = []
                    shadow_attach_points = []
                    shadow_src_bases = []
                    shadow_window_sizes = []
                    shadow_dst_bases = []
                    if shadow_enable:
                        raw_shadow_attach_points = getattr(
                            options, "shadow_attach_points", None
                        )
                        shadow_attach_opt_provided = _cli_opt_provided(
                            "--shadow-attach-points"
                        )
                        if shadow_attach_opt_provided or (
                            raw_shadow_attach_points not in (None, "", "mesh3.local0")
                        ):
                            shadow_attach_points = _parse_csv_list(raw_shadow_attach_points)
                        else:
                            if default_attach_points is not None:
                                if shadow_count > len(default_attach_points):
                                    raise ValueError(
                                        "shadow attach default list is insufficient: "
                                        f"count={shadow_count}, max_default={len(default_attach_points)}; "
                                        "please specify --shadow-attach-points explicitly"
                                    )
                                shadow_attach_points = list(
                                    default_attach_points[:shadow_count]
                                )
                            else:
                                shadow_attach_points = _parse_csv_list(
                                    default_attach_point
                                )

                        shadow_bridges = [
                            CHIBridge(networkPort=make_chi_port("down"))
                            for _ in range(shadow_count)
                        ]
                        raw_shadow_src_bases = getattr(options, "shadow_src_bases", "")
                        raw_shadow_window_sizes = getattr(
                            options, "shadow_window_sizes", ""
                        )
                        raw_shadow_dst_bases = getattr(options, "shadow_dst_bases", "")

                        shadow_src_opt_provided = _cli_opt_provided("--shadow-src-bases")
                        shadow_window_opt_provided = _cli_opt_provided(
                            "--shadow-window-sizes"
                        )
                        shadow_dst_opt_provided = _cli_opt_provided("--shadow-dst-bases")

                        shadow_src_user_provided = shadow_src_opt_provided or bool(
                            _parse_csv_list(raw_shadow_src_bases)
                        )
                        shadow_window_user_provided = shadow_window_opt_provided or bool(
                            _parse_csv_list(raw_shadow_window_sizes)
                        )
                        shadow_dst_user_provided = shadow_dst_opt_provided or bool(
                            _parse_csv_list(raw_shadow_dst_bases)
                        )
                        auto_fill_shadow_mapping = (
                            enable_auto_mapping_defaults
                            and not shadow_src_user_provided
                            and not shadow_window_user_provided
                            and not shadow_dst_user_provided
                        )

                        if auto_fill_shadow_mapping:
                            default_src_base = 0x80000000
                            default_window_size = 0x80000000
                            default_dst_base_start = 0x100000000
                            shadow_src_bases = [
                                default_src_base for _ in range(shadow_count)
                            ]
                            shadow_window_sizes = [
                                default_window_size for _ in range(shadow_count)
                            ]
                            shadow_dst_bases = [
                                default_dst_base_start + i * default_window_size
                                for i in range(shadow_count)
                            ]
                        else:
                            shadow_src_bases = _parse_csv_addr_list(
                                raw_shadow_src_bases, "shadow-src-bases"
                            )
                            shadow_window_sizes = _parse_csv_addr_list(
                                raw_shadow_window_sizes, "shadow-window-sizes"
                            )
                            shadow_dst_bases = _parse_csv_addr_list(
                                raw_shadow_dst_bases, "shadow-dst-bases"
                            )
                            if len(shadow_src_bases) == 1 and shadow_count > 1:
                                shadow_src_bases *= shadow_count
                            if len(shadow_window_sizes) == 1 and shadow_count > 1:
                                shadow_window_sizes *= shadow_count

                        if len(shadow_attach_points) != shadow_count:
                            raise ValueError(
                                f"shadow attach points count mismatch: expected {shadow_count}, "
                                f"got {len(shadow_attach_points)}"
                            )
                        if len(shadow_src_bases) != shadow_count:
                            raise ValueError(
                                f"shadow src bases count mismatch: expected {shadow_count}, "
                                f"got {len(shadow_src_bases)}"
                            )
                        if len(shadow_window_sizes) != shadow_count:
                            raise ValueError(
                                f"shadow window sizes count mismatch: expected {shadow_count}, "
                                f"got {len(shadow_window_sizes)}"
                            )
                        if len(shadow_dst_bases) != shadow_count:
                            raise ValueError(
                                f"shadow dst bases count mismatch: expected {shadow_count}, "
                                f"got {len(shadow_dst_bases)}"
                            )

                        for i, src_base in enumerate(shadow_src_bases):
                            if src_base < 0:
                                raise ValueError(
                                    f"shadow src base must be >= 0, got shadow[{i}]={src_base}"
                                )
                        for i, window_size in enumerate(shadow_window_sizes):
                            if window_size <= 0:
                                raise ValueError(
                                    "shadow window size must be > 0, "
                                    f"got shadow[{i}]={window_size}"
                                )
                        for i, dst_base in enumerate(shadow_dst_bases):
                            if dst_base < 0:
                                raise ValueError(
                                    f"shadow dst base must be >= 0, got shadow[{i}]={dst_base}"
                                )

                        _validate_shadow_dst_windows_non_overlapping(
                            shadow_dst_bases, shadow_window_sizes
                        )

                        for i in range(shadow_count):
                            print(
                                "[xsCHI][ShadowCfg] "
                                f"idx={i} attach={shadow_attach_points[i]} "
                                f"src={shadow_src_bases[i]:#x} "
                                f"window={shadow_window_sizes[i]:#x} "
                                f"dst={shadow_dst_bases[i]:#x}"
                            )

                    return {
                        "shadow_enable": shadow_enable,
                        "shadow_bridges": shadow_bridges,
                        "shadow_attach_points": shadow_attach_points,
                        "shadow_src_bases": shadow_src_bases,
                        "shadow_window_sizes": shadow_window_sizes,
                        "shadow_dst_bases": shadow_dst_bases,
                    }

                def _build_attach_points(
                    raw_attach_points,
                    flag_name,
                    count,
                    kind,
                    default_attach_point,
                    default_attach_points=None,
                ):
                    attach_opt_provided = _cli_opt_provided(flag_name)
                    if attach_opt_provided or (
                        raw_attach_points not in (None, "", default_attach_point)
                    ):
                        attach_points = _parse_csv_list(raw_attach_points)
                    else:
                        if default_attach_points is not None:
                            if count > len(default_attach_points):
                                raise ValueError(
                                    f"{kind.lower()} attach default list is insufficient: "
                                    f"count={count}, max_default={len(default_attach_points)}; "
                                    f"please specify {flag_name} explicitly"
                                )
                            attach_points = list(default_attach_points[:count])
                        else:
                            attach_points = _parse_csv_list(default_attach_point)

                    if len(attach_points) != count:
                        raise ValueError(
                            f"{kind.lower()} attach points count mismatch: expected {count}, "
                            f"got {len(attach_points)}"
                        )
                    return attach_points

                def _build_mesh_node_grid(node_x, node_y, mesh_width, mesh_height):
                    kwargs = dict(
                        node_x=node_x,
                        node_y=node_y,
                        voq_depth=chi_voq_depth,
                        ib_depth=chi_ib_depth_effective,
                        voq_depth_per_ingress=chi_voq_depth_per_ingress,
                        port_local0=make_mesh_port("up"),
                        port_local1=make_mesh_port("up"),
                    )
                    if node_x + 1 < mesh_width:
                        kwargs["port_east"] = make_mesh_port()
                    if node_x > 0:
                        kwargs["port_west"] = make_mesh_port()
                    if node_y + 1 < mesh_height:
                        kwargs["port_north"] = make_mesh_port()
                    if node_y > 0:
                        kwargs["port_south"] = make_mesh_port()
                    return MeshNode(**kwargs)

                if chi_topology in (
                    'L2L3DramSys_5x3',
                    'L2L3DramSys_6x4',
                    'L2L3DramSys_6x6',
                ):
                    if chi_topology == 'L2L3DramSys_5x3':
                        topo_cls_name = 'L2L3DramSys5x3'
                        mesh_width = 5
                        mesh_height = 3
                        default_rn_attach_point = "mesh0.local0"
                        default_shadow_attach_point = "mesh14.local0"
                        default_shadow_attach_points = [
                            "mesh14.local0",
                            "mesh12.local0",
                            "mesh10.local0",
                        ]
                        default_hn_attach_point = "mesh6.local0"
                        default_hn_attach_points = None
                        default_dram_attach_point = "mesh6.local1"
                        default_dram_attach_points = None
                        topology_variant = (
                            "rn_configurable_hn_m6_local0_dram_m6_local1"
                        )
                    elif chi_topology == 'L2L3DramSys_6x4':
                        topo_cls_name = 'L2L3DramSys6x4'
                        mesh_width = 6
                        mesh_height = 4
                        default_rn_attach_point = "mesh1.local0"
                        default_shadow_attach_point = "mesh2.local0"
                        default_shadow_attach_points = [
                            "mesh2.local0",
                            "mesh3.local0",
                            "mesh4.local0",
                            "mesh7.local0",
                            "mesh8.local0",
                            "mesh9.local0",
                            "mesh10.local0",
                            "mesh13.local0",
                            "mesh14.local0",
                            "mesh15.local0",
                            "mesh16.local0",
                            "mesh19.local0",
                            "mesh20.local0",
                            "mesh21.local0",
                            "mesh22.local0",
                        ]
                        default_hn_attach_point = "mesh6.local0"
                        default_hn_attach_points = [
                            "mesh1.local1",
                            "mesh2.local1",
                            "mesh3.local1",
                            "mesh4.local1",
                            "mesh7.local1",
                            "mesh8.local1",
                            "mesh9.local1",
                            "mesh10.local1",
                            "mesh13.local1",
                            "mesh14.local1",
                            "mesh15.local1",
                            "mesh16.local1",
                            "mesh19.local1",
                            "mesh20.local1",
                            "mesh21.local1",
                            "mesh22.local1",
                        ]
                        default_dram_attach_point = "mesh6.local1"
                        default_dram_attach_points = [
                            "mesh6.local0",
                            "mesh12.local0",
                            "mesh11.local0",
                            "mesh17.local0",
                        ]
                        topology_variant = (
                            "rn_m1_local0_16hn_local1_4sn_cmn700"
                        )
                    else:
                        topo_cls_name = 'L2L3DramSys6x6'
                        mesh_width = 6
                        mesh_height = 6
                        default_rn_attach_point = "mesh7.local0"
                        default_shadow_attach_point = "mesh8.local0"
                        default_shadow_attach_points = [
                            "mesh8.local0",
                            "mesh9.local0",
                            "mesh10.local0",
                            "mesh13.local0",
                            "mesh14.local0",
                            "mesh15.local0",
                            "mesh16.local0",
                            "mesh19.local0",
                            "mesh20.local0",
                            "mesh21.local0",
                            "mesh22.local0",
                            "mesh25.local0",
                            "mesh26.local0",
                            "mesh27.local0",
                            "mesh28.local0",
                        ]
                        default_hn_attach_point = "mesh7.local1"
                        default_hn_attach_points = [
                            "mesh7.local1",
                            "mesh8.local1",
                            "mesh9.local1",
                            "mesh10.local1",
                            "mesh13.local1",
                            "mesh14.local1",
                            "mesh15.local1",
                            "mesh16.local1",
                            "mesh19.local1",
                            "mesh20.local1",
                            "mesh21.local1",
                            "mesh22.local1",
                            "mesh25.local1",
                            "mesh26.local1",
                            "mesh27.local1",
                            "mesh28.local1",
                        ]
                        default_dram_attach_point = "mesh1.local0"
                        default_dram_attach_points = [
                            "mesh1.local0",
                            "mesh4.local0",
                            "mesh31.local0",
                            "mesh34.local0",
                        ]
                        topology_variant = (
                            "rn_m7_local0_16hn_local1_4sn_cmn700_6x6"
                        )

                    l2l3_topo_cls = globals().get(topo_cls_name)
                    if l2l3_topo_cls is None:
                        raise RuntimeError(
                            f"CHI topology '{chi_topology}' requires SimObject "
                            f"'{topo_cls_name}', but it is unavailable in this build. "
                            "Please rebuild gem5 with xsCHI TopoSys enabled."
                        )

                    shadow_cfg = _build_shadow_l2_config(
                        default_attach_point=default_shadow_attach_point,
                        default_attach_points=default_shadow_attach_points,
                        enable_auto_mapping_defaults=True,
                    )
                    hn_count = int(getattr(options, "chi_hn_count", 1))
                    dram_count = int(getattr(options, "chi_dram_count", 1))
                    if default_hn_attach_points is not None:
                        if not _cli_opt_provided("--chi-hn-count"):
                            hn_count = len(default_hn_attach_points)
                    if default_dram_attach_points is not None:
                        if not _cli_opt_provided("--chi-dram-count"):
                            dram_count = len(default_dram_attach_points)
                    if hn_count <= 0:
                        raise ValueError("--chi-hn-count must be > 0")
                    if dram_count <= 0:
                        raise ValueError("--chi-dram-count must be > 0")

                    raw_rn_attach_point = getattr(
                        options, "chi_rn_attach_point", None
                    )
                    if _cli_opt_provided("--chi-rn-attach-point") or (
                        raw_rn_attach_point not in (None, "", "mesh0.local0")
                    ):
                        rn_attach_point = raw_rn_attach_point
                    else:
                        rn_attach_point = default_rn_attach_point
                    hn_attach_points = _build_attach_points(
                        getattr(options, "chi_hn_attach_points", None),
                        "--chi-hn-attach-points",
                        hn_count,
                        "HN",
                        default_hn_attach_point,
                        default_attach_points=default_hn_attach_points,
                    )
                    dram_attach_points = _build_attach_points(
                        getattr(options, "chi_dram_attach_points", None),
                        "--chi-dram-attach-points",
                        dram_count,
                        "DRAM",
                        default_dram_attach_point,
                        default_attach_points=default_dram_attach_points,
                    )

                    per_hn_l3_size, per_hn_l3_mshrs = (
                        _split_l3_resources_for_multi_hn(
                            total_l3_size=options.l3_size,
                            total_l3_mshrs=total_l3_mshrs,
                            hn_count=hn_count,
                            cacheline_size=system.cache_line_size,
                            l3_assoc=options.l3_assoc,
                        )
                    )
                    print(
                        "[xsCHI][L3Split] "
                        f"hn_count={hn_count} "
                        f"total_l3_size={options.l3_size} "
                        f"per_hn_l3_size={per_hn_l3_size}B "
                        f"total_l3_mshrs={total_l3_mshrs} "
                        f"per_hn_l3_mshrs={per_hn_l3_mshrs}"
                    )
                    per_hn_l3_size_str = f"{per_hn_l3_size}B"

                    hn_objs = []
                    for i in range(hn_count):
                        hn_cache_wrapper = L3CacheWrapper(
                            clk_domain=system.cpu_clk_domain,
                            num_slices=1,
                            cache_size=per_hn_l3_size_str,
                            cache_assoc=options.l3_assoc,
                            block_bits=int(math.log2(system.cache_line_size)),
                        )
                        hn_cache_slice = L2CacheSlice(clk_domain=system.cpu_clk_domain)
                        hn_cache_opts = _get_cache_opts(system.cpu[0], 'l3', options)
                        hn_cache_opts['size'] = per_hn_l3_size_str
                        hn_cache = L3Cache(
                            clk_domain=system.cpu_clk_domain,
                            **hn_cache_opts
                        )
                        hn_cache_wrapper.slices = [hn_cache_slice]
                        hn_cache_slice.inner_cache = hn_cache
                        hn_cache.tags.indexing_policy.num_slices = 1
                        hn_cache.tags.indexing_policy.slice_idx = 0
                        if isinstance(hn_cache.replacement_policy, DRRIPRP):
                            hn_cache.replacement_policy.num_slices = 1
                            hn_cache.replacement_policy.num_sets_per_slice = (
                                hn_cache.size // (64 * hn_cache.assoc)
                            )
                        hn_cache_wrapper.addCacheAccessor(hn_cache)
                        hn_cache_wrapper.addSliceAccessor(hn_cache_slice)
                        hn_cache_slice.setCacheAccessor(hn_cache)
                        hn_cache.do_fast_writeline = not options.kmh_align
                        hn_cache.mshrs = per_hn_l3_mshrs
                        if options.ideal_cache:
                            hn_cache.response_latency = 0
                            hn_cache.tag_latency = 1
                            hn_cache.data_latency = 1
                            hn_cache.sequential_access = False
                            hn_cache.writeback_clean = False
                            hn_cache.mshrs = per_hn_l3_mshrs
                        if options.xiangshan_ecore:
                            hn_cache.response_latency = 66
                            hn_cache.writeback_clean = False
                        hn_cache_wrapper.slice_cpuside_ports = hn_cache_slice.cpu_side
                        hn_cache_slice.inner_cpu_port = hn_cache.cpu_side
                        hn_cache.mem_side = hn_cache_slice.inner_mem_port

                        hn_obj = CHI_L3(
                            networkPort=make_chi_port("down"),
                            coherent_xbar=make_chi_l3_internal_xbar(),
                            cache_wrapper=hn_cache_wrapper,
                        )
                        hn_obj.inner_req_port = hn_obj.coherent_xbar.cpu_side_ports
                        hn_obj.coherent_xbar.mem_side_ports = (
                            hn_obj.cache_wrapper.cpu_side
                        )
                        hn_obj.inner_resp_port = hn_cache_slice.mem_side
                        hn_objs.append(hn_obj)

                    if len(system.mem_ranges) != 1:
                        raise ValueError(
                            f"CHI topology '{chi_topology}' currently supports "
                            "exactly one base memory range, got "
                            f"{len(system.mem_ranges)}"
                        )

                    dram_ranges = _build_xschi_dram_interleaved_ranges(
                        system.mem_ranges[0],
                        dram_count,
                    )
                    for idx, dram_range in enumerate(dram_ranges):
                        print(
                            "[xsCHI][DRAMRange] "
                            f"idx={idx} "
                            f"match={getattr(dram_range, 'intlvMatch', 0)} "
                            f"masks={[hex(mask) for mask in getattr(dram_range, 'masks', [])]} "
                            f"range={dram_range}"
                        )

                    dram_objs = [
                        DDRWrapper(
                            networkPort=make_chi_port("down"),
                            range=dram_ranges[i],
                            configFile=dramsim3_config_file,
                            filePath=os.path.join(root_dir, 'ext/dramsim3/DRAMsim3/'),
                            read_response_padding_cycles=(
                                chi_ddr_read_response_padding_cycles),
                        )
                        for i in range(dram_count)
                    ]

                    mesh_nodes = []
                    for y in range(mesh_height):
                        for x in range(mesh_width):
                            mesh_nodes.append(
                                _build_mesh_node_grid(
                                    x, y, mesh_width, mesh_height
                                )
                            )
                    mesh_kwargs = {
                        f"MeshNode{i}": mesh_nodes[i]
                        for i in range(len(mesh_nodes))
                    }

                    system.CHIsys = l2l3_topo_cls(
                        L2Wrapper=CHI_L2(
                            RNBridge=CHIBridge(networkPort=make_chi_port("down")),
                            ShadowRNBridges=shadow_cfg["shadow_bridges"],
                            shadow_enable=shadow_cfg["shadow_enable"],
                            shadow_src_bases=shadow_cfg["shadow_src_bases"],
                            shadow_window_sizes=shadow_cfg["shadow_window_sizes"],
                            shadow_dst_bases=shadow_cfg["shadow_dst_bases"],
                        ),
                        rn_attach_point=rn_attach_point,
                        HNs=hn_objs,
                        hn_attach_points=hn_attach_points,
                        dramsim3s=dram_objs,
                        dram_attach_points=dram_attach_points,
                        ShadowRNBridges=shadow_cfg["shadow_bridges"],
                        shadow_attach_points=shadow_cfg["shadow_attach_points"],
                        **mesh_kwargs,
                    )
                    system.memories = dram_objs
                    system.CHIsys.ShadowRNBridges = shadow_cfg["shadow_bridges"]
                    system.CHIsys.shadow_attach_points = shadow_cfg["shadow_attach_points"]
                    system.CHIsys.mem_side_port = system.membus.cpu_side_ports
                    mesh_summary = " ".join(
                        f"M{i}=({i % mesh_width},{i // mesh_width})"
                        for i in range(mesh_width * mesh_height)
                    )
                    print(
                        f"[xsCHI][Build] mesh={mesh_width}x{mesh_height} "
                        f"{mesh_summary} "
                        f"endpoints: RN@{rn_attach_point} HN@{hn_attach_points} "
                        f"DRAM@{dram_attach_points} topology={chi_topology} "
                        f"variant={topology_variant} "
                        f"shadow_enable={shadow_cfg['shadow_enable']} "
                        f"shadow_count={len(shadow_cfg['shadow_bridges'])} "
                        f"shadow_attach={shadow_cfg['shadow_attach_points']}"
                    )

                elif chi_topology in (
                    'L2L3DramSys',
                    'L2L3DramSys_M1Local1Dram',
                    'L2L3DramSys_3x3',
                ):
                    topology_cls_map = {
                        'L2L3DramSys': 'L2L3DramSys',
                        'L2L3DramSys_M1Local1Dram': 'L2L3DramSysM1Local1Dram',
                        'L2L3DramSys_3x3': 'L2L3DramSys3x3',
                    }
                    topo_cls_name = topology_cls_map[chi_topology]
                    l2l3_topo_cls = globals().get(topo_cls_name)
                    if l2l3_topo_cls is None:
                        raise RuntimeError(
                            f"CHI topology '{chi_topology}' requires SimObject "
                            f"'{topo_cls_name}', but it is unavailable in this build. "
                            "Please rebuild gem5 with xsCHI TopoSys enabled."
                        )

                    use_mesh1_local1_dram = (
                        chi_topology == 'L2L3DramSys_M1Local1Dram'
                    )
                    use_mesh_3x3 = (chi_topology == 'L2L3DramSys_3x3')
                    topology_variant = (
                        "rn_m0_local0_hn_m4_local0_dram_m4_local1"
                        if use_mesh_3x3
                        else (
                            "rn_m0_local0_hn_m1_local0_dram_m1_local1"
                            if use_mesh1_local1_dram
                            else "rn_m0_local0_hn_m1_local0_dram_m2_local0"
                        )
                    )
                    if use_mesh_3x3:
                        shadow_cfg = _build_shadow_l2_config(
                            default_attach_point="mesh8.local0",
                            default_attach_points=[
                                "mesh8.local0",
                                "mesh6.local0",
                                "mesh2.local0",
                            ],
                            enable_auto_mapping_defaults=True,
                        )
                    else:
                        shadow_cfg = _build_shadow_l2_config("mesh3.local0")

                    l3_inner_cache_wrapper = L3CacheWrapper(
                        clk_domain=system.cpu_clk_domain,
                        num_slices=1,
                        cache_size=options.l3_size,
                        cache_assoc=options.l3_assoc,
                        block_bits=int(math.log2(system.cache_line_size)),
                    )
                    system.CHIsys = l2l3_topo_cls(
                        dramsim3=DDRWrapper(
                            networkPort=make_chi_port("down"),
                            range=system.mem_ranges[0],
                            configFile=dramsim3_config_file,
                            filePath=os.path.join(root_dir, 'ext/dramsim3/DRAMsim3/'),
                            read_response_padding_cycles=(
                                chi_ddr_read_response_padding_cycles),
                        ),
                        ShadowRNBridges=shadow_cfg["shadow_bridges"],
                        shadow_attach_points=shadow_cfg["shadow_attach_points"],
                    )
                    system.CHIsys.L2Wrapper = CHI_L2(
                        RNBridge=CHIBridge(networkPort=make_chi_port("down")),
                        ShadowRNBridges=shadow_cfg["shadow_bridges"],
                        shadow_enable=shadow_cfg["shadow_enable"],
                        shadow_src_bases=shadow_cfg["shadow_src_bases"],
                        shadow_window_sizes=shadow_cfg["shadow_window_sizes"],
                        shadow_dst_bases=shadow_cfg["shadow_dst_bases"],
                    )
                    system.CHIsys.L3 = CHI_L3(
                        networkPort=make_chi_port("down"),
                        coherent_xbar=make_chi_l3_internal_xbar(),
                        cache_wrapper=l3_inner_cache_wrapper
                    )
                    system.CHIsys.ShadowRNBridges = shadow_cfg["shadow_bridges"]
                    system.CHIsys.shadow_attach_points = shadow_cfg["shadow_attach_points"]

                    if use_mesh_3x3:
                        # 3x3 mesh used by xsCHI TopoSys.
                        # Coordinates (row-major):
                        # M0=(0,0) M1=(1,0) M2=(2,0)
                        # M3=(0,1) M4=(1,1) M5=(2,1)
                        # M6=(0,2) M7=(1,2) M8=(2,2)
                        system.CHIsys.MeshNode0 = MeshNode(
                            node_x=0, node_y=0,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_east=make_mesh_port(),
                            port_north=make_mesh_port())
                        system.CHIsys.MeshNode1 = MeshNode(
                            node_x=1, node_y=0,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_east=make_mesh_port(),
                            port_west=make_mesh_port(),
                            port_north=make_mesh_port())
                        system.CHIsys.MeshNode2 = MeshNode(
                            node_x=2, node_y=0,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_west=make_mesh_port(),
                            port_north=make_mesh_port())
                        system.CHIsys.MeshNode3 = MeshNode(
                            node_x=0, node_y=1,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_east=make_mesh_port(),
                            port_north=make_mesh_port(),
                            port_south=make_mesh_port())
                        system.CHIsys.MeshNode4 = MeshNode(
                            node_x=1, node_y=1,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_east=make_mesh_port(),
                            port_west=make_mesh_port(),
                            port_north=make_mesh_port(),
                            port_south=make_mesh_port())
                        system.CHIsys.MeshNode5 = MeshNode(
                            node_x=2, node_y=1,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_west=make_mesh_port(),
                            port_north=make_mesh_port(),
                            port_south=make_mesh_port())
                        system.CHIsys.MeshNode6 = MeshNode(
                            node_x=0, node_y=2,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_east=make_mesh_port(),
                            port_south=make_mesh_port())
                        system.CHIsys.MeshNode7 = MeshNode(
                            node_x=1, node_y=2,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_east=make_mesh_port(),
                            port_west=make_mesh_port(),
                            port_south=make_mesh_port())
                        system.CHIsys.MeshNode8 = MeshNode(
                            node_x=2, node_y=2,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_west=make_mesh_port(),
                            port_south=make_mesh_port())
                        print(
                            "[xsCHI][Build] mesh=3x3 "
                            "M0=(0,0) M1=(1,0) M2=(2,0) "
                            "M3=(0,1) M4=(1,1) M5=(2,1) "
                            "M6=(0,2) M7=(1,2) M8=(2,2) "
                            "endpoints: RN@M0.local0 HN@M4.local0 DRAM@M4.local1 "
                            f"topology={chi_topology} variant={topology_variant} "
                            f"shadow_enable={shadow_cfg['shadow_enable']} "
                            f"shadow_count={len(shadow_cfg['shadow_bridges'])} "
                            f"shadow_attach={shadow_cfg['shadow_attach_points']}"
                        )
                    else:
                        # 2x2 mesh used by xsCHI TopoSys.
                        # Coordinates:
                        #   Mesh0=(0,0), Mesh1=(1,0), Mesh2=(1,1), Mesh3=(0,1)
                        system.CHIsys.MeshNode0 = MeshNode(
                            node_x=0, node_y=0,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_east=make_mesh_port(),
                            port_north=make_mesh_port())
                        system.CHIsys.MeshNode1 = MeshNode(
                            node_x=1, node_y=0,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_west=make_mesh_port(),
                            port_north=make_mesh_port())
                        system.CHIsys.MeshNode2 = MeshNode(
                            node_x=1, node_y=1,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_west=make_mesh_port(),
                            port_south=make_mesh_port())
                        system.CHIsys.MeshNode3 = MeshNode(
                            node_x=0, node_y=1,
                            voq_depth=chi_voq_depth,
                            ib_depth=chi_ib_depth_effective,
                            voq_depth_per_ingress=chi_voq_depth_per_ingress,
                            port_local0=make_mesh_port("up"),
                            port_local1=make_mesh_port("up"),
                            port_east=make_mesh_port(),
                            port_south=make_mesh_port())
                        print(
                            "[xsCHI][Build] mesh=2x2 "
                            "M0=(0,0) M1=(1,0) M2=(1,1) M3=(0,1) "
                            "endpoints: RN@M0.local0 HN@M1.local0 "
                            f"DRAM@{'M1.local1' if use_mesh1_local1_dram else 'M2.local0'} "
                            f"topology={chi_topology} variant={topology_variant} "
                            f"shadow_enable={shadow_cfg['shadow_enable']} "
                            f"shadow_count={len(shadow_cfg['shadow_bridges'])} "
                            f"shadow_attach={shadow_cfg['shadow_attach_points']}"
                        )

                    l3_cache_slice = L2CacheSlice(clk_domain=system.cpu_clk_domain)
                    l3_inner_cache = L3Cache(
                        clk_domain=system.cpu_clk_domain,
                        **_get_cache_opts(system.cpu[0], 'l3', options)
                    )
                    l3_inner_cache_wrapper.slices = [l3_cache_slice]
                    l3_cache_slice.inner_cache = l3_inner_cache

                    l3_inner_cache.tags.indexing_policy.num_slices = 1
                    l3_inner_cache.tags.indexing_policy.slice_idx = 0
                    if isinstance(l3_inner_cache.replacement_policy, DRRIPRP):
                        l3_inner_cache.replacement_policy.num_slices = 1
                        l3_inner_cache.replacement_policy.num_sets_per_slice = (
                            l3_inner_cache.size // (64 * l3_inner_cache.assoc)
                        )

                    l3_inner_cache_wrapper.addCacheAccessor(l3_inner_cache)
                    l3_inner_cache_wrapper.addSliceAccessor(l3_cache_slice)
                    l3_cache_slice.setCacheAccessor(l3_inner_cache)

                    l3_inner_cache.do_fast_writeline = not options.kmh_align
                    l3_inner_cache.mshrs = total_l3_mshrs
                    if options.ideal_cache:
                        l3_inner_cache.response_latency = 0
                        l3_inner_cache.tag_latency = 1
                        l3_inner_cache.data_latency = 1
                        l3_inner_cache.sequential_access = False
                        l3_inner_cache.writeback_clean = False
                        l3_inner_cache.mshrs = total_l3_mshrs
                    if options.xiangshan_ecore:
                        l3_inner_cache.response_latency = 66
                        l3_inner_cache.writeback_clean = False

                    l3_inner_cache_wrapper.slice_cpuside_ports = l3_cache_slice.cpu_side
                    l3_cache_slice.inner_cpu_port = l3_inner_cache.cpu_side
                    l3_inner_cache.mem_side = l3_cache_slice.inner_mem_port

                    system.CHIsys.L3.inner_req_port = system.CHIsys.L3.coherent_xbar.cpu_side_ports
                    system.CHIsys.L3.coherent_xbar.mem_side_ports = system.CHIsys.L3.cache_wrapper.cpu_side
                    system.CHIsys.L3.inner_resp_port = l3_cache_slice.mem_side
                    system.CHIsys.mem_side_port = system.membus.cpu_side_ports
                else:
                    if chi_topology not in ('L2ToDramSys', 'L2ToDramSys_M1Local1Dram'):
                        raise ValueError(
                            f"Unsupported --chi-topology for xsCHI L2ToDramSys branch: {chi_topology}"
                        )
                    use_mesh1_local1_dram = (chi_topology == 'L2ToDramSys_M1Local1Dram')
                    topology_variant = (
                        "rn_m0_local0_hn_m1_local0_dram_m1_local1"
                        if use_mesh1_local1_dram
                        else "rn_m0_local0_hn_m1_local0_dram_m2_local0"
                    )
                    shadow_cfg = _build_shadow_l2_config("mesh3.local0")

                    system.CHIsys = L2ToDramSys(
                        configFile=dramsim3_config_file,
                        topology_variant=topology_variant,
                    )
                    system.CHIsys.L2Wrapper = CHI_L2(
                        RNBridge=CHIBridge(networkPort=make_chi_port("down")),
                        ShadowRNBridges=shadow_cfg["shadow_bridges"],
                        shadow_enable=shadow_cfg["shadow_enable"],
                        shadow_src_bases=shadow_cfg["shadow_src_bases"],
                        shadow_window_sizes=shadow_cfg["shadow_window_sizes"],
                        shadow_dst_bases=shadow_cfg["shadow_dst_bases"],
                    )
                    system.CHIsys.L3 = FakeL3(networkPort=make_chi_port("down"))
                    system.CHIsys.ShadowRNBridges = shadow_cfg["shadow_bridges"]
                    system.CHIsys.shadow_attach_points = shadow_cfg["shadow_attach_points"]

                    system.CHIsys.MeshNode0 = MeshNode(
                        node_x=0, node_y=0,
                        voq_depth=chi_voq_depth,
                        ib_depth=chi_ib_depth_effective,
                        voq_depth_per_ingress=chi_voq_depth_per_ingress,
                        port_local0=make_mesh_port("up"),
                        port_local1=make_mesh_port("up"),
                        port_east=make_mesh_port(),
                        port_north=make_mesh_port())
                    system.CHIsys.MeshNode1 = MeshNode(
                        node_x=1, node_y=0,
                        voq_depth=chi_voq_depth,
                        ib_depth=chi_ib_depth_effective,
                        voq_depth_per_ingress=chi_voq_depth_per_ingress,
                        port_local0=make_mesh_port("up"),
                        port_local1=make_mesh_port("up"),
                        port_west=make_mesh_port(),
                        port_north=make_mesh_port())
                    system.CHIsys.MeshNode2 = MeshNode(
                        node_x=1, node_y=1,
                        voq_depth=chi_voq_depth,
                        ib_depth=chi_ib_depth_effective,
                        voq_depth_per_ingress=chi_voq_depth_per_ingress,
                        port_local0=make_mesh_port("up"),
                        port_local1=make_mesh_port("up"),
                        port_west=make_mesh_port(),
                        port_south=make_mesh_port())
                    system.CHIsys.MeshNode3 = MeshNode(
                        node_x=0, node_y=1,
                        voq_depth=chi_voq_depth,
                        ib_depth=chi_ib_depth_effective,
                        voq_depth_per_ingress=chi_voq_depth_per_ingress,
                        port_local0=make_mesh_port("up"),
                        port_local1=make_mesh_port("up"),
                        port_east=make_mesh_port(),
                        port_south=make_mesh_port())
                    print(
                        "[xsCHI][Build] mesh=2x2 "
                        "M0=(0,0) M1=(1,0) M2=(1,1) M3=(0,1) "
                        "endpoints: RN@M0.local0 HN@M1.local0 "
                        f"DRAM@{'M1.local1' if use_mesh1_local1_dram else 'M2.local0'} "
                        f"topology={chi_topology} variant={topology_variant} "
                        f"shadow_enable={shadow_cfg['shadow_enable']} "
                        f"shadow_count={len(shadow_cfg['shadow_bridges'])} "
                        f"shadow_attach={shadow_cfg['shadow_attach_points']}"
                    )
                    system.CHIsys.mem_side_port = system.membus.cpu_side_ports
            else:
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
                    if options.CHI:
                        system.l2_caches[i].mem_side = system.CHIsys.cpu_side_port
                    else:
                        system.l2_caches[i].mem_side = system.tol3bus.cpu_side_ports
                else:
                    if options.CHI:
                        system.l2_wrappers[i].xbar.mem_side_ports = system.CHIsys.cpu_side_port
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
            dcache.pipe_latency = 3 if options.kmh_align else 0
            l2_prefetcher = system.l2_caches[i].prefetcher if options.classic_l2 else system.l2_wrappers[i].prefetcher
            if (not options.no_pf) and options.l1_to_l2_pf_hint:
                assert dcache.prefetcher != NULL and \
                    l2_prefetcher != NULL
                dcache.prefetcher.add_pf_downstream(l2_prefetcher)

            if (not options.no_pf) and options.l3cache and options.l2_to_l3_pf_hint and(not options.CHI):
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
        set_lsq_bank_conflict_cache_params(system.cpu[i], system)
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
