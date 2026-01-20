# upstream PR 级别汇总（按 release tag）

- 范围：`a3be84cb1b854da51716d6399ca139016714bd54..upstream/stable`
- PR 数量（可识别）：1495；无 PR 号的 commits：2900

| release tag | 日期 | PR 数 | commits | churn(+/-) | Top subsys |
|---|---|---:|---:|---:|---|
| `v22.0.0.1` | 2022-06-18 | 0 | 0 | 0 | - |
| `v22.0.0.2` | 2022-07-27 | 0 | 9 | 54 | - |
| `v22.1.0.0` | 2022-12-30 | 0 | 462 | 143693 | - |
| `v23.0.0.0` | 2023-07-07 | 0 | 505 | 98364 | - |
| `v23.0.0.1` | 2023-07-10 | 0 | 6 | 37 | - |
| `v23.0.1.0` | 2023-08-11 | 15 | 54 | 4135 | tests, cpu, src, arch |
| `v23.1.0.0` | 2023-12-28 | 350 | 939 | 85396 | python, mem, configs, arch |
| `v24.0.0.0` | 2024-06-27 | 318 | 633 | 246847 | arch, python, tests, .github |
| `v24.0.0.1` | 2024-08-08 | 4 | 10 | 296 | .github |
| `v24.1.0.0` | 2024-12-07 | 242 | 522 | 97319 | arch, mem, cpu, tests |
| `v24.1.0.1` | 2024-12-19 | 5 | 7 | 191 | mem, docs, .gitignore, configs |
| `v24.1.0.2` | 2025-02-12 | 2 | 4 | 43 | mem |
| `v24.1.0.3` | 2025-04-11 | 2 | 4 | 9 | base |
| `v25.0.0.0` | 2025-06-18 | 274 | 655 | 113237 | arch, mem, dev, python |
| `v25.0.0.1` | 2025-08-11 | 9 | 12 | 476 | cpu/simple, arch, python, util |
| `v25.1.0.0` | 2025-12-31 | 274 | 673 | 125973 | ext, arch, dev, python |

## 每个版本 Top PR（按 churn）

### v23.0.1.0

| PR | 标题 | churn | files | subsys | arch | rep |
|---:|---|---:|---:|---|---|---|
| #95 | cpu-kvm: Make using perf when using KVM CPU optional | 500 | 5 | cpu, configs | - | `41abc6ab77b6` |
| #71 | misc: Update README/README.md | 282 | 4 | README, README.md, build_tools, src | - | `b82ae1481bd1` |
| #79 | tests: Improve Pyunit tests gem5 Resources' downloads | 76 | 4 | tests | - | `189bf66b3e48` |
| #87 | util: Add "Improving stability" sec to github-vagrant-runner | 28 | 1 | util | - | `84c4451cebac` |
| #104 | scons: Add extra parent dir to CPPPATH if --no-duplicate-sources | 16 | 1 | src | - | `919dd5efbd67` |
| #93 | python: fix fatal in main.py (github #78) | 14 | 1 | python | - | `e810f53ebee1` |
| #75 | arch-arm: Fix assert fail when UQRSHL shiftAmt==0 | 8 | 1 | arch | arm | `2242196f03c6` |
| #72 | mem-garnet: Fix packet_id val in flit | 4 | 1 | mem | - | `7665c338e268` |
| #122 | misc: Merge develop .github into stable | 0 | 0 |  | - | `48b4788bbe7a` |
| #139 | misc: Merge stable into minor-release-staging-v23-0-1-0 | 0 | 0 |  | - | `1be9501ecdca` |
| #125 | misc: Update version to v23.0.1.0 | 0 | 0 |  | - | `010baba2a700` |
| #128 | misc: Update RELEASE-NOTES.md for v23.0.1.0 | 0 | 0 |  | - | `4e8df826f6bb` |
| #103 | misc: Merge cherry-picked commits from develop | 0 | 0 |  | - | `1b67297efcc6` |
| #168 | misc: Merge develop .github into stable | 0 | 0 |  | - | `c8239cca6d5a` |
| #174 | misc: Minor release v23.0.1.0 | 0 | 0 |  | - | `6835f0665744` |

### v23.1.0.0

