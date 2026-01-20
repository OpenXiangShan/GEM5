# 重点目录/子系统总结（相对分叉点）

- 范围：`a3be84cb1b854da51716d6399ca139016714bd54..upstream/stable`
- 说明：该报告以“目录前缀”为单位聚合，便于只关注你关心的模块。

## `src/arch/riscv/` - RISC-V ISA/平台

- unique commits: 375
- unique PRs: 128
- Top topics: `arch-riscv`(315), `misc`(11), `arch`(9), `arch,cpu`(4), `sim`(3), `base`(3)

### Top PR（按 churn）

| PR | 标题 | churn |
|---:|---|---:|
| #2022 | arch-riscv: Add support for vector stride segment load/store instructions | 2240 |
| #2341 | arch-riscv: Remove N extension | 1104 |
| #1712 | arch-riscv: Fix incorrect vector slide instructions and statically filter redundant uops | 1078 |
| #1538 | arch-riscv: add VLEN/ELEN as class attributes for all vec insts | 949 |
| #851 | arch-riscv: adding vector unit-stride segment loads to RISC-V | 708 |
| #519 | arch-riscv: Fix line length of CSRData declaration | 702 |
| #913 | arch-riscv: adding vector unit-stride segment stores to RISC-V | 594 |
| #681 | arch-riscv: Add support for RISC-V semihosting | 499 |
| #1525 | arch-riscv: Add support for riscv hardware probing syscall | 482 |
| #813 | arch-riscv: adding support for local interrupts | 478 |
| #1761 | arch-riscv: Implement Zcmt | 378 |
| #2023 | arch-riscv: Add support for fault-only-first unit-stride segment load instructions | 378 |
| #1767 | arch-riscv: Add support for Zfa extension | 367 |
| #914 | arch-riscv: Move alignment check to Physical Memory Attribute(PMA) | 298 |
| #606 | arch-riscv: Fix narrow datatypes in RVV isa files | 230 |
| #794 | arch-riscv: add unit-stride fault-only-first loads (i.e. vle*ff) | 212 |
| #2223 | arch-riscv: Fix CMO decoding | 152 |
| #1264 | arch-riscv: Fix TLB lookup with vaddrs | 145 |
| #1123 | arch-riscv: Add RVV FP16 support (Zvfh & Zvfhmin) | 144 |
| #2021 | arch-riscv: Fix incorrect vector unit-stride segment load instructions | 138 |

### Top 文件（按 churn）

| 文件 | churn |
|---|---:|
| `src/arch/riscv/isa/decoder.isa` | 11036 |
| `src/arch/riscv/isa/templates/vector_mem.isa` | 4855 |
| `src/arch/riscv/isa/templates/vector_arith.isa` | 4355 |
| `src/arch/riscv/regs/misc.hh` | 4110 |
| `src/arch/riscv/isa/formats/vector_arith.isa` | 2922 |
| `src/arch/riscv/isa.cc` | 2040 |
| `src/arch/riscv/pagetable_walker.cc` | 1851 |
| `src/arch/riscv/linux/se_workload.cc` | 1850 |
| `src/arch/riscv/insts/vector.hh` | 1581 |
| `src/arch/riscv/insts/vector.cc` | 1337 |
| `src/arch/riscv/utility.hh` | 841 |
| `src/arch/riscv/remote_gdb.cc` | 832 |
| `src/arch/riscv/isa/formats/zcmp.isa` | 816 |
| `src/arch/riscv/interrupts.cc` | 708 |
| `src/arch/riscv/rvk.hh` | 643 |
| `src/arch/riscv/tlb.cc` | 608 |
| `src/arch/riscv/isa/formats/vector_conf.isa` | 473 |
| `src/arch/riscv/isa/formats/mem.isa` | 430 |
| `src/arch/riscv/isa/formats/vector_mem.isa` | 395 |
| `src/arch/riscv/faults.cc` | 315 |

## `src/cpu/o3/` - O3 CPU

- unique commits: 131
- unique PRs: 41
- Top topics: `cpu-o3`(58), `cpu`(24), `misc`(15), `arch,cpu`(5), `arch-riscv`(5), `base-stats`(2)

### Top PR（按 churn）

