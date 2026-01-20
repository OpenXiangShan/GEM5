# upstream release 版本摘要（从分叉点开始）

- 分叉基线：`a3be84cb1b854da51716d6399ca139016714bd54`（tag: `v22.0.0.1`）
- upstream：`upstream/stable`（tag: `v25.1.0.0`）
- 说明：此文件结合 git tag 时间线与 `RELEASE-NOTES.md` 的内容做中文化梳理。

## tag 时间线（含别名）

- 说明：部分 tag 指向同一个 commit（例如 `v25.1` 与 `v25.1.0.0`），属于别名/重复标记。

| tag | 日期 | commit | subject |
|---|---|---|---|
| `v22.0.0.1` | 2022-06-18 | `39f85b7a3be1` | misc: Update version info to v22.0.0.1 |
| `v22.0.0.2` | 2022-07-27 | `1d03f6de9415` | misc: Update RELEASE-NOTES.md for v22.0.0.2 |
| `v22.1` | 2022-12-30 | `5fa484e2e026` | misc: Merge the v22.1 release staging into stable |
| `v22.1.0.0` | 2022-12-30 | `5fa484e2e026` | misc: Merge the v22.1 release staging into stable |
| `v23.0.0.0` | 2023-07-07 | `1db206b9d371` | misc: Merge branch 'release-staging-v23-0' into stable |
| `v23.0.0.1` | 2023-07-10 | `af72b9ba5805` | misc: Update RELEASE-NOTES.md for v23.0.0.1 hotfix |
| `v23.0` | 2023-08-11 | `6835f0665744` | misc: Minor release v23.0.1.0 (#174) |
| `v23.0.1.0` | 2023-08-11 | `6835f0665744` | misc: Minor release v23.0.1.0 (#174) |
| `v23.1` | 2023-12-28 | `bae34876780d` | misc: Merge `release-staging-v23-1` into `stable` (#711) |
| `v23.1.0.0` | 2023-12-28 | `bae34876780d` | misc: Merge `release-staging-v23-1` into `stable` (#711) |
| `v24.0.0.0` | 2024-06-27 | `43769abaf051` | misc: Merge v24.0 release staging branch to stable (#1274) |
| `v24.0` | 2024-08-08 | `b1a44b89c7ba` | misc: v24.0.0.1 Hotfix release (#1425) |
| `v24.0.0.1` | 2024-08-08 | `b1a44b89c7ba` | misc: v24.0.0.1 Hotfix release (#1425) |
| `v24.1.0.0` | 2024-12-07 | `63d25922a2db` | tests: Update pyunit tests references to include 24.1 (#1843) |
| `v24.1.0.1` | 2024-12-19 | `c9625ce9cc5b` | v24.1.0.1 Hotfix Release  (#1875) |
| `v24.1.0.2` | 2025-02-12 | `186a913a48f1` | misc: Hotfix v24.1.0.2 (#1964) |
| `v24.1` | 2025-04-11 | `b9da2bfe1e21` | misc: Hotfix v24.1.0.3 (#2177) |
| `v24.1.0.3` | 2025-04-11 | `b9da2bfe1e21` | misc: Hotfix v24.1.0.3 (#2177) |
| `v25.0.0.0` | 2025-06-18 | `d22064c1c05f` | misc: Release v25.0.0.0 (#2322) |
| `v25.0` | 2025-08-11 | `ddd4ae35adb0` | misc: Hotfix 25.0.0.1 (#2496) |
| `v25.0.0.1` | 2025-08-11 | `ddd4ae35adb0` | misc: Hotfix 25.0.0.1 (#2496) |
| `v25.1` | 2025-12-31 | `7a2b0e413d06` | misc: Release v25.1.0.0 (#2803) |
| `v25.1.0.0` | 2025-12-31 | `7a2b0e413d06` | misc: Release v25.1.0.0 (#2803) |

## 按版本区间统计（去重后）

| tag | 日期 | commit | 该版本区间 commits | 该版本区间 PR 数 | release-notes key |
|---|---|---|---:|---:|---|
| `v22.0.0.1` | 2022-06-18 | `39f85b7a3be1` | 0 | 0 | `22.0.0.1` |
| `v22.0.0.2` | 2022-07-27 | `1d03f6de9415` | 9 | 0 | `22.0.0.2` |
| `v22.1.0.0` | 2022-12-30 | `5fa484e2e026` | 462 | 0 | `22.1.0.0` |
| `v23.0.0.0` | 2023-07-07 | `1db206b9d371` | 505 | 0 | `23.0` |
| `v23.0.0.1` | 2023-07-10 | `af72b9ba5805` | 6 | 0 | `23.0.0.1` |
| `v23.0.1.0` | 2023-08-11 | `6835f0665744` | 54 | 15 | `23.0.1.0` |
| `v23.1.0.0` | 2023-12-28 | `bae34876780d` | 939 | 350 | `23.1` |
| `v24.0.0.0` | 2024-06-27 | `43769abaf051` | 633 | 318 | `24.0` |
| `v24.0.0.1` | 2024-08-08 | `b1a44b89c7ba` | 10 | 4 | `24.0.0.1` |
| `v24.1.0.0` | 2024-12-07 | `63d25922a2db` | 522 | 242 | `24.1` |
| `v24.1.0.1` | 2024-12-19 | `c9625ce9cc5b` | 7 | 5 | `24.1.0.1` |
| `v24.1.0.2` | 2025-02-12 | `186a913a48f1` | 4 | 2 | `24.1.0.2` |
| `v24.1.0.3` | 2025-04-11 | `b9da2bfe1e21` | 4 | 2 | `24.1.0.3` |
| `v25.0.0.0` | 2025-06-18 | `d22064c1c05f` | 655 | 274 | `25.0` |
| `v25.0.0.1` | 2025-08-11 | `ddd4ae35adb0` | 12 | 9 | `25.0.0.1` |
| `v25.1.0.0` | 2025-12-31 | `7a2b0e413d06` | 673 | 274 | `25.1` |

## 各版本要点（中文摘要）

### v22.0.0.1 (2022-06-18)

- commits: 0
- PR 数（可识别）：0
- 变更规模（churn）：0

### v22.0.0.2 (2022-07-27)

- commits: 9
- PR 数（可识别）：0
- 变更规模（churn）：54
- 新特性/重要变化（中文归纳，best-effort）：
  - 要点：修复 the ARM booting of Linux kernels making use of FEAT_PAuth.
  - 要点：移除 incorrect `requires` functions in AbstractProcessor 与 AbstractGeneratorCore.
  - 要点：修复 the standard library's `set_se_binary_workload` function to exit on Exit Events (work items) by default.
  - 要点：Connects a previously unconnected PCI port in the example SST RISC-V config to the membus.
  - 要点：Updates the SST-gem5 README with the correct download links.
  - 要点：新增 a `getAddrRanges` function to the `HBMCtrl`.
- Release notes 摘要（自动提取）：
  - 要点：修复 the ARM booting of Linux kernels making use of FEAT_PAuth.
  - 要点：移除 incorrect `requires` functions in AbstractProcessor 与 AbstractGeneratorCore.
  - 要点：修复 the standard library's `set_se_binary_workload` function to exit on Exit Events (work items) by default.
  - 要点：Connects a previously unconnected PCI port in the example SST RISC-V config to the membus.
  - 要点：Updates the SST-gem5 README with the correct download links.
  - 要点：新增 a `getAddrRanges` function to the `HBMCtrl`.
  - 要点：修复 test_download_resources.py so the correct parameter is passed to the download test script.

### v22.1.0.0 (2022-12-30)

- commits: 462
- PR 数（可识别）：0
- 变更规模（churn）：143693
- 新特性/重要变化（中文归纳，best-effort）：
  - 要点：The gem5 binary can now be compiled to include 多 ISA targets.
  - 要点：The `m5` Python module now includes functions to set exit events are particular simululation ticks:
  - 要点：We now include the `RiscvMatched` board as part of the gem5 stdlib.
  - 要点：An API for [SimPoints](https://doi.org/10.1145/885651.781076) has been 新增.
  - 要点："工作负载" have been introduced to gem5.
  - 要点：To aid gem5 developers, we have incorporated [pre-commit](https://pre-commit.com) checks into gem5.
- Release notes 摘要（自动提取）：
  - 要点：The gem5 binary can now be compiled to include 多 ISA targets.
  - 要点：The `m5` Python module now includes functions to set exit events are particular simululation ticks:
  - 要点：We now include the `RiscvMatched` board as part of the gem5 stdlib.
  - 要点：An API for [SimPoints](https://doi.org/10.1145/885651.781076) has been 新增.
  - 要点："工作负载" have been introduced to gem5.
  - 要点：To aid gem5 developers, we have incorporated [pre-commit](https://pre-commit.com) checks into gem5.
  - 要点：A multiprocessing module has been 新增.
  - 要点：The stdlib's `ArmBoard` now 支持 Ruby caches.
  - 要点：Due to numerious 修复与 改进, Ubuntu 22.04 can be booted as a gem5 工作负载, both in FS 与 SE mode.
  - 要点：Substantial 改进 have been made to gem5's GDB capabilities.

### v23.0.0.0 (2023-07-07)

- commits: 505
- PR 数（可识别）：0
- 变更规模（churn）：98364
- 新特性/重要变化（中文归纳，best-effort）：
  - 主题：Major renaming of CPU stats
  - 主题：`fs.py` 与 `se.py` deprecated
  - 主题：Renaming of `DEBUG` guard into `GEM5_DEBUG`
  - 主题：Other API changes
  - 要点：移除 deprecated namespaces. Namespace names were updated a couple of releases ago. This release 移除 the old names.
  - 要点：Uses `MemberEventWrapper` in favor of `EventWrapper` for instance member functions.
- Release notes 摘要（自动提取）：
  - 主题：Major renaming of CPU stats
  - 主题：`fs.py` 与 `se.py` deprecated
  - 主题：Renaming of `DEBUG` guard into `GEM5_DEBUG`
  - 主题：Other API changes
  - 要点：移除 deprecated namespaces. Namespace names were updated a couple of releases ago. This release 移除 the old names.
  - 要点：Uses `MemberEventWrapper` in favor of `EventWrapper` for instance member functions.
  - 要点：新增 an extension mechanism to `Packet` 与 `Request`.
  - 要点：Sets x86 CPU vendor string to "HygoneGenuine" to better 支持 GLIBC.
  - 主题：Large 改进 to gem5 resources 与 gem5 resources website
  - 主题：Arm ISA 改进

### v23.0.0.1 (2023-07-10)

- commits: 6
- PR 数（可识别）：0
- 变更规模（churn）：37
- 新特性/重要变化（中文归纳，best-effort）：
  - 要点：移除 the use of 'std::random_shuffle'.
  - 要点：新增 missing 'overrides' in "src/arch/amdgpu/vega/insts/instructions.hh".
  - 要点：修复 Linux specific includes, allowing for compilation on non-linux systems.
  - 要点：新增 a missing include in "src/gpu-compute/dispatcher.cc".
- Release notes 摘要（自动提取）：
  - 要点：移除 the use of 'std::random_shuffle'.
  - 要点：新增 missing 'overrides' in "src/arch/amdgpu/vega/insts/instructions.hh".
  - 要点：修复 Linux specific includes, allowing for compilation on non-linux systems.
  - 要点：新增 a missing include in "src/gpu-compute/dispatcher.cc".

### v23.0.1.0 (2023-08-11)

- commits: 54
- PR 数（可识别）：15
- 变更规模（churn）：4135
- 新特性/重要变化（中文归纳，best-effort）：
  - 要点："TESTING.md" has been updated to more accurately reflect our current testing 基础设施.
  - 要点："README" has been replaced with "README.md" 与 includes more up-to-date information on using gem5.
  - 要点："CONTRIBUTING.md" has been updated to reflect our migration to GitHub 与 the changes in policy 与 proceedures.
  - 要点：Where needed old references to Gerrit have been 移除 in favor of GitHub.
  - 要点：修复 an 断言失败 when using ARM which was trigged when `shiftAmt` is 0 for a UQRSH instruction.
  - 要点：修复 `name 'fatal' is not defined` being thrown when tracing is off.
  - 【其他】cpu-kvm：使在使用 KVM CPU 时使用 perf 变为可选。
  - 【文档/示例】misc：更新README/README.md。
- Release notes 摘要（自动提取）：
  - 要点："TESTING.md" has been updated to more accurately reflect our current testing 基础设施.
  - 要点："README" has been replaced with "README.md" 与 includes more up-to-date information on using gem5.
  - 要点："CONTRIBUTING.md" has been updated to reflect our migration to GitHub 与 the changes in policy 与 proceedures.
  - 要点：Where needed old references to Gerrit have been 移除 in favor of GitHub.
  - 要点：修复 an 断言失败 when using ARM which was trigged when `shiftAmt` is 0 for a UQRSH instruction.
  - 要点：修复 `name 'fatal' is not defined` being thrown when tracing is off.
  - 要点：修复 a bug in ARM in which the TLBIOS instructions were decoded as normal MSR instructions with no effect on the TLBs.
  - 要点：修复 invalid `packet_id` value in flit.
  - 要点：修复 default CustomMesh for use with Garnet.
  - 要点：The gem5 resources downloader now outputs more helpful errors in the case of a failure.
- Top PR（按 churn，Top 8）：
  - #95 cpu-kvm: Make using perf when using KVM CPU optional (churn=500)
  - #71 misc: Update README/README.md (churn=282)
  - #79 tests: Improve Pyunit tests gem5 Resources' downloads (churn=76)
  - #87 util: Add "Improving stability" sec to github-vagrant-runner (churn=28)
  - #104 scons: Add extra parent dir to CPPPATH if --no-duplicate-sources (churn=16)
  - #93 python: fix fatal in main.py (github #78) (churn=14)
  - #75 arch-arm: Fix assert fail when UQRSHL shiftAmt==0 (churn=8)
  - #72 mem-garnet: Fix packet_id val in flit (churn=4)

### v23.1.0.0 (2023-12-28)

- commits: 939
- PR 数（可识别）：350
- 变更规模（churn）：85396
- 新特性/重要变化（中文归纳，best-effort）：
  - 主题：The gem5 build can is now configured with `kconfig`
  - 要点：Most gem5 builds without customized options (excluding double dash options) (e.g. , build/X86/gem5.opt) are backwards compatible 与 require no changes to your current workflows.
  - 要点：All of the default builds in `build_opts` are unchanged 与 still available.
  - 要点：However, if you want to specialize your build. For example, use customized ruby protocol. The command `scons PROTOCOL=<PROTOCAL_NAME> build/ALL/gem5.opt` will not work anymore. you now have to use `scons <kconfig command>` to update the ruby protocol as example. The double dash options (`--without-tcmalloc`, `--with-asan` 与 so on) are still continue to work as normal.
  - 要点：For more details refer to the 文档 here: [kconfig 文档](https://www.gem5.org/文档/general_docs/kconfig_build_system/)
  - 主题：Standard library 改进
  - 【更新/依赖】misc, stdlib：更新文档 to adhere to RST formatting.。
  - 【CI】misc：Copy .github directory from develop to stable。
- Release notes 摘要（自动提取）：
  - 主题：The gem5 build can is now configured with `kconfig`
  - 要点：Most gem5 builds without customized options (excluding double dash options) (e.g. , build/X86/gem5.opt) are backwards compatible 与 require no changes to your current workflows.
  - 要点：All of the default builds in `build_opts` are unchanged 与 still available.
  - 要点：However, if you want to specialize your build. For example, use customized ruby protocol. The command `scons PROTOCOL=<PROTOCAL_NAME> build/ALL/gem5.opt` will not work anymore. you now have to use `scons <kconfig command>` to update the ruby protocol as example. The double dash options (`--without-tcmalloc`, `--with-asan` 与 so on) are still continue to work as normal.
  - 要点：For more details refer to the 文档 here: [kconfig 文档](https://www.gem5.org/文档/general_docs/kconfig_build_system/)
  - 主题：Standard library 改进
  - 要点：The `工作负载` 与 `CustomWorkload` classes are now deprecated. They have been transformed into wrappers for the `obtain_resource` 与 `WorkloadResource` classes in `resource.py`, respectively.
  - 要点：Code utilizing the older API will continue to function as expected but will trigger a warning message. To update code using the `工作负载` class, change the call from `工作负载(id='resource_id', resource_version='1.0.0')` to `obtain_resource(id='resource_id', resource_version='1.0.0')`. Similarly, to update code using the `CustomWorkload` class, change the call from `CustomWorkload(function=func, parameters=params)` to `WorkloadResource(function=func, parameters=params)`.
  - 要点：工作负载 resources in gem5 can now be directly acquired using the `obtain_resource` function, just like other resources.
  - 要点：All resource object now have their own `id` 与 `category`. Each resource class has its own `__str__()` function which return its information in the form of **category(id, version)** like **BinaryResource(id='riscv-hello', resource_version='1.0.0')**.
- Top PR（按 churn，Top 8）：
  - #631 misc, stdlib: Update documentation to adhere to RST formatting. (churn=1989)
  - #458 misc: Copy .github directory from develop to stable (churn=1238)
  - #519 arch-riscv: Fix line length of CSRData declaration (churn=702)
  - #510 util: Added script to copy resources from mongodb (churn=612)
  - #525 configs,ext,stdlib: Update DRAMSys integration (churn=467)
  - #90 Add feature to output citations automatically based on configuration (churn=365)
  - #241 configs,stdlib,tests: Remove get_runtime_isa() (churn=303)
  - #73 base: Unit tests miscellaneous patches (churn=273)

### v24.0.0.0 (2024-06-27)

- commits: 633
- PR 数（可识别）：318
- 变更规模（churn）：246847
- 新特性/重要变化（中文归纳，best-effort）：
  - 要点：The GCN3 GPU model has been 移除 in favor of the newer VEGA_X86 GPU model.
  - 要点：gem5 now 支持 building, running, 与 simulating Ubuntu 24.04.
  - 主题：Compiler 与 OS 支持
  - 主题：gem5 MultiSim: Multiprocessing for gem5
  - 主题：RISC-V Vector Extension 支持
  - 要点：修复 viota (#1137)
  - 【新增/支持】arch-riscv：增加对RISC-V semihosting的支持。
  - 【重构/整理】arch-arm：重构performTlbi to use map instead of switch。
- Release notes 摘要（自动提取）：
  - 要点：The GCN3 GPU model has been 移除 in favor of the newer VEGA_X86 GPU model.
  - 要点：gem5 now 支持 building, running, 与 simulating Ubuntu 24.04.
  - 主题：Compiler 与 OS 支持
  - 主题：gem5 MultiSim: Multiprocessing for gem5
  - 主题：RISC-V Vector Extension 支持
  - 要点：修复 viota (#1137)
  - 要点：修复 vrgather (#1134)
  - 要点：新增 RVV FP16 支持 (#1123)
  - 要点：修复 widening 与 narrowing instructions (#1079)
  - 要点：修复 bug in vfmv.f.s (#863)
- Top PR（按 churn，Top 8）：
  - #681 arch-riscv: Add support for RISC-V semihosting (churn=3323)
  - #1166 arch-arm: Rewrite performTlbi to use map instead of switch (churn=1607)
  - #1070 tests: fix persistence issue in pyunit tests (churn=1526)
  - #779 stdlib: Enable bundled resource requests from the databases (churn=1159)
  - #1167 stdlib,configs,tests: Add gem5 MultiSim (MultiProcessing for gem5) (churn=1001)
  - #1272 Adding an example for Spatter (churn=726)
  - #851 arch-riscv: adding vector unit-stride segment loads to RISC-V (churn=711)
  - #913 arch-riscv: adding vector unit-stride segment stores to RISC-V (churn=597)

### v24.0.0.1 (2024-08-08)

- commits: 10
- PR 数（可识别）：4
- 变更规模（churn）：296
- 新特性/重要变化（中文归纳，best-effort）：
  - 【测试】tests,misc：Sync .github dir develop -> stable。
  - 【新增/支持】misc：新增scheduler.yaml。
  - 【测试】misc, tests：修复GPU tests 中的 missing 's'。
  - 【其他】misc：v24.0.0.1 Hotfix release。
- Top PR（按 churn，Top 8）：
  - #1361 tests,misc: Sync .github dir develop -> stable (churn=109)
  - #1308 misc: Add scheduler.yaml (churn=91)
  - #1306 misc, tests: Fix missing 's' in GPU tests (churn=4)
  - #1425 misc: v24.0.0.1 Hotfix release (churn=0)

### v24.1.0.0 (2024-12-07)

- commits: 522
- PR 数（可识别）：242
- 变更规模（churn）：97319
- 新特性/重要变化（中文归纳，best-effort）：
  - 要点：The [行为 of the 统计 `simInsts` 与 `simOps` has been changed](https://github.com/gem5/gem5/pull/1615).
  - 要点：Instances of kB, MB, 与 GB have been changed to KiB, MiB, 与 GiB for memory 与 cache sizes #1479
  - 要点：Random number generator is no longer shared across components. This may modify simulation results. #1534
  - 主题：gem5 Standard Library
  - 要点：SE mode has been 新增 to X86Board, X86DemoBoard, 与 RiscvBoard #1702
  - 要点：ArmDemoBoard 与 RiscvDemoBoard have been 新增 to the standard library #1478 #1490
  - 【新增/支持】arch-arm：实现FEAT_XS。
  - 【移除/弃用】mem-ruby：移除static methods from RubySystem。
- Release notes 摘要（自动提取）：
  - 要点：The [行为 of the 统计 `simInsts` 与 `simOps` has been changed](https://github.com/gem5/gem5/pull/1615).
  - 要点：Instances of kB, MB, 与 GB have been changed to KiB, MiB, 与 GiB for memory 与 cache sizes #1479
  - 要点：Random number generator is no longer shared across components. This may modify simulation results. #1534
  - 主题：gem5 Standard Library
  - 要点：SE mode has been 新增 to X86Board, X86DemoBoard, 与 RiscvBoard #1702
  - 要点：ArmDemoBoard 与 RiscvDemoBoard have been 新增 to the standard library #1478 #1490
  - 要点：The values in the X86DemoBoard have been modified to make it more similar to the other DemoBoards #1618
  - 主题：预取器
  - 要点：The [行为 of the`StridePrefetcher` has been altered](https://github.com/gem5/gem5/pull/1449) as follows:
  - 修复：修复实现 of Best Offset 预取器 #1403
- Top PR（按 churn，Top 8）：
  - #1303 arch-arm: Implement FEAT_XS (churn=2567)
  - #1453 mem-ruby: Remove static methods from RubySystem (churn=1465)
  - #1534 misc: Do not share the random number generator across components (churn=1272)
  - #1840 misc: v24.1 release notes update (churn=1081)
  - #1695 tests: Fix replacement_policies tests' refs (churn=996)
  - #1538 arch-riscv: add VLEN/ELEN as class attributes for all vec insts (churn=949)
  - #1619 configs: Deprecate Vega10 (churn=651)
  - #1537 Implement BTB using the cache library (churn=527)

### v24.1.0.1 (2024-12-19)

- commits: 7
- PR 数（可识别）：5
- 变更规模（churn）：191
- 新特性/重要变化（中文归纳，best-effort）：
  - 要点：Generalization of the class types in CHI RNF/MN generators thus fixing an issue with missing attributes when using the CHI protocol.
  - 新增：新增 Sphinx 文档 for the gem5 standard library.
  - 新增：新增 missing `RubySystem` member 与 related methods in `PerfectCacheMemory`'s entries.
  - 新增：新增 `useSecondaryLoadLinked` function to "src/mem/ruby/slicc_interface/ProtocolInfo.hh".
  - 【新增/支持】misc：新增sphinx stdlib 文档。
  - 【新增/支持】mem-ruby：新增ProtocolInfo 中的 missing option。
  - 【修复/纠错】mem-ruby：修复PerfectCacheMemory's entries 中的 missing RubySystem。
  - 【其他】configs：Generalize class types in CHI RNF/MN generators。
- Release notes 摘要（自动提取）：
  - 要点：Generalization of the class types in CHI RNF/MN generators thus fixing an issue with missing attributes when using the CHI protocol.
  - 新增：新增 Sphinx 文档 for the gem5 standard library.
  - 新增：新增 missing `RubySystem` member 与 related methods in `PerfectCacheMemory`'s entries.
  - 新增：新增 `useSecondaryLoadLinked` function to "src/mem/ruby/slicc_interface/ProtocolInfo.hh".
- Top PR（按 churn，Top 8）：
  - #335 misc: Add sphinx stdlib documentation (churn=230)
  - #1865 mem-ruby: Add missing option in ProtocolInfo (churn=52)
  - #1864 mem-ruby: Fix missing RubySystem in PerfectCacheMemory's entries (churn=46)
  - #1851 configs: Generalize class types in CHI RNF/MN generators (churn=8)
  - #1875 v24.1.0.1 Hotfix Release (churn=0)

### v24.1.0.2 (2025-02-12)

- commits: 4
- PR 数（可识别）：2
- 变更规模（churn）：43
- 新特性/重要变化（中文归纳，best-effort）：
  - 【其他】mem-ruby：set RubySystem pointer during TBE alloc。
  - 【其他】misc：Hotfix v24.1.0.2。
- Top PR（按 churn，Top 8）：
  - #1930 mem-ruby: set RubySystem pointer during TBE alloc (churn=64)
  - #1964 misc: Hotfix v24.1.0.2 (churn=0)

### v24.1.0.3 (2025-04-11)

- commits: 4
- PR 数（可识别）：2
- 变更规模（churn）：9
- 新特性/重要变化（中文归纳，best-effort）：
  - 【测试】base：修复failing compiler tests。
  - 【其他】misc：Hotfix v24.1.0.3。
- Top PR（按 churn，Top 8）：
  - #1793 base: Fix failing compiler tests (churn=2)
  - #2177 misc: Hotfix v24.1.0.3 (churn=0)

### v25.0.0.0 (2025-06-18)

- commits: 655
- PR 数（可识别）：274
- 变更规模（churn）：113237
- 新特性/重要变化（中文归纳，best-effort）：
  - Hypercalls 与新增 Exit Event Handlers（Hypercalls and New Exit Event Handlers）
  - 改进 RISC-V 与 Arm ISA 支持（Improved RISC-V and Arm ISA Support）
  - Python Utilities
  - OptionalParam 与 DictParam（OptionalParam and DictParam）
- Major Highlights（摘自 RELEASE-NOTES）：
  - Hypercalls and New Exit Event Handlers
  - Improved RISC-V and Arm ISA Support
  - Python Utilities
  - OptionalParam and DictParam
- Release notes 摘要（自动提取）：
  - 要点：**Hypercalls 与新增 Exit Event Handlers**: Exit events now use hypercalls 与
  - 要点：**改进 RISC-V 与 Arm ISA 支持**: Includes major architectural
  - 要点：**Python Utilities**: Introduction of `gem5term`, an m5term replacement in
  - 要点：**OptionalParam 与 DictParam**: Introduction of OptionalParam, which allows
  - 要点：The addition of the **gem5 bridge driver** means that `sudo` is no longer
  - 要点：**SE mode** in the standard library now 支持 multi-program 工作负载
  - 要点：新增 `switch_processor()` to `simulator.py`, allowing processor switching
  - 要点：Users can now print exit event information at runtime
  - 要点：When passing `-re` to a Multisim simulation, the redirected terminal output
  - 主题：Exit Event Framework 与 Hypercalls
- Top PR（按 churn，Top 8）：
  - #2022 arch-riscv: Add support for vector stride segment load/store instructions (churn=2260)
  - #2002 arch-arm: Split the decodeFp function in subfunctions (churn=1175)
  - #2341 arch-riscv: Remove N extension (churn=1104)
  - #1949 dev: rework PCI to add type1 header (churn=1023)
  - #1912 resources: Add exceptions if the resource JSON has schema issues (churn=631)
  - #117 ruby: Enable all protocols in a single gem5 build (churn=586)
  - #1825 sim-se: Implement free-list-based physical page allocator for SE mode (churn=512)
  - #2082 tests: add tests for restoring from checkpoints using multisim (churn=466)

### v25.0.0.1 (2025-08-11)

- commits: 12
- PR 数（可识别）：9
- 变更规模（churn）：476
- 新特性/重要变化（中文归纳，best-effort）：
  - 要点：[#2492](https://github.com/gem5/gem5/pull/2492): 修复 the writeback type for AArch FP16 instructions.
  - 要点：[#2422](https://github.com/gem5/gem5/pull/2422): 修复 incorrect address translation caused by TLB in VEGA.
  - 要点：[#2399](https://github.com/gem5/gem5/pull/2399): 修复 Looppoint analysis.
  - 要点：[#2397](https://github.com/gem5/gem5/pull/2397): Bumps urlob3 to 2.5.0 for gem5-resources-manager.
  - 要点：[#2415](https://github.com/gem5/gem5/pull/2415): 移除 duplicate `ClassicGeneratorExitHandler` class.
  - 要点：[#2441](https://github.com/gem5/gem5/pull/2441): 新增 FEAT_FP16 FP instructions to the ARM ISA.
  - 【新增/支持】arch-arm：新增FEAT_FP16 FP instructions。
  - 【移除/弃用】stdlib：移除duplicate ClassicGeneratorExitHandler。
- Release notes 摘要（自动提取）：
  - 要点：[#2492](https://github.com/gem5/gem5/pull/2492): 修复 the writeback type for AArch FP16 instructions.
  - 要点：[#2422](https://github.com/gem5/gem5/pull/2422): 修复 incorrect address translation caused by TLB in VEGA.
  - 要点：[#2399](https://github.com/gem5/gem5/pull/2399): 修复 Looppoint analysis.
  - 要点：[#2397](https://github.com/gem5/gem5/pull/2397): Bumps urlob3 to 2.5.0 for gem5-resources-manager.
  - 要点：[#2415](https://github.com/gem5/gem5/pull/2415): 移除 duplicate `ClassicGeneratorExitHandler` class.
  - 要点：[#2441](https://github.com/gem5/gem5/pull/2441): 新增 FEAT_FP16 FP instructions to the ARM ISA.
  - 要点：[#2464](https://github.com/gem5/gem5/pull/2464): Populates logBytes/paddr after functional page 页表遍历 in the RISC-V. This 修复 [#2410](https://github.com/gem5/gem5/pull/2410) which caused the gem5-bridge `readfile` command to fail in RISC-V simulations.
  - 要点：[#2502](https://github.com/gem5/gem5/pull/2502): 新增 the simpoint listen to 新增 probe structure
  - 要点：[#2512](https://github.com/gem5/gem5/pull/2512): 修复 the time buffer in the O3 CPU when clearing states.
- Top PR（按 churn，Top 8）：
  - #2441 arch-arm: Add FEAT_FP16 FP instructions (churn=694)
  - #2415 stdlib: remove duplicate ClassicGeneratorExitHandler (churn=126)
  - #2399 cpu: Fix looppoint analysis v25 (churn=18)
  - #2502 cpu: Adapt simpoint listener to new probe structure (churn=16)
  - #2512 cpu-o3: properly index time buffer when clearing states (churn=14)
  - #2464 arch-riscv: populate logBytes/paddr after functional pt walk (churn=12)
  - #2397 util: bump urllib3 to 2.5.0 in util/gem5-resources-manager (churn=4)
  - #2492 arch-arm: fix writeback type for AArch32 FP16 instructions (churn=4)

### v25.1.0.0 (2025-12-31)

- commits: 673
- PR 数（可识别）：274
- 变更规模（churn）：125973
- 新特性/重要变化（中文归纳，best-effort）：
  - Neoverse V2 核心模型.（Neoverse V2 core model.）
  - 新增分支预测器.（New branch predictor.）
  - 推进 Armv9 支持 with a 完整 FEAT_SVE2 实现.（Towards Armv9 support with a full FEAT_SVE2 implementation.）
  - 解耦前端与 fetch-directed 预取器 (FDP).（Decoupled front end and fetch-directed prefetcher (FDP).）
  - 分布式 instruction/发射队列.（Distributed instruction/issue queue.）
  - 非序列化行为 for O3CPU MiscRegClass 寄存器.（Non-serializing behavior for O3CPU MiscRegClass registers.）
  - 改进 Arm 页表遍历机制.（Improved Arm table-walk machinery.）
  - 多 GPUs 与可配置 GPU 显存大小.（Multiple GPUs and configurable GPU memory size.）
  - 改进统计基础设施.（Improved statistics infrastructure.）
- Major Highlights（摘自 RELEASE-NOTES）：
  - Neoverse V2 core model.
  - New branch predictor.
  - Towards Armv9 support with a full FEAT_SVE2 implementation.
  - Decoupled front end and fetch-directed prefetcher (FDP).
  - Distributed instruction/issue queue.
  - Non-serializing behavior for O3CPU MiscRegClass registers.
  - Improved Arm table-walk machinery.
  - Multiple GPUs and configurable GPU memory size.
  - Improved statistics infrastructure.
- Release notes 摘要（自动提取）：
  - 要点：**Neoverse V2 核心模型.**
  - 要点：**新增分支预测器.**
  - 要点：**推进 Armv9 支持 with a 完整 FEAT_SVE2 实现.**
  - 要点：**解耦前端与 fetch-directed 预取器 (FDP).**
  - 要点：**分布式 instruction/发射队列.**
  - 要点：**非序列化行为 for O3CPU MiscRegClass 寄存器.**
  - 要点：**改进 Arm 页表遍历机制.**
  - 要点：**多 GPUs 与可配置 GPU 显存大小.**
  - 要点：**改进统计基础设施.**
  - 要点：**系统调用改进.**
- Top PR（按 churn，Top 8）：
  - #2551 ext: Update Pybind11 to v3.0.0 (churn=34760)
  - #2298 dev: reworks PCI to add a PCI host bridge (churn=1845)
  - #2409 scons: Update build_tools to enable importing (churn=1757)
  - #2079 util: Add validator and tests for full system workloads (disk and kernels) (churn=1248)
  - #1712 arch-riscv: Fix incorrect vector slide instructions and statically filter redundant uops (churn=1078)
  - #2741 arch-arm,sim-se: Implement sigreturn for Arm64 (churn=805)
  - #2465 arch-arm: Split decodeBranchExcSys into multiple sub-functions (churn=774)
  - #2652 configs, cpu-o3: Implement a distributed InstructionQueue (churn=757)