| PR | 标题 | churn | files | subsys | arch | rep |
|---:|---|---:|---:|---|---|---|
| #631 | misc, stdlib: Update documentation to adhere to RST formatting. | 1989 | 92 | python | - | `d96b6cdae7e9` |
| #458 | misc: Copy .github directory from develop to stable | 1238 | 8 | .github | - | `3157cde32449` |
| #519 | arch-riscv: Fix line length of CSRData declaration | 702 | 1 | arch | riscv | `e4cdd73a595b` |
| #510 | util: Added script to copy resources from mongodb | 612 | 3 | util | - | `d76a01973a82` |
| #525 | configs,ext,stdlib: Update DRAMSys integration | 467 | 12 | mem, ext, configs, python, .github | - | `e95cab429f03` |
| #90 | Add feature to output citations automatically based on configuration | 365 | 10 | mem, python, gpu-compute | - | `442923c414d5` |
| #241 | configs,stdlib,tests: Remove get_runtime_isa() | 303 | 20 | configs, python, tests | - | `569e21f798f6` |
| #73 | base: Unit tests miscellaneous patches | 273 | 6 | base, sim | - | `4c4419296b84` |
| #120 | mem-ruby,configs: Add GPU GLC Atomic Resource Constraints | 247 | 9 | mem, configs | - | `be5c03ea9f82` |
| #606 | arch-riscv: Fix narrow datatypes in RVV isa files | 230 | 4 | arch | riscv | `b0cefac9b2c7` |
| #546 | mem-ruby: Fix for not creating log entries on atomic no return requests | 194 | 12 | mem | - | `65b44e651609` |
| #80 | misc: Drop older compilers and Ubuntu 18.04 | 172 | 6 | util, .github, tests | - | `65fc9a6bfaf6` |
| #88 | misc: Update CI test workflow | 164 | 3 | .github | - | `424350f446fe` |
| #386 | dev: add debug flag in register bank. | 154 | 2 | dev | - | `83f1fe3fec2c` |
| #639 | sim: Rework the Linux Kernel exit events | 150 | 5 | src, sim, arch | arm | `d9c870f6417c` |
| #102 | stdlib,configs,tests: Remove deprecated Resource classes usage | 146 | 25 | configs, tests, python | - | `01623fac68d1` |
| #236 | util-docker: Add GitHub Action to create Docker Images | 144 | 5 | util, .github | - | `fceb7e05a338` |
| #376 | arch-riscv: Change to VS bits to DIRTY for rvv insts changing vregs | 128 | 2 | arch | riscv | `d048ad34d6d3` |
| #447 | misc: Add release notes for version 23.1 | 128 | 1 | RELEASE-NOTES.md | - | `cafc5e685dd4` |
| #397 | mem-ruby: SLICC Fixes to GLC Atomics in WB L2 | 120 | 1 | mem | - | `1204267fd8dc` |
| #76 | base: Find lsb set generalization and optimization | 117 | 2 | base | - | `6fb72d84e171` |
| #101 | mem-ruby: Added support for non-system-scope atomics in VIPER | 117 | 2 | mem | - | `1705853b12df` |
| #453 | python: Enable -m switch on gem5 binary | 96 | 2 | python | - | `20f5555f30e1` |
| #77 | base: Ostream helpers (iterable, tuple, pair, enum, pointers, optional) | 93 | 3 | base | - | `75b6fa5ad11e` |
| #655 | configs: Make riscv/fs_linux work in build/ALL/gem5.opt | 92 | 2 | configs | - | `6b80a2e81c27` |

- ... +325 PRs（详见 `prs_detailed_zh.md`）

### v24.0.0.0