| PR | 标题 | churn |
|---:|---|---:|
| #2652 | configs, cpu-o3: Implement a distributed InstructionQueue | 725 |
| #2119 | cpu-o3: Replace C++03 boilerplate with range-based for loops | 380 |
| #2518 | cpu-o3: Bundle some Fetch/IEW/Commit stats into a vector | 367 |
| #1659 | cpu-o3: Use the generic cache library to build store sets | 232 |
| #1926 | cpu-o3: add retry resp to LSQ with throttling params | 209 |
| #2510 | cpu-o3: Bundle some RenameStats into a vector | 86 |
| #2517 | cpu-o3: Bundle some DecodeStats into a vector | 75 |
| #1516 | cpu-o3: Panic if no FU exists for an instruction needing to issue | 62 |
| #2725 | cpu-o3: Add LQ and SQ average occupancy stat | 38 |
| #1872 | cpu-o3, stats: Stats Added to O3 CPU | 30 |
| #2173 | cpu-o3: put unsent mem req to retry queue | 30 |
| #2308 | cpu: Add user-mode stats | 24 |
| #1056 | cpu-o3: prioritize exiting threads when committing | 18 |
| #842 | cpu-o3: add PerThreadUnifiedThreadMap to O3 CPU | 17 |
| #2670 | cpu-o3: Instant ROB squash | 16 |
| #2312 | cpu-o3: stall fetch from commit when trap event is pending | 15 |
| #2512 | cpu-o3: properly index time buffer when clearing states | 14 |
| #2738 | cpu-o3: Increase MaxWidth to 16 | 14 |
| #1640 | cpu-o3: Add Matrix OpDesc to the O3 Default FU | 12 |
| #1534 | misc: Do not share the random number generator across components | 12 |

### Top 文件（按 churn）

| 文件 | churn |
|---|---:|
| `src/cpu/o3/bac.cc` | 1031 |
| `src/cpu/o3/cpu.cc` | 883 |
| `src/cpu/o3/fetch.cc` | 777 |
| `src/cpu/o3/inst_queue.cc` | 521 |
| `src/cpu/o3/bac.hh` | 473 |
| `src/cpu/o3/commit.cc` | 457 |
| `src/cpu/o3/iew.cc` | 424 |
| `src/cpu/o3/ftq.hh` | 362 |
| `src/cpu/o3/ftq.cc` | 300 |
| `src/cpu/o3/BaseO3CPU.py` | 295 |
| `src/cpu/o3/lsq.cc` | 285 |
| `src/cpu/o3/FuncUnitConfig.py` | 231 |
| `src/cpu/o3/rename.cc` | 229 |
| `src/cpu/o3/cpu.hh` | 186 |
| `src/cpu/o3/dyn_inst.hh` | 185 |
| `src/cpu/o3/inst_queue.hh` | 150 |
| `src/cpu/o3/store_set.cc` | 142 |
| `src/cpu/o3/regfile.cc` | 135 |
| `src/cpu/o3/fetch.hh` | 134 |
| `src/cpu/o3/decode.cc` | 109 |

## `src/cpu/pred/` - 分支预测/BTB/预测器

- unique commits: 38
- unique PRs: 13
- Top topics: `cpu`(30), `misc`(6), `python`(1), `no-prefix`(1)

### Top PR（按 churn）

| PR | 标题 | churn |
|---:|---|---:|
| #1537 | Implement BTB using the cache library | 500 |
| #2477 | cpu: move conditional pred out of bpred_unit | 418 |
| #2303 | cpu:Add gshare branch predictor model | 267 |
| #1534 | misc: Do not share the random number generator across components | 110 |
| #2515 | cpu: BTB update at squash/commit option | 99 |
| #2063 | cpu: Fix incorrect return address after flush | 41 |
| #2516 | cpu: Tage GHR out-of-bounds at rollover | 24 |
| #2127 | cpu: Fix bug exposed by clang 18's -Woverloaded-virtual | 8 |
| #2220 | cpu: fix memory leak on indirect branch prediction | 6 |
| #2499 | cpu: Workaround missing IsUnconditional flag | 4 |
| #2795 | cpu: Fix Branch Pred Simple BTB sets check | 4 |
| #1077 | cpu: Indirect predictor track conditional indirect | 3 |
| #2567 | cpu: Link tagBits to tag_bits in the SimpleBTB | 2 |