| PR | 标题 | churn | files | subsys | arch | rep |
|---:|---|---:|---:|---|---|---|
| #681 | arch-riscv: Add support for RISC-V semihosting | 3323 | 21 | arch, configs, sim | arm, generic, riscv | `1bb5d3b99e74` |
| #1166 | arch-arm: Rewrite performTlbi to use map instead of switch | 1607 | 4 | arch | arm | `10dbfb8bb771` |
| #1070 | tests: fix persistence issue in pyunit tests | 1526 | 5 | tests | - | `d75afeabb152` |
| #779 | stdlib: Enable bundled resource requests from the databases | 1159 | 8 | python | - | `97a05304527f` |
| #1167 | stdlib,configs,tests: Add gem5 MultiSim (MultiProcessing for gem5) | 1001 | 28 | tests, python, configs | - | `1a00ecfaf926` |
| #1272 | Adding an example for Spatter | 726 | 4 | configs, python | - | `21bd1c28abf4` |
| #851 | arch-riscv: adding vector unit-stride segment loads to RISC-V | 711 | 9 | arch, cpu, cpu/minor | riscv | `f6c61836b3cb` |
| #913 | arch-riscv: adding vector unit-stride segment stores to RISC-V | 597 | 8 | arch, cpu | riscv | `1e743fd85ab5` |
| #813 | arch-riscv: adding support for local interrupts | 478 | 6 | arch | riscv | `f289f9e8b5f5` |
| #762 | cpu,stdlib: Updating strided generator | 348 | 8 | cpu, python | - | `b79fe82e5c4d` |
| #1127 | Revert "cpu-kvm: Support perf counters on hybrid host architectures" | 341 | 5 | cpu | - | `0824d7f2cd9a` |
| #914 | arch-riscv: Move alignment check to Physical Memory Attribute(PMA) | 298 | 13 | arch | riscv | `dbae09e4d9fb` |
| #676 | tests: Added tests for suites | 278 | 3 | tests | - | `d1fca18eb37d` |
| #1175 | arch-arm: Implement HCR_EL2 force broadcast for EL1&0 TLBIs | 249 | 5 | arch | arm | `c4ed23a10b51` |
| #831 | misc: Merge develop .github dir into stable | 238 | 5 | .github | - | `5b2766829b8c` |
| #886 | arch-riscv,dev: Update the PLIC implementation | 221 | 7 | dev, python | - | `bcf455755e7a` |
| #1170 | util-docker,gpu,gpu-compute: Improve GCN-GPU Dockerfile | 214 | 1 | util | - | `ce0bb4655c9e` |
| #794 | arch-riscv: add unit-stride fault-only-first loads (i.e. vle*ff) | 212 | 7 | arch | riscv | `804f1373252f` |
| #976 | dev: Remove duplicate virtio files | 191 | 2 | dev | - | `63706f04b59e` |
| #902 | dev: RegisterBank addRegistersAt for fragmented reg banks | 175 | 2 | dev | - | `c0e5d58a96d9` |
| #911 | misc: Add a DevContainer specification to the gem5 repo | 165 | 4 | .devcontainer, util | - | `392a2b4ffa33` |
| #725 | arm,stdlib: added kvm support to the ARM board | 163 | 3 | configs, python, util | - | `b5d18b84a823` |
| #887 | sim-se: Implement statx system call for Linux x86-64 | 153 | 3 | arch, sim | x86 | `00d4b6825c69` |
| #931 | tests,arch-riscv: update bitmanip asmtest binaries | 150 | 1 | tests | - | `6b4dbdcedbfc` |
| #1123 | arch-riscv: Add RVV FP16 support (Zvfh & Zvfhmin) | 145 | 5 | arch, ext | riscv | `d48191d6088c` |

- ... +293 PRs（详见 `prs_detailed_zh.md`）

### v24.0.0.1

| PR | 标题 | churn | files | subsys | arch | rep |
|---:|---|---:|---:|---|---|---|
| #1361 | tests,misc: Sync .github dir develop -> stable | 109 | 3 | .github | - | `b88f814e633f` |
| #1308 | misc: Add scheduler.yaml | 91 | 1 | .github | - | `bb418d41eb6d` |
| #1306 | misc, tests: Fix missing 's' in GPU tests | 4 | 1 | .github | - | `a7645cdf20ef` |
| #1425 | misc: v24.0.0.1 Hotfix release | 0 | 0 |  | - | `b1a44b89c7ba` |

### v24.1.0.0