### Top 文件（按 churn）

| 文件 | churn |
|---|---:|
| `src/cpu/pred/bpred_unit.cc` | 1298 |
| `src/cpu/pred/BranchPredictor.py` | 839 |
| `src/cpu/pred/bpred_unit.hh` | 637 |
| `src/cpu/pred/tage_base.cc` | 535 |
| `src/cpu/pred/tage_sc_l.cc` | 391 |
| `src/cpu/pred/simple_indirect.cc` | 337 |
| `src/cpu/pred/btb_entry.hh` | 288 |
| `src/cpu/pred/simple_btb.cc` | 271 |
| `src/cpu/pred/ras.cc` | 254 |
| `src/cpu/pred/simple_btb.hh` | 208 |
| `src/cpu/pred/ras.hh` | 195 |
| `src/cpu/pred/statistical_corrector.cc` | 178 |
| `src/cpu/pred/gshare.cc` | 164 |
| `src/cpu/pred/btb.cc` | 156 |
| `src/cpu/pred/multiperspective_perceptron_tage.cc` | 156 |
| `src/cpu/pred/tage_base.hh` | 152 |
| `src/cpu/pred/conditional.hh` | 149 |
| `src/cpu/pred/btb.hh` | 129 |
| `src/cpu/pred/tournament.cc` | 122 |
| `src/cpu/pred/simple_indirect.hh` | 113 |

## `src/mem/cache/prefetch/` - Prefetcher 相关

- unique commits: 59
- unique PRs: 8
- Top topics: `mem-cache`(32), `mem`(15), `base, mem-cache`(4), `misc`(2), `python`(1), `misc,python`(1)

### Top PR（按 churn）

| PR | 标题 | churn |
|---:|---|---:|
| #1403 | mem: Fixed implementation of Best Offset Prefetcher | 170 |
| #564 | mem-cache: Prefetchers Improvements | 39 |
| #1449 | mem: Stride Prefetcher Fix | 35 |
| #2598 | mem-cache: Add cache_snoop parameter to Fetch Directed Prefetcher | 9 |
| #2291 | mem: fixed bug with prefetcher probes | 6 |
| #871 | mem-cache: Fix possible crash in base prefetcher | 4 |
| #2846 | mem-cache: fix memory leakage in fetch directed prefetcher | 4 |
| #2644 | mem-cache: Remove unused header in prefetch | 2 |

### Top 文件（按 churn）

| 文件 | churn |
|---|---:|
| `src/mem/cache/prefetch/Prefetcher.py` | 897 |
| `src/mem/cache/prefetch/fdp.cc` | 431 |
| `src/mem/cache/prefetch/associative_set.hh` | 347 |
| `src/mem/cache/prefetch/sms.cc` | 233 |
| `src/mem/cache/prefetch/fdp.hh` | 224 |
| `src/mem/cache/prefetch/bop.cc` | 190 |
| `src/mem/cache/prefetch/associative_set_impl.hh` | 187 |
| `src/mem/cache/prefetch/stride.cc` | 175 |
| `src/mem/cache/prefetch/base.cc` | 138 |
| `src/mem/cache/prefetch/spatio_temporal_memory_streaming.cc` | 136 |
| `src/mem/cache/prefetch/base.hh` | 95 |
| `src/mem/cache/prefetch/signature_path.cc` | 92 |
| `src/mem/cache/prefetch/irregular_stream_buffer.cc` | 91 |
| `src/mem/cache/prefetch/sms.hh` | 82 |
| `src/mem/cache/prefetch/signature_path_v2.cc` | 81 |
| `src/mem/cache/prefetch/pif.cc` | 78 |
| `src/mem/cache/prefetch/stride.hh` | 66 |
| `src/mem/cache/prefetch/indirect_memory.cc` | 63 |
| `src/mem/cache/prefetch/queued.cc` | 59 |
| `src/mem/cache/prefetch/indirect_memory.hh` | 47 |

## `src/mem/cache/` - Cache 相关（含替换策略等）

- unique commits: 123
- unique PRs: 25
- Top topics: `mem-cache`(74), `mem`(18), `misc`(9), `base, mem-cache`(4), `tests`(3), `python`(2)

### Top PR（按 churn）

| PR | 标题 | churn |
|---:|---|---:|
| #1977 | mem-cache: Unit test FIFO RP | 340 |
| #1403 | mem: Fixed implementation of Best Offset Prefetcher | 170 |
| #564 | mem-cache: Prefetchers Improvements | 43 |
| #1534 | misc: Do not share the random number generator across components | 36 |
| #1449 | mem: Stride Prefetcher Fix | 35 |
| #205 | mem-cache: Allow clflush's uncacheable requests on classic cache | 25 |
| #2598 | mem-cache: Add cache_snoop parameter to Fetch Directed Prefetcher | 9 |
| #2824 | mem-cache: fix segmentation fault caused by MSHR debug flag | 7 |
| #2291 | mem: fixed bug with prefetcher probes | 6 |
| #2685 | mem-cache: register TagExtractor for SectorTag | 6 |
| #1075 | mem-cache: Fix TreePLRU num leaves error | 5 |
| #2431 | mem-cache: Report Blocked_NoWBBuffers as cache blocking cause | 5 |
| #871 | mem-cache: Fix possible crash in base prefetcher | 4 |
| #1659 | cpu-o3: Use the generic cache library to build store sets | 4 |
| #2646 | tests: fix failing FIFO RP daily tests | 4 |
| #2846 | mem-cache: fix memory leakage in fetch directed prefetcher | 4 |
| #1061 | mem-cache: Remove power-of-2 requirement for TreePLRU num leaves | 3 |
| #534 | Fix calculation of compressed size in bytes | 2 |
| #1179 | mem-cache: Fix maybe-uninitialized warning | 2 |
| #1263 | gpu-compute,mem,systemc: This commit corrects typos of 'cache' | 2 |

### Top 文件（按 churn）

| 文件 | churn |
|---|---:|
| `src/mem/cache/prefetch/Prefetcher.py` | 897 |
| `src/mem/cache/replacement_policies/tree_plru_rp.test.cc` | 617 |
| `src/mem/cache/prefetch/fdp.cc` | 431 |
| `src/mem/cache/prefetch/associative_set.hh` | 347 |
| `src/mem/cache/replacement_policies/lfu_rp.test.cc` | 316 |
| `src/mem/cache/tags/tagged_entry.hh` | 288 |
| `src/mem/cache/replacement_policies/mru_rp.test.cc` | 275 |
| `src/mem/cache/replacement_policies/fifo_rp.test.cc` | 248 |
| `src/mem/cache/prefetch/sms.cc` | 233 |
| `src/mem/cache/prefetch/fdp.hh` | 224 |
| `src/mem/cache/prefetch/bop.cc` | 190 |
| `src/mem/cache/prefetch/associative_set_impl.hh` | 187 |
| `src/mem/cache/base.cc` | 184 |
| `src/mem/cache/tags/partitioning_policies/max_capacity_pp.cc` | 184 |
| `src/mem/cache/compressors/Compressors.py` | 181 |
| `src/mem/cache/prefetch/stride.cc` | 175 |
| `src/mem/cache/Cache.py` | 164 |
| `src/mem/cache/base.hh` | 151 |
| `src/mem/cache/prefetch/base.cc` | 138 |
| `src/mem/cache/prefetch/spatio_temporal_memory_streaming.cc` | 136 |

## `src/mem/ruby/` - Ruby 内存系统

- unique commits: 229
- unique PRs: 74
- Top topics: `mem-ruby`(152), `misc`(10), `mem-ruby, gpu-compute`(7), `no-prefix`(6), `scons`(6), `mem-ruby,sim-se`(5)

### Top PR（按 churn）