| PR | 标题 | churn | files | subsys | arch | rep |
|---:|---|---:|---:|---|---|---|
| #1303 | arch-arm: Implement FEAT_XS | 2567 | 14 | arch | arm | `b28659d4f9f7` |
| #1453 | mem-ruby: Remove static methods from RubySystem | 1465 | 123 | mem, python, configs, cpu | - | `4f7b3ed82741` |
| #1534 | misc: Do not share the random number generator across components | 1272 | 88 | cpu, cpu/pred, dev, mem, mem/cache/rp, arch | arm, riscv | `b82ab5ac8956` |
| #1840 | misc: v24.1 release notes update | 1081 | 1 | RELEASE-NOTES.md | - | `8f37677c9bae` |
| #1695 | tests: Fix replacement_policies tests' refs | 996 | 42 | tests | - | `2c679bfa04c0` |
| #1538 | arch-riscv: add VLEN/ELEN as class attributes for all vec insts | 949 | 11 | arch | riscv | `d1ce4fb6c7cb` |
| #1619 | configs: Deprecate Vega10 | 651 | 6 | configs | - | `c8c75959addc` |
| #1537 | Implement BTB using the cache library | 527 | 8 | cpu/pred, configs | - | `50f652a2ee19` |
| #1525 | arch-riscv: Add support for riscv hardware probing syscall | 482 | 2 | arch | riscv | `652a72d122ff` |
| #1350 | arch-vega: Pass s_memtime through smem pipe | 453 | 25 | mem, arch, gpu-compute, configs | amdgpu | `a8447b7fc01d` |
| #1270 | gpu-compute,tests: Move GPU tests to testlib | 352 | 8 | .github, tests, configs, ext | - | `f600db4a98d4` |
| #1584 | tests: Add Pannotia GPU Tests | 350 | 2 | tests | - | `e987c60a4c14` |
| #1399 | mem-ruby: Prevent LL/SC livelock in MESI protocols (#1384) | 347 | 3 | mem | - | `7bddc764cc23` |
| #1490 | stdlib, configs: Add RiscvDemoBoard | 327 | 4 | python, configs | - | `f01d68bf9676` |
| #1698 | tests: move weekly gpu tests to have separate jobs | 314 | 2 | .github, tests | - | `c91af552d469` |
| #1692 | dev-amdgpu, gpu-compute, mem-ruby: Add support for writeback L2 in GPU | 309 | 11 | mem, gpu-compute, dev | - | `d463868f28f1` |
| #1605 | tests, configs, util, mem, python, systemc: Change base 10 units to base 2 | 264 | 45 | configs, tests, util, mem, python, src | - | `c10feed524a8` |
| #1651 | mem-ruby,tests: Add CHI with ISA tests | 237 | 4 | tests | - | `709f2c769534` |
| #1478 | arch-arm: Add arm demo board | 205 | 3 | python, configs | - | `946bf83b7520` |
| #1580 | misc: Make random gen portable across compilers. | 204 | 3 | base | - | `36264938dbe6` |
| #1403 | mem: Fixed implementation of Best Offset Prefetcher | 170 | 3 | mem/cache/prefetch | - | `f6010439fe54` |
| #1307 | misc: Add 'scheduler.yaml' workflow | 167 | 4 | .github | - | `3142464ff7bc` |
| #1843 | tests: Update pyunit tests references to include 24.1 | 166 | 6 | tests | - | `63d25922a2db` |
| #1445 | arch-vega: Swizzle multi-dword scratch requests | 165 | 4 | arch, gpu-compute | amdgpu | `7d46c5066356` |
| #1388 | arch-arm: Add support for AArch32 PMEVCNTR*/PMEVTYPER*/PMCCFILTR | 138 | 4 | arch | arm | `b23a4c7806b6` |

- ... +217 PRs（详见 `prs_detailed_zh.md`）

### v24.1.0.1

| PR | 标题 | churn | files | subsys | arch | rep |
|---:|---|---:|---:|---|---|---|
| #335 | misc: Add sphinx stdlib documentation | 230 | 6 | docs, .gitignore | - | `e146f1b2bcfe` |
| #1865 | mem-ruby: Add missing option in ProtocolInfo | 52 | 5 | mem | - | `0fe31664f3ed` |
| #1864 | mem-ruby: Fix missing RubySystem in PerfectCacheMemory's entries | 46 | 2 | mem | - | `b6c941c9cabd` |
| #1851 | configs: Generalize class types in CHI RNF/MN generators | 8 | 1 | configs | - | `b5e27f5ed873` |
| #1875 | v24.1.0.1 Hotfix Release | 0 | 0 |  | - | `c9625ce9cc5b` |

### v24.1.0.2

| PR | 标题 | churn | files | subsys | arch | rep |
|---:|---|---:|---:|---|---|---|
| #1930 | mem-ruby: set RubySystem pointer during TBE alloc | 64 | 2 | mem | - | `dc448c953074` |
| #1964 | misc: Hotfix v24.1.0.2 | 0 | 0 |  | - | `186a913a48f1` |

### v24.1.0.3

| PR | 标题 | churn | files | subsys | arch | rep |
|---:|---|---:|---:|---|---|---|
| #1793 | base: Fix failing compiler tests | 2 | 1 | base | - | `837a9a5c54ee` |
| #2177 | misc: Hotfix v24.1.0.3 | 0 | 0 |  | - | `b9da2bfe1e21` |

### v25.0.0.0