| PR | 标题 | churn |
|---:|---|---:|
| #1453 | mem-ruby: Remove static methods from RubySystem | 1079 |
| #117 | ruby: Enable all protocols in a single gem5 build | 586 |
| #2675 | mem-ruby: Add support for CLFLUSH type instructions in MESI Three Level protocol | 533 |
| #1399 | mem-ruby: Prevent LL/SC livelock in MESI protocols (#1384) | 347 |
| #1350 | arch-vega: Pass s_memtime through smem pipe | 320 |
| #120 | mem-ruby,configs: Add GPU GLC Atomic Resource Constraints | 236 |
| #2740 | mem-ruby: Update Ruby Network to use new-style stats | 225 |
| #546 | mem-ruby: Fix for not creating log entries on atomic no return requests | 194 |
| #1692 | dev-amdgpu, gpu-compute, mem-ruby: Add support for writeback L2 in GPU | 187 |
| #1101 | mem-ruby: Implement MakeReadUnique in CHI | 126 |
| #397 | mem-ruby: SLICC Fixes to GLC Atomics in WB L2 | 120 |
| #101 | mem-ruby: Added support for non-system-scope atomics in VIPER | 117 |
| #2723 | mem-ruby: Annotate CHI-TLM scheduling time in Transaction obj | 74 |
| #1254 | gpu-compute,mem-ruby: Revert "Add RubyHitMiss flag for TCP and TCC cache" | 70 |
| #692 | mem-ruby: Implement WriteUniqueZero CHI transaction | 69 |
| #1117 | mem-ruby: Reduce handshaking between CorePair and dir | 61 |
| #2241 | mem-ruby: Fix DMA sequencer request size above 64 | 56 |
| #2059 | mem-ruby: Implement CHI ReadNoSnp Request | 54 |
| #1865 | mem-ruby: Add missing option in ProtocolInfo | 50 |
| #2158 | mem-ruby: Add link name to each throttle stat | 49 |

### Top 文件（按 churn）

| 文件 | churn |
|---|---:|
| `src/mem/ruby/network/fault_model/FaultModel.py` | 2786 |
| `src/mem/ruby/protocol/chi/tlm/utils.cc` | 1410 |
| `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` | 1103 |
| `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` | 967 |
| `src/mem/ruby/protocol/chi/tlm/controller.cc` | 914 |
| `src/mem/ruby/protocol/chi/tlm/generator.hh` | 684 |
| `src/mem/ruby/protocol/chi/generic/CHIGenericController.cc` | 593 |
| `src/mem/ruby/protocol/chi/tlm/generator.cc` | 576 |
| `src/mem/ruby/protocol/chi/tlm/tlm_chi.cc` | 568 |
| `src/mem/ruby/protocol/GPU_VIPER-TCP.sm` | 473 |
| `src/mem/ruby/protocol/chi/CHI-cache-transitions.sm` | 462 |
| `src/mem/ruby/protocol/chi/generic/CHIGenericController.hh` | 443 |
| `src/mem/ruby/system/Sequencer.cc` | 433 |
| `src/mem/ruby/protocol/chi/tlm/controller.hh` | 427 |
| `src/mem/ruby/protocol/SConscript` | 409 |
| `src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm` | 399 |
| `src/mem/ruby/system/GPUCoalescer.cc` | 385 |
| `src/mem/ruby/protocol/chi/tlm/tlm_chi_gen.cc` | 279 |
| `src/mem/ruby/protocol/MESI_Three_Level-L0cache.sm` | 274 |
| `src/mem/ruby/structures/RubyPrefetcherProxy.cc` | 260 |

## `configs/` - 配置脚本/示例

- unique commits: 351
- unique PRs: 80
- Top topics: `configs`(137), `stdlib`(25), `misc`(18), `no-prefix`(17), `gpu-compute`(11), `cpu`(11)

### Top PR（按 churn）

| PR | 标题 | churn |
|---:|---|---:|
| #1619 | configs: Deprecate Vega10 | 651 |
| #2724 | cpu: Add Arm Neoverse V2 config | 634 |
| #95 | cpu-kvm: Make using perf when using KVM CPU optional | 276 |
| #1167 | stdlib,configs,tests: Add gem5 MultiSim (MultiProcessing for gem5) | 229 |
| #1272 | Adding an example for Spatter | 196 |
| #241 | configs,stdlib,tests: Remove get_runtime_isa() | 147 |
| #725 | arm,stdlib: added kvm support to the ARM board | 139 |
| #2284 | dev: Update MI300X model to use real firmware | 115 |
| #1490 | stdlib, configs: Add RiscvDemoBoard | 112 |
| #655 | configs: Make riscv/fs_linux work in build/ALL/gem5.opt | 92 |
| #1478 | arch-arm: Add arm demo board | 92 |
| #1110 | misc: Add resource versions to examples | 82 |
| #1605 | tests, configs, util, mem, python, systemc: Change base 10 units to base 2 | 82 |
| #652 | configs: Fix issues after get_runtime_isa() #241 removed | 77 |
| #102 | stdlib,configs,tests: Remove deprecated Resource classes usage | 76 |
| #607 | misc: update gapbs example to use suites | 73 |
| #2160 | configs,mem-ruby: Update ruby configs for ALL target | 64 |
| #940 | gpu-compute: Add support for skipping GPU kernels | 62 |
| #1453 | mem-ruby: Remove static methods from RubySystem | 55 |
| #1753 | configs: Update legacy RISC-V FS Linux script | 43 |

### Top 文件（按 churn）

| 文件 | churn |
|---|---:|
| `configs/common/cores/arm/HPI.py` | 1466 |
| `configs/example/apu_se.py` | 1173 |
| `configs/common/Options.py` | 1033 |
| `configs/common/cpu2000.py` | 992 |
| `configs/ruby/GPU_VIPER.py` | 889 |
| `configs/example/hsaTopology.py` | 882 |
| `configs/example/gem5_library/x86-npb-benchmarks.py` | 876 |
| `configs/example/fs.py` | 690 |
| `configs/common/FSConfig.py` | 644 |
| `configs/example/gem5_library/x86-gapbs-benchmarks.py` | 635 |
| `configs/example/gem5_library/checkpoints/simpoints-se-restore.py` | 607 |
| `configs/ruby/CHI_config.py` | 582 |
| `configs/common/HMC.py` | 543 |
| `configs/example/gem5_library/x86-spec-cpu2017-benchmarks.py` | 535 |
| `configs/example/gem5_library/x86-spec-cpu2006-benchmarks.py` | 508 |
| `configs/deprecated/example/fs.py` | 495 |
| `configs/example/gem5_library/x86-ubuntu-run-with-kvm-no-perf.py` | 489 |
| `configs/example/gpufs/vega10.py` | 485 |
| `configs/example/gem5_library/x86-global-inst-tracker.py` | 473 |
| `configs/ruby/Ruby.py` | 472 |

## `util/` - 工具脚本/资源管理

- unique commits: 261
- unique PRs: 74
- Top topics: `util`(99), `util-docker`(67), `misc`(32), `util-docker,tests`(7), `arch-riscv`(5), `build(deps)`(4)

### Top PR（按 churn）

| PR | 标题 | churn |
|---:|---|---:|
| #2079 | util: Add validator and tests for full system workloads (disk and kernels) | 1248 |
| #510 | util: Added script to copy resources from mongodb | 612 |
| #1935 | util: Add Python implementation of terminal client (gem5term) | 412 |
| #362 | misc: Add git-clang-format to pre-commit with wrapper script | 365 |
| #1170 | util-docker,gpu,gpu-compute: Improve GCN-GPU Dockerfile | 214 |
| #80 | misc: Drop older compilers and Ubuntu 18.04 | 168 |
| #1982 | sim,stdlib: Fixes for external signal | 153 |
| #911 | misc: Add a DevContainer specification to the gem5 repo | 90 |
| #236 | util-docker: Add GitHub Action to create Docker Images | 89 |
| #1876 | util: update checkpoint upgrader for MISCREG_SENVCFG | 82 |
| #1709 | arch-riscv: Fix misprediction of control flow instruction caused by vset{i}vl{i} | 76 |
| #1761 | arch-riscv: Implement Zcmt | 74 |
| #1025 | util-docker: Bump gpu-fs build docker to ROCm 6.0.2 | 70 |
| #858 | tests: Add compiler test for gcc 13 | 58 |
| #1949 | dev: rework PCI to add type1 header | 52 |
| #1592 | util-docker: Minor housekeeping to Dockerfiles | 50 |
| #1731 | util-docker: Add qemu-riscv-env Dockerfile | 50 |
| #861 | util: update list_changes.py to support multiple Change-Ids | 48 |
| #1017 | util-docker: Update docker-compose URLs to 'ghcr.io/gem5' | 48 |
| #1605 | tests, configs, util, mem, python, systemc: Change base 10 units to base 2 | 48 |

### Top 文件（按 churn）