| PR | 标题 | churn | files | subsys | arch | rep |
|---:|---|---:|---:|---|---|---|
| #2022 | arch-riscv: Add support for vector stride segment load/store instructions | 2260 | 8 | arch, cpu, cpu/o3 | riscv | `6f0574b91c1f` |
| #2002 | arch-arm: Split the decodeFp function in subfunctions | 1175 | 1 | arch | arm | `32ca62ea4c2e` |
| #2341 | arch-riscv: Remove N extension | 1104 | 11 | arch | riscv | `6856aee02cf9` |
| #1949 | dev: rework PCI to add type1 header | 1023 | 26 | dev, configs, python, tests, util | - | `13eb3bd72083` |
| #1912 | resources: Add exceptions if the resource JSON has schema issues | 631 | 6 | python, tests | - | `dc55377210bc` |
| #117 | ruby: Enable all protocols in a single gem5 build | 586 | 6 | mem | - | `8c05375061ad` |
| #1825 | sim-se: Implement free-list-based physical page allocator for SE mode | 512 | 11 | sim, base | - | `81256852e498` |
| #2082 | tests: add tests for restoring from checkpoints using multisim | 466 | 3 | tests | - | `4b61b826a678` |
| #1761 | arch-riscv: Implement Zcmt | 454 | 17 | arch, configs, util | riscv | `6d3434a8b352` |
| #2234 | gpu: Don't use VLA in device (de)serialization | 436 | 4 | dev, sim | - | `8a869c8407ce` |
| #1935 | util: Add Python implementation of terminal client (gem5term) | 412 | 2 | util | - | `53b3727e2baf` |
| #2284 | dev: Update MI300X model to use real firmware | 398 | 15 | dev, configs, python | - | `ffbfe65b2512` |
| #2119 | cpu-o3: Replace C++03 boilerplate with range-based for loops | 383 | 12 | cpu/o3, cpu | - | `ef486ff89383` |
| #2023 | arch-riscv: Add support for fault-only-first unit-stride segment load instructions | 383 | 6 | arch, cpu, cpu/o3 | riscv | `a2de450019c7` |
| #1767 | arch-riscv: Add support for Zfa extension | 367 | 3 | arch | riscv | `f5eb43dae8e4` |
| #1982 | sim,stdlib: Fixes for external signal | 359 | 4 | sim, python, util | - | `599dc98e4695` |
| #1659 | cpu-o3: Use the generic cache library to build store sets | 240 | 8 | cpu/o3, configs, mem/cache | - | `350470de7566` |
| #2336 | misc: v25.0.0.0 release notes | 228 | 1 | RELEASE-NOTES.md | - | `0a90390078a2` |
| #1961 | stdlib: Add SE mode support to multi-program workloads | 212 | 1 | python | - | `2824a0a36e78` |
| #1926 | cpu-o3: add retry resp to LSQ with throttling params | 209 | 3 | cpu/o3 | - | `b0a782ceceba` |
| #2343 | tests: Update pyunit tests to work with v25.0 | 196 | 6 | tests | - | `ac32d2abab75` |
| #1709 | arch-riscv: Fix misprediction of control flow instruction caused by vset{i}vl{i} | 177 | 9 | arch, configs, util | riscv | `afd31c741664` |
| #2223 | arch-riscv: Fix CMO decoding | 152 | 1 | arch | riscv | `85ef2cf6ca5d` |
| #2021 | arch-riscv: Fix incorrect vector unit-stride segment load instructions | 146 | 3 | arch, cpu/o3 | riscv | `9222fe71493a` |
| #2242 | gpu: Remove SDMA header heap allocations | 140 | 3 | dev | - | `12fbd8ebc000` |

- ... +249 PRs（详见 `prs_detailed_zh.md`）

### v25.0.0.1