| 文件 | churn |
|---|---:|
| `util/gem5-resources-manager/server.py` | 920 |
| `util/dockerfiles/docker-bake.hcl` | 824 |
| `util/gem5-resources-manager/test/api_test.py` | 735 |
| `util/minorview/model.py` | 652 |
| `util/streamline/m5stats2streamline.py` | 623 |
| `util/gem5-resources-manager/static/js/editor.js` | 589 |
| `util/o3-pipeview.py` | 557 |
| `util/offline_db/get-resources-from-db.py` | 496 |
| `util/cpt_upgraders/armv8.py` | 448 |
| `util/run-git-clang-format.py` | 419 |
| `util/gem5-resources-manager/test/comprehensive_test.py` | 418 |
| `util/dockerfiles/docker-compose.yaml` | 411 |
| `util/disk-image-validator/config_tester.py` | 389 |
| `util/github-runners-vagrant/README.md` | 374 |
| `util/gen_arm_fs_files.py` | 363 |
| `util/gem5-resources-manager/templates/editor.html` | 355 |
| `util/style/verifiers.py` | 354 |
| `util/dockerfiles/ubuntu-22.04_clang-16/llvm.sh` | 352 |
| `util/hypercall_external_signal/transmitter.py` | 348 |
| `util/gem5-resources-manager/api/create_resources_json.py` | 344 |

## `tests/` - 测试

- unique commits: 325
- unique PRs: 55
- Top topics: `tests`(205), `stdlib`(23), `misc`(12), `stdlib,tests`(11), `util-docker,tests`(4), `mem-ruby`(4)

### Top PR（按 churn）

| PR | 标题 | churn |
|---:|---|---:|
| #1070 | tests: fix persistence issue in pyunit tests | 1526 |
| #1695 | tests: Fix replacement_policies tests' refs | 996 |
| #2082 | tests: add tests for restoring from checkpoints using multisim | 466 |
| #2524 | tests: IPC Regression Tests | 371 |
| #1584 | tests: Add Pannotia GPU Tests | 350 |
| #2613 | tests: add tests for configuration related output files | 308 |
| #676 | tests: Added tests for suites | 278 |
| #1651 | mem-ruby,tests: Add CHI with ISA tests | 237 |
| #1912 | resources: Add exceptions if the resource JSON has schema issues | 234 |
| #2450 | tests: Add tests for running scripts via readfile | 201 |
| #2343 | tests: Update pyunit tests to work with v25.0 | 196 |
| #1843 | tests: Update pyunit tests references to include 24.1 | 166 |
| #1270 | gpu-compute,tests: Move GPU tests to testlib | 153 |
| #2852 | misc: Add a PyPort to write to physmem from python | 152 |
| #931 | tests,arch-riscv: update bitmanip asmtest binaries | 150 |
| #1167 | stdlib,configs,tests: Add gem5 MultiSim (MultiProcessing for gem5) | 148 |
| #1698 | tests: move weekly gpu tests to have separate jobs | 141 |
| #1605 | tests, configs, util, mem, python, systemc: Change base 10 units to base 2 | 126 |
| #901 | tests: Update tests to use specific resource versions | 108 |
| #2356 | tests: Update fs tests to use the 24.04 disk images | 85 |

### Top 文件（按 churn）

| 文件 | churn |
|---|---:|
| `tests/pyunit/stdlib/resources/pyunit_client_wrapper_checks.py` | 2516 |
| `tests/gem5/asmtest/tests.py` | 1472 |
| `tests/pyunit/stdlib/resources/pyunit_resource_specialization.py` | 1448 |
| `tests/gem5/gem5_library_example_tests/test_gem5_library_examples.py` | 1186 |
| `tests/pyunit/stdlib/resources/pyunit_workload_checks.py` | 1098 |
| `tests/pyunit/stdlib/resources/refs/resource-specialization.json` | 919 |
| `tests/pyunit/stdlib/resources/pyunit_obtain_resources_check.py` | 894 |
| `tests/pyunit/stdlib/resources/refs/resources.json` | 805 |
| `tests/pyunit/stdlib/pyunit_looppoint.py` | 796 |
| `tests/configs/gpu-ruby.py` | 777 |
| `tests/gem5/gpu/test_gpu_pannotia.py` | 725 |
| `tests/pyunit/stdlib/resources/pyunit_suite_checks.py` | 653 |
| `tests/gem5/traffic_gen/trusted_stats/GUPSGenerator-1-CHIL1-gem5.components.memory-DualChannelDDR3_1600/trusted_stats.json` | 564 |
| `tests/gem5/traffic_gen/trusted_stats/GUPSGenerator-1-CHIL1-gem5.components.memory-DualChannelDDR3_2133/trusted_stats.json` | 564 |
| `tests/gem5/traffic_gen/trusted_stats/GUPSGenerator-1-CHIL1-gem5.components.memory-DualChannelDDR4_2400/trusted_stats.json` | 564 |
| `tests/gem5/traffic_gen/trusted_stats/GUPSGenerator-1-CHIL1-gem5.components.memory-DualChannelLPDDR3_1600/trusted_stats.json` | 564 |
| `tests/gem5/traffic_gen/trusted_stats/GUPSGenerator-1-CHIL1-gem5.components.memory-HBM2Stack/trusted_stats.json` | 564 |
| `tests/gem5/traffic_gen/trusted_stats/GUPSGenerator-1-CHIL1-gem5.components.memory-SingleChannelDDR3_1600/trusted_stats.json` | 564 |
| `tests/gem5/traffic_gen/trusted_stats/GUPSGenerator-1-CHIL1-gem5.components.memory-SingleChannelDDR3_2133/trusted_stats.json` | 564 |
| `tests/gem5/traffic_gen/trusted_stats/GUPSGenerator-1-CHIL1-gem5.components.memory-SingleChannelDDR4_2400/trusted_stats.json` | 564 |

## `.github/` - CI/工作流

- unique commits: 247
- unique PRs: 72
- Top topics: `misc`(120), `misc,tests`(48), `tests`(27), `tests,misc`(26), `misc, tests`(4), `tests,gpu-compute`(3)

### Top PR（按 churn）

| PR | 标题 | churn |
|---:|---|---:|
| #458 | misc: Copy .github directory from develop to stable | 1238 |
| #831 | misc: Merge develop .github dir into stable | 238 |
| #1698 | tests: move weekly gpu tests to have separate jobs | 173 |
| #1270 | gpu-compute,tests: Move GPU tests to testlib | 169 |
| #1307 | misc: Add 'scheduler.yaml' workflow | 167 |
| #88 | misc: Update CI test workflow | 164 |
| #1361 | tests,misc: Sync .github dir develop -> stable | 109 |
| #1308 | misc: Add scheduler.yaml | 91 |
| #1043 | misc: Merge .github develop dir to stable | 87 |
| #2796 | tests: Add Mac OS .opt & .fast Compilations to CI Workflow | 83 |
| #1383 | misc,tests: Rm gem5 binary pre-build from dailys | 78 |
| #1178 | misc,tests: Download all gem5 bins via one artifact | 73 |
| #1181 | misc: Sync .github develop -> stable | 73 |
| #485 | misc: Copy .github directory from develop to stable | 67 |
| #912 | misc: Copy the develop .github dir to stable | 66 |
| #1595 | misc,tests: Add cache of ALL/gem5.opt to ci-test.yaml | 63 |
| #85 | misc: Add bug report template | 62 |
| #236 | util-docker: Add GitHub Action to create Docker Images | 55 |
| #1588 | misc: Fix docker-build.yaml | 51 |
| #987 | misc: Sync develop .github to stable | 50 |

### Top 文件（按 churn）

| 文件 | churn |
|---|---:|
| `.github/workflows/daily-tests.yaml` | 6513 |
| `.github/workflows/weekly-tests.yaml` | 2273 |
| `.github/workflows/ci-tests.yaml` | 1683 |
| `.github/workflows/docker-build.yaml` | 597 |
| `.github/workflows/gpu-tests.yaml` | 590 |
| `.github/workflows/compiler-tests.yaml` | 529 |
| `.github/workflows/scheduler.yaml` | 186 |
| `.github/ISSUE_TEMPLATE/bug_report.md` | 176 |
| `.github/workflows/utils.yaml` | 94 |
| `.github/workflows/dependabot.yml` | 17 |
| `.github/dependabot.yml` | 17 |
| `.github/{workflows => }/dependabot.yml` | 0 |