| PR | 标题 | churn | files | subsys | arch | rep |
|---:|---|---:|---:|---|---|---|
| #2441 | arch-arm: Add FEAT_FP16 FP instructions | 694 | 2 | arch | arm | `0dd766800279` |
| #2415 | stdlib: remove duplicate ClassicGeneratorExitHandler | 126 | 1 | python | - | `4e2718f50d93` |
| #2399 | cpu: Fix looppoint analysis v25 | 18 | 2 | cpu/simple | - | `35de1ef5d3dc` |
| #2502 | cpu: Adapt simpoint listener to new probe structure | 16 | 2 | cpu/simple | - | `e82a08d080db` |
| #2512 | cpu-o3: properly index time buffer when clearing states | 14 | 1 | cpu/o3 | - | `d03208b6aa21` |
| #2464 | arch-riscv: populate logBytes/paddr after functional pt walk | 12 | 1 | arch | riscv | `415cbec52985` |
| #2397 | util: bump urllib3 to 2.5.0 in util/gem5-resources-manager | 4 | 1 | util | - | `a5ffb90272b2` |
| #2492 | arch-arm: fix writeback type for AArch32 FP16 instructions | 4 | 1 | arch | arm | `3a6f3c16b696` |
| #2496 | misc: Hotfix 25.0.0.1 | 0 | 0 |  | - | `ddd4ae35adb0` |

### v25.1.0.0

| PR | 标题 | churn | files | subsys | arch | rep |
|---:|---|---:|---:|---|---|---|
| #2551 | ext: Update Pybind11 to v3.0.0 | 34760 | 307 | ext | - | `740177429a8a` |
| #2298 | dev: reworks PCI to add a PCI host bridge | 1845 | 50 | dev, python, configs | - | `2d2883c95f3f` |
| #2409 | scons: Update build_tools to enable importing | 1757 | 8 | build_tools | - | `da3597cf5dbc` |
| #2079 | util: Add validator and tests for full system workloads (disk and kernels) | 1248 | 7 | util | - | `d516a6397f57` |
| #1712 | arch-riscv: Fix incorrect vector slide instructions and statically filter redundant uops | 1078 | 5 | arch | riscv | `635ac5d0da02` |
| #2741 | arch-arm,sim-se: Implement sigreturn for Arm64 | 805 | 5 | arch, sim | arm | `9d52f319d070` |
| #2465 | arch-arm: Split decodeBranchExcSys into multiple sub-functions | 774 | 1 | arch | arm | `355e3a88cece` |
| #2652 | configs, cpu-o3: Implement a distributed InstructionQueue | 757 | 14 | cpu/o3, configs, arch | x86 | `2d4d1bf2bbd3` |
| #2724 | cpu: Add Arm Neoverse V2 config | 678 | 3 | configs, tests | - | `51759b538e1d` |
| #2675 | mem-ruby: Add support for CLFLUSH type instructions in MESI Three Level protocol | 536 | 11 | mem, configs | - | `8b263b8d9c01` |
| #2623 | arch-arm: Decouple insts from generated decoder | 492 | 3 | arch | arm | `189cbae7f971` |
| #2477 | cpu: move conditional pred out of bpred_unit | 488 | 22 | cpu/pred, configs, cpu/minor, cpu/o3, python | - | `23f4be207519` |
| #362 | misc: Add git-clang-format to pre-commit with wrapper script | 468 | 3 | .clang-format, .pre-commit-config.yaml, util | - | `6e287f3da539` |
| #1786 | tests: Add a unit test for bloom filters | 396 | 2 | base | - | `a56499a75d1e` |
| #2570 | dev: Fix PCI host bridge with no range from up | 371 | 14 | dev, python, mem | - | `723b231dcb01` |
| #2524 | tests: IPC Regression Tests | 371 | 4 | tests | - | `105485a61858` |
| #2518 | cpu-o3: Bundle some Fetch/IEW/Commit stats into a vector | 367 | 6 | cpu/o3 | - | `33ce67ae93ee` |
| #1977 | mem-cache: Unit test FIFO RP | 340 | 2 | mem/cache/rp | - | `13e943ee492a` |
| #2802 | gpu-compute: Add missing MFMA timings | 315 | 1 | gpu-compute | - | `fb83f56f2f22` |
| #2350 | arch-vega: Included modifiers support in vop3_cmp instructions | 308 | 1 | arch | amdgpu | `00aae5865693` |
| #2613 | tests: add tests for configuration related output files | 308 | 3 | tests | - | `66d23ca867fe` |
| #2303 | cpu:Add gshare branch predictor model | 267 | 4 | cpu/pred | - | `22e4e8331d62` |
| #2852 | misc: Add a PyPort to write to physmem from python | 259 | 7 | python, sim, tests | - | `186a6ed39809` |
| #2740 | mem-ruby: Update Ruby Network to use new-style stats | 225 | 6 | mem | - | `53123ba58a5f` |
| #2450 | tests: Add tests for running scripts via readfile | 201 | 4 | tests | - | `618fbfbcff71` |

- ... +249 PRs（详见 `prs_detailed_zh.md`）

