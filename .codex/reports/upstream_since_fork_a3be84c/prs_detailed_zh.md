# upstream PR 逐条摘要（中文结构 + 原始标题）

- 说明：该文件为“机器生成”的 digest，主要用于快速检索。
- PR 的“标题/内容”依赖提交信息；更完整信息需要访问对应 PR 页面。

- 范围：`a3be84cb1b854da51716d6399ca139016714bd54..upstream/stable`
- PR 数量（可识别）：1495；无 PR 号的 commits：2900

## v23.0.1.0 (2023-08-11)

- PR 数：15

### #75 arch-arm: Fix assert fail when UQRSHL shiftAmt==0

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/75
- 代表 commit: `2242196f03c6` (2023-07-18)
- 变更规模: commits=2, files=1, +6/-2 (churn=8)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/insts/neon64.isa` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2023-07-13 `18470b474726` arch-arm: Fix assert fail when UQRSHL shiftAmt==0
  - 2023-07-18 `2242196f03c6` arch-arm: Fix assert fail when UQRSHL shiftAmt==0
- 复现: `git show 2242196f03c6f73d62077b1437929eadee8e8741`

### #79 tests: Improve Pyunit tests gem5 Resources' downloads

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/79
- 代表 commit: `189bf66b3e48` (2023-07-18)
- 变更规模: commits=2, files=4, +38/-38 (churn=76)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/pyunit/stdlib/resources/refs/resource-specialization.json` (churn=36)
  - `tests/pyunit/stdlib/resources/refs/workload-checks.json` (churn=32)
  - `tests/pyunit/stdlib/resources/pyunit_resource_specialization.py` (churn=4)
  - `tests/pyunit/stdlib/resources/pyunit_workload_checks.py` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2023-07-14 `bc99a6e3467f` tests: Improve Pyunit tests gem5 Resources' downloads
  - 2023-07-18 `189bf66b3e48` tests: Improve Pyunit tests gem5 Resources' downloads
- 复现: `git show 189bf66b3e488ec01e003b1813599708307b4ac8`

### #87 util: Add "Improving stability" sec to github-vagrant-runner

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/87
- 代表 commit: `84c4451cebac` (2023-07-19)
- 变更规模: commits=2, files=1, +28/-0 (churn=28)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/github-runners-vagrant/README.md` (churn=28)
- commits 列表（按 topo-order，Top 12）：
  - 2023-07-17 `6062214d873c` util: Add "Improving stability" sec to github-vagrant-runner
  - 2023-07-19 `84c4451cebac` util: Add "Improving stability" sec to github-vagrant-runner
- 复现: `git show 84c4451cebac98a9e17e3ac87da940b308c90a27`

### #71 misc: Update README/README.md

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/71
- 代表 commit: `b82ae1481bd1` (2023-07-19)
- 变更规模: commits=2, files=4, +192/-90 (churn=282)
- 影响范围: topdirs=README, README.md, build_tools, src; subsys=README, README.md, build_tools, src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `README.md` (churn=180)
  - `README` (churn=86)
  - `build_tools/infopy.py` (churn=12)
  - `src/SConscript` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2023-07-17 `f80015ea1827` misc: Update README/README.md
  - 2023-07-19 `b82ae1481bd1` misc: Update README/README.md
- 复现: `git show b82ae1481bd10c87f98f9d52bfcc8e432ab2a856`

### #93 python: fix fatal in main.py (github #78)

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/93
- 代表 commit: `e810f53ebee1` (2023-07-19)
- 变更规模: commits=2, files=1, +8/-6 (churn=14)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/main.py` (churn=14)
- commits 列表（按 topo-order，Top 12）：
  - 2023-07-18 `573523c07a65` python: fix fatal in main.py (github #78)
  - 2023-07-19 `e810f53ebee1` python: fix fatal in main.py (github #78)
- 复现: `git show e810f53ebee1d95af3fc0b9725438f88224c8039`

### #104 scons: Add extra parent dir to CPPPATH if --no-duplicate-sources

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/104
- 代表 commit: `919dd5efbd67` (2023-07-26)
- 变更规模: commits=2, files=1, +16/-0 (churn=16)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/SConscript` (churn=16)
- commits 列表（按 topo-order，Top 12）：
  - 2023-07-20 `566308dad910` scons: Add extra parent dir to CPPPATH if --no-duplicate-sources
  - 2023-07-26 `919dd5efbd67` scons: Add extra parent dir to CPPPATH if --no-duplicate-sources
- 复现: `git show 919dd5efbd67ee27579014c9a4e0f67d47eb9c1f`

### #95 cpu-kvm: Make using perf when using KVM CPU optional

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/95
- 代表 commit: `41abc6ab77b6` (2023-07-26)
- 变更规模: commits=2, files=5, +442/-58 (churn=500)
- 影响范围: topdirs=src, configs; subsys=cpu, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gem5_library/x86-ubuntu-run-with-kvm-no-perf.py` (churn=276)
  - `src/cpu/kvm/base.cc` (churn=168)
  - `src/cpu/kvm/perfevent.cc` (churn=32)
  - `src/cpu/kvm/base.hh` (churn=14)
  - `src/cpu/kvm/BaseKvmCPU.py` (churn=10)
- commits 列表（按 topo-order，Top 12）：
  - 2023-07-20 `f7da973f34cd` cpu-kvm: Make using perf when using KVM CPU optional
  - 2023-07-26 `41abc6ab77b6` cpu-kvm: Make using perf when using KVM CPU optional
- 复现: `git show 41abc6ab77b6dc24f0156aa0a9cab2d4eb1ab90e`

### #72 mem-garnet: Fix packet_id val in flit

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/72
- 代表 commit: `7665c338e268` (2023-07-26)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/network/garnet/flit.cc` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2023-07-20 `427b4d596eae` mem-garnet: Fix packet_id val in flit
  - 2023-07-26 `7665c338e268` mem-garnet: Fix packet_id val in flit
- 复现: `git show 7665c338e268736c0636400b1d3d13f169a0a61e`

### #122 misc: Merge develop .github into stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/122
- 代表 commit: `48b4788bbe7a` (2023-07-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 48b4788bbe7a091b917aed51236bca2b0456cf37`

### #139 misc: Merge stable into minor-release-staging-v23-0-1-0

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/139
- 代表 commit: `1be9501ecdca` (2023-07-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1be9501ecdcab0739881b92b746ed25cf4cc232e`

### #125 misc: Update version to v23.0.1.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/125
- 代表 commit: `010baba2a700` (2023-07-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 010baba2a7003b6523d3fc8cef37102e47118ceb`

### #128 misc: Update RELEASE-NOTES.md for v23.0.1.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/128
- 代表 commit: `4e8df826f6bb` (2023-07-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4e8df826f6bbd5da7cc0a55ae10ac253cf9cf471`

### #103 misc: Merge cherry-picked commits from develop

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/103
- 代表 commit: `1b67297efcc6` (2023-07-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1b67297efcc688643e56841344db27baa194bed0`

### #168 misc: Merge develop .github into stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/168
- 代表 commit: `c8239cca6d5a` (2023-08-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c8239cca6d5a67c0e59fecccc5d4b78d42e97209`

### #174 misc: Minor release v23.0.1.0

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/174
- 代表 commit: `6835f0665744` (2023-08-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6835f0665744bba3d56921c9406ee97e841b60a0`

## v23.1.0.0 (2023-12-28)

- PR 数：350

### #64 gpu-compute, tests: Fix GPU_X86 compilation, add compiler tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/64
- 代表 commit: `753933d47172` (2023-07-11)
- 变更规模: commits=1, files=5, +17/-7 (churn=24)
- 影响范围: topdirs=src, .github, tests; subsys=.github, arch, cpu, dev, tests; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/testers/gpu_ruby_test/address_manager.cc` (churn=11)
  - `src/dev/hsa/kfd_ioctl.h` (churn=5)
  - `src/arch/amdgpu/vega/insts/instructions.hh` (churn=4)
  - `.github/workflows/compiler-tests.yaml` (churn=2)
  - `tests/compiler-tests.sh` (churn=2)
- 复现: `git show 753933d47172d931f577120ce3c930cee71ecfe3`

### #70 Sanitizer libraries static linking

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/70
- 代表 commit: `2a880053bbc1` (2023-07-12)
- 变更规模: commits=1, files=1, +6/-2 (churn=8)
- 影响范围: topdirs=SConstruct; subsys=SConstruct; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=8)
- 复现: `git show 2a880053bbc1cf794d4488f006defee2818f2dc8`

### #65 misc: Merge v23.0.0.1 Hotfix into develop

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/65
- 代表 commit: `552ae9a1a2fe` (2023-07-13)
- 变更规模: commits=1, files=1, +12/-0 (churn=12)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=12)
- 复现: `git show 552ae9a1a2fecd693007b7802670d865ca718743`

### #63 stdlib: Deviding range for linear multicore.

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/63
- 代表 commit: `b2fcc558d8a4` (2023-07-14)
- 变更规模: commits=1, files=3, +28/-6 (churn=34)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/processors/abstract_generator.py` (churn=16)
  - `src/python/gem5/components/processors/linear_generator.py` (churn=10)
  - `src/python/gem5/components/processors/complex_generator.py` (churn=8)
- 复现: `git show b2fcc558d8a467aa517b62023b976c7f7365d82b`

### #81 arch-riscv: Fix clearLoadReservation merge

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/81
- 代表 commit: `52d9259396af` (2023-07-14)
- 变更规模: commits=1, files=2, +6/-1 (churn=7)
- 影响范围: topdirs=src; subsys=arch; arch=generic, riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.hh` (churn=6)
  - `src/arch/generic/isa.hh` (churn=1)
- 复现: `git show 52d9259396af6d56047f1c72c049f8c43b5d8030`

### #67 mem-ruby: Added WIB State to VIPER TCC Cache

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/67
- 代表 commit: `f8f5dd98bf93` (2023-07-17)
- 变更规模: commits=1, files=1, +24/-8 (churn=32)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` (churn=32)
- 复现: `git show f8f5dd98bf937f8052654ebccb9cf80355ee5bf2`

### #90 Add feature to output citations automatically based on configuration

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/90
- 代表 commit: `442923c414d5` (2023-07-17)
- 变更规模: commits=1, files=10, +365/-0 (churn=365)
- 影响范围: topdirs=src; subsys=mem, python, gpu-compute; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/citations.py` (churn=191)
  - `src/mem/ruby/network/garnet/GarnetNetwork.py` (churn=38)
  - `src/gpu-compute/GPU.py` (churn=33)
  - `src/mem/DRAMSys.py` (churn=29)
  - `src/mem/DRAMsim3.py` (churn=23)
  - `src/mem/MemCtrl.py` (churn=22)
  - `src/mem/DRAMSim2.py` (churn=21)
  - `src/python/m5/SimObject.py` (churn=4)
- 复现: `git show 442923c414d5d3f3d94d462ed0f9baae83db177f`

### #92 configs: fix GPU's default number of HW barrier/CU

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/92
- 代表 commit: `efa1d87addd9` (2023-07-17)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=gpu-compute; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/gpu-compute/GPU.py` (churn=2)
- 复现: `git show efa1d87addd9caff066a12cc0472164f1c5dd7f5`

### #68 scons: Use pkgconfig to get correct Protobuf dependency

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/68
- 代表 commit: `162f2e2dba06` (2023-07-17)
- 变更规模: commits=1, files=1, +4/-10 (churn=14)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/proto/SConsopts` (churn=14)
- 复现: `git show 162f2e2dba069ad3df7d488136f07536d54b7795`

### #76 base: Find lsb set generalization and optimization

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/76
- 代表 commit: `6fb72d84e171` (2023-07-17)
- 变更规模: commits=1, files=2, +91/-26 (churn=117)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/bitfield.hh` (churn=99)
  - `src/base/bitfield.test.cc` (churn=18)
- 复现: `git show 6fb72d84e171de3cd575b442f392ebdf4c4e08fc`

### #88 misc: Update CI test workflow

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/88
- 代表 commit: `424350f446fe` (2023-07-18)
- 变更规模: commits=1, files=3, +132/-32 (churn=164)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=139)
  - `.github/workflows/weekly-tests.yaml` (churn=20)
  - `.github/workflows/ci-tests.yaml` (churn=5)
- 复现: `git show 424350f446fe5965ca44de68acb4b8cd3441fe6a`

### #85 misc: Add bug report template

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/85
- 代表 commit: `8450b93f8e4a` (2023-07-18)
- 变更规模: commits=1, files=1, +62/-0 (churn=62)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/ISSUE_TEMPLATE/bug_report.md` (churn=62)
- 复现: `git show 8450b93f8e4a939fba6e075b225c07f112a5a72b`

### #80 misc: Drop older compilers and Ubuntu 18.04

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/80
- 代表 commit: `65fc9a6bfaf6` (2023-07-18)
- 变更规模: commits=1, files=6, +5/-167 (churn=172)
- 影响范围: topdirs=util, .github, tests; subsys=util, .github, tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/ubuntu-18.04_clang-version/Dockerfile` (churn=53)
  - `util/dockerfiles/ubuntu-18.04_gcc-version/Dockerfile` (churn=49)
  - `util/dockerfiles/ubuntu-18.04_all-dependencies/Dockerfile` (churn=39)
  - `util/dockerfiles/docker-compose.yaml` (churn=27)
  - `.github/workflows/compiler-tests.yaml` (churn=2)
  - `tests/compiler-tests.sh` (churn=2)
- 复现: `git show 65fc9a6bfaf625e2eab3aea82380fcb7a23cd3b8`

### #99 base: Added missing backup dummy __has_builtin definition

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/99
- 代表 commit: `4d9bd7dedf3b` (2023-07-19)
- 变更规模: commits=1, files=1, +7/-0 (churn=7)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/bitfield.hh` (churn=7)
- 复现: `git show 4d9bd7dedf3b0615e62095659c93d68c3d6cb680`

### #73 base: Unit tests miscellaneous patches

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/73
- 代表 commit: `4c4419296b84` (2023-07-19)
- 变更规模: commits=1, files=6, +135/-138 (churn=273)
- 影响范围: topdirs=src; subsys=base, sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/inifile.cc` (churn=105)
  - `src/base/amo.test.cc` (churn=74)
  - `src/base/trie.hh` (churn=64)
  - `src/base/inifile.hh` (churn=23)
  - `src/sim/serialize.cc` (churn=5)
  - `src/base/memoizer.hh` (churn=2)
- 复现: `git show 4c4419296b8430d754a96da0eaae731c594f4d06`

### #98 arch-riscv: Set default check alignment True

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/98
- 代表 commit: `5d2edca1e361` (2023-07-19)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/RiscvISA.py` (churn=2)
- 复现: `git show 5d2edca1e36189052739250c25471c285746665b`

### #101 mem-ruby: Added support for non-system-scope atomics in VIPER

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/101
- 代表 commit: `1705853b12df` (2023-07-20)
- 变更规模: commits=1, files=2, +103/-14 (churn=117)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` (churn=109)
  - `src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm` (churn=8)
- 复现: `git show 1705853b12df627d0e7279ef2acbd01dd1291838`

### #91 stdlib: Change resource compatibility warning

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/91
- 代表 commit: `3c6563d6f7cf` (2023-07-20)
- 变更规模: commits=1, files=2, +12/-16 (churn=28)
- 影响范围: topdirs=src, tests; subsys=python, tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/client_api/client_wrapper.py` (churn=22)
  - `tests/pyunit/stdlib/resources/pyunit_obtain_resources_check.py` (churn=6)
- 复现: `git show 3c6563d6f7cf35f5e801b7a1dc9363619bb55239`

### #102 stdlib,configs,tests: Remove deprecated Resource classes usage

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/102
- 代表 commit: `01623fac68d1` (2023-07-20)
- 变更规模: commits=1, files=25, +73/-73 (churn=146)
- 影响范围: topdirs=configs, tests, src; subsys=configs, tests, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/workload.py` (churn=14)
  - `configs/example/gem5_library/x86-spec-cpu2017-benchmarks.py` (churn=10)
  - `tests/gem5/configs/arm_boot_exit_run.py` (churn=8)
  - `tests/pyunit/stdlib/resources/pyunit_workload_checks.py` (churn=8)
  - `configs/example/gem5_library/checkpoints/riscv-hello-restore-checkpoint.py` (churn=6)
  - `configs/example/gem5_library/riscv-fs.py` (churn=6)
  - `configs/example/gem5_library/x86-gapbs-benchmarks.py` (churn=6)
  - `configs/example/gem5_library/x86-npb-benchmarks.py` (churn=6)
- 复现: `git show 01623fac68d18fd96eaf981798f75e39d75ea260`

### #77 base: Ostream helpers (iterable, tuple, pair, enum, pointers, optional)

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/77
- 代表 commit: `75b6fa5ad11e` (2023-07-21)
- 变更规模: commits=1, files=3, +89/-4 (churn=93)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/stl_helpers/ostream_helpers.test.cc` (churn=54)
  - `src/base/stl_helpers/ostream_helpers.hh` (churn=36)
  - `src/base/cprintf_formats.hh` (churn=3)
- 复现: `git show 75b6fa5ad11e93f37a97a9388ddbf2d787457938`

### #96 misc: Add workflow to close stale issues

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/96
- 代表 commit: `0dd433462261` (2023-07-21)
- 变更规模: commits=1, files=1, +19/-0 (churn=19)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/utils.yaml` (churn=19)
- 复现: `git show 0dd43346226150330ee3e969b338c31e1e6d21c2`

### #110 mem-ruby,configs: Add GLC Atomic Latency VIPER Parameter

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/110
- 代表 commit: `984499329d5b` (2023-07-23)
- 变更规模: commits=1, files=6, +46/-12 (churn=58)
- 影响范围: topdirs=src, configs; subsys=mem, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/slicc/ast/EnqueueStatementAST.py` (churn=26)
  - `src/mem/ruby/network/MessageBuffer.cc` (churn=11)
  - `src/mem/slicc/parser.py` (churn=8)
  - `src/mem/ruby/network/MessageBuffer.hh` (churn=6)
  - `configs/ruby/GPU_VIPER.py` (churn=4)
  - `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` (churn=3)
- 复现: `git show 984499329d5b70c1841c39e545878303b4a5ddd5`

### #111 misc: Update ci-tests.yaml to always clean runner

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/111
- 代表 commit: `9f56bbd7dd93` (2023-07-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 9f56bbd7dd934cccea74d88666d4885feb025f16`

### #114 arch-arm: Hook TLBIOS instructions to the TlbiShareable obj

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/114
- 代表 commit: `189d514f2fd9` (2023-07-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 189d514f2fd93230453ba6251646989fa0ee108e`

### #109 base: Add `maybe_unused` to `findLsbSetFallback`

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/109
- 代表 commit: `556c9154ddae` (2023-07-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 556c9154ddae36ff184a83f7b31ad51cd7083f6a`

### #107 cpu-minor: Check pc valid before printing

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/107
- 代表 commit: `7601fcfba60f` (2023-07-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7601fcfba60fe06b320230e00b9dcf9ea4064772`

### #126 tests: Deprecate Gerrit/Jenkins testing scripts

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/126
- 代表 commit: `c056ef07a58c` (2023-07-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c056ef07a58ca7a8de546599684ba7496dd226af`

### #121 misc: Updating TESTING.md

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/121
- 代表 commit: `6a503d52cde9` (2023-07-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6a503d52cde967edb5c18925d7deeaebb6f34c94`

### #124 mem: Make functional request a response when satisfied by queue

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/124
- 代表 commit: `21b4ad609f28` (2023-07-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 21b4ad609f28487dc65ab7f7d54ba09768642d2e`

### #105 misc: Split up tests in daily-tests.yaml

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/105
- 代表 commit: `5888ea68a36e` (2023-07-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5888ea68a36e508516579879b6c9de96a6f81230`

### #133 cpu: Set SLC bit for GPU tester

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/133
- 代表 commit: `ea18c2f417dc` (2023-07-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ea18c2f417dc0115e88087aa6200f52f1a976a49`

### #135 learning-gem5: Add a missing override

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/135
- 代表 commit: `5aa955212f1d` (2023-07-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5aa955212f1d6cc09c26dcd71e315e1d7e4d1c03`

### #136 misc: Add missing dependency to daily tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/136
- 代表 commit: `42b65cad68f1` (2023-07-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 42b65cad68f1d2ddb21534807134dadc7599f11c`

### #134 util: Ignore line length check for #include pragma in C/C++ files

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/134
- 代表 commit: `65b99fffc93e` (2023-07-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 65b99fffc93e3a7bddfffb4d3607e91c5a065779`

### #130 misc: Sync CONTRIBUTING.md with website

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/130
- 代表 commit: `31230025e9d4` (2023-07-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 31230025e9d4d0676a93605902f4115a639e12a4`

### #113 arch-x86: Move CPUID values to python

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/113
- 代表 commit: `6b020ed03308` (2023-07-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6b020ed03308ddaf1208fcfbde46fc434d41e111`

### #140 gpu-compute: "<random>" -> "base/random.hh" in testers/gpu...

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/140
- 代表 commit: `81cc57b828fc` (2023-07-28)
- 变更规模: commits=2, files=1, +9/-0 (churn=9)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/testers/gpu_ruby_test/protocol_tester.cc` (churn=9)
- commits 列表（按 topo-order，Top 12）：
  - 2023-07-28 `08a3762a14d9` gpu-compute: Add warn for `random_seed == 0` case
  - 2023-07-28 `81cc57b828fc` gpu-compute: "<random>" -> "base/random.hh" in testers/gpu...
- 复现: `git show 81cc57b828fc74198719782c86da5c9a10c22784`

### #142 arch-vega: Fix vop2Helper scalar support

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/142
- 代表 commit: `b35c2ba8c5d7` (2023-07-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b35c2ba8c5d7b16750aae470b7183c35c4094354`

### #129 arch-vega, dev-amdgpu: Fix for memory leaks

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/129
- 代表 commit: `618b2a60dee4` (2023-07-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 618b2a60dee4badc1f92c35b5f556b4cac86cbbe`

### #143 mem: Minor typo fix in packet.hh

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/143
- 代表 commit: `4ee6dbc330fd` (2023-07-31)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4ee6dbc330fdd46ea040b1595c2e9d2f54a65497`

### #141 dev-amdgpu: Support for ROCm 5.4+ and MI200

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/141
- 代表 commit: `dceabe5fda59` (2023-07-31)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show dceabe5fda598937b00959cc648fb7f2fbbe950a`

### #150 stdlib,resources: Enable loading of local Resources data via JSON file path

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/150
- 代表 commit: `fbcf50befd98` (2023-08-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fbcf50befd98ea5931a5812424ac6f08833da7e8`

### #83 arch-riscv: Relation chain on RVV support

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/83
- 代表 commit: `5eda9fe2ca8d` (2023-08-03)
- 变更规模: commits=2, files=4, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=arch; arch=mips, power, sparc, x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/mips/isa.cc` (churn=2)
  - `src/arch/power/isa.cc` (churn=2)
  - `src/arch/sparc/isa.cc` (churn=2)
  - `src/arch/x86/isa.cc` (churn=2)
- commits 列表（按 topo-order，Top 12）：
  - 2023-08-03 `5eda9fe2ca8d` arch-riscv: Relation chain on RVV support
  - 2024-02-02 `d031244ca70d` misc: When unused, set #MatRegClass registers to 0
- 复现: `git show 5eda9fe2ca8dd825223b7c79766aedcaad722cc9`

### #153 stdlib, resources: Fixed keyerror: 'is_zipped' bug

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/153
- 代表 commit: `2bef8efb94fa` (2023-08-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2bef8efb94fafe058b6bbc44fd61f063c0172155`

### #149 stdlib, resources: fixed style issue in isa.hh

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/149
- 代表 commit: `0ff485f7d02c` (2023-08-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0ff485f7d02c2c0dcc10d020650b583e843ba5ea`

### #152 tests: download_check.py to rm each resource after check

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/152
- 代表 commit: `6e39f2097d6f` (2023-08-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6e39f2097d6f9a404668a0b89d6cc6b2108562b8`

### #115 util: fix cpt upgrader for rvv changes in PR #83

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/115
- 代表 commit: `ed44df5d0252` (2023-08-04)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ed44df5d0252c1b40a4e24c70ca86fad2ed09b0b`

### #137 arch-riscv: Implemented zicbom/zicboz extensions for RISC V

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/137
- 代表 commit: `7a9f7f51ae61` (2023-08-04)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7a9f7f51ae610a6b5873da6812b3e08f807dc60e`

### #158 misc: Fix daily tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/158
- 代表 commit: `3d39bc160cbb` (2023-08-04)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3d39bc160cbbba8d54eb38df654145e4b679ccec`

### #156 tests: Refactor test configs

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/156
- 代表 commit: `4114114beda2` (2023-08-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4114114beda2996817f3f0cfc353b505ed308a4b`

### #160 misc: Refactor weekly-tests.yaml

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/160
- 代表 commit: `5200d9ca3dec` (2023-08-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5200d9ca3dec47226093714aee75b4fafae1e099`

### #164 tests: Temporarily cease using PARSEC disk image in tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/164
- 代表 commit: `faed0d3f6d4d` (2023-08-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show faed0d3f6d4da8693347a0f9add1ef7a5adafe58`

### #163 misc: Update where runners are cleaned in workflow files

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/163
- 代表 commit: `572c6bc1bb6e` (2023-08-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 572c6bc1bb6ee220a3bb4c2a7928011011f966ec`

### #170 arch-riscv: Add checking CSR condition for RVV instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/170
- 代表 commit: `4cac85cb8047` (2023-08-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4cac85cb80476eed890826953b7f551ef0f47de7`

### #169 cpu: Fix segment fault when using debug flags Branch

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/169
- 代表 commit: `b88e60ff2818` (2023-08-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b88e60ff2818753f73c9bf43bcf3c33050d15bcc`

### #172 cpu-o3: bugfix of rename squash when SMT

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/172
- 代表 commit: `77e63b6a6c44` (2023-08-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 77e63b6a6c44b25b6237f0c93ae43c00dd85ed4a`

### #155 misc: Update MAINTAINERS.yaml

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/155
- 代表 commit: `cfea9afae3d0` (2023-08-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show cfea9afae3d0bffa6853d5bf04f1a84b1b5c60f2`

### #173 tests: Move replacement policy and simulator config files

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/173
- 代表 commit: `fa918f61d190` (2023-08-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fa918f61d19075d916df019e79a176eac6809ea3`

### #175 misc: Add continue-on-error to matrix runs

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/175
- 代表 commit: `41dcd3c5d5e1` (2023-08-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 41dcd3c5d5e1aa85584d6951c273ea70f484a050`

### #179 misc: Sync GitHub Workflow files from develop to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/179
- 代表 commit: `f29bfc0640c8` (2023-08-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f29bfc0640c88a79eb7f94454ce31b3237ec0066`

### #185 mem: Fixing memory size type issue in port proxy.

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/185
- 代表 commit: `954328fa2883` (2023-08-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 954328fa28831e40294727099cc870ea0479a423`

### #180 mem: Port trace in xbar when address error

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/180
- 代表 commit: `9ee400ff9260` (2023-08-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 9ee400ff9260ba7431592d15993b2eb9bfdc8943`

### #183 fastmodel: Add option to retry licence server connection.

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/183
- 代表 commit: `f6d44ac7b3e7` (2023-08-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f6d44ac7b3e7def977f99ab542cd10118d3b6876`

### #184 gpu-compute: Change kernel-based exit location

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/184
- 代表 commit: `bc9bbc10f030` (2023-08-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bc9bbc10f030546ab3c07dd2199aa147790e4bba`

### #190 util-docker: Fix clang-version-8 docker container

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/190
- 代表 commit: `f6b116d8a0d2` (2023-08-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f6b116d8a0d295d0221bd52842c6bac06901f178`

### #189 arch-x86,cpu-kvm: Fix gem5.fast due to unused variable

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/189
- 代表 commit: `3ff6fe0e90aa` (2023-08-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3ff6fe0e90aa943dacf11e11b93a5106bceeb281`

### #187 arch-riscv: Check CSR before executing VMem instructions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/187
- 代表 commit: `fe43e4a3e3f5` (2023-08-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fe43e4a3e3f5cb9f3fabb33b541e182312731815`

### #166 Fix reporting traps (faults) to GDB in SE mode

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/166
- 代表 commit: `22c52f4fbabc` (2023-08-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 22c52f4fbabc59fc1003c1deedb06c0249d2b6ef`

### #195 stdlib: Allow passing of func as Exit Event generator

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/195
- 代表 commit: `30ab2c19b1f5` (2023-08-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 30ab2c19b1f538cd6fa10acc2b6b6d53d4d39162`

### #194 misc: Update matrix runs in scheduled tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/194
- 代表 commit: `ac8887101720` (2023-08-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ac88871017200c963926395c2ec1a1f2580bf73a`

### #167 tests: Add checkpoint tests for all ISAs

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/167
- 代表 commit: `d7d441becb30` (2023-08-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d7d441becb30bd6cdffa01404fbc9a70ec4a32ca`

### #202 ext: Update DRAMSys README

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/202
- 代表 commit: `e5fcc116ec69` (2023-08-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e5fcc116ec697ffc9f08357f958227d071760568`

### #196 arch-riscv,systemc: Update cxx_config_cc.py to use is port.is_source

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/196
- 代表 commit: `f98cd15ec765` (2023-08-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f98cd15ec765bf281fb66bc5642cc288fa9fd915`

### #205 mem-cache: Allow clflush's uncacheable requests on classic cache

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/205
- 代表 commit: `63b91b51a266` (2023-08-21)
- 变更规模: commits=2, files=1, +15/-10 (churn=25)
- 影响范围: topdirs=src; subsys=mem/cache; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/cache.cc` (churn=25)
- commits 列表（按 topo-order，Top 12）：
  - 2023-08-21 `63b91b51a266` mem-cache: Allow clflush's uncacheable requests on classic cache
  - 2023-09-08 `91d1a5deb532` mem-cache: Fix bug in classic cache while clflush
- 复现: `git show 63b91b51a266e3a8e6c7cdc3e2887c95cf2d1bf3`

### #198 misc: Add DRAMSys tests to our weekly tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/198
- 代表 commit: `f9a4a794b731` (2023-08-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f9a4a794b7312711c101f48e3a9c7bd1584b1ce2`

### #203 base: Make 'findLsbSetFallback' constexpr to fix gcc-8 comp

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/203
- 代表 commit: `e3414c709878` (2023-08-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e3414c709878b53e59d23d446dd276962ea23ab7`

### #206 tests: Update asmtest script and add more test binaries

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/206
- 代表 commit: `c218104f5227` (2023-08-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c218104f5227bc31f36dbba632b253cc6d95acdd`

### #209 ext: Specialize GDBSignal MACRO to gem5

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/209
- 代表 commit: `2d9ad02ae7ae` (2023-08-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2d9ad02ae7ae1c5da24c7af2121e0ce218894c87`

### #223 gpu-compute,arch-vega: Fix ALU-only LDS counters

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/223
- 代表 commit: `9fd846f48dcd` (2023-08-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 9fd846f48dcd2e14ae8c09f5defc44472a2ce400`

### #217 mem-ruby: fix CHI Evict race condition

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/217
- 代表 commit: `e77666d9e81c` (2023-08-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e77666d9e81c17765ea5ea4d330c438d51afb2c1`

### #210 sim: provide a signal constructor with an init_state

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/210
- 代表 commit: `56a8ab3f3c4f` (2023-08-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 56a8ab3f3c4f98ba07c138af1c81f5955ccbd496`

### #225 cpu-minor: Separate the reg_index of VecClassReg and VecElemReg

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/225
- 代表 commit: `7aa896fe8fa9` (2023-08-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7aa896fe8fa95bb70d05def65dc972b67e23e480`

### #186 tests, gpu-compute: Updating weekly.sh to use mmapped version of FW

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/186
- 代表 commit: `cf997c93a555` (2023-08-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show cf997c93a555c309fb6f0d8086596bc7d7ff4091`

### #224 dev-amdgpu: Tell OS about PCIe atomic support

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/224
- 代表 commit: `fcbed2bd8a28` (2023-08-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fcbed2bd8a2898d3b1eaf17c18242275ac93fcdf`

### #222 misc: Move compiler tests to run on 'build' runners

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/222
- 代表 commit: `5cb604559aff` (2023-08-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5cb604559afff3a4f70b7dcbd5954b552b4e32cf`

### #230 gpu-compute: Use timing DMAs for GPUFS HSA signals

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/230
- 代表 commit: `a9b32cdb3a4f` (2023-08-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a9b32cdb3a4f6daf33bab26937f888f480547bc1`

### #231 gpu-compute: Flat scratch implementation and bug fixes

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/231
- 代表 commit: `82ffc16e6ec3` (2023-08-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 82ffc16e6ec3ea84294bbd8398f54b09da63f456`

### #220 mem-ruby: Improve Ruby/CHI stats for in/out trans

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/220
- 代表 commit: `4bd3d2f864ab` (2023-08-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4bd3d2f864aba6376578f237583031389a704ffa`

### #229 misc: Update CI tests to not run on draft PRs

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/229
- 代表 commit: `9d2e860d7441` (2023-08-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 9d2e860d744167ef1413241acdcc9fea945d887b`

### #218 mem-ruby: fix assert on CHI ReadUnique

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/218
- 代表 commit: `737c611e72df` (2023-08-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 737c611e72dfe6e24e61b3463d9e05f37a5e3b28`

### #219 mem-ruby: fix CHI sending the wrong snoop response

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/219
- 代表 commit: `68a48a2dfa3f` (2023-08-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 68a48a2dfa3f8772d2f65c6630a64e88130a5628`

### #211 util: Update & fix bug in m5stats2streamline.py

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/211
- 代表 commit: `815d5b1cbaab` (2023-08-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 815d5b1cbaab5f548b42e7b106bafaca97a7174a`

### #204 resources, stdlib: Add support for local files in obtain_resource

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/204
- 代表 commit: `c156df620de5` (2023-08-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c156df620de59b1379f2ac6e64ec25cfdd7d1a62`

### #236 util-docker: Add GitHub Action to create Docker Images

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/236
- 代表 commit: `fceb7e05a338` (2023-08-30)
- 变更规模: commits=2, files=5, +112/-32 (churn=144)
- 影响范围: topdirs=util, .github; subsys=util, .github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/docker-bake.hcl` (churn=83)
  - `.github/workflows/docker-build.yaml` (churn=55)
  - `util/dockerfiles/ubuntu-20.04_all-dependencies/Dockerfile` (churn=2)
  - `util/dockerfiles/ubuntu-20.04_clang-version/Dockerfile` (churn=2)
  - `util/dockerfiles/ubuntu-22.04_all-dependencies/Dockerfile` (churn=2)
- commits 列表（按 topo-order，Top 12）：
  - 2023-08-30 `fceb7e05a338` util-docker: Add GitHub Action to create Docker Images
  - 2023-09-05 `1b0bb678ab8d` util-docker: Proof-of-concept using Docker buildx
- 复现: `git show fceb7e05a3387fdeffc3f812eb4b24a581d81e0e`

### #200 mem: Atomic ops to same address

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/200
- 代表 commit: `0e323bc40953` (2023-08-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0e323bc40953670430cc9185d1a51625ee4b43ee`

### #243 misc: Copy .github directory from develop to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/243
- 代表 commit: `48a40cf2f518` (2023-08-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 48a40cf2f5182a82de360b7efa497d82e06b1631`

### #247 gpu-compute: Set LDS/scratch aperture base register

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/247
- 代表 commit: `ddd1bc1e4882` (2023-08-31)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ddd1bc1e4882877bc6891125369ac2bec6d9e136`

### #245 misc: Remove 'run-name' from compiler-tests.yaml

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/245
- 代表 commit: `4de4e2255306` (2023-08-31)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4de4e2255306e1b784080854cb9a2150c8c09a65`

### #251 arch-x86: Fix wrong x86 assembly

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/251
- 代表 commit: `8d47cda8b674` (2023-09-01)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 8d47cda8b674ff28da9eab4bf3b183fe88ae52cb`

### #248 util: Add gdb to gcn-gpu Dockerfile

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/248
- 代表 commit: `c0db065c26fe` (2023-09-01)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c0db065c26fe0af22fd364523ed244e551909239`

### #255 mem-ruby: Reorder SLC atomic and response actions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/255
- 代表 commit: `2eeecc532a60` (2023-09-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2eeecc532a604ef7a63b8b3310e525e61d5171ea`

### #268 misc: Improve ".github/ISSUE_TEMPLATE/bug_report.md"

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/268
- 代表 commit: `d10d752d7eb7` (2023-09-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d10d752d7eb7612af340c0456ceb9b0b3ab17a05`

### #275 misc: Fix CI GitHub Action to stop if Workflow re-triggered

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/275
- 代表 commit: `5d98d18fb6f2` (2023-09-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5d98d18fb6f2d34e17bb5f6453c1468a4d32e4e2`

### #270 misc: Fix buggy special path comparisons

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/270
- 代表 commit: `cc757cfe7ad6` (2023-09-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show cc757cfe7ad6daaacd777f170801e01c0243a806`

### #267 ext: Stop excluding 'ext/testlib' from pre-commit and format

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/267
- 代表 commit: `e80cde07139d` (2023-09-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e80cde07139d1390bf561562a0bd7dc488d77ae3`

### #279 sim: add bypass_on_change to the set() of a signal

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/279
- 代表 commit: `1fa1575f58f9` (2023-09-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1fa1575f58f993fbe16f9a1efcd4a169d39d7453`

### #273 util-docker: Proof-of-concept using Docker buildx

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/273
- 代表 commit: `84e0224e85cd` (2023-09-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 84e0224e85cd08d667cadf10180425ecf82086cb`

### #271 util: Add docker prune cron to GitHub runners

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/271
- 代表 commit: `9cdd6093bdb1` (2023-09-07)
- 变更规模: commits=2, files=1, +0/-4 (churn=4)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/github-runners-vagrant/provision_nonroot.sh` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2023-09-07 `9cdd6093bdb1` util: Add docker prune cron to GitHub runners
  - 2023-09-10 `7091a8b7a0b1` util: Revert "Add docker prune cron to GitHub..."
- 复现: `git show 9cdd6093bdb128432e7ff6dac0c9029d53845e9e`

### #212 resources,stdlib: Add workload to resource specialization and deprecate workload.py

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/212
- 代表 commit: `eb5ae353411d` (2023-09-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show eb5ae353411d5f40177373845464988394e61035`

### #233 misc: Add test status badges to README.md

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/233
- 代表 commit: `aca67fe3a326` (2023-09-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show aca67fe3a32640596fdb2138f17e2f007e19d816`

### #277 sim-se: Fix crash in chdirFunc() on nonexistent directory

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/277
- 代表 commit: `ce27f5c07afd` (2023-09-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ce27f5c07afdd80c9532a4038b8e595918a84d4e`

### #244 scons: Add an option specifying the path to mold linker binary

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/244
- 代表 commit: `d5f5211b91b7` (2023-09-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d5f5211b91b7ce68c4442d974e05103620fd5a30`

### #221 redirect_path patch for restoring cpt

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/221
- 代表 commit: `ebde1133c005` (2023-09-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ebde1133c0057ca12cb6d58695df64071e4e4915`

### #299 util: Revert "Add docker prune cron to GitHub..."

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/299
- 代表 commit: `a89aeb39067a` (2023-09-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a89aeb39067a233f81dabc2bef19ae6607a6b5a3`

### #290 util: Update gcn-gpu Dockerfile

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/290
- 代表 commit: `a217c218e0e4` (2023-09-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a217c218e0e4e82a62ee1de7d3df4d8ac8fe7fc8`

### #228 arch-riscv: Enable RVV run in Minor and O3 CPU

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/228
- 代表 commit: `5fefbe29336b` (2023-09-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5fefbe29336b351e6e6244c2cf01442c8d69a29f`

### #298 cpu-kvm: properly set x86 xsave header on gem5->KVM transition

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/298
- 代表 commit: `d67a6603c1fb` (2023-09-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d67a6603c1fb4ba976265b7aa87bdf7e4047ea7d`

### #286 sim-se: Fix tgkill logic bug in handling signal argument

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/286
- 代表 commit: `94e5a0cccf56` (2023-09-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 94e5a0cccf564780fb2f904399d65a762aaa0c43`

### #283 sim-se: Use tgt_stat64 instead of tgt_stat in newfstatatFunc

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/283
- 代表 commit: `1bebf6a3ccf5` (2023-09-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1bebf6a3ccf5bbdfab3a979381848d211a1afb4a`

### #302 cpu, configs: Fix TraceCPU after multi-ISA addition

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/302
- 代表 commit: `5fd901ffbb42` (2023-09-12)
- 变更规模: commits=2, files=2, +14/-16 (churn=30)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/tlm/examples/tlm_elastic_slave_with_l2.py` (churn=17)
  - `util/tlm/conf/tlm_elastic_slave.py` (churn=13)
- commits 列表（按 topo-order，Top 12）：
  - 2023-09-12 `5fd901ffbb42` cpu, configs: Fix TraceCPU after multi-ISA addition
  - 2023-09-13 `f95e1505b839` util: Fix TLM configs making use of TraceCPU replayer
- 复现: `git show 5fd901ffbb42bbb89e636e56caf5d3901e2376d3`

### #310 util: Fix TLM configs making use of TraceCPU replayer

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/310
- 代表 commit: `23c1014677fb` (2023-09-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 23c1014677fbf344758645c0216b4e41d345f9b1`

### #304 arch-x86: initialize and correct bitwidth for FPU tag word

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/304
- 代表 commit: `673d4b2ac2b8` (2023-09-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 673d4b2ac2b86f67a91467ddfc2847e37bc2b295`

### #294 mem-ruby: This commit patches an error in AbstractController.cc

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/294
- 代表 commit: `d38c02919514` (2023-09-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d38c029195140740b581f6c826d7fa7b0a6f6cdb`

### #285 misc,util-docker: Fix docker-build.yaml

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/285
- 代表 commit: `b53a31136348` (2023-09-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b53a31136348de12eb029ec0045b88ba465ff4ff`

### #313 scons: Revert "Add an option specifying the path to mold linker binary"

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/313
- 代表 commit: `1d160e6ab066` (2023-09-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1d160e6ab06654c5ee44894d1ef52fa5758464c0`

### #319 misc: Fix docker build workflow

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/319
- 代表 commit: `61339b64710e` (2023-09-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 61339b64710eb8170424b36a162f6a7a4917234c`

### #320 misc: Use 'workdir' for docker-build.yaml

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/320
- 代表 commit: `7a17c780bd0b` (2023-09-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7a17c780bd0b306e5eee02675f4ebed9239cc7a6`

### #314 configs: 'memoy' -> 'memory' spelling mistake fix

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/314
- 代表 commit: `26a1ee4e61de` (2023-09-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 26a1ee4e61de85189b261fca1d595315a3f33721`

### #274 mem-cache: Fix bug in classic cache while clflush

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/274
- 代表 commit: `59a96c8c2fc6` (2023-09-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 59a96c8c2fc64a6bdf89d7f18d872b585c649100`

### #322 misc: Update docker-build.yaml artifact actions to v3

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/322
- 代表 commit: `1c5870d775ac` (2023-09-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1c5870d775ac96074d968bcf76308ed9c91fd6a7`

### #318 misc,tests: Remove duplicate running of daily `gem5_library_tests`

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/318
- 代表 commit: `017fb51fadbe` (2023-09-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 017fb51fadbeb0d5556c82054615ad8388a31f16`

### #321 misc,tests: Use GitHub Docker registry for 22.04 all-deps

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/321
- 代表 commit: `46be2d233933` (2023-09-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 46be2d233933a2f34b80a79e28085098ed4ade62`

### #317 util,resources,stdlib: Add 'obtain-resource.py' utility to easily obtain resources from the CLI

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/317
- 代表 commit: `23442727f701` (2023-09-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 23442727f701353b30606232f0f7a309dc5e5510`

### #316 mem-ruby: patch fixes a protocol error in MOESI_CMP_Directory

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/316
- 代表 commit: `3bdcfd6f7abd` (2023-09-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3bdcfd6f7abdf0c9125ec37018df76a29998a055`

### #258 misc: Add HACC GPU tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/258
- 代表 commit: `6eb7c10eb934` (2023-09-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6eb7c10eb9349ea6551a84e2cd8970231b85dd11`

### #332 arch-x86: fix negative overflow check bug in PACK micro-op

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/332
- 代表 commit: `4526a314a989` (2023-09-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4526a314a9894af3247307ee0d45aac2a128f0fe`

### #328 dev-amdgpu: Handle GPU atomics on host memory addresses

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/328
- 代表 commit: `aa0702c6eb1a` (2023-09-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show aa0702c6eb1a0585b0a951c790334fc97a8b502b`

### #325 arch-riscv: Fix inst flags for jal and jalr

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/325
- 代表 commit: `958eda6961bc` (2023-09-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 958eda6961bc4e2b3b9bb52089c491eaf59353d6`

### #307 python,util: Add Python MyPy Stubgen to enable Pylance IntelliSense

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/307
- 代表 commit: `3f9afe96c6e3` (2023-09-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3f9afe96c6e3b1c014ae8369fced500014d97e82`

### #337 configs: Fixed Typo

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/337
- 代表 commit: `f5a255c68d1c` (2023-09-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f5a255c68d1ce0318da0d46f744ee7513e0acda9`

### #351 arch: Enable customized decoder class name

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/351
- 代表 commit: `83224e2c851d` (2023-09-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 83224e2c851d6d23921b209c3b4c84b96d4a173e`

### #348 cpu: Add override to TraceCPU init function

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/348
- 代表 commit: `9d63a1492ada` (2023-09-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 9d63a1492adaee73ec81ed0d6fc540295bd1af63`

### #350 arch-riscv: Make RISC-V decodeInst overridable

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/350
- 代表 commit: `010ac43369ab` (2023-09-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 010ac43369abafd1dd2d68e08de772570b9399bb`

### #288 mem-ruby: start using txnid and DBID identifiers in CHI transactions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/288
- 代表 commit: `f5968da41c24` (2023-09-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f5968da41c248f8be7c748d5adc00f92a3ca48bb`

### #356 sim: Probe listener template with lambda

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/356
- 代表 commit: `cfa13f9feb90` (2023-09-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show cfa13f9feb905cfd9853397dfc23239e1121e175`

### #345 arch-x86: make popx87 micro-op actually pop st(0)

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/345
- 代表 commit: `4638434b9712` (2023-09-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4638434b97128f595c5b37535c647f024ce6224d`

### #347 arch-x86: properly initialize the auxv platform string

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/347
- 代表 commit: `49a1d4826403` (2023-09-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 49a1d4826403cd88fa96c5ce642581199823c0e6`

### #360 cpu-o3: Mark getWritableRegOperand() in O3CPU as a regwrite

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/360
- 代表 commit: `3a0f4598b934` (2023-09-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3a0f4598b93452e29834576707e555ff79a1836e`

### #263 misc,ext,tests: Automatically split CI TestLib tests across GitHub Action jobs

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/263
- 代表 commit: `074fa4c604f0` (2023-09-27)
- 变更规模: commits=2, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gem5_library/checkpoints/simpoints-se-checkpoint.py` (churn=1)
- commits 列表（按 topo-order，Top 12）：
  - 2023-09-27 `074fa4c604f0` misc,ext,tests: Automatically split CI TestLib tests across GitHub Action jobs
  - 2023-10-09 `1fe0056d3b9b` configs,tests: Remove `mkdir` in simpoint-se-checkpoint.py
- 复现: `git show 074fa4c604f0f15324dcad6acafc14b59beb191e`

### #361 base: Add a warning when failing to insert a whole symbol table

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/361
- 代表 commit: `14b928f77ca4` (2023-09-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 14b928f77ca40b643a1b86d0abe27d4a8c96fffb`

### #323 stdlib, resources: Added pretty printing resource

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/323
- 代表 commit: `5d254ffb02aa` (2023-09-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5d254ffb02aa81eae1d720cb336dda017d4af12b`

### #250 misc: 'sim{out/err}' -> 'sim{out/err}.txt'

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/250
- 代表 commit: `62d34ef37460` (2023-09-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 62d34ef37460e9e8cbbbaeb50605572a861565b9`

### #370 arch-riscv: Update FS bits when doing floating point loads

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/370
- 代表 commit: `3a35bdf57a76` (2023-09-29)
- 变更规模: commits=2, files=1, +14/-4 (churn=18)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=18)
- commits 列表（按 topo-order，Top 12）：
  - 2023-09-29 `3a35bdf57a76` arch-riscv: Update FS bits when doing floating point loads
  - 2023-10-01 `da72590c1961` arch-riscv: FS bits -> DIRTY for more floating point loads
- 复现: `git show 3a35bdf57a76fe4ffe06ef4d7a052da6560764c1`

### #363 misc: fix g++13 overloaded-virtual warning

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/363
- 代表 commit: `2b791ff556c1` (2023-09-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2b791ff556c1c1bbe9d1996189ae533aa1b4798e`

### #265 mem: fix bug in 3-level cache

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/265
- 代表 commit: `f9781af6e5be` (2023-09-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f9781af6e5be204dfa2c21c079e5f0613b1b249f`

### #369 python: Add importer to standalone gem5py_m5

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/369
- 代表 commit: `7301d4bd1936` (2023-10-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7301d4bd193691ee7bf101c6400532303db1da0d`

### #381 arch-riscv: FS bits -> DIRTY for more floating point loads

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/381
- 代表 commit: `57e0c7d0064e` (2023-10-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 57e0c7d0064e19ce970dd512f7bb5f6926377c0b`

### #357 arch: Add instruction size and PC set methods

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/357
- 代表 commit: `7806eaad5118` (2023-10-04)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7806eaad51187bcbf6c2b4667b784ffdae36a73d`

### #365 misc: Update gem5 to use clang-15 and clang-16

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/365
- 代表 commit: `6f5d877b1aac` (2023-10-04)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6f5d877b1aacd551749dafa87da26600a4f01155`

### #399 arch-riscv: Implement Zcb instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/399
- 代表 commit: `ee8c569513cd` (2023-10-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ee8c569513cd2b9f345136c4bc6405d099008804`

### #391 gpu-compute: Fix dynamic scratch size test

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/391
- 代表 commit: `f5c7ea01ef0d` (2023-10-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f5c7ea01ef0ddda615aca3602349d01a7b543dac`

### #334 arch-arm: Implement FEAT_FGT

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/334
- 代表 commit: `761f6b73a009` (2023-10-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 761f6b73a009a0c7524cb3eb982eb37dc17c328f`

### #191 resources, stdlib: Adding 'suite' category to gem5

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/191
- 代表 commit: `4db748a50768` (2023-10-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4db748a50768643c46618cc0c51a9743d9b60ca5`

### #389 configs: Add configurable GPU L1,L2 num banks and L2 latencies

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/389
- 代表 commit: `85340973bf9a` (2023-10-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 85340973bf9a1e1df380106fa2f9be836e619882`

### #177 mem-ruby: Add new feature far atomics in CHI

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/177
- 代表 commit: `ae104cc431c0` (2023-10-06)
- 变更规模: commits=2, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` (churn=3)
- commits 列表（按 topo-order，Top 12）：
  - 2023-10-06 `ae104cc431c0` mem-ruby: Add new feature far atomics in CHI
  - 2023-10-29 `1b05c0050bde` mem-ruby: Clear the atomic log from the DataBlock in CHI
- 复现: `git show ae104cc431c065fbf53ab003cbf4aba23de12ac1`

### #371 util: Update the GitHub Self-Hosted Runners

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/371
- 代表 commit: `b0e1efb555d7` (2023-10-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b0e1efb555d70f4bb6781857df129978e2d45235`

### #407 mem-ruby: Far atomics fix

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/407
- 代表 commit: `226052ed5a31` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 226052ed5a318bc153916a057cd5637fccd037d2`

### #343 sim-se: zero out memory allocated via brk()

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/343
- 代表 commit: `5cd70bf9bf46` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5cd70bf9bf461029152d7272b7891f2eed9a972d`

### #411 cpu-kvm, arch-x86: flush TLB after syscalls

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/411
- 代表 commit: `d4be9c76c52c` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d4be9c76c52c3840ee83d1a9513f57f6626ba19c`

### #414 arch-arm: Implement FEAT_TLBIRANGE extension

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/414
- 代表 commit: `ec7921305b21` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ec7921305b2126302fcf1535e2aac357e341e577`

### #412 cpu: Restructure BTB

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/412
- 代表 commit: `d8fc0180a53c` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d8fc0180a53cfd3d39a93177834843d817bb094e`

### #402 stdlib: Del comment stating SE mode limited to single thread

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/402
- 代表 commit: `79f40ffdabc2` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 79f40ffdabc2c549f07a2e06a40c9510a7b60464`

### #151 New function to kernel_disk_workload to allow new disk device location

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/151
- 代表 commit: `452a600c495f` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 452a600c495f079f2ab4622d20854a35e52fc973`

### #400 tests,misc: Fix compilation tests failures

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/400
- 代表 commit: `bbe05b0cba0e` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bbe05b0cba0e3f58251cdaa37eba002ab2fb02bb`

### #415 configs: Add an example elastic trace generation script

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/415
- 代表 commit: `21c5d7700072` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 21c5d7700072294fcad7bd98ac86744db409c828`

### #410 dev-amdgpu,gpu-compute: Implement GPU and HSA timestamps

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/410
- 代表 commit: `93704a81f122` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 93704a81f122498e9400921fe1c22209f110bc57`

### #423 misc,python: Add yaml formatter to pre commit

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/423
- 代表 commit: `c5f06265bbb5` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c5f06265bbb554f9ec435b028c3e1b1a1a5447ae`

### #425 configs,tests: Remove `mkdir` in simpoint-se-checkpoint.py

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/425
- 代表 commit: `486916b5d433` (2023-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 486916b5d4339ea68141b11bafdb167176cf2fb4`

### #430 arch-arm: Make interrupt masking handle VHE/SEL2 cases

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/430
- 代表 commit: `d9fe0cfe1cfb` (2023-10-10)
- 变更规模: commits=2, files=2, +83/-0 (churn=83)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/interrupts.cc` (churn=81)
  - `src/arch/arm/interrupts.hh` (churn=2)
- commits 列表（按 topo-order，Top 12）：
  - 2023-10-10 `d9fe0cfe1cfb` arch-arm: Make interrupt masking handle VHE/SEL2 cases
  - 2023-11-21 `b8fabc15d99d` arch-arm: Revamp takeVirtualInt to take FEAT_SEL2 into account
- 复现: `git show d9fe0cfe1cfba5c5ad3b11238e0a8b562e757973`

### #377 dev-amdgpu,mem-ruby: Add support to checkpoint and restore between kernels in GPUFS

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/377
- 代表 commit: `ec633b3d68c2` (2023-10-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ec633b3d68c2264980ecbcee17bbbbb3e00b8e70`

### #341 arch,arch-riscv: Remove setRegOperand in VecRegOperand

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/341
- 代表 commit: `141b06d335ad` (2023-10-10)
- 变更规模: commits=2, files=3, +2/-4 (churn=6)
- 影响范围: topdirs=src; subsys=arch, cpu/o3; arch=isa_parser, riscv
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/cpu.cc` (churn=4)
  - `src/arch/isa_parser/operand_types.py` (churn=1)
  - `src/arch/riscv/insts/vector.hh` (churn=1)
- commits 列表（按 topo-order，Top 12）：
  - 2023-09-25 `b759f22cc946` cpu-o3: Mark getWritableRegOperand() in O3CPU as a regwrite
  - 2023-10-10 `141b06d335ad` arch,arch-riscv: Remove setRegOperand in VecRegOperand
- 复现: `git show 141b06d335adce2ea6594a0059b020164ea0e148`

### #427 stdlib: Fix use internal _hashlib in md5_utils.py

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/427
- 代表 commit: `0ec1fb167bfc` (2023-10-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0ec1fb167bfc2dcbcce98cfab72ad8d23335caf3`

### #417 tests: Update test workflows for new runners

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/417
- 代表 commit: `58140bba1f90` (2023-10-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 58140bba1f902a751d59fae6f2007c4d18b82f23`

### #422 misc,python: Add `requirements-txt-fixer` to pre-commit

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/422
- 代表 commit: `25b2786db899` (2023-10-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 25b2786db8996f1842b98350e2bad87a59d4a5ae`

### #401 Learning-gem5: fix formatting

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/401
- 代表 commit: `ad2fe4268677` (2023-10-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ad2fe4268677be2588bada5f51afeec28c266459`

### #404 stdlib: Improve handing of errors in Atlas request failures

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/404
- 代表 commit: `d559c24ac27b` (2023-10-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d559c24ac27b891825b4629de97c5e6b4d9782bd`

### #419 misc: Run `pre-commit autoupdate`

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/419
- 代表 commit: `3f5d7d647a30` (2023-10-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3f5d7d647a30ac9c0754c54af4afba421b9b99ae`

### #416 arch-arm: Implement FEAT_TCR2 and FEAT_SCTLR2

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/416
- 代表 commit: `891250192d4a` (2023-10-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 891250192d4a0b3177ed729067ee538319b5a8fe`

### #408 gpu-compute: Update tokens for flat global/scratch

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/408
- 代表 commit: `da11427ba681` (2023-10-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show da11427ba681de06684c156221d9672b17bf80c1`

### #424 misc,python: Add `pyupgrade` to pre-commit

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/424
- 代表 commit: `70b6b53e54fa` (2023-10-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 70b6b53e54fae8a60004f94c4d6ae54bf8b5651d`

### #396 configs,ext: Updated the gem5 SST Bridge to use SST 13.0.0

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/396
- 代表 commit: `c855dbf7c5e7` (2023-10-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c855dbf7c5e70983737398b85d43b4205e72f4e1`

### #433 configs: GPUFS option to disable KVM perf counters

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/433
- 代表 commit: `f7ad8fe4350b` (2023-10-11)
- 变更规模: commits=1, files=2, +13/-0 (churn=13)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gpufs/runfs.py` (churn=7)
  - `configs/example/gpufs/system/system.py` (churn=6)
- 复现: `git show f7ad8fe4350be7d4299b22d9030385e3f261c7d8`

### #438 arch-vega: Ignore s_setprio instruction instead of panic

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/438
- 代表 commit: `7bae5464dc6e` (2023-10-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7bae5464dc6e7d91abd58ef873f0b8be26ef2d24`

### #439 arch-vega: Implement buffer_atomic_cmpswap

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/439
- 代表 commit: `4d336c0636ac` (2023-10-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4d336c0636acd301a7919c9142f4e7d478c45e66`

### #444 misc,tests: Add dummy jobs to workflows for status checks

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/444
- 代表 commit: `3455d9e68da4` (2023-10-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3455d9e68da423334107110dc60129bc33c4cde6`

### #429 cpu: Refactor indirect predictor

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/429
- 代表 commit: `59f96deb0fff` (2023-10-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 59f96deb0fff522134f1e73753a7e526e7fe5c7a`

### #441 tests: updated the nightly tests to use SST 13.0.0

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/441
- 代表 commit: `68af3f45c945` (2023-10-13)
- 变更规模: commits=1, files=4, +13/-9 (churn=22)
- 影响范围: topdirs=util, .github, tests; subsys=util, .github, tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/{sst-11.1.0 => sst}/Dockerfile` (churn=14)
  - `.github/workflows/daily-tests.yaml` (churn=3)
  - `tests/deprecated/nightly.sh` (churn=3)
  - `util/dockerfiles/docker-compose.yaml` (churn=2)
- 复现: `git show 68af3f45c94546ca9e2886633bd10c1ae7b64d03`

### #448 mem-ruby: Update cache recorder to use RubyPort and remove BUILD_GPU guards

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/448
- 代表 commit: `7706e958e59b` (2023-10-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7706e958e59b322ffeaf51818adf613bd1189edd`

### #443 arch-riscv: Fix write back register issue of vmask_mv_micro

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/443
- 代表 commit: `a3c51ca38cdc` (2023-10-13)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/insts/vector.hh` (churn=2)
- 复现: `git show a3c51ca38cdccfde04d6a8771da3b267c7a2f113`

### #367 mem-ruby: Always pass on GPU atomics to dir in write-through TCC

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/367
- 代表 commit: `4931fb001082` (2023-10-14)
- 变更规模: commits=1, files=3, +24/-18 (churn=42)
- 影响范围: topdirs=src, configs; subsys=mem, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` (churn=32)
  - `src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm` (churn=9)
  - `configs/ruby/GPU_VIPER.py` (churn=1)
- 复现: `git show 4931fb0010825ca5fe979226e2fcd293423e143a`

### #457 configs: Fix missing param exchange for GPUFS

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/457
- 代表 commit: `ca2592d3ba6b` (2023-10-14)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/ruby/GPU_VIPER.py` (churn=1)
- 复现: `git show ca2592d3ba6b5344de0502865cf20ad233cd0056`

### #453 python: Enable -m switch on gem5 binary

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/453
- 代表 commit: `20f5555f30e1` (2023-10-14)
- 变更规模: commits=1, files=2, +59/-37 (churn=96)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/main.py` (churn=93)
  - `src/python/importer.py` (churn=3)
- 复现: `git show 20f5555f30e13ae0931464f36f32c65336711569`

### #458 misc: Copy .github directory from develop to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/458
- 代表 commit: `3157cde32449` (2023-10-14)
- 变更规模: commits=1, files=8, +708/-530 (churn=1238)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=491)
  - `.github/workflows/ci-tests.yaml` (churn=272)
  - `.github/workflows/weekly-tests.yaml` (churn=186)
  - `.github/workflows/compiler-tests.yaml` (churn=92)
  - `.github/workflows/docker-build.yaml` (churn=86)
  - `.github/workflows/gpu-tests.yaml` (churn=61)
  - `.github/workflows/utils.yaml` (churn=26)
  - `.github/ISSUE_TEMPLATE/bug_report.md` (churn=24)
- 复现: `git show 3157cde32449fea7b0a0ad5e8241481bc6ee76c3`

### #364 arch-arm: Remove Jazelle state + ThumbEE support

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/364
- 代表 commit: `2e85c95f4bfb` (2023-10-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2e85c95f4bfb1b1f12bbdcbc718241f88ef266bb`

### #454 misc: fix clang13 overloaded-virtual warning

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/454
- 代表 commit: `d702d3b90abf` (2023-10-16)
- 变更规模: commits=1, files=2, +5/-5 (churn=10)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/ext/core/sc_port.hh` (churn=5)
  - `src/systemc/ext/tlm_core/2/sockets/initiator_socket.hh` (churn=5)
- 复现: `git show d702d3b90abf6a7fb521d87028970ae25b0a05d7`

### #329 cpu: Explicitly define cache_line_size -> 64-bit unsigned int

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/329
- 代表 commit: `d42eeb6b6821` (2023-10-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d42eeb6b682191c0e8385b617429a5a9fcb47a4b`

### #465 cpu, arch-arm: Add IsPseudo tag for gem5 pseudo instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/465
- 代表 commit: `f9cf8bf8a2c5` (2023-10-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f9cf8bf8a2c5f570f086222ad82ed24038111757`

### #459 arch-arm: Fix line-length error in misc.cc

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/459
- 代表 commit: `97f4b44dd394` (2023-10-16)
- 变更规模: commits=2, files=1, +25/-7 (churn=32)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/regs/misc.cc` (churn=32)
- commits 列表（按 topo-order，Top 12）：
  - 2023-10-16 `97f4b44dd394` arch-arm: Fix line-length error in misc.cc
  - 2023-10-16 `322b105b9d7c` arch-arm: Fix (another) line-length error in misc.cc
- 复现: `git show 97f4b44dd394674d2221c1979a0dc8db335362ba`

### #460 util: Fix runners to extent to max disk size

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/460
- 代表 commit: `5240c07d3c83` (2023-10-16)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/github-runners-vagrant/provision_root.sh` (churn=1)
- 复现: `git show 5240c07d3c8302714e70299cd4878a2139082aa4`

### #466 stdlib,resources: Generalize exception for request retry

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/466
- 代表 commit: `a9464a41f571` (2023-10-16)
- 变更规模: commits=1, files=1, +1/-2 (churn=3)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/client_api/atlasclient.py` (churn=3)
- 复现: `git show a9464a41f571b9d54872fa1da4487dc0ee6b9536`

### #463 arch-riscv: Mark vector configuration insts as vector insts

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/463
- 代表 commit: `9b2b6cd8d25b` (2023-10-16)
- 变更规模: commits=1, files=1, +3/-1 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/insts/vector.hh` (churn=4)
- 复现: `git show 9b2b6cd8d25b055c4b64f044e7d0c7dab76e2595`

### #462 misc: Add missing RISCV valid ISA option to README.md

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/462
- 代表 commit: `2825bc1d552e` (2023-10-16)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=README.md; subsys=README.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `README.md` (churn=4)
- 复现: `git show 2825bc1d552e71b4f9816ad186be394b21991f06`

### #376 arch-riscv: Change to VS bits to DIRTY for rvv insts changing vregs

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/376
- 代表 commit: `d048ad34d6d3` (2023-10-16)
- 变更规模: commits=1, files=2, +128/-0 (churn=128)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=93)
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=35)
- 复现: `git show d048ad34d6d316c065f2d7f072fa0457de4da309`

### #468 arch-arm: Fix (other) line-length errors

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/468
- 代表 commit: `adb54709969a` (2023-10-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show adb54709969a06c0ed390cf0013aa86dd27785cf`

### #336 dockerfiles: multi-platform setup

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/336
- 代表 commit: `df471092d94c` (2023-10-16)
- 变更规模: commits=1, files=11, +12/-12 (churn=24)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/gem5-all-min-dependencies/Dockerfile` (churn=4)
  - `util/dockerfiles/gcn-gpu/Dockerfile` (churn=2)
  - `util/dockerfiles/gpu-fs/Dockerfile` (churn=2)
  - `util/dockerfiles/llvm-gnu-cross-compiler-riscv64/Dockerfile` (churn=2)
  - `util/dockerfiles/sst/Dockerfile` (churn=2)
  - `util/dockerfiles/systemc-2.3.3/Dockerfile` (churn=2)
  - `util/dockerfiles/ubuntu-20.04_gcc-version/Dockerfile` (churn=2)
  - `util/dockerfiles/ubuntu-22.04_clang-16/Dockerfile` (churn=2)
- 复现: `git show df471092d94cec540525a4d9c529eaed046b2718`

### #428 cpu: Restructure RAS

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/428
- 代表 commit: `42d1c8b3c398` (2023-10-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 42d1c8b3c3983f251e8034309f3c166689cd11e2`

### #470 util: Improve GitHub Action runners: Enable KVM; Better Cleanup; Better Tooling

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/470
- 代表 commit: `e9fe9cb00119` (2023-10-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e9fe9cb001196a8323bc97999b8f566095e06f3f`

### #390 arch-riscv: Add bootloader+kernel workload

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/390
- 代表 commit: `334df18dce98` (2023-10-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 334df18dce9857fb33b96d991dee5d12001450e4`

### #477 tests: Changed percent atomics to 0 in memtest to fix daily test

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/477
- 代表 commit: `7bd0b99635fe` (2023-10-18)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/testers/memtest/MemTest.py` (churn=2)
- 复现: `git show 7bd0b99635fe34fc7c9c2b67397b9978ac0eda10`

### #479 arch-riscv: Copy Misc Regs when swiching cpus

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/479
- 代表 commit: `c3acfdc9b89b` (2023-10-18)
- 变更规模: commits=1, files=1, +4/-0 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.cc` (churn=4)
- 复现: `git show c3acfdc9b89b60cfd6a426af24d25c5cf58ef2ae`

### #420 misc: Add additional `pre-commit` hook checks

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/420
- 代表 commit: `be89758f0ef6` (2023-10-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show be89758f0ef64ab3e20379b06fa2100ae898e4dd`

### #418 docker-images: Use GitHub Container Registry

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/418
- 代表 commit: `62e51987964b` (2023-10-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 62e51987964b335757cc4f5af2fcf0161b07a599`

### #256 misc: Add LULESH GPU tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/256
- 代表 commit: `34314b3f929f` (2023-10-18)
- 变更规模: commits=1, files=1, +37/-2 (churn=39)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/gpu-tests.yaml` (churn=39)
- 复现: `git show 34314b3f929fc0d9b199dc09b06b23b08d698d5d`

### #485 misc: Copy .github directory from develop to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/485
- 代表 commit: `1bb9bb33086a` (2023-10-18)
- 变更规模: commits=1, files=4, +51/-16 (churn=67)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/gpu-tests.yaml` (churn=43)
  - `.github/workflows/daily-tests.yaml` (churn=14)
  - `.github/workflows/weekly-tests.yaml` (churn=6)
  - `.github/workflows/compiler-tests.yaml` (churn=4)
- 复现: `git show 1bb9bb33086aec5f39fdc0148552d01e29326cb0`

### #171 arch-riscv: Add dynamic VLEN and ELEN configuration support to RVV path

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/171
- 代表 commit: `73c48a482831` (2023-10-19)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 73c48a482831ac87b176751dd713d02d378ea7be`

### #489 mem,tests: Set Ruby Mem Test atomic percent to 0

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/489
- 代表 commit: `531067fffa26` (2023-10-19)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/ruby_mem_test.py` (churn=2)
- 复现: `git show 531067fffa26ab6722f7de4b68a5896372b99afe`

### #490 scons: Explicit some config options HAVE_* to boolean type

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/490
- 代表 commit: `b13102fcc452` (2023-10-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b13102fcc45201a9e217b01c523b009ac784b259`

### #488 misc: Fix weekly-tests.yaml container uris

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/488
- 代表 commit: `cb56c67a8bc8` (2023-10-20)
- 变更规模: commits=1, files=2, +2/-2 (churn=4)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/gpu-tests.yaml` (churn=2)
  - `.github/workflows/weekly-tests.yaml` (churn=2)
- 复现: `git show cb56c67a8bc87b0fbc347662443b8b140b296184`

### #494 misc: Integrate a Capstone Disassembler in gem5

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/494
- 代表 commit: `8b78e87f1bfb` (2023-10-20)
- 变更规模: commits=2, files=2, +30/-1 (churn=31)
- 影响范围: topdirs=src; subsys=src, cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/Kconfig` (churn=29)
  - `src/Kconfig` (churn=2)
- commits 列表（按 topo-order，Top 12）：
  - 2023-10-20 `8b78e87f1bfb` misc: Integrate a Capstone Disassembler in gem5
  - 2023-11-23 `4d632cb73fba` scons: Add new config option HAVE_CAPSTONE to Kconfig
- 复现: `git show 8b78e87f1bfb08ab952fde5161ec9e2397f86014`

### #486 arch-arm: Fix KVM Failed to set register (0x603000000013808c)

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/486
- 代表 commit: `6ddf8c94ee13` (2023-10-20)
- 变更规模: commits=1, files=1, +13/-13 (churn=26)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/regs/misc.cc` (churn=26)
- 复现: `git show 6ddf8c94ee1368f8a573fc7f782dcf6a8534293b`

### #495 misc: Merge develop .github dir to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/495
- 代表 commit: `e9da8d67bdbb` (2023-10-20)
- 变更规模: commits=1, files=2, +2/-2 (churn=4)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/gpu-tests.yaml` (churn=2)
  - `.github/workflows/weekly-tests.yaml` (churn=2)
- 复现: `git show e9da8d67bdbb23fbd0578a379f08f42bce50121d`

### #496 util: Add 'sudo' to rm WORK_DIR command

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/496
- 代表 commit: `b670ed9fba7c` (2023-10-24)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/github-runners-vagrant/action-run.sh` (churn=2)
- 复现: `git show b670ed9fba7c99092830e59e573d5a33e27dde00`

### #497 misc: Add GitHub Runner API rate limiting

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/497
- 代表 commit: `b6ce2d0db891` (2023-10-24)
- 变更规模: commits=1, files=1, +14/-0 (churn=14)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/github-runners-vagrant/action-run.sh` (churn=14)
- 复现: `git show b6ce2d0db891a2dee0bace10222dec06cb4c175b`

### #455 cpu: Branch Predictor Refactoring

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/455
- 代表 commit: `60290c7c2f3d` (2023-10-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 60290c7c2f3d14c416228d3f2959a8386c1a6480`

### #475 misc: Fix spelling error in MAINTAINERS.yaml

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/475
- 代表 commit: `ecc248c3c18c` (2023-10-26)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=MAINTAINERS.yaml; subsys=MAINTAINERS.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `MAINTAINERS.yaml` (churn=4)
- 复现: `git show ecc248c3c18c0144fb8976a589f9a27f0d26cfc0`

### #500 arch-riscv: Move RVV implementation from header to source

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/500
- 代表 commit: `06bf783a8533` (2023-10-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 06bf783a8533fb1f78c041d2d49492b53eed26b4`

### #515 arch-arm: Set UNCACHEABLE flag in Request in SE mode

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/515
- 代表 commit: `d131ff488e4b` (2023-10-30)
- 变更规模: commits=1, files=1, +9/-6 (churn=15)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/mmu.cc` (churn=15)
- 复现: `git show d131ff488e4b5f88c019c3473ab8f76af41f3b2a`

### #511 arch-riscv: Correct BootloaderKernelWorkload symbol table

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/511
- 代表 commit: `0218103162b2` (2023-10-30)
- 变更规模: commits=1, files=1, +1/-2 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/linux/fs_workload.cc` (churn=3)
- 复现: `git show 0218103162b2f530ba2b50f105387b408a5fa7c7`

### #514 mem-ruby, stdlib: Far atomics fix

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/514
- 代表 commit: `3d935849005c` (2023-10-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3d935849005c7d04db060f92e7cd02423b993f27`

### #464 arch-riscv: Dynamically add V extension to device tree

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/464
- 代表 commit: `d0113185c666` (2023-10-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d0113185c6666d106086d292b28b42e987d40e15`

### #519 arch-riscv: Fix line length of CSRData declaration

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/519
- 代表 commit: `e4cdd73a595b` (2023-11-06)
- 变更规模: commits=1, files=1, +467/-235 (churn=702)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/regs/misc.hh` (churn=702)
- 复现: `git show e4cdd73a595b348a869ab5d039ba78667c88d626`

### #498 gpu-compute,dev-hsa: ROCm 5.5+ support

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/498
- 代表 commit: `71973b386ee7` (2023-11-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 71973b386ee7375137c6e27865ab07db8036f616`

### #534 Fix calculation of compressed size in bytes

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/534
- 代表 commit: `10374f2f052c` (2023-11-07)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=mem/cache; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/compressors/base.cc` (churn=2)
- 复现: `git show 10374f2f052c23e01480d9d686edf4a6e846dd67`

### #521 python: Handle unicode characters in config files

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/521
- 代表 commit: `f97adbaac7e0` (2023-11-07)
- 变更规模: commits=1, files=1, +3/-1 (churn=4)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/main.py` (churn=4)
- 复现: `git show f97adbaac7e0372083dc1037bff588b566d4771c`

### #541 arch-arm,kvm: Fix copy-paste error

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/541
- 代表 commit: `1f1e15e48f8b` (2023-11-08)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/kvm/arm_cpu.hh` (churn=2)
- 复现: `git show 1f1e15e48f8b5178a1aac0122627a241f779f875`

### #530 mem-ruby, gpu-compute: update GPU L1I$ MRU info

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/530
- 代表 commit: `86131d4323f4` (2023-11-08)
- 变更规模: commits=1, files=1, +6/-0 (churn=6)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/GPU_VIPER-SQC.sm` (churn=6)
- 复现: `git show 86131d4323f4ec70e4f60074d4e6b13c6d1f3bf1`

### #507 configs,ext: gem5 SST bridge calls m5.instantiate() in gem5

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/507
- 代表 commit: `0442c9a88c5e` (2023-11-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0442c9a88c5e0031d766470c76d1cd8138b313cb`

### #397 mem-ruby: SLICC Fixes to GLC Atomics in WB L2

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/397
- 代表 commit: `1204267fd8dc` (2023-11-09)
- 变更规模: commits=1, files=1, +82/-38 (churn=120)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` (churn=120)
- 复现: `git show 1204267fd8dc2076b4f3b6288a0040116017193d`

### #537 mem-ruby: update RubyRequest print to include GPU fields

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/537
- 代表 commit: `f61d70932113` (2023-11-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f61d70932113b9587fd4327ec93cc7e7e772f1bc`

### #517 arch-riscv: Fixing CMO instructions and allowing using CMO instructions in FS mode

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/517
- 代表 commit: `52354662aa4e` (2023-11-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 52354662aa4eed5fc794d1492c1b0705b95ef15d`

### #512 base,sim: Add the SymbolType field to the Symbol object

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/512
- 代表 commit: `b62308dfa350` (2023-11-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b62308dfa3505c95f6aa10a6185de438024c6caf`

### #538 mem-ruby, gpu-compute: fix GPU SQC/TCP Ruby formatting

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/538
- 代表 commit: `3642bc489295` (2023-11-13)
- 变更规模: commits=1, files=2, +13/-14 (churn=27)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/GPU_VIPER-TCP.sm` (churn=25)
  - `src/mem/ruby/protocol/GPU_VIPER-SQC.sm` (churn=2)
- 复现: `git show 3642bc48929597ea0b4fd02f654b91ff2538d99e`

### #561 mem-ruby: fix hex print in CacheMemory

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/561
- 代表 commit: `f31280436468` (2023-11-13)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/structures/CacheMemory.cc` (churn=2)
- 复现: `git show f3128043646859a426bf821751fbdd2bdf07069d`

### #529 gpu-compute: Fix typo with GPUTLB print

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/529
- 代表 commit: `75ca2c42829f` (2023-11-13)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/common/tlb_coalescer.cc` (churn=2)
- 复现: `git show 75ca2c42829fa5034f450df76174bfda647b07fc`

### #535 mem-ruby, gpu-compute: fix typo in GPU coalescer deadlock print

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/535
- 代表 commit: `7d0a1fb28403` (2023-11-13)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/GPUCoalescer.cc` (churn=3)
- 复现: `git show 7d0a1fb28403e64b78bc3555a412d34b8e69a81f`

### #536 mem-ruby, gpu-compute: fix formatting of TCC

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/536
- 代表 commit: `48fde5a9c64f` (2023-11-13)
- 变更规模: commits=1, files=1, +22/-25 (churn=47)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` (churn=47)
- 复现: `git show 48fde5a9c64fd5f901482dc793d9e8a903e76298`

### #556 tests,util-docker: Remove gcc 9 support

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/556
- 代表 commit: `1c7934c9d68e` (2023-11-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1c7934c9d68ebdb0d0170ba765a7b16bd2739b0b`

### #560 python,util: Fix magic number check in decode_inst_dep_trace.py

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/560
- 代表 commit: `f71450d26da6` (2023-11-14)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/decode_inst_dep_trace.py` (churn=2)
- 复现: `git show f71450d26da63b63c90ed0b9eb7c4386a9498b64`

### #552 cpu: Remove SLC bit restraint for GPU tester

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/552
- 代表 commit: `dde3d10aea9f` (2023-11-14)
- 变更规模: commits=1, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/testers/gpu_ruby_test/gpu_wavefront.cc` (churn=1)
- 复现: `git show dde3d10aea9fe23dde28edecd1e80c0b381742af`

### #432 tests,misc: Add "build/ALL/gem5.fast" Clang compilation to CI

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/432
- 代表 commit: `6ac6d0c34060` (2023-11-14)
- 变更规模: commits=1, files=1, +16/-0 (churn=16)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/ci-tests.yaml` (churn=16)
- 复现: `git show 6ac6d0c3406057b5ec19a521cf3839cbcd92478e`

### #520 systemc: Fix gcc13 systemC compilation error

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/520
- 代表 commit: `f11227b4a09b` (2023-11-14)
- 变更规模: commits=1, files=1, +5/-0 (churn=5)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/ext/core/sc_port.hh` (churn=5)
- 复现: `git show f11227b4a09bedcc92b961d70f47b232095dbce7`

### #120 mem-ruby,configs: Add GPU GLC Atomic Resource Constraints

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/120
- 代表 commit: `be5c03ea9f82` (2023-11-14)
- 变更规模: commits=1, files=9, +242/-5 (churn=247)
- 影响范围: topdirs=src, configs; subsys=mem, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/structures/ALUFreeListArray.cc` (churn=106)
  - `src/mem/ruby/structures/ALUFreeListArray.hh` (churn=78)
  - `src/mem/ruby/structures/CacheMemory.cc` (churn=24)
  - `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` (churn=15)
  - `configs/ruby/GPU_VIPER.py` (churn=11)
  - `src/mem/ruby/structures/CacheMemory.hh` (churn=5)
  - `src/mem/ruby/structures/RubyCache.py` (churn=5)
  - `src/mem/ruby/protocol/RubySlicc_Exports.sm` (churn=2)
- 复现: `git show be5c03ea9f82d43c7637488649fc1dabb1c29e7b`

### #546 mem-ruby: Fix for not creating log entries on atomic no return requests

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/546
- 代表 commit: `65b44e651609` (2023-11-14)
- 变更规模: commits=1, files=12, +156/-38 (churn=194)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/MOESI_AMD_Base-Region-dir.sm` (churn=54)
  - `src/mem/ruby/protocol/MOESI_AMD_Base-probeFilter.sm` (churn=38)
  - `src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm` (churn=29)
  - `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` (churn=26)
  - `src/mem/ruby/common/WriteMask.cc` (churn=14)
  - `src/mem/ruby/protocol/MOESI_AMD_Base-RegionBuffer.sm` (churn=12)
  - `src/mem/ruby/protocol/GPU_VIPER-TCP.sm` (churn=7)
  - `src/mem/ruby/common/DataBlock.cc` (churn=5)
- 复现: `git show 65b44e6516096d5f812fcfa6e4946663eabda10c`

### #542 systemc: Fix two bugs in gem5-to-tlm bridge

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/542
- 代表 commit: `99553fdbee97` (2023-11-14)
- 变更规模: commits=1, files=1, +3/-1 (churn=4)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/tlm_bridge/gem5_to_tlm.cc` (churn=4)
- 复现: `git show 99553fdbee97a866c2c5d3b458f5a3a5ddb246d2`

### #525 configs,ext,stdlib: Update DRAMSys integration

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/525
- 代表 commit: `e95cab429f03` (2023-11-14)
- 变更规模: commits=2, files=12, +331/-136 (churn=467)
- 影响范围: topdirs=src, ext, configs, .github; subsys=mem, ext, configs, python, .github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/dramsys.cc` (churn=138)
  - `ext/dramsys/SConscript` (churn=90)
  - `src/python/gem5/components/memory/dramsys.py` (churn=76)
  - `ext/dramsys/CMakeLists.txt` (churn=38)
  - `src/mem/dramsys.hh` (churn=34)
  - `src/mem/dramsys_wrapper.cc` (churn=27)
  - `configs/example/gem5_library/dramsys/dramsys-traffic.py` (churn=24)
  - `src/mem/dramsys_wrapper.hh` (churn=16)
- commits 列表（按 topo-order，Top 12）：
  - 2023-11-14 `e95cab429f03` configs,ext,stdlib: Update DRAMSys integration
  - 2023-11-26 `36e83943b594` tests,misc: Update DRAMSys test clone command
- 复现: `git show e95cab429f03d113a9032d3c2fcafaccf297b4b4`

### #563 tests,gpu-compute: Fix Lulesh 'Obtain LULESH' step

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/563
- 代表 commit: `8859592893fb` (2023-11-14)
- 变更规模: commits=1, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/gpu-tests.yaml` (churn=1)
- 复现: `git show 8859592893fb8cfc835c056227d80407a87b6b38`

### #562 tests: Remove multiple suites per job for Weekly tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/562
- 代表 commit: `30787b59d4ea` (2023-11-14)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/weekly-tests.yaml` (churn=2)
- 复现: `git show 30787b59d4eaccdfaab153a8f031010d5ffa349e`

### #566 misc: Merge develop .github dir to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/566
- 代表 commit: `d0d3c74ce00f` (2023-11-14)
- 变更规模: commits=1, files=4, +19/-4 (churn=23)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/ci-tests.yaml` (churn=16)
  - `.github/workflows/compiler-tests.yaml` (churn=4)
  - `.github/workflows/weekly-tests.yaml` (churn=2)
  - `.github/workflows/gpu-tests.yaml` (churn=1)
- 复现: `git show d0d3c74ce00f6d852a39060f3b7ce083887b1cfd`

### #565 gpu-compute: Minor edits for atomic no returns and stores

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/565
- 代表 commit: `4a5ec70e0822` (2023-11-15)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/GPUCoalescer.cc` (churn=2)
- 复现: `git show 4a5ec70e0822da711ce0c4a1f5e33bf63114885d`

### #554 arch-riscv: Move fault handler addr logic to ISA

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/554
- 代表 commit: `a8440f367dea` (2023-11-15)
- 变更规模: commits=1, files=3, +15/-3 (churn=18)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.cc` (churn=10)
  - `src/arch/riscv/faults.cc` (churn=4)
  - `src/arch/riscv/isa.hh` (churn=4)
- 复现: `git show a8440f367deab31abcbd44fe3bbee17298b07a6c`

### #386 dev: add debug flag in register bank.

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/386
- 代表 commit: `83f1fe3fec2c` (2023-11-15)
- 变更规模: commits=2, files=2, +76/-78 (churn=154)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/reg_bank.hh` (churn=152)
  - `src/dev/SConscript` (churn=2)
- commits 列表（按 topo-order，Top 12）：
  - 2023-11-15 `83f1fe3fec2c` dev: add debug flag in register bank.
  - 2023-11-20 `08c0d1f27ae3` dev: Fix `std::min` type mismatch in reg_bank.hh
- 复现: `git show 83f1fe3fec2c3d07d2e66a16749e72f62397b74a`

### #568 arch-riscv: Add overrides to RISC-V Interrupts class

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/568
- 代表 commit: `ceabe86b311e` (2023-11-15)
- 变更规模: commits=1, files=1, +8/-8 (churn=16)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/interrupts.hh` (churn=16)
- 复现: `git show ceabe86b311e3bdd7603b4e4631d2ed18365a5bd`

### #532 stdlib, resources: Update JSON data in workload

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/532
- 代表 commit: `bfe899e48ed7` (2023-11-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bfe899e48ed7f91ced31a5ac677a3dbf0e7c554c`

### #540 mem-ruby, gpu-compute: fix SQC/TCP requests to same line

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/540
- 代表 commit: `4965367724d9` (2023-11-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4965367724d9b58016f3ffbd0e0c0a1140f0f34d`

### #545 mem-ruby: AtomicNoReturn should check comp_anr instead of comp_wu

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/545
- 代表 commit: `4ca2efac1668` (2023-11-16)
- 变更规模: commits=1, files=1, +3/-3 (churn=6)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` (churn=6)
- 复现: `git show 4ca2efac1668f9ccbdc852ef29d0f0049b63c626`

### #564 mem-cache: Prefetchers Improvements

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/564
- 代表 commit: `db6a86978645` (2023-11-16)
- 变更规模: commits=2, files=6, +16/-27 (churn=43)
- 影响范围: topdirs=src; subsys=mem/cache/prefetch, mem/cache; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/prefetch/stride.cc` (churn=19)
  - `src/mem/cache/prefetch/Prefetcher.py` (churn=9)
  - `src/mem/cache/Cache.py` (churn=4)
  - `src/mem/cache/prefetch/queued.cc` (churn=4)
  - `src/mem/cache/prefetch/stride.hh` (churn=4)
  - `src/mem/cache/prefetch/base.cc` (churn=3)
- commits 列表（按 topo-order，Top 12）：
  - 2023-11-16 `db6a86978645` mem-cache: Prefetchers Improvements
  - 2023-11-20 `f26867a0758d` mem-cache: Revert "Prefetchers Improvements"
- 复现: `git show db6a8697864564aab1a11081edc05152c0478f83`

### #571 util: Bump GPUFS build docker to 5.4.2

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/571
- 代表 commit: `3896673ddc94` (2023-11-18)
- 变更规模: commits=1, files=1, +3/-3 (churn=6)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/gpu-fs/Dockerfile` (churn=6)
- 复现: `git show 3896673ddc943cdf2157651d090920da466bc6a7`

### #582 dev: Fix `std::min` type mismatch in reg_bank.hh

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/582
- 代表 commit: `d772f3967bf5` (2023-11-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d772f3967bf52eae5362149558d8bd66dbc7c467`

### #579 mem-ruby: Fix typo in CHI's Send_CompI

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/579
- 代表 commit: `3009e0fb5713` (2023-11-20)
- 变更规模: commits=1, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` (churn=1)
- 复现: `git show 3009e0fb5713a28a6b3a50983edc348dcdd03167`

### #570 dev-amdgpu: Add VMID map to checkpoint

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/570
- 代表 commit: `23a22ed95c38` (2023-11-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 23a22ed95c38c07b9e1c121891b2d0cb33eeb984`

### #585 mem-ruby: Fixes for new AtomicWait event in VIPER TCC

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/585
- 代表 commit: `6e433ed8851b` (2023-11-22)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` (churn=3)
- 复现: `git show 6e433ed8851ba40ae29071bef1aeece5615f5322`

### #584 arch-arm: Fix Virtual Interrupt logic in secure mode

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/584
- 代表 commit: `ab1d5dc3a084` (2023-11-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ab1d5dc3a08437892793713ddab72d0cb3dbb395`

### #581 mem-cache: Revert "Prefetchers Improvements"

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/581
- 代表 commit: `0b2c56ef6623` (2023-11-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0b2c56ef6623350a0023d6a7f0563b966a2fc872`

### #599 arch-vega,arch-gcn3: Bugfix V_PERM_B32 and V_OR3_B32

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/599
- 代表 commit: `cc9f81b08a04` (2023-11-26)
- 变更规模: commits=1, files=3, +3/-3 (churn=6)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/gcn3/insts/instructions.cc` (churn=2)
  - `src/arch/amdgpu/vega/decoder.cc` (churn=2)
  - `src/arch/amdgpu/vega/insts/instructions.cc` (churn=2)
- 复现: `git show cc9f81b08a0493d82a10145f15d36f9453e3625b`

### #600 tests: fix lulesh

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/600
- 代表 commit: `1de992bc757e` (2023-11-27)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/gpu-tests.yaml` (churn=2)
- 复现: `git show 1de992bc757e59c592df13e94c9299d3544330ff`

### #577 ext,github,tests: Update DRAMSys tests to v5.0 and handle new dependencies

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/577
- 代表 commit: `0f6eabe8c927` (2023-11-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0f6eabe8c927f6f7d642c69131c728091510ebf5`

### #597 dev-amdgpu: Writeback PM4 queue rptr when empty

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/597
- 代表 commit: `9e6a87e67a90` (2023-11-27)
- 变更规模: commits=1, files=1, +18/-1 (churn=19)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/amdgpu/pm4_packet_processor.cc` (churn=19)
- 复现: `git show 9e6a87e67a907ee04b06eeaa1d34e5549dbed7ce`

### #69 scons: Change to Kconfig build system

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/69
- 代表 commit: `d94d6017b051` (2023-11-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d94d6017b051d1b33d8950bddcbb89662fbec503`

### #608 misc: Merge develop .github dir to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/608
- 代表 commit: `3bf0b1d22a37` (2023-11-27)
- 变更规模: commits=1, files=3, +22/-13 (churn=35)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=27)
  - `.github/workflows/weekly-tests.yaml` (churn=6)
  - `.github/workflows/gpu-tests.yaml` (churn=2)
- 复现: `git show 3bf0b1d22a375f9a1da34150293e8f8ddf433391`

### #493 cpu: Require BTB hit to detect branches.

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/493
- 代表 commit: `0c30353c59c4` (2023-11-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0c30353c59c4bf3cfd4b61d773520b6d4c6c37de`

### #593 arch-x86: Fix misc registers in mov instructions

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/593
- 代表 commit: `3fe5e58f2832` (2023-11-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3fe5e58f2832cc9b8712058db96373a57d36c462`

### #610 arch-riscv: fix tlb bug

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/610
- 代表 commit: `089b82b2e95a` (2023-11-29)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/tlb.cc` (churn=2)
- 复现: `git show 089b82b2e95a326899af7c5ae95a3c690bbf785c`

### #614 cpu-o3: Fix discarded requests str-ld forwarding

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/614
- 代表 commit: `eb13b3231427` (2023-11-29)
- 变更规模: commits=1, files=2, +5/-3 (churn=8)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/lsq_unit.cc` (churn=6)
  - `src/cpu/o3/lsq.hh` (churn=2)
- 复现: `git show eb13b3231427b6e86c2714515d8f9fbccc66bfb7`

### #606 arch-riscv: Fix narrow datatypes in RVV isa files

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/606
- 代表 commit: `b0cefac9b2c7` (2023-11-29)
- 变更规模: commits=1, files=4, +116/-114 (churn=230)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=95)
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=68)
  - `src/arch/riscv/insts/vector.hh` (churn=65)
  - `src/arch/riscv/isa/decoder.isa` (churn=2)
- 复现: `git show b0cefac9b2c73db246f6e8828351458df4af3b75`

### #522 arch-riscv: Support combination of privilege modes configuration

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/522
- 代表 commit: `a2e7bd469811` (2023-11-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a2e7bd4698118f2ba39a14aa4321c5dea9fbf836`

### #617 scons: Move CPPPATH systemc_home to "src/systemc" folder

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/617
- 代表 commit: `57ba3fccb7e2` (2023-11-29)
- 变更规模: commits=1, files=2, +4/-7 (churn=11)
- 影响范围: topdirs=src; subsys=arch, src; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/fastmodel/SConscript` (churn=6)
  - `src/systemc/SConscript` (churn=5)
- 复现: `git show 57ba3fccb7e2946071dedd0e22d75c106230ba09`

### #601 scons: Add an option to reduce memory usage of ld

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/601
- 代表 commit: `1d1cba297b66` (2023-11-29)
- 变更规模: commits=1, files=1, +10/-0 (churn=10)
- 影响范围: topdirs=SConstruct; subsys=SConstruct; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=10)
- 复现: `git show 1d1cba297b6651c54ee2aff048da723eaa94e515`

### #592 arch-x86: Fixes page fault for CLFLUSH on write-protected pages

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/592
- 代表 commit: `fcbcd1ce7229` (2023-11-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fcbcd1ce72298db6e2b2ec228fc375d8f0a2d276`

### #611 stdlib, resources: removed  deprecated if statement  in obtain_resource for workload resources

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/611
- 代表 commit: `392086b43d70` (2023-11-29)
- 变更规模: commits=1, files=1, +8/-16 (churn=24)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/resource.py` (churn=24)
- 复现: `git show 392086b43d7068292e440f45b54ddf240909442f`

### #620 tests: switch lulesh/hacc to use vega_x86

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/620
- 代表 commit: `403bf38a0e1d` (2023-11-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 403bf38a0e1db17634e1341bcfc5d64da9631dcd`

### #626 misc: Merge .github directory from develop to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/626
- 代表 commit: `b99af93183b2` (2023-11-29)
- 变更规模: commits=1, files=1, +12/-12 (churn=24)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/gpu-tests.yaml` (churn=24)
- 复现: `git show b99af93183b2f77781b6f4759bc06f717d3bb9bc`

### #431 misc,python: Add `isort` hook to pre-commit

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/431
- 代表 commit: `dcdebec0f62e` (2023-11-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show dcdebec0f62e93d2f5d02d1655c43ad8bb3ad9d4`

### #531 sim,python: Restore sigint handler in python

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/531
- 代表 commit: `9afe9932bc88` (2023-11-30)
- 变更规模: commits=1, files=3, +30/-5 (churn=35)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/init_signals.cc` (churn=23)
  - `src/sim/simulate.cc` (churn=8)
  - `src/sim/init_signals.hh` (churn=4)
- 复现: `git show 9afe9932bc8814e3027242740ac2f69f31032e09`

### #629 scons: Limit adding fastmodel files and libpath

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/629
- 代表 commit: `a16fd8a59210` (2023-11-30)
- 变更规模: commits=1, files=7, +20/-0 (churn=20)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/fastmodel/CortexA76/SConscript` (churn=3)
  - `src/arch/arm/fastmodel/CortexR52/SConscript` (churn=3)
  - `src/arch/arm/fastmodel/GIC/SConscript` (churn=3)
  - `src/arch/arm/fastmodel/PL330_DMAC/SConscript` (churn=3)
  - `src/arch/arm/fastmodel/iris/SConscript` (churn=3)
  - `src/arch/arm/fastmodel/reset_controller/SConscript` (churn=3)
  - `src/arch/arm/fastmodel/SConscript` (churn=2)
- 复现: `git show a16fd8a592104528bfd89d6b54e6219d8024399b`

### #502 Support for classic prefetchers in Ruby

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/502
- 代表 commit: `b3e7af9d7975` (2023-11-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b3e7af9d7975c10c8af95fa76a34761a83da97e4`

### #627 util-docker: Enforce cmake version >=3.24 for DRAMSys

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/627
- 代表 commit: `bfd25f535216` (2023-11-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bfd25f535216dbc3cca6a5fb9bc70da18f4068b3`

### #631 misc, stdlib: Update documentation to adhere to RST formatting.

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/631
- 代表 commit: `d96b6cdae7e9` (2023-12-01)
- 变更规模: commits=1, files=92, +1054/-935 (churn=1989)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/simulator.py` (churn=239)
  - `src/python/gem5/resources/resource.py` (churn=194)
  - `src/python/gem5/resources/looppoint.py` (churn=92)
  - `src/python/gem5/components/boards/abstract_board.py` (churn=84)
  - `src/python/m5/ext/pystats/serializable_stat.py` (churn=79)
  - `src/python/gem5/components/boards/kernel_disk_workload.py` (churn=78)
  - `src/python/gem5/components/processors/complex_generator_core.py` (churn=76)
  - `src/python/m5/stats/gem5stats.py` (churn=74)
- 复现: `git show d96b6cdae7e956d6ed248a2b9285953b8116d7bd`

### #598 mem-ruby: Unused L3CacheCntrl freed

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/598
- 代表 commit: `fc0a043950a8` (2023-12-01)
- 变更规模: commits=1, files=2, +2/-1 (churn=3)
- 影响范围: topdirs=configs, src; subsys=configs, mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/GPU_VIPER.slicc` (churn=2)
  - `configs/ruby/GPU_VIPER.py` (churn=1)
- 复现: `git show fc0a043950a828c1f513bfe421f33eeb70a6aa83`

### #634 misc: Fix precommit install

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/634
- 代表 commit: `39fd61d7ddea` (2023-12-01)
- 变更规模: commits=1, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=site_scons; subsys=scons; arch=-
- 主要改动文件（Top 8 by churn）:
  - `site_scons/site_tools/git.py` (churn=1)
- 复现: `git show 39fd61d7ddea49e5ab0cdcada5c734a6826f3636`

### #637 mem-ruby: update CacheMemory RubyCache debug prints

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/637
- 代表 commit: `bd2838d18e77` (2023-12-01)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bd2838d18e77a2ba98dfe8c8319d4523d3bd40bf`

### #638 tests: fix artifact reference in HACC tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/638
- 代表 commit: `9d108826b073` (2023-12-01)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/gpu-tests.yaml` (churn=2)
- 复现: `git show 9d108826b07386ceaa875d6668786c4551440cf8`

### #615 systemc: Bugfix in TlmToGem5Bridge

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/615
- 代表 commit: `84efeb976a23` (2023-12-01)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/tlm_bridge/tlm_to_gem5.cc` (churn=1)
- 复现: `git show 84efeb976a23d27fcb737d01cd3fe419143711ff`

### #630 stdlib: Integrate BootloaderKernelWorkload

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/630
- 代表 commit: `48f3cd1c0e60` (2023-12-01)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 48f3cd1c0e602db5f517cf81bc866f36d2feb572`

### #625 stdlib: Mv resource download to `get_local_path` and add `ShadowResource`

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/625
- 代表 commit: `500a4221a03b` (2023-12-01)
- 变更规模: commits=3, files=1, +8/-4 (churn=12)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/resource.py` (churn=12)
- commits 列表（按 topo-order，Top 12）：
  - 2023-12-01 `500a4221a03b` stdlib: Mv resource download to `get_local_path` and add `ShadowResource`
  - 2023-12-12 `4eb81296b1c1` stdlib: Add `get_local_path()` call to Looppoint resources
  - 2023-12-14 `d8cc5305979e` stdlib: Add `get_local_path()` call to Looppoint resources
- 复现: `git show 500a4221a03b2b4c6e2e41ad36b1c73c8aea185a`

### #642 misc: Add gem5_build/ to .gitignore

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/642
- 代表 commit: `ecb72b74f831` (2023-12-01)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=.gitignore; subsys=.gitignore; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.gitignore` (churn=1)
- 复现: `git show ecb72b74f831d2a8495944be2f4f874d63ad1dbe`

### #639 sim: Rework the Linux Kernel exit events

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/639
- 代表 commit: `d9c870f6417c` (2023-12-01)
- 变更规模: commits=1, files=5, +89/-61 (churn=150)
- 影响范围: topdirs=src; subsys=src, sim, arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/kern/linux/events.hh` (churn=41)
  - `src/sim/Workload.py` (churn=34)
  - `src/kern/linux/events.cc` (churn=30)
  - `src/arch/arm/linux/fs_workload.cc` (churn=29)
  - `src/sim/SConscript` (churn=16)
- 复现: `git show d9c870f6417c7c344e898c5e9d69912f79aa3033`

### #636 Fix for gem5 Issue #550

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/636
- 代表 commit: `21919addca34` (2023-12-01)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 21919addca34623105691204ff1db9782c38afff`

### #643 misc: Update .github dir in stable from develop

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/643
- 代表 commit: `461af5157552` (2023-12-01)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/gpu-tests.yaml` (churn=2)
- 复现: `git show 461af5157552d916baec732dfc7d8e97a2e781d8`

### #607 misc: update gapbs example to use suites

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/607
- 代表 commit: `88c57e22de5e` (2023-12-03)
- 变更规模: commits=1, files=1, +5/-68 (churn=73)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gem5_library/x86-gapbs-benchmarks.py` (churn=73)
- 复现: `git show 88c57e22de5e51352647c9af37f83b28548d802e`

### #553 arch-arm: Only build ArmCapstoneDisassembler when ISA is arm

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/553
- 代表 commit: `7a5052b3a0e1` (2023-12-03)
- 变更规模: commits=1, files=3, +14/-6 (churn=20)
- 影响范围: topdirs=src; subsys=cpu, arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/SConscript` (churn=8)
  - `src/arch/arm/tracers/SConscript` (churn=6)
  - `src/cpu/Kconfig` (churn=6)
- 复现: `git show 7a5052b3a0e1bfb54f3f1c3dd3eba633526829b0`

### #641 arch-riscv: fix o3 cpu stuck in spinlock bug

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/641
- 代表 commit: `5eba3941f498` (2023-12-03)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=2)
- 复现: `git show 5eba3941f498a266b44970811bb8aa849d533735`

### #587 misc: update x86-npb-benchmarks.py to use suites

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/587
- 代表 commit: `bad569a3f83e` (2023-12-03)
- 变更规模: commits=1, files=1, +7/-33 (churn=40)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gem5_library/x86-npb-benchmarks.py` (churn=40)
- 复现: `git show bad569a3f83e5870f1274d62e0980a360853c321`

### #645 stdlib: Add comment to ShadowResource

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/645
- 代表 commit: `c718e94753e7` (2023-12-03)
- 变更规模: commits=1, files=1, +6/-0 (churn=6)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/resource.py` (churn=6)
- 复现: `git show c718e94753e78cbe5b87578ef60d8e949ef23338`

### #646 mem-ruby: Fix compile error in chi-dvm-funcs

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/646
- 代表 commit: `895944fa27c2` (2023-12-03)
- 变更规模: commits=1, files=1, +7/-3 (churn=10)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/CHI-dvm-misc-node-funcs.sm` (churn=10)
- 复现: `git show 895944fa27c2e84a27705f814e4782357df7514b`

### #635 arch-riscv: correctly pass arguments to kernel with new bootloader+kernel

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/635
- 代表 commit: `7b9864195330` (2023-12-04)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/riscv_board.py` (churn=2)
- 复现: `git show 7b986419533083dc48d961dcc502701f673a9af2`

### #241 configs,stdlib,tests: Remove get_runtime_isa()

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/241
- 代表 commit: `569e21f798f6` (2023-12-04)
- 变更规模: commits=1, files=20, +125/-178 (churn=303)
- 影响范围: topdirs=configs, src, tests; subsys=configs, python, tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/processors/simple_core.py` (churn=39)
  - `src/python/gem5/runtime.py` (churn=37)
  - `configs/common/ObjectList.py` (churn=33)
  - `configs/example/hmc_hello.py` (churn=25)
  - `configs/common/CacheConfig.py` (churn=22)
  - `src/python/gem5/components/processors/simple_switchable_processor.py` (churn=21)
  - `tests/gem5/multi_isa/test_multi_isa.py` (churn=20)
  - `configs/common/Caches.py` (churn=19)
- 复现: `git show 569e21f798f61c22a91e8f6bad083a82fdf43034`

### #651 configs: Fix apu_se.py CPU type checks

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/651
- 代表 commit: `f00d7f70a4c8` (2023-12-04)
- 变更规模: commits=1, files=2, +13/-16 (churn=29)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/apu_se.py` (churn=27)
  - `configs/ruby/GPU_VIPER.py` (churn=2)
- 复现: `git show f00d7f70a4c88ff6b3ee703cb09ff33cc617f74f`

### #652 configs: Fix issues after get_runtime_isa() #241 removed

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/652
- 代表 commit: `9bd61f217fcb` (2023-12-06)
- 变更规模: commits=1, files=10, +46/-31 (churn=77)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/common/ObjectList.py` (churn=17)
  - `configs/common/Caches.py` (churn=11)
  - `configs/common/CpuConfig.py` (churn=11)
  - `configs/common/Options.py` (churn=10)
  - `configs/common/CacheConfig.py` (churn=8)
  - `configs/example/hmc_hello.py` (churn=6)
  - `configs/ruby/Ruby.py` (churn=6)
  - `configs/common/Simulation.py` (churn=4)
- 复现: `git show 9bd61f217fcbf0d787d96550307108f7ba614f55`

### #573 arch-riscv: Add PCEvent for RISCV FS Workload kernel panic/oops

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/573
- 代表 commit: `75544b2abf31` (2023-12-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 75544b2abf310d258885e2562106e3ee670e46ff`

### #662 misc: Update version to v23.1.0.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/662
- 代表 commit: `d006f866c0d2` (2023-12-06)
- 变更规模: commits=1, files=2, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=src, base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/Doxyfile` (churn=2)
  - `src/base/version.cc` (churn=2)
- 复现: `git show d006f866c0d236d8a0d7e5b6223527ff76ebc0b5`

### #658 stdlib: Fix the chi protocol of arm boot tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/658
- 代表 commit: `db286903ee7f` (2023-12-13)
- 变更规模: commits=2, files=3, +10/-0 (churn=10)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/cachehierarchies/chi/nodes/dma_requestor.py` (churn=4)
  - `src/python/gem5/components/cachehierarchies/chi/nodes/private_l1_moesi_cache.py` (churn=4)
  - `src/python/gem5/components/cachehierarchies/chi/nodes/directory.py` (churn=2)
- commits 列表（按 topo-order，Top 12）：
  - 2023-12-07 `10a0c950da26` stdlib: Fix the chi protocol of arm boot tests
  - 2023-12-13 `db286903ee7f` stdlib: Fix the chi protocol of arm boot tests
- 复现: `git show db286903ee7fe8e2441c118db27ed71c40a99cc4`

### #655 configs: Make riscv/fs_linux work in build/ALL/gem5.opt

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/655
- 代表 commit: `6b80a2e81c27` (2023-12-13)
- 变更规模: commits=2, files=2, +46/-46 (churn=92)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/common/Options.py` (churn=52)
  - `configs/example/riscv/fs_linux.py` (churn=40)
- commits 列表（按 topo-order，Top 12）：
  - 2023-12-12 `5a6901c40537` configs: Make riscv/fs_linux work in build/ALL/gem5.opt
  - 2023-12-13 `6b80a2e81c27` configs: Make riscv/fs_linux work in build/ALL/gem5.opt
- 复现: `git show 6b80a2e81c279d2d6cd4172edb2f373e95aa408c`

### #671 tests: fix gapbs and npb tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/671
- 代表 commit: `34f784f59cb6` (2023-12-14)
- 变更规模: commits=2, files=1, +4/-10 (churn=14)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/gem5_library_example_tests/test_gem5_library_examples.py` (churn=14)
- commits 列表（按 topo-order，Top 12）：
  - 2023-12-12 `bc12e7269d06` tests: fix gapbs and npb tests
  - 2023-12-14 `34f784f59cb6` tests: fix gapbs and npb tests
- 复现: `git show 34f784f59cb6938e66a4eff2c842d592f5e21cd5`

### #677 arch-riscv: fix riscv matched board for se mode

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/677
- 代表 commit: `9aab380775ba` (2023-12-14)
- 变更规模: commits=2, files=1, +24/-16 (churn=40)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/prebuilt/riscvmatched/riscvmatched_board.py` (churn=40)
- commits 列表（按 topo-order，Top 12）：
  - 2023-12-13 `c66862f6e3fb` arch-riscv: fix riscv matched board for se mode
  - 2023-12-14 `9aab380775ba` arch-riscv: fix riscv matched board for se mode
- 复现: `git show 9aab380775ba59a62ae2b6a26a0557f3e8e1f5df`

### #682 misc: Cherry-pick from `develop` to `release-staging-v23-1` [Nov 13th]

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/682
- 代表 commit: `a84cfd2f0df1` (2023-12-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a84cfd2f0df116c5bdb48a602eec2c42306e80d2`

### #666 arch-x86: Fix two_byte_opcodes.isa `0x6` -> `0x0`

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/666
- 代表 commit: `29b77260f39d` (2023-12-14)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/isa/decoder/two_byte_opcodes.isa` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2023-12-12 `37e41733515b` arch-x86: Fix two_byte_opcodes.isa `0x6` -> `0x0`
  - 2023-12-14 `29b77260f39d` arch-x86: Fix two_byte_opcodes.isa `0x6` -> `0x0`
- 复现: `git show 29b77260f39d47c293e6bd1cd166ff24f567eeb0`

### #683 misc: Cherry-pick PR #666 from `develop` to `release-staging-v23-1`

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/683
- 代表 commit: `9064249fabb0` (2023-12-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 9064249fabb03c98fbd532f5d253b7408983a10f`

### #684 configs: Fix SMT cpu type checking

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/684
- 代表 commit: `ce6fd7f084ad` (2023-12-17)
- 变更规模: commits=1, files=2, +3/-1 (churn=4)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/deprecated/example/se.py` (churn=3)
  - `configs/common/ObjectList.py` (churn=1)
- 复现: `git show ce6fd7f084ad2f632f6d8486899a7328ea4d8f35`

### #689 sim: Remove trailing / from proc/meminfo special path

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/689
- 代表 commit: `27d89379d2b1` (2023-12-18)
- 变更规模: commits=2, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/syscall_emul.hh` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2023-12-17 `9b0bf33f790d` sim: Remove trailing / from proc/meminfo special path
  - 2023-12-18 `27d89379d2b1` sim: Remove trailing / from proc/meminfo special path
- 复现: `git show 27d89379d2b12f1150ef3c26a17ee9f262f78d77`

### #510 util: Added script to copy resources from mongodb

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/510
- 代表 commit: `d76a01973a82` (2023-12-18)
- 变更规模: commits=2, files=3, +612/-0 (churn=612)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/offline_db/get-resources-from-db.py` (churn=496)
  - `util/offline_db/README.md` (churn=90)
  - `util/offline_db/gem5_default_config.json` (churn=26)
- commits 列表（按 topo-order，Top 12）：
  - 2023-12-18 `b42d9fabf758` util: Added script to copy resources from mongodb
  - 2023-12-18 `d76a01973a82` util: Added script to copy resources from mongodb
- 复现: `git show d76a01973a82c91047e844802bbaaeab97a3524f`

### #699 misc: Cherry pick changes from develop to the v23.1 staging branch

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/699
- 代表 commit: `211d00f48f72` (2023-12-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 211d00f48f72011cf3aa7db5d068392c8bf27a5b`

### #696 misc: Turn off 'maybe-uninitialized' warn for regex include

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/696
- 代表 commit: `e95389920a45` (2023-12-21)
- 变更规模: commits=2, files=1, +40/-0 (churn=40)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/kern/linux/helpers.cc` (churn=40)
- commits 列表（按 topo-order，Top 12）：
  - 2023-12-20 `2f58f1c87b86` misc: Turn off 'maybe-uninitialized' warn for regex include
  - 2023-12-21 `e95389920a45` misc: Turn off 'maybe-uninitialized' warn for regex include
- 复现: `git show e95389920a45ef8ea0ab6860a18098cdea34e9fd`

### #706 misc: Fix 'maybe-uninitialized' warn turn off

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/706
- 代表 commit: `c4146d8813a0` (2023-12-21)
- 变更规模: commits=2, files=1, +8/-12 (churn=20)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/kern/linux/helpers.cc` (churn=20)
- commits 列表（按 topo-order，Top 12）：
  - 2023-12-21 `25e0e96741e4` misc: Fix 'maybe-uninitialized' warn turn off
  - 2023-12-21 `c4146d8813a0` misc: Fix 'maybe-uninitialized' warn turn off
- 复现: `git show c4146d8813a0f8a7f83584f2a131504cee663baa`

### #705 mem: Updated bytesRead and bytesWritten stat

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/705
- 代表 commit: `70aeaaa0e9cb` (2023-12-21)
- 变更规模: commits=2, files=4, +38/-28 (churn=66)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/nvm_interface.cc` (churn=26)
  - `src/mem/dram_interface.cc` (churn=24)
  - `src/mem/dram_interface.hh` (churn=8)
  - `src/mem/nvm_interface.hh` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2023-12-21 `5288dbbf903a` mem: Updated bytesRead and bytesWritten stat
  - 2023-12-21 `70aeaaa0e9cb` mem: Updated bytesRead and bytesWritten stat
- 复现: `git show 70aeaaa0e9cb62f7ab23e8be1a800f3eb84c87a6`

### #707 misc: Cherry-pick commits from develop to v23.1 staging

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/707
- 代表 commit: `d48ed780d2b9` (2023-12-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d48ed780d2b9daaa97f99ac574f3855a2b1ec2df`

### #708 scons: Remove warnings-as-errors comp feature for v23.1

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/708
- 代表 commit: `4c02ae214f48` (2023-12-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4c02ae214f48b9cb2ab3176c98aecf5857b7e26d`

### #447 misc: Add release notes for version 23.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/447
- 代表 commit: `cafc5e685dd4` (2023-12-23)
- 变更规模: commits=1, files=1, +128/-0 (churn=128)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=128)
- 复现: `git show cafc5e685dd4121342bd25b2d01d4ed444ae0686`

### #714 misc: Fix kconfig section format of RELEASE-NOTE.md

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/714
- 代表 commit: `5c4e41ad23c8` (2023-12-27)
- 变更规模: commits=1, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=8)
- 复现: `git show 5c4e41ad23c84c6c44d867989d1225544e5a3926`

### #717 misc: Merge `stable` into `release-staging-v23-1`

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/717
- 代表 commit: `e0706e9270a1` (2023-12-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e0706e9270a1c1a330914b758f23cd08b7cd1fa3`

### #711 misc: Merge `release-staging-v23-1` into `stable`

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/711
- 代表 commit: `bae34876780d` (2023-12-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bae34876780dfb2bc22b9151bfda1d39ee80cfb1`

## v24.0.0.0 (2024-06-27)

- PR 数：318

### #596 arch-arm: add Sve mla and mls indexed

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/596
- 代表 commit: `81d3c6307d90` (2023-12-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 81d3c6307d90bf11b58380029dc774b14d73e075`

### #654 arch-riscv: Update riscv matched board

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/654
- 代表 commit: `ea1226119c8f` (2023-12-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ea1226119c8f96894f11b25e0b2d48613434f026`

### #674 arch,arch-riscv: Fix inst flag of RISC-V vector store macro instructions

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/674
- 代表 commit: `c8cc193db885` (2023-12-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c8cc193db8853cecf266090b82e72cdd15df95cb`

### #656 mem: Add a flag on AbstractMemory to control statistics collection

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/656
- 代表 commit: `eff08ba113c3` (2023-12-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show eff08ba113c38a5f1950b342f93448a42e1482e9`

### #657 arch-arm: Partial SVE2 Implementation

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/657
- 代表 commit: `8d09e954208a` (2023-12-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 8d09e954208a7c2ce9446465d4de983faf686e93`

### #672 arch-riscv: squash walks with tlb hits in startWalkWrapper

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/672
- 代表 commit: `da3e3b806d6d` (2023-12-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show da3e3b806d6dd243bfd0d6b263264c1335142268`

### #675 stdlib,resources: Fix obtaining gem5 Looppoint resources

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/675
- 代表 commit: `695c350f31c7` (2023-12-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 695c350f31c7f73c410d28c99e70ee9576eca17d`

### #688 mem-ruby: Implement a dummy StashOnceShared/Unique

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/688
- 代表 commit: `a008cd2611e2` (2023-12-16)
- 变更规模: commits=1, files=5, +33/-5 (churn=38)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/CHI-cache-funcs.sm` (churn=12)
  - `src/mem/ruby/protocol/chi/CHI-cache-transitions.sm` (churn=11)
  - `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` (churn=10)
  - `src/mem/ruby/protocol/chi/CHI-msg.sm` (churn=3)
  - `src/mem/ruby/protocol/chi/CHI-cache.sm` (churn=2)
- 复现: `git show a008cd2611e2738de4799f06d90594e9d7aafabc`

### #679 tests: Silence Clang 16 warnings

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/679
- 代表 commit: `2700f392cbbc` (2023-12-18)
- 变更规模: commits=1, files=2, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/bitunion.test.cc` (churn=2)
  - `src/base/compiler.hh` (churn=2)
- 复现: `git show 2700f392cbbcefda9d64ffddfbba42224555be27`

### #692 mem-ruby: Implement WriteUniqueZero CHI transaction

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/692
- 代表 commit: `4f5d4b9bafc1` (2023-12-19)
- 变更规模: commits=1, files=5, +67/-2 (churn=69)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/CHI-cache-transitions.sm` (churn=37)
  - `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` (churn=24)
  - `src/mem/ruby/protocol/chi/CHI-cache.sm` (churn=4)
  - `src/mem/ruby/protocol/chi/CHI-cache-funcs.sm` (churn=3)
  - `src/mem/ruby/protocol/chi/CHI-msg.sm` (churn=1)
- 复现: `git show 4f5d4b9bafc111526520a88e4bfcd2a22ec17b1a`

### #695 tests: Fix Daily memory tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/695
- 代表 commit: `82b5c332b7ae` (2023-12-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 82b5c332b7aeb04f365f6d5d6cfbe725139228c5`

### #704 mem-ruby,configs: Enable Ruby with NULL build

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/704
- 代表 commit: `d6b798431f06` (2023-12-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d6b798431f06ad14372aca6dae46e574be8bc235`

### #698 configs: Fix SMT cpu type checking

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/698
- 代表 commit: `025ccadc6823` (2023-12-22)
- 变更规模: commits=1, files=2, +3/-1 (churn=4)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/deprecated/example/se.py` (churn=3)
  - `configs/common/ObjectList.py` (churn=1)
- 复现: `git show 025ccadc6823eff93ee9e0a20712000978bbc53e`

### #716 misc: Merge v23.1 staging branch into develop

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/716
- 代表 commit: `88ea70886b8d` (2023-12-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 88ea70886b8df352752713cef464dc4115eee9c0`

### #680 scons: Add option to use libc++

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/680
- 代表 commit: `e7d7199ea471` (2023-12-28)
- 变更规模: commits=1, files=2, +18/-4 (churn=22)
- 影响范围: topdirs=SConstruct, site_scons; subsys=SConstruct, scons; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=16)
  - `site_scons/gem5_scons/configure.py` (churn=6)
- 复现: `git show e7d7199ea4710ec39d0bb50f076ad66419a9eb68`

### #732 arch-arm: Handle invalid case for encodeAArch64SysReg

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/732
- 代表 commit: `5e2e748f3a16` (2024-01-04)
- 变更规模: commits=1, files=4, +12/-9 (churn=21)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/insts/misc64.cc` (churn=10)
  - `src/arch/arm/insts/mem64.cc` (churn=4)
  - `src/arch/arm/regs/misc.cc` (churn=4)
  - `src/arch/arm/regs/misc.hh` (churn=3)
- 复现: `git show 5e2e748f3a16b384ebf010a1b9f901dfa7a951ab`

### #734 mem-ruby: fix missing txnId for prefetch requests

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/734
- 代表 commit: `b652ab85581b` (2024-01-04)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` (churn=1)
- 复现: `git show b652ab85581b59edf82492901aa85e15065e974e`

### #730 gpu-compute: Added register file cache support

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/730
- 代表 commit: `dc85d1492cec` (2024-01-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show dc85d1492cec536752c1b1f68a17711c933c50df`

### #731 gpu-compute: WAX dependency detection

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/731
- 代表 commit: `ab9e61ea039b` (2024-01-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ab9e61ea039b77aa79458e385c873d7836495b6a`

### #735 fastmodel: Fix the Fastmodel RemoteGDB initial

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/735
- 代表 commit: `74dd0bb9bb0e` (2024-01-10)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/fastmodel/remote_gdb.cc` (churn=2)
- 复现: `git show 74dd0bb9bb0e599b7a159eba2e0bc131867c53a4`

### #691 arch-riscv: Move PMAChecker and PMP to RiscvISA namespace

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/691
- 代表 commit: `2f24ee570e55` (2024-01-10)
- 变更规模: commits=1, files=6, +30/-16 (churn=46)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/pmp.cc` (churn=26)
  - `src/arch/riscv/pmp.hh` (churn=8)
  - `src/arch/riscv/pma_checker.cc` (churn=4)
  - `src/arch/riscv/pma_checker.hh` (churn=4)
  - `src/arch/riscv/PMAChecker.py` (churn=2)
  - `src/arch/riscv/PMP.py` (churn=2)
- 复现: `git show 2f24ee570e552dfca6fb08377ed669b5e3ebaf62`

### #764 configs: Add o3 --cpu choice to the starter_se.py script

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/764
- 代表 commit: `7487c1318170` (2024-01-12)
- 变更规模: commits=1, files=1, +11/-2 (churn=13)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/arm/starter_se.py` (churn=13)
- 复现: `git show 7487c131817023377b09a9b9fc28ad9b36d82310`

### #701 mem-ruby: allow comparison of int and Addr in SLICC

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/701
- 代表 commit: `e5bdc760e320` (2024-01-12)
- 变更规模: commits=1, files=1, +9/-1 (churn=10)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/slicc/ast/OperatorExprAST.py` (churn=10)
- 复现: `git show e5bdc760e32045a75f9213e3cab8f164f0d6ce40`

### #756 arch-riscv: Remove the check of bit 63 of the physical address

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/756
- 代表 commit: `85eb99388a96` (2024-01-12)
- 变更规模: commits=1, files=1, +0/-14 (churn=14)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/tlb.cc` (churn=14)
- 复现: `git show 85eb99388a96ec0da1e1bdbdd4db63175aea6819`

### #733 gpu-compute: Support for MI200 GPU model

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/733
- 代表 commit: `6a9e80c54c29` (2024-01-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6a9e80c54c29e8312ac9b95d3d65e9959e0f9fd8`

### #774 arch-vega: Fix upsize cast error in newer compilers

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/774
- 代表 commit: `70376d43a3df` (2024-01-16)
- 变更规模: commits=1, files=1, +9/-9 (churn=18)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/instructions.cc` (churn=18)
- 复现: `git show 70376d43a3dfbbe2bd5f1e8d3735694a693d4341`

### #773 mem-ruby: fix ruby startup() to reset exit event correctly

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/773
- 代表 commit: `c2a22b03b433` (2024-01-17)
- 变更规模: commits=1, files=1, +5/-0 (churn=5)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/RubySystem.cc` (churn=5)
- 复现: `git show c2a22b03b43368fde6fe0622224d07cd0570f440`

### #715 arch-riscv: Fix issue when vl=0 in VectorIntMaskMacroConstructor

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/715
- 代表 commit: `511729ab767a` (2024-01-17)
- 变更规模: commits=1, files=1, +1/-5 (churn=6)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=6)
- 复现: `git show 511729ab767a45118c73d6a82b16f1aa484311cc`

### #780 arch-riscv: Refactor the RISC-V multiplication utility

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/780
- 代表 commit: `f56459470a48` (2024-01-18)
- 变更规模: commits=1, files=2, +39/-69 (churn=108)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/utility.hh` (churn=84)
  - `src/arch/riscv/isa/decoder.isa` (churn=24)
- 复现: `git show f56459470a481c49d1c81992dc4ae34a13589f66`

### #784 arch-arm: Fix compile error in kvm

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/784
- 代表 commit: `a555449c1202` (2024-01-19)
- 变更规模: commits=1, files=1, +10/-4 (churn=14)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/kvm/gic.cc` (churn=14)
- 复现: `git show a555449c12025c092fdf6d17f7fd0a69b03631bb`

### #647 misc: Merge Weekly GPU tests into Weekly Tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/647
- 代表 commit: `f2916e1b2b41` (2024-01-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f2916e1b2b415efdfdbaaaa88210b94f82a3d404`

### #789 arch-vega: Reorganize inst and misc files

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/789
- 代表 commit: `4fe64890389b` (2024-01-23)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4fe64890389bd873253314f76d8a92e12821cac7`

### #737 util: updated resource manager dependencies

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/737
- 代表 commit: `fea410641445` (2024-01-23)
- 变更规模: commits=1, files=1, +5/-5 (churn=10)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=10)
- 复现: `git show fea41064144592e512026b40438132244677daba`

### #801 arch-vega: Remove deleted instruction.cc from build

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/801
- 代表 commit: `dfafc5792a4b` (2024-01-23)
- 变更规模: commits=1, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/SConscript` (churn=1)
- 复现: `git show dfafc5792a4bbab09112e3aefce9c5cc9f1a86da`

### #792 dev-amdgpu: Check privledge bit for SDMA RLC queues

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/792
- 代表 commit: `0ac110ac9570` (2024-01-24)
- 变更规模: commits=1, files=2, +22/-7 (churn=29)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/amdgpu/sdma_engine.cc` (churn=26)
  - `src/dev/amdgpu/sdma_engine.hh` (churn=3)
- 复现: `git show 0ac110ac9570458b07d69dc4459e70efcd9d44cc`

### #803 arch-vega: Implement memory aperture operands

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/803
- 代表 commit: `44c78d843c6c` (2024-01-24)
- 变更规模: commits=1, files=3, +54/-4 (churn=58)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/operand.hh` (churn=38)
  - `src/arch/amdgpu/vega/gpu_registers.cc` (churn=12)
  - `src/arch/amdgpu/vega/gpu_registers.hh` (churn=8)
- 复现: `git show 44c78d843c6cb865097492106cba452d7d1782bb`

### #793 arch-riscv: Simply implementation of vector multiply and divide instructions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/793
- 代表 commit: `6dd936e5b521` (2024-01-24)
- 变更规模: commits=1, files=1, +14/-81 (churn=95)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=95)
- 复现: `git show 6dd936e5b52104461c03c60d1bf7e6bcc95def88`

### #805 arch-riscv: Fix vsadd_vi and vsaddu_vi to match v-spec

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/805
- 代表 commit: `7a96709b1141` (2024-01-24)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=4)
- 复现: `git show 7a96709b1141aecbb7e502435cdacc2204e56622`

### #781 arch-gcn3: Remove gcn3

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/781
- 代表 commit: `24e0d71034f6` (2024-01-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 24e0d71034f68ea11fa1074aa5c58d0390bb13df`

### #786 base: Fix Integer overflow in AddrRange

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/786
- 代表 commit: `1c0127ae7c2d` (2024-01-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1c0127ae7c2da9b8689c2a785b312c9eccb7c7b1`

### #739 misc: Update .mailmap file

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/739
- 代表 commit: `235f6bd43fb3` (2024-01-25)
- 变更规模: commits=1, files=1, +85/-45 (churn=130)
- 影响范围: topdirs=.mailmap; subsys=.mailmap; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.mailmap` (churn=130)
- 复现: `git show 235f6bd43fb3f0855197d9e51a42aa56b9c5dc4a`

### #806 dev-amdgpu: Limit SDMA NOP count to wptr boundary

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/806
- 代表 commit: `7f71477f154f` (2024-01-25)
- 变更规模: commits=1, files=1, +9/-1 (churn=10)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/amdgpu/sdma_engine.cc` (churn=10)
- 复现: `git show 7f71477f154f27688eae74b399fac23131c9601b`

### #767 misc: Added dependabot config file

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/767
- 代表 commit: `8a6804231c6e` (2024-01-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 8a6804231c6ec504580b217463eaa5fc711da943`

### #810 arch-arm: Replace CRYPTO extension with canonical names

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/810
- 代表 commit: `ce32d7c523ae` (2024-01-26)
- 变更规模: commits=1, files=3, +35/-28 (churn=63)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/regs/misc.cc` (churn=36)
  - `src/arch/arm/ArmSystem.py` (churn=19)
  - `src/arch/arm/ArmISA.py` (churn=8)
- 复现: `git show ce32d7c523ae5c9612dfd6242092a3630e301898`

### #814 arch-riscv: Fix RVV instructions vmsbf/vmsif/vmsof

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/814
- 代表 commit: `bb5d55510fb9` (2024-01-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bb5d55510fb91d53efc92b5591339ff3d087df14`

### #812 misc: move dependabot.yml to .github

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/812
- 代表 commit: `5a7d61d99084` (2024-01-29)
- 变更规模: commits=1, files=1, +0/-0 (churn=0)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/{workflows => }/dependabot.yml` (churn=0)
- 复现: `git show 5a7d61d99084247632efb8fd5b6d48d9dbbb9275`

### #653 util: add scripts that help maintain mongoDB

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/653
- 代表 commit: `c0100b18cce3` (2024-01-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c0100b18cce3e536dd0c4b9c407d86fd776bce06`

### #676 tests: Added tests for suites

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/676
- 代表 commit: `d1fca18eb37d` (2024-01-29)
- 变更规模: commits=1, files=3, +278/-0 (churn=278)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/suite_tests/configs/suite_run_workload.py` (churn=156)
  - `tests/gem5/suite_tests/test_suite.py` (churn=114)
  - `tests/gem5/suite_tests/README.md` (churn=8)
- 复现: `git show d1fca18eb37d53b1e6fcb7802b35f4be87fd81ec`

### #816 arch-riscv: Fix fence.i instruction in O3 CPU

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/816
- 代表 commit: `b3870ee7b021` (2024-01-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b3870ee7b021698e4c49deef1dac7f69a6c5ea48`

### #819 tests: remove GCN3_X86 from compiler tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/819
- 代表 commit: `76c3c02acba6` (2024-01-30)
- 变更规模: commits=1, files=1, +2/-3 (churn=5)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/compiler-tests.yaml` (churn=5)
- 复现: `git show 76c3c02acba6c307b8d5e5b177d13f3da2610fc7`

### #725 arm,stdlib: added kvm support to the ARM board

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/725
- 代表 commit: `b5d18b84a823` (2024-01-31)
- 变更规模: commits=2, files=3, +162/-1 (churn=163)
- 影响范围: topdirs=configs, src, util; subsys=configs, python, util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gem5_library/arm-ubuntu-run-with-kvm.py` (churn=139)
  - `src/python/gem5/components/boards/arm_board.py` (churn=21)
  - `util/m5/src/abi/arm64/SConsopts` (churn=3)
- commits 列表（按 topo-order，Top 12）：
  - 2024-01-31 `b5d18b84a823` arm,stdlib: added kvm support to the ARM board
  - 2024-03-28 `294dd6dd0128` util-m5: Add default M5OP_ADDR to arm64
- 复现: `git show b5d18b84a823e91a86c9186ead58c1943b43106e`

### #804 util: Update gcn-gpu to remove GCN3 add gfx902

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/804
- 代表 commit: `2ff57b09d878` (2024-01-31)
- 变更规模: commits=1, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/gcn-gpu/Dockerfile` (churn=1)
- 复现: `git show 2ff57b09d878f6f9a04b8f2ead8c2ba70763d588`

### #817 tests: Switch to vega_x86 from gcn3_x86 in daily tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/817
- 代表 commit: `b5fae2f6206d` (2024-02-01)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b5fae2f6206db1021d457884530dffa1cbf548e2`

### #762 cpu,stdlib: Updating strided generator

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/762
- 代表 commit: `b79fe82e5c4d` (2024-02-01)
- 变更规模: commits=1, files=8, +299/-49 (churn=348)
- 影响范围: topdirs=src; subsys=cpu, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/processors/strided_generator_core.py` (churn=106)
  - `src/python/gem5/components/processors/strided_generator.py` (churn=101)
  - `src/cpu/testers/traffic_gen/strided_gen.hh` (churn=50)
  - `src/cpu/testers/traffic_gen/strided_gen.cc` (churn=29)
  - `src/cpu/testers/traffic_gen/traffic_gen.cc` (churn=29)
  - `src/cpu/testers/traffic_gen/base.cc` (churn=25)
  - `src/cpu/testers/traffic_gen/base.hh` (churn=4)
  - `src/python/SConscript` (churn=4)
- 复现: `git show b79fe82e5c4d5ea4d2b0cfb144e279381268507c`

### #824 dev: Fix off-by-one in IDE controller PCI register allocation

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/824
- 代表 commit: `197be3a0ddb3` (2024-02-01)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/storage/ide_ctrl.hh` (churn=3)
- 复现: `git show 197be3a0ddb309e3b3ffc68c41a5cb9d214ea795`

### #831 misc: Merge develop .github dir into stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/831
- 代表 commit: `5b2766829b8c` (2024-02-01)
- 变更规模: commits=1, files=5, +120/-118 (churn=238)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/weekly-tests.yaml` (churn=101)
  - `.github/workflows/gpu-tests.yaml` (churn=95)
  - `.github/workflows/daily-tests.yaml` (churn=20)
  - `.github/dependabot.yml` (churn=17)
  - `.github/workflows/compiler-tests.yaml` (churn=5)
- 复现: `git show 5b2766829b8cb8294afed67a366e43850c2d4c0b`

### #829 arch-arm: Adopt new TranslationRegime data type in MMU translations

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/829
- 代表 commit: `33e62b8e8a02` (2024-02-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 33e62b8e8a02893c5e7941b7da9853cdca326f37`

### #840 tests: fix wget link for gpu tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/840
- 代表 commit: `858acacb2086` (2024-02-02)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=4)
- 复现: `git show 858acacb208665ecc20a8d1dbc7811cf24235cd9`

### #832 misc: bump pre-commit from 2.20.0 to 3.6.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/832
- 代表 commit: `ea3face87bd2` (2024-02-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ea3face87bd2a14608b197f9692e7d4411cc0295`

### #841 misc: Merge .github dir from develop to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/841
- 代表 commit: `c890e6b113a5` (2024-02-02)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=4)
- 复现: `git show c890e6b113a59caeec3eea9bafea4a03a471f89d`

### #844 arch-riscv: Fix control flow in VectorFloatMaskMacroConstructor

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/844
- 代表 commit: `85059a369ecd` (2024-02-05)
- 变更规模: commits=1, files=1, +1/-5 (churn=6)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=6)
- 复现: `git show 85059a369ecd1fcaab203a68b429be062e482b3b`

### #843 arch-riscv: Fix RVV instructions vmv.s.x/vfmv.s.f

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/843
- 代表 commit: `40ecdf5fb40a` (2024-02-05)
- 变更规模: commits=1, files=1, +7/-3 (churn=10)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=10)
- 复现: `git show 40ecdf5fb40a001872ac54767c89e63869424683`

### #837 misc: bump mypy from 1.5.1 to 1.8.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/837
- 代表 commit: `df83efe1295a` (2024-02-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show df83efe1295ab2c37e2c155dacb30582d07645fb`

### #836 misc: Update actions/checkout from v3 to v4

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/836
- 代表 commit: `6f1d9b47e9ce` (2024-02-05)
- 变更规模: commits=1, files=5, +24/-24 (churn=48)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/ci-tests.yaml` (churn=14)
  - `.github/workflows/daily-tests.yaml` (churn=14)
  - `.github/workflows/weekly-tests.yaml` (churn=14)
  - `.github/workflows/compiler-tests.yaml` (churn=4)
  - `.github/workflows/docker-build.yaml` (churn=2)
- 复现: `git show 6f1d9b47e9ce3c8e08a13dfb55babfbca10026bb`

### #833 misc: bump tqdm from 4.64.1 to 4.66.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/833
- 代表 commit: `61516e863f43` (2024-02-05)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=optional-requirements.txt; subsys=optional-requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `optional-requirements.txt` (churn=2)
- 复现: `git show 61516e863f43f862534079dd72a3fbed289ebf01`

### #795 systemc: Reduce unnecessary backdoor request in atomic transaction

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/795
- 代表 commit: `e4e359135eb5` (2024-02-05)
- 变更规模: commits=1, files=2, +43/-13 (churn=56)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/tlm_bridge/tlm_to_gem5.cc` (churn=54)
  - `src/systemc/tlm_bridge/tlm_to_gem5.hh` (churn=2)
- 复现: `git show e4e359135eb5c2f456800f212fe0c3ef52a43770`

### #848 arch-arm: Crypto instruction execution requires SIMD to be enabled

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/848
- 代表 commit: `05f93175a76e` (2024-02-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 05f93175a76e0d7b6872519ead99a8e80be69211`

### #835 sim: Updating Process::Map

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/835
- 代表 commit: `8efe6dc1bc17` (2024-02-05)
- 变更规模: commits=1, files=2, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/process.cc` (churn=2)
  - `src/sim/process.hh` (churn=2)
- 复现: `git show 8efe6dc1bc177ea7673c4ccd391bacf3ea22dcc9`

### #849 arch-arm: Remove unused/unimplemented TLB methods

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/849
- 代表 commit: `a60d6960c7ca` (2024-02-06)
- 变更规模: commits=1, files=1, +0/-8 (churn=8)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/tlb.hh` (churn=8)
- 复现: `git show a60d6960c7ca4f126f95f7beb2a8c0bbe998e981`

### #846 arch-riscv: Add BasePMAChecker to support customized PMA

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/846
- 代表 commit: `ba6c569b8de2` (2024-02-06)
- 变更规模: commits=1, files=9, +36/-15 (churn=51)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/pma_checker.hh` (churn=19)
  - `src/arch/riscv/PMAChecker.py` (churn=9)
  - `src/arch/riscv/pma_checker.cc` (churn=8)
  - `src/arch/riscv/RiscvTLB.py` (churn=4)
  - `src/arch/riscv/SConscript` (churn=3)
  - `src/arch/riscv/RiscvMMU.py` (churn=2)
  - `src/arch/riscv/mmu.hh` (churn=2)
  - `src/arch/riscv/pagetable_walker.hh` (churn=2)
- 复现: `git show ba6c569b8de23319647ace0947ad86aa3a8363ee`

### #847 tests: Allow pyunit tests to run on specific directories

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/847
- 代表 commit: `44aaebc49a5c` (2024-02-06)
- 变更规模: commits=1, files=1, +14/-1 (churn=15)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/run_pyunit.py` (churn=15)
- 复现: `git show 44aaebc49a5ca21d5a75be3ee252b677307758b0`

### #850 misc: Add 'workflow_dispatch' to daily tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/850
- 代表 commit: `c7426f9427e3` (2024-02-06)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=2)
- 复现: `git show c7426f9427e3f463d8b1d2e0972d151b76a05b66`

### #845 tests: move to obtain-resources from wget

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/845
- 代表 commit: `de0342128cc4` (2024-02-06)
- 变更规模: commits=1, files=2, +3/-7 (churn=10)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=8)
  - `.github/workflows/weekly-tests.yaml` (churn=2)
- 复现: `git show de0342128cc45741feefaf05fd1f9c24a24928b1`

### #813 arch-riscv: adding support for local interrupts

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/813
- 代表 commit: `f289f9e8b5f5` (2024-02-06)
- 变更规模: commits=1, files=6, +350/-128 (churn=478)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/interrupts.cc` (churn=248)
  - `src/arch/riscv/interrupts.hh` (churn=140)
  - `src/arch/riscv/faults.hh` (churn=49)
  - `src/arch/riscv/RiscvInterrupts.py` (churn=26)
  - `src/arch/riscv/regs/misc.hh` (churn=13)
  - `src/arch/riscv/SConscript` (churn=2)
- 复现: `git show f289f9e8b5f53585a7f870377898cd4b27f8fd6d`

### #855 stdlib: fix typo in error message

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/855
- 代表 commit: `4aecf9d35cae` (2024-02-06)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/memory/memory.py` (churn=2)
- 复现: `git show 4aecf9d35cae8a6418f7b01abfa6372382e8a85f`

### #794 arch-riscv: add unit-stride fault-only-first loads (i.e. vle*ff)

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/794
- 代表 commit: `804f1373252f` (2024-02-08)
- 变更规模: commits=1, files=7, +208/-4 (churn=212)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/insts/vector.cc` (churn=87)
  - `src/arch/riscv/isa/decoder.isa` (churn=32)
  - `src/arch/riscv/insts/vector.hh` (churn=24)
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=20)
  - `src/arch/riscv/isa/formats/vector_mem.isa` (churn=19)
  - `src/arch/riscv/faults.cc` (churn=18)
  - `src/arch/riscv/faults.hh` (churn=12)
- 复现: `git show 804f1373252ff1e4a93c48a1724bbb12ef616d3d`

### #856 util: Remove action runner add-apt-repo git-core/ppa

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/856
- 代表 commit: `b2d13ee63ae6` (2024-02-08)
- 变更规模: commits=1, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/github-runners-vagrant/provision_root.sh` (churn=1)
- 复现: `git show b2d13ee63ae68b3fbef10363f854b34c4c4fe5d7`

### #859 arch-riscv: Fix load and store to use EEW instead of SEW

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/859
- 代表 commit: `7fe1588546d5` (2024-02-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7fe1588546d5925b8d2daa94c2b2b9f7cc3abcbb`

### #830 arch-riscv: fix vl in mask load/store (i.e vlm.v/vsm.v)

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/830
- 代表 commit: `7d80658a39b6` (2024-02-08)
- 变更规模: commits=1, files=1, +2/-4 (churn=6)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=6)
- 复现: `git show 7d80658a39b693a956e8afc717eb93a31a3e183e`

### #857 mem-cache: Fix circular dependency in QoS mem

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/857
- 代表 commit: `fd3aac1518de` (2024-02-09)
- 变更规模: commits=1, files=2, +4/-6 (churn=10)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/qos/QoSMemSinkCtrl.py` (churn=7)
  - `src/mem/qos/QoSMemSinkInterface.py` (churn=3)
- 复现: `git show fd3aac1518dedd51f6cad8e4376347e9cdd24f54`

### #852 arch-vega,gpu-compute,mem-ruby: SQC Invalidation Support

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/852
- 代表 commit: `a840dda23ab0` (2024-02-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a840dda23ab054581e130f66398548fc9bc0d008`

### #842 cpu-o3: add PerThreadUnifiedThreadMap to O3 CPU

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/842
- 代表 commit: `b826d96f40e7` (2024-02-12)
- 变更规模: commits=1, files=6, +11/-6 (churn=17)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/cpu.hh` (churn=7)
  - `src/cpu/o3/commit.cc` (churn=2)
  - `src/cpu/o3/commit.hh` (churn=2)
  - `src/cpu/o3/rename.cc` (churn=2)
  - `src/cpu/o3/rename.hh` (churn=2)
  - `src/cpu/o3/rename_map.hh` (churn=2)
- 复现: `git show b826d96f40e7a5484bbdace4d4b3658bbc3ec91e`

### #866 arch-riscv: Remove unnecessary assert

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/866
- 代表 commit: `47c4dad86944` (2024-02-13)
- 变更规模: commits=1, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/interrupts.cc` (churn=1)
- 复现: `git show 47c4dad8694448c40435875309a8399abebd7fc2`

### #871 mem-cache: Fix possible crash in base prefetcher

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/871
- 代表 commit: `308fef6b467e` (2024-02-17)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=mem/cache/prefetch; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/prefetch/base.cc` (churn=4)
- 复现: `git show 308fef6b467ec5a82fcff2bdb38106ab8c397d4f`

### #828 cpu-o3, arch: Fix SMT bug arising from v23.0 and make gem5 more robust with SMT

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/828
- 代表 commit: `8759131df3e4` (2024-02-19)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 8759131df3e4af1080fb96c67d8708869c9d74c0`

### #878 dev-arm: Remove the dependency of Platform for ArmSigInterruptPin

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/878
- 代表 commit: `4e75e35a3339` (2024-02-20)
- 变更规模: commits=1, files=2, +3/-1 (churn=4)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/arm/base_gic.cc` (churn=3)
  - `src/dev/arm/Gic.py` (churn=1)
- 复现: `git show 4e75e35a3339349e63c461aaaf283e753316d06c`

### #877 arch-x86, cpu-kvm: initialize x87 FCW

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/877
- 代表 commit: `7ac97331992c` (2024-02-20)
- 变更规模: commits=1, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/process.cc` (churn=3)
- 复现: `git show 7ac97331992c402a7cf0ef2bc056571125f7deec`

### #873 arch-arm: Add FEAT_FGT trapping for debug registers

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/873
- 代表 commit: `c719ea960a18` (2024-02-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c719ea960a18d5484641f385781494a279c13a4a`

### #888 tests: Update checkpoint tests to new checkpoints

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/888
- 代表 commit: `0f79b15b2faa` (2024-02-21)
- 变更规模: commits=1, files=4, +4/-4 (churn=8)
- 影响范围: topdirs=tests, configs; subsys=tests, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gem5_library/checkpoints/simpoints-se-restore.py` (churn=2)
  - `tests/gem5/checkpoint_tests/configs/power-hello-restore-checkpoint.py` (churn=2)
  - `tests/gem5/checkpoint_tests/configs/x86-fs-restore-checkpoint.py` (churn=2)
  - `tests/gem5/checkpoint_tests/configs/x86-hello-restore-checkpoint.py` (churn=2)
- 复现: `git show 0f79b15b2faa01f70c1db7279c66823688932d26`

### #868 arch-riscv: Fix fflags behavior of float inst. in O3 CPU

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/868
- 代表 commit: `816ef46c78a9` (2024-02-22)
- 变更规模: commits=1, files=4, +24/-11 (churn=35)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.cc` (churn=21)
  - `src/arch/riscv/isa/formats/fp.isa` (churn=5)
  - `src/arch/riscv/regs/misc.hh` (churn=5)
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=4)
- 复现: `git show 816ef46c78a9a4b345eab77ff52710f9036de0a1`

### #890 stdlib: Add get_last_exit_event_code to get m5 exit status code

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/890
- 代表 commit: `47f3ad45d370` (2024-02-23)
- 变更规模: commits=1, files=1, +6/-0 (churn=6)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/simulator.py` (churn=6)
- 复现: `git show 47f3ad45d370359648654b5d8955376b3415c42d`

### #880 python,util: Fix SimObjectParams default constructor and destructor

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/880
- 代表 commit: `00ed1d30cfde` (2024-02-26)
- 变更规模: commits=1, files=1, +1/-2 (churn=3)
- 影响范围: topdirs=build_tools; subsys=build_tools; arch=-
- 主要改动文件（Top 8 by churn）:
  - `build_tools/sim_object_param_struct_hh.py` (churn=3)
- 复现: `git show 00ed1d30cfde05b921de1e61e7be1b5396a30e5e`

### #791 mem-ruby: Fix possible dirty line loss in CHI when ReadShared hit on UD line

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/791
- 代表 commit: `61ee36eee6b3` (2024-02-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 61ee36eee6b32a6aa01ea854d9ac700922d87772`

### #891 tests: Exit riscv_asmtest script with simulator status code

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/891
- 代表 commit: `521a7c1de02f` (2024-02-26)
- 变更规模: commits=1, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/asmtest/configs/riscv_asmtest.py` (churn=3)
- 复现: `git show 521a7c1de02f736361bc88621028ee7e6ae6cf16`

### #886 arch-riscv,dev: Update the PLIC implementation

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/886
- 代表 commit: `bcf455755e7a` (2024-02-26)
- 变更规模: commits=1, files=7, +155/-66 (churn=221)
- 影响范围: topdirs=src; subsys=dev, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/riscv/plic.cc` (churn=83)
  - `src/dev/riscv/Plic.py` (churn=45)
  - `src/python/gem5/components/boards/riscv_board.py` (churn=23)
  - `src/python/gem5/prebuilt/riscvmatched/riscvmatched_board.py` (churn=23)
  - `src/dev/riscv/plic.hh` (churn=22)
  - `src/python/gem5/components/boards/experimental/lupv_board.py` (churn=19)
  - `src/dev/riscv/HiFive.py` (churn=6)
- 复现: `git show bcf455755e7abf2fde3dddcb8a1c1917aa8a91ca`

### #875 configs: Ensure m5ops base doesn't overlap physical mem in KVM

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/875
- 代表 commit: `19901861708d` (2024-02-26)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/deprecated/example/se.py` (churn=2)
- 复现: `git show 19901861708d99184539a665edac132d8e003b23`

### #858 tests: Add compiler test for gcc 13

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/858
- 代表 commit: `920497c19fd4` (2024-02-26)
- 变更规模: commits=1, files=3, +61/-3 (churn=64)
- 影响范围: topdirs=util, .github; subsys=util, .github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/ubuntu-22.04_gcc_13-version/Dockerfile` (churn=53)
  - `.github/workflows/compiler-tests.yaml` (churn=6)
  - `util/dockerfiles/docker-compose.yaml` (churn=5)
- 复现: `git show 920497c19fd4616561fc4e702292111ea3caaf42`

### #889 mem: QoS q_policy assertions fix

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/889
- 代表 commit: `e5eea7efcca4` (2024-02-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e5eea7efcca4facbdcb9b8976fedd573f1a7557d`

### #861 util: update list_changes.py to support multiple Change-Ids

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/861
- 代表 commit: `4e12f2486b58` (2024-02-27)
- 变更规模: commits=1, files=1, +21/-27 (churn=48)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/maint/list_changes.py` (churn=48)
- 复现: `git show 4e12f2486b58a155595a85dc70ab11904446224d`

### #894 mem-ruby: Add missing transition for SLC writes to VIPER TCC

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/894
- 代表 commit: `8a28ca8ffb50` (2024-02-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 8a28ca8ffb500a3788428a09e0c08d35fd80f95b`

### #899 mem-ruby: Add categorization of bypassed atomics in TCC

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/899
- 代表 commit: `777ac91bb04d` (2024-02-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 777ac91bb04dfe1523dfe74b887c25f4fb89057e`

### #797 Increased packets sanity check limit to 1024

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/797
- 代表 commit: `0d79b5098b41` (2024-02-29)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/packet_queue.cc` (churn=4)
- 复现: `git show 0d79b5098b418273913937b627aad0152a1714e4`

### #892 sim-se, arch-x86: initialize max stack size from parameter

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/892
- 代表 commit: `69762e272edc` (2024-02-29)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/process.cc` (churn=2)
- 复现: `git show 69762e272edc586bde345229cc54b7999b795be9`

### #895 arch-vega: Implement accumulation offset

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/895
- 代表 commit: `db42aeb630cc` (2024-02-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show db42aeb630cc5e652ef450eaf7035054804c3aa3`

### #826 python: Adding fatal statement to notify user mistakes.

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/826
- 代表 commit: `9bd71bff0c04` (2024-02-29)
- 变更规模: commits=1, files=1, +15/-0 (churn=15)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/SimObject.py` (churn=15)
- 复现: `git show 9bd71bff0c046ecb69bf3a4fcc1bf51004b5cefa`

### #902 dev: RegisterBank addRegistersAt for fragmented reg banks

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/902
- 代表 commit: `c0e5d58a96d9` (2024-03-01)
- 变更规模: commits=1, files=2, +163/-12 (churn=175)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/reg_bank.test.cc` (churn=89)
  - `src/dev/reg_bank.hh` (churn=86)
- 复现: `git show c0e5d58a96d950fb308647097cd4af02d37c34e5`

### #904 stdlib: Fix initialization for self.pic.hart_config in lupv_board

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/904
- 代表 commit: `61adfa38b27a` (2024-03-01)
- 变更规模: commits=1, files=1, +3/-1 (churn=4)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/experimental/lupv_board.py` (churn=4)
- 复现: `git show 61adfa38b27aaf992502414639155b8eea1ed1b7`

### #903 sim-se: Catch None value if binary is not compatible with gem5

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/903
- 代表 commit: `fae5f5e00b3a` (2024-03-01)
- 变更规模: commits=1, files=1, +6/-0 (churn=6)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/Workload.py` (churn=6)
- 复现: `git show fae5f5e00b3a24e0b9dab5499408b6abbc7bcccd`

### #872 mem-cache: Prefetchers Improvements

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/872
- 代表 commit: `c1d5ffe7c7bf` (2024-03-04)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c1d5ffe7c7bf6f956f5b47e9b02512a3e7293546`

### #765 mem-cache: Add support for partitioning caches

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/765
- 代表 commit: `c57a6b0d595c` (2024-03-04)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c57a6b0d595cbeccd468e356e1e1d8a388c71c33`

### #869 arch-riscv: adding stats to show completed page walks

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/869
- 代表 commit: `676d571009cf` (2024-03-04)
- 变更规模: commits=1, files=2, +36/-1 (churn=37)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/pagetable_walker.cc` (churn=23)
  - `src/arch/riscv/pagetable_walker.hh` (churn=14)
- 复现: `git show 676d571009cf32797bb0390c5079b40e450dc27e`

### #908 misc: Tag checkpoints with the ISA of the CPUs

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/908
- 代表 commit: `b930c57d54b4` (2024-03-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b930c57d54b4b5de06b75a0c41479065d3695d7e`

### #912 misc: Copy the develop .github dir to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/912
- 代表 commit: `650b92124bd8` (2024-03-05)
- 变更规模: commits=1, files=5, +32/-34 (churn=66)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=24)
  - `.github/workflows/weekly-tests.yaml` (churn=16)
  - `.github/workflows/ci-tests.yaml` (churn=14)
  - `.github/workflows/compiler-tests.yaml` (churn=10)
  - `.github/workflows/docker-build.yaml` (churn=2)
- 复现: `git show 650b92124bd8ed4d4716a5112d2f004fd2a5dbc0`

### #851 arch-riscv: adding vector unit-stride segment loads to RISC-V

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/851
- 代表 commit: `f6c61836b3cb` (2024-03-06)
- 变更规模: commits=1, files=9, +683/-28 (churn=711)
- 影响范围: topdirs=src; subsys=arch, cpu, cpu/minor; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=296)
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=232)
  - `src/arch/riscv/insts/vector.cc` (churn=97)
  - `src/arch/riscv/insts/vector.hh` (churn=59)
  - `src/arch/riscv/isa/formats/vector_mem.isa` (churn=13)
  - `src/arch/riscv/utility.hh` (churn=11)
  - `src/cpu/FuncUnit.py` (churn=1)
  - `src/cpu/minor/BaseMinorCPU.py` (churn=1)
- 复现: `git show f6c61836b3cbe06eb1da56a28dc0b542ad052c0f`

### #906 misc: bump tqdm from 4.66.1 to 4.66.2

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/906
- 代表 commit: `ceee8fed2964` (2024-03-06)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=optional-requirements.txt; subsys=optional-requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `optional-requirements.txt` (churn=2)
- 复现: `git show ceee8fed2964b5c7a71bd04486ac1c8479290076`

### #905 misc: bump pre-commit from 3.6.0 to 3.6.2

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/905
- 代表 commit: `f35815cd48a0` (2024-03-06)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show f35815cd48a0716452a88594563c2c5131d69424`

### #853 build(deps): bump cryptography from 39.0.2 to 42.0.0 in /util/gem5-resources-manager

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/853
- 代表 commit: `f70dc88c8a18` (2024-03-07)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=2)
- 复现: `git show f70dc88c8a18630ae1663dc6c6ec23c882ce368f`

### #920 dev-arm: Handle translation aborts and add IRQ support to the SMMU

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/920
- 代表 commit: `bbde68c08c3e` (2024-03-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bbde68c08c3e1b3e0ffb7f884f8488efb9d301d1`

### #922 READ_MODIFY_WRITE flag fix

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/922
- 代表 commit: `942979162a4e` (2024-03-11)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/request.hh` (churn=2)
- 复现: `git show 942979162a4eb6e841e3b87e1e182b8ff32f0519`

### #924 misc: Fix weekly tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/924
- 代表 commit: `85a20773c712` (2024-03-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 85a20773c71210f124a21a5351ae5b70dae80bbd`

### #929 build(deps): bump cryptography from 42.0.0 to 42.0.4 in /util/gem5-resources-manager

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/929
- 代表 commit: `6f90feca5642` (2024-03-11)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=2)
- 复现: `git show 6f90feca5642274f246977504545645fead3c22d`

### #928 misc: Sync stable .github dir with develop

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/928
- 代表 commit: `e8bc4fc137a5` (2024-03-11)
- 变更规模: commits=1, files=1, +2/-4 (churn=6)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/weekly-tests.yaml` (churn=6)
- 复现: `git show e8bc4fc137a5a7e82b601432271a027b652ae69b`

### #934 dev-arm: Fix SMMUv3 DTB autogen

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/934
- 代表 commit: `0ec8cf8d05bb` (2024-03-14)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/arm/SMMUv3.py` (churn=2)
- 复现: `git show 0ec8cf8d05bb76b16d41d799d99323867c50c885`

### #938 mem: Fix callback of functional access in port wrapper

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/938
- 代表 commit: `84da503d3764` (2024-03-18)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/port_wrapper.cc` (churn=2)
- 复现: `git show 84da503d37645277669d520844f79c6fcf2150e7`

### #914 arch-riscv: Move alignment check to Physical Memory Attribute(PMA)

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/914
- 代表 commit: `dbae09e4d9fb` (2024-03-18)
- 变更规模: commits=1, files=13, +221/-77 (churn=298)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/pma_checker.cc` (churn=90)
  - `src/arch/riscv/pma_checker.hh` (churn=47)
  - `src/arch/riscv/isa/formats/amo.isa` (churn=36)
  - `src/arch/riscv/isa/formats/mem.isa` (churn=36)
  - `src/arch/riscv/tlb.cc` (churn=27)
  - `src/arch/riscv/mmu.hh` (churn=17)
  - `src/arch/riscv/pagetable_walker.cc` (churn=17)
  - `src/arch/riscv/insts/static_inst.cc` (churn=13)
- 复现: `git show dbae09e4d9fb377803cb48a1af8edff0cdaa92d1`

### #935 stdlib, tests, configs: Add a new PrivateL1PrivateL2WalkCache hierarchy

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/935
- 代表 commit: `2b67d0eba6a9` (2024-03-19)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2b67d0eba6a9dfa12817776fb681b293ee293b4c`

### #939 gpu-compute: Support cache line sizes >64B in GPUFS

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/939
- 代表 commit: `ba2f5615bae8` (2024-03-20)
- 变更规模: commits=1, files=2, +11/-3 (churn=14)
- 影响范围: topdirs=configs, src; subsys=configs, gpu-compute; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/gpu-compute/gpu_command_processor.cc` (churn=12)
  - `configs/example/gpufs/Disjoint_VIPER.py` (churn=2)
- 复现: `git show ba2f5615bae872efa4d94bb3400e42e6b568921d`

### #940 gpu-compute: Add support for skipping GPU kernels

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/940
- 代表 commit: `acd9d3ff940a` (2024-03-21)
- 变更规模: commits=1, files=8, +111/-20 (churn=131)
- 影响范围: topdirs=src, configs; subsys=gpu-compute, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gpufs/runfs.py` (churn=50)
  - `src/gpu-compute/gpu_command_processor.cc` (churn=45)
  - `configs/example/gpufs/system/system.py` (churn=12)
  - `src/gpu-compute/gpu_command_processor.hh` (churn=6)
  - `src/gpu-compute/shader.cc` (churn=6)
  - `src/gpu-compute/shader.hh` (churn=6)
  - `src/gpu-compute/GPU.py` (churn=4)
  - `src/gpu-compute/dispatcher.cc` (churn=2)
- 复现: `git show acd9d3ff940a3fae80586cc89e7d261b8581cd2e`

### #952 misc: Add ".DS_Store" to .gitignore

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/952
- 代表 commit: `4c333975924a` (2024-03-21)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=.gitignore; subsys=.gitignore; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.gitignore` (churn=1)
- 复现: `git show 4c333975924a3eb12a23b585a9383d7a8bee3898`

### #901 tests: Update tests to use specific resource versions

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/901
- 代表 commit: `76965c6431a8` (2024-03-21)
- 变更规模: commits=1, files=18, +84/-24 (churn=108)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/checkpoint_tests/configs/x86-fs-restore-checkpoint.py` (churn=12)
  - `tests/gem5/checkpoint_tests/configs/x86-hello-restore-checkpoint.py` (churn=9)
  - `tests/gem5/kvm_fork_tests/configs/boot_kvm_fork_run.py` (churn=8)
  - `tests/gem5/kvm_switch_tests/configs/boot_kvm_switch_exit.py` (churn=8)
  - `tests/gem5/parsec_benchmarks/configs/parsec_disk_run.py` (churn=8)
  - `tests/gem5/checkpoint_tests/configs/arm-hello-save-checkpoint.py` (churn=7)
  - `tests/gem5/checkpoint_tests/configs/sparc-hello-save-checkpoint.py` (churn=7)
  - `tests/gem5/checkpoint_tests/configs/x86-hello-save-checkpoint.py` (churn=7)
- 复现: `git show 76965c6431a8c00f0be8e940e082fe455c84a271`

### #953 arch-vega: Implement flat_load_sbyte instruction

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/953
- 代表 commit: `803dbbfdacb4` (2024-03-21)
- 变更规模: commits=1, files=1, +27/-1 (churn=28)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/flat.cc` (churn=28)
- 复现: `git show 803dbbfdacb46f05347f3176ec3417ccbfc02a63`

### #950 arch-vega: Various vega fixes to enable nanogpt

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/950
- 代表 commit: `dca040983b60` (2024-03-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show dca040983b60ba6be48b4e44b35e1c47ec249212`

### #926 dev-amdgpu: Support for ROCm 6.0

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/926
- 代表 commit: `7d62da6d10fe` (2024-03-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7d62da6d10fe54b9a9967e4eaae9554a75990c06`

### #913 arch-riscv: adding vector unit-stride segment stores to RISC-V

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/913
- 代表 commit: `1e743fd85ab5` (2024-03-22)
- 变更规模: commits=1, files=8, +555/-42 (churn=597)
- 影响范围: topdirs=src; subsys=arch, cpu; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=229)
  - `src/arch/riscv/isa/decoder.isa` (churn=180)
  - `src/arch/riscv/insts/vector.cc` (churn=111)
  - `src/arch/riscv/insts/vector.hh` (churn=59)
  - `src/arch/riscv/isa/formats/vector_mem.isa` (churn=13)
  - `src/arch/riscv/utility.hh` (churn=2)
  - `src/cpu/op_class.hh` (churn=2)
  - `src/cpu/FuncUnit.py` (churn=1)
- 复现: `git show 1e743fd85ab55ea75301f2fed20243387806fe72`

### #956 sim-se,cpu-kvm: Fix SE workload setup on KVM CPUs

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/956
- 代表 commit: `dd5a30d41ef6` (2024-03-23)
- 变更规模: commits=1, files=1, +9/-0 (churn=9)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/se_binary_workload.py` (churn=9)
- 复现: `git show dd5a30d41ef6da7ea6f879ded282084552ee8d87`

### #975 arch: Add getIsaName in BaseISA

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/975
- 代表 commit: `896c32cd0d47` (2024-03-28)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=arch; arch=generic
- 主要改动文件（Top 8 by churn）:
  - `src/arch/generic/isa.hh` (churn=1)
- 复现: `git show 896c32cd0d47f59e968e0cbc48f9289d10d42791`

### #976 dev: Remove duplicate virtio files

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/976
- 代表 commit: `63706f04b59e` (2024-03-28)
- 变更规模: commits=1, files=2, +0/-191 (churn=191)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/virtio/rng 2.hh` (churn=97)
  - `src/dev/virtio/rng 2.cc` (churn=94)
- 复现: `git show 63706f04b59eddc982443efc1ee3681c3c93d3fc`

### #864 stdlib: add socks proxy to atlas client

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/864
- 代表 commit: `9207458fd771` (2024-03-28)
- 变更规模: commits=1, files=4, +68/-23 (churn=91)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/utils/socks_ssl_context.py` (churn=58)
  - `src/python/gem5/resources/downloader.py` (churn=28)
  - `src/python/gem5/resources/client_api/atlasclient.py` (churn=3)
  - `src/python/SConscript` (churn=2)
- 复现: `git show 9207458fd771e402cc98f67806dc83f5a0f79a0c`

### #863 arch-riscv: This commit fixes bug in vfmv.f.s impl. in riscv

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/863
- 代表 commit: `ec690de0da53` (2024-03-29)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=2)
- 复现: `git show ec690de0da53d9604cc24cb9384ea66b10228cb3`

### #887 sim-se: Implement statx system call for Linux x86-64

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/887
- 代表 commit: `00d4b6825c69` (2024-04-01)
- 变更规模: commits=1, files=3, +152/-1 (churn=153)
- 影响范围: topdirs=src; subsys=arch, sim; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/sim/syscall_emul.hh` (churn=108)
  - `src/arch/x86/linux/linux.hh` (churn=43)
  - `src/arch/x86/linux/syscall_tbl64.cc` (churn=2)
- 复现: `git show 00d4b6825c697a7c2b56d86ee54dc033c3c87794`

### #955 arch-vega: Operand selectors for accumulation registers

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/955
- 代表 commit: `78cf39bf634d` (2024-04-01)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/gpu_registers.hh` (churn=2)
- 复现: `git show 78cf39bf634d12f646ea0f8af63cd093fbaee6a8`

### #977 util-m5: Add default M5OP_ADDR to arm64

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/977
- 代表 commit: `ed5ffee49c62` (2024-04-01)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ed5ffee49c6288e89a376966ad4cb954d4bb019a`

### #985 arch-riscv: Use TeX's escape seq in Python instead of Unicode

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/985
- 代表 commit: `628826896f96` (2024-04-02)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/RiscvInterrupts.py` (churn=2)
- 复现: `git show 628826896f960d2afa9c790d9bf9f4bce3833f9e`

### #979 misc,github: Upgrade checkout and upload/download-artifact Actions to latest version

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/979
- 代表 commit: `dea8fc0ee849` (2024-04-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show dea8fc0ee849c77d34b4af90c5ceb4ea8690fe0b`

### #959 base: Fix 'doGzipLoad' str manipulation

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/959
- 代表 commit: `c238b7a3e0e9` (2024-04-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c238b7a3e0e9ed54ac3a987e2a0aea676f33af89`

### #989 sim-se: Fix copyOutStatxBuf compile error

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/989
- 代表 commit: `32ee09df4a39` (2024-04-02)
- 变更规模: commits=1, files=1, +26/-34 (churn=60)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/syscall_emul.hh` (churn=60)
- 复现: `git show 32ee09df4a39705facff2a1af65102814f42475b`

### #987 misc: Sync develop .github to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/987
- 代表 commit: `ee6f1377d7c5` (2024-04-02)
- 变更规模: commits=1, files=4, +25/-25 (churn=50)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=28)
  - `.github/workflows/weekly-tests.yaml` (churn=12)
  - `.github/workflows/ci-tests.yaml` (churn=6)
  - `.github/workflows/docker-build.yaml` (churn=4)
- 复现: `git show ee6f1377d7c54422137dfa47cd4d73407814867d`

### #986 arch-arm,stdlib: ARM release for_kvm is moved to configs

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/986
- 代表 commit: `28b081b34844` (2024-04-03)
- 变更规模: commits=1, files=2, +4/-9 (churn=13)
- 影响范围: topdirs=configs, src; subsys=configs, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/arm_board.py` (churn=7)
  - `configs/example/gem5_library/arm-ubuntu-run-with-kvm.py` (churn=6)
- 复现: `git show 28b081b34844b3e393219a8dd5f5d38662bf9af3`

### #984 misc: bump pre-commit from 3.6.2 to 3.7.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/984
- 代表 commit: `514b759d63a6` (2024-04-03)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 514b759d63a642f10b2a1f0e774c8c07fa350d33`

### #964 arch-riscv: Fix the RiscvBareMetal parameter reset_vect

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/964
- 代表 commit: `1fa25a60c810` (2024-04-03)
- 变更规模: commits=1, files=2, +12/-2 (churn=14)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/bare_metal/fs_workload.cc` (churn=9)
  - `src/arch/riscv/RiscvFsWorkload.py` (churn=5)
- 复现: `git show 1fa25a60c810c3fee56b414b763906ae9122b10f`

### #945 mem-ruby: Copyback UD_RU line when evicted in CHI protocol

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/945
- 代表 commit: `ffd0680a2c3b` (2024-04-03)
- 变更规模: commits=1, files=2, +26/-11 (churn=37)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` (churn=29)
  - `src/mem/ruby/protocol/chi/CHI-cache-transitions.sm` (churn=8)
- 复现: `git show ffd0680a2c3bcfb850da531f005381c94e311011`

### #993 python: Add is_subset to the AddrRange param class

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/993
- 代表 commit: `0c6543d781ae` (2024-04-04)
- 变更规模: commits=1, files=1, +4/-1 (churn=5)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/params.py` (churn=5)
- 复现: `git show 0c6543d781ae08e27ca9745bacc8ad5d55e0a0fb`

### #972 stdlib: Fix 'nozero' for Scalar SimStats

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/972
- 代表 commit: `4ff34a75bb86` (2024-04-04)
- 变更规模: commits=1, files=2, +8/-1 (churn=9)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/stats/gem5stats.py` (churn=6)
  - `src/python/pybind11/stats.cc` (churn=3)
- 复现: `git show 4ff34a75bb866fc673608526c69d4ba2b3844c48`

### #971 stdlib: Specify typing for SimStat Scalar value

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/971
- 代表 commit: `213b4183918a` (2024-04-04)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/ext/pystats/statistic.py` (churn=2)
- 复现: `git show 213b4183918ac3a150fdc8921f0bb01ad017ece9`

### #970 stdlib: Move SimStat 'unit' and 'datatype' field to Scalar

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/970
- 代表 commit: `8d7e3fb16b4d` (2024-04-04)
- 变更规模: commits=1, files=2, +4/-25 (churn=29)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/ext/pystats/statistic.py` (churn=24)
  - `src/python/m5/stats/gem5stats.py` (churn=5)
- 复现: `git show 8d7e3fb16b4d8183a52d2ce5e9b690cba99049ff`

### #983 misc: bump mypy from 1.8.0 to 1.9.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/983
- 代表 commit: `9b143930b62c` (2024-04-04)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 9b143930b62c4cd24e874caf08da873ef5779e9b`

### #951 scons: Disable Address Sanitizer for GCC

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/951
- 代表 commit: `be00691cd3fa` (2024-04-04)
- 变更规模: commits=1, files=1, +20/-14 (churn=34)
- 影响范围: topdirs=SConstruct; subsys=SConstruct; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=34)
- 复现: `git show be00691cd3fa71c57220803c45c308103bdc7191`

### #973 stdlib,tests: Add StatTester SimObject and Scalar tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/973
- 代表 commit: `c65071282d00` (2024-04-04)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c65071282d007b3bde1778ef126af444b33c23ac`

### #1005 arch-riscv: Fix c.fsw source register

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1005
- 代表 commit: `71b0b1f2b68f` (2024-04-08)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=2)
- 复现: `git show 71b0b1f2b68f5d4245f132ebe05ce9484a9875b1`

### #998 arch-riscv: fix c.fswsp source register

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/998
- 代表 commit: `841b82126133` (2024-04-08)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=3)
- 复现: `git show 841b8212613344cd599c56749798cfc6108920d1`

### #900 arch-riscv,sim: m5ops argument / return fix for 32 bit RISC-V

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/900
- 代表 commit: `a8d778516db9` (2024-04-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a8d778516db94060cb7fd09fd8699e547c6cc834`

### #966 mem-cache, configs, arch-arm: Handle partitioning policies through a PartitionManager

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/966
- 代表 commit: `3af15a535e42` (2024-04-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3af15a535e422b1a89d34bf1d8a189d650ff1e55`

### #967 stdlib: Add tree structure to the AbstractCacheHierarchy

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/967
- 代表 commit: `5641c5e4642f` (2024-04-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5641c5e4642f7d44651f783bdb018d3cf8ba01b5`

### #1010 arch-riscv: Remove a tab character

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1010
- 代表 commit: `bc3627d6822b` (2024-04-10)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/bitfields.isa` (churn=2)
- 复现: `git show bc3627d6822b1537430c290439aea922b02eec4a`

### #1006 arch-riscv: Make c.flwsp destination register more maintainable

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1006
- 代表 commit: `116c483a42d6` (2024-04-10)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=2)
- 复现: `git show 116c483a42d6b5681614198c500261ed2e429528`

### #745 Add a generic cache template library

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/745
- 代表 commit: `3b5ae7b4d17b` (2024-04-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3b5ae7b4d17ba9c2a72ff83271b8b54208580e1e`

### #589 cpu,arch-arm,arch-riscv: adding new instruction types to RISC-V

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/589
- 代表 commit: `db1c336237d7` (2024-04-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show db1c336237d7371fd0dfcfe3d2c5e0e5c7e18ed1`

### #1007 cpu-o3, arch-x86: initialize interrupts for all SMT threads

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1007
- 代表 commit: `bc39283451c6` (2024-04-11)
- 变更规模: commits=1, files=1, +12/-8 (churn=20)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/BaseCPU.py` (churn=20)
- 复现: `git show bc39283451c6b563f8db6660ce694e838a3b38b0`

### #1013 cpu: Fix KVM false negative warning after Kconfig transition

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1013
- 代表 commit: `ebb70dea99c4` (2024-04-12)
- 变更规模: commits=1, files=2, +11/-3 (churn=14)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/kvm/Kconfig` (churn=7)
  - `src/cpu/kvm/SConsopts` (churn=7)
- 复现: `git show ebb70dea99c43da510aa561741a56f1786855585`

### #911 misc: Add a DevContainer specification to the gem5 repo

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/911
- 代表 commit: `392a2b4ffa33` (2024-04-12)
- 变更规模: commits=1, files=4, +164/-1 (churn=165)
- 影响范围: topdirs=.devcontainer, util; subsys=.devcontainer, util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/devcontainer/Dockerfile` (churn=80)
  - `.devcontainer/on-create.sh` (churn=38)
  - `.devcontainer/devcontainer.json` (churn=37)
  - `util/dockerfiles/docker-bake.hcl` (churn=10)
- 复现: `git show 392a2b4ffa330ba3208d1e0af27722891f695ce8`

### #1017 util-docker: Update docker-compose URLs to 'ghcr.io/gem5'

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1017
- 代表 commit: `bdaeb082c36a` (2024-04-13)
- 变更规模: commits=1, files=1, +24/-24 (churn=48)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/docker-compose.yaml` (churn=48)
- 复现: `git show bdaeb082c36abff63ffc87218540c168181ae151`

### #1015 util: Update resource manager dependencies

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1015
- 代表 commit: `dbb71948ce9e` (2024-04-15)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=4)
- 复现: `git show dbb71948ce9e527e3945544c150e2369554a0a2e`

### #1027 misc: bump dnspython in /util/gem5-resource-manager

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1027
- 代表 commit: `a7330ac4fbab` (2024-04-15)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=2)
- 复现: `git show a7330ac4fbab5ee3eb39f6b5d67fa66603220d8b`

### #1030 dev-arm: Do not mark the MpamMSC as abstract

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1030
- 代表 commit: `bdcffdd0f031` (2024-04-15)
- 变更规模: commits=1, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/arm/Mpam.py` (churn=1)
- 复现: `git show bdcffdd0f03159f5629f4d76abe488f07f994037`

### #1022 github: Update 'ubuntu-22.04' to 'ubuntu-latest'

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/1022
- 代表 commit: `630f3822b860` (2024-04-15)
- 变更规模: commits=1, files=4, +6/-6 (churn=12)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/ci-tests.yaml` (churn=6)
  - `.github/workflows/compiler-tests.yaml` (churn=2)
  - `.github/workflows/daily-tests.yaml` (churn=2)
  - `.github/workflows/weekly-tests.yaml` (churn=2)
- 复现: `git show 630f3822b86093f869b3411a0b1d5d70358d32f2`

### #1011 mem,gpu-compute: Implement GPU TCC directed invalidate

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1011
- 代表 commit: `7e2d8dee426a` (2024-04-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7e2d8dee426a079ae8ea404c03a59495b2e7fe20`

### #1023 arch-vega: Fix output warnings, gem5.fast

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1023
- 代表 commit: `a03319bef75e` (2024-04-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a03319bef75e300e2f4778f4be873f0f5bcade2a`

### #1018 tests,util-docker,github: Add Ubuntu 24.04 Docker image & updated tests/actions to use it

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1018
- 代表 commit: `56a2346b8d93` (2024-04-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 56a2346b8d93f602b7de698a7793ffc4b019f127`

### #1021 tests,github: Update CI Tests' GitHub Actions versions

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1021
- 代表 commit: `1aa0bf8ec60d` (2024-04-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1aa0bf8ec60dd699c1f114e09619f35e90137d31`

### #1025 util-docker: Bump gpu-fs build docker to ROCm 6.0.2

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1025
- 代表 commit: `9b463dbdfda0` (2024-04-15)
- 变更规模: commits=1, files=2, +56/-14 (churn=70)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/gpu-fs/Dockerfile` (churn=37)
  - `util/dockerfiles/gpu-fs/README.md` (churn=33)
- 复现: `git show 9b463dbdfda06a1c9157d8eb56b957605a5f1453`

### #931 tests,arch-riscv: update bitmanip asmtest binaries

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/931
- 代表 commit: `6b4dbdcedbfc` (2024-04-16)
- 变更规模: commits=1, files=1, +75/-75 (churn=150)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/asmtest/tests.py` (churn=150)
- 复现: `git show 6b4dbdcedbfcbfcb04bf58c48f0b01a20da90cf0`

### #1035 cpu: Fix Ruby/x86 pio port connections

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1035
- 代表 commit: `c13aa7727daf` (2024-04-17)
- 变更规模: commits=1, files=1, +11/-6 (churn=17)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/BaseCPU.py` (churn=17)
- 复现: `git show c13aa7727daf616ae72f473b957055374e24758a`

### #1009 dev: Fix interrupt logic in uart8250

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1009
- 代表 commit: `84cba2a8a8c4` (2024-04-17)
- 变更规模: commits=1, files=2, +54/-29 (churn=83)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/serial/uart8250.cc` (churn=82)
  - `src/dev/serial/uart8250.hh` (churn=1)
- 复现: `git show 84cba2a8a8c45147c81b0426ccdead6472346b4f`

### #1024 arch-x86: Movfp account for dataSize=4

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1024
- 代表 commit: `c44b8635aba3` (2024-04-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c44b8635aba37fbcd569a70d57c3431bfcfd24e5`

### #994 mem-ruby: Implement no_alloc Far Atomics in CHI

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/994
- 代表 commit: `42ffa5290776` (2024-04-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 42ffa52907765ce758664594c001c6745c812d94`

### #1038 misc: Fix jq install for testlib-quick-matrix

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1038
- 代表 commit: `cbf0334762b7` (2024-04-18)
- 变更规模: commits=1, files=1, +3/-3 (churn=6)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/ci-tests.yaml` (churn=6)
- 复现: `git show cbf0334762b75f9508bf0c8aa1a17938c4b18da2`

### #1026 github,tests: Add Pyunit tests to CI GitHub Action Workflow

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1026
- 代表 commit: `e578f83739e5` (2024-04-19)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e578f83739e54ed0fdbb7a68dfce688a4080760e`

### #1043 misc: Merge .github develop dir to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/1043
- 代表 commit: `0b2fa9900b37` (2024-04-19)
- 变更规模: commits=1, files=5, +62/-25 (churn=87)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/ci-tests.yaml` (churn=63)
  - `.github/workflows/daily-tests.yaml` (churn=10)
  - `.github/workflows/weekly-tests.yaml` (churn=8)
  - `.github/workflows/compiler-tests.yaml` (churn=4)
  - `.github/workflows/docker-build.yaml` (churn=2)
- 复现: `git show 0b2fa9900b3754787b4fd564937b8c145bcfbd5d`

### #1041 configs: GPUFS: Turn off SSE4 and fancy XSAVEs

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1041
- 代表 commit: `c54039da5bfe` (2024-04-20)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gpufs/system/system.py` (churn=4)
- 复现: `git show c54039da5bfe9d2101bfa3889252a15230053b11`

### #1048 misc,tests: Remove zip step from Workflows

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1048
- 代表 commit: `dd2689905fcb` (2024-04-21)
- 变更规模: commits=1, files=3, +7/-22 (churn=29)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=18)
  - `.github/workflows/weekly-tests.yaml` (churn=9)
  - `.github/workflows/ci-tests.yaml` (churn=2)
- 复现: `git show dd2689905fcb77a65f190705ba5f2b793535e28c`

### #1051 misc: Sync stable .github with develop

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/1051
- 代表 commit: `115322319c70` (2024-04-21)
- 变更规模: commits=1, files=3, +7/-22 (churn=29)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=18)
  - `.github/workflows/weekly-tests.yaml` (churn=9)
  - `.github/workflows/ci-tests.yaml` (churn=2)
- 复现: `git show 115322319c709a6e2a12e8b5c5299aa813d94633`

### #1046 util: Enable m5term Apple Mac OS Compilation

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1046
- 代表 commit: `40fdf368d8e9` (2024-04-22)
- 变更规模: commits=1, files=1, +5/-1 (churn=6)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/term/term.c` (churn=6)
- 复现: `git show 40fdf368d8e96be2296cd6b695ad57df644df9b8`

### #779 stdlib: Enable bundled resource requests from the databases

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/779
- 代表 commit: `97a05304527f` (2024-04-22)
- 变更规模: commits=1, files=8, +655/-504 (churn=1159)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/resource.py` (churn=355)
  - `src/python/gem5/resources/client_api/client_wrapper.py` (churn=335)
  - `src/python/gem5/resources/client.py` (churn=224)
  - `src/python/gem5/resources/client_api/jsonclient.py` (churn=67)
  - `src/python/gem5/resources/client_api/abstract_client.py` (churn=62)
  - `src/python/gem5/resources/client_api/client_query.py` (churn=57)
  - `src/python/gem5/resources/client_api/atlasclient.py` (churn=55)
  - `src/python/SConscript` (churn=4)
- 复现: `git show 97a05304527f0d1112ff7aee20e22a9de11760f9`

### #1067 stdlib: Tests Fix/Disable pyunit tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1067
- 代表 commit: `9f5c97c7fd37` (2024-04-23)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 9f5c97c7fd3721ee52640bad893eb643485aeb82`

### #1061 mem-cache: Remove power-of-2 requirement for TreePLRU num leaves

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1061
- 代表 commit: `ed8a09303a33` (2024-04-24)
- 变更规模: commits=1, files=1, +1/-2 (churn=3)
- 影响范围: topdirs=src; subsys=mem/cache/rp; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/replacement_policies/tree_plru_rp.cc` (churn=3)
- 复现: `git show ed8a09303a33e0bbd3364a1e63ca50464961cb9e`

### #1060 arch-arm: Refactor PTW

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/1060
- 代表 commit: `cc3655cdadbf` (2024-04-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show cc3655cdadbf8fba6293935e1d19f5fe584b46a8`

### #1063 tests: Fix gem5 testlib compilation

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1063
- 代表 commit: `b83a53e521ce` (2024-04-24)
- 变更规模: commits=1, files=1, +17/-13 (churn=30)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/fixture.py` (churn=30)
- 复现: `git show b83a53e521ce8bb7588d886b5f37b602ba9aad7d`

### #1069 tests: Revert "tests: Move the arm+ruby tests to not use ALL"

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1069
- 代表 commit: `01602cdf1390` (2024-04-24)
- 变更规模: commits=1, files=1, +4/-34 (churn=38)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/fs/linux/arm/test.py` (churn=38)
- 复现: `git show 01602cdf13905c218ab15748755b25f1a56185e5`

### #1065 cpu-kvm: Support perf counters on hybrid host architectures

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1065
- 代表 commit: `85d21b57189b` (2024-04-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 85d21b57189bba624e11eb2e6598be920f851cac`

### #1072 arch-arm: Add misc_accessor templated functions to read/write regs at different ELs

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1072
- 代表 commit: `83e55743e1f7` (2024-04-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 83e55743e1f792692b8a2fe41081ffa35aa07464`

### #1070 tests: fix persistence issue in pyunit tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1070
- 代表 commit: `d75afeabb152` (2024-04-25)
- 变更规模: commits=1, files=5, +812/-714 (churn=1526)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/pyunit/stdlib/resources/pyunit_client_wrapper_checks.py` (churn=580)
  - `tests/pyunit/stdlib/resources/pyunit_resource_specialization.py` (churn=438)
  - `tests/pyunit/stdlib/resources/pyunit_suite_checks.py` (churn=235)
  - `tests/pyunit/stdlib/resources/pyunit_workload_checks.py` (churn=167)
  - `tests/pyunit/stdlib/resources/pyunit_obtain_resources_check.py` (churn=106)
- 复现: `git show d75afeabb152dbd855a571b8533eb3a5a30ee8ae`

### #1045 mem-ruby: Fix functional reads for MESI Three-Level messages

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1045
- 代表 commit: `66decb2e931b` (2024-04-25)
- 变更规模: commits=1, files=1, +3/-2 (churn=5)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/MESI_Three_Level-msg.sm` (churn=5)
- 复现: `git show 66decb2e931bbb50bc218eef9e9c3daa145032e8`

### #1047 cpu-o3: Clear current macro-op in fetch if squashing after last micro-op

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1047
- 代表 commit: `51d546cb06fe` (2024-04-25)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/fetch.cc` (churn=3)
- 复现: `git show 51d546cb06fee62080ce2eb1d1b6f5b0efe38c62`

### #1056 cpu-o3: prioritize exiting threads when committing

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1056
- 代表 commit: `c679c9c12746` (2024-04-25)
- 变更规模: commits=1, files=1, +18/-0 (churn=18)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/commit.cc` (churn=18)
- 复现: `git show c679c9c1274684393b7e3d1eeaeba17c9037f722`

### #1059 systemc: remove if clause in Gem5ToTlmBridgeBase

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1059
- 代表 commit: `1b323a957115` (2024-04-25)
- 变更规模: commits=1, files=1, +7/-9 (churn=16)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/tlm_bridge/gem5_to_tlm.cc` (churn=16)
- 复现: `git show 1b323a9571152672423e1484910022e2fb792b10`

### #1075 mem-cache: Fix TreePLRU num leaves error

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1075
- 代表 commit: `939d8e28dfdf` (2024-04-26)
- 变更规模: commits=1, files=1, +3/-2 (churn=5)
- 影响范围: topdirs=src; subsys=mem/cache/rp; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/replacement_policies/tree_plru_rp.cc` (churn=5)
- 复现: `git show 939d8e28dfdf32adcd08057e281516be88a4287e`

### #681 arch-riscv: Add support for RISC-V semihosting

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/681
- 代表 commit: `1bb5d3b99e74` (2024-04-27)
- 变更规模: commits=1, files=21, +2024/-1299 (churn=3323)
- 影响范围: topdirs=src, configs; subsys=arch, configs, sim; arch=arm, generic, riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/generic/semihosting.cc` (churn=854)
  - `src/arch/arm/semihosting.cc` (churn=806)
  - `src/arch/generic/semihosting.hh` (churn=555)
  - `src/arch/arm/semihosting.hh` (churn=483)
  - `src/arch/riscv/semihosting.hh` (churn=201)
  - `src/arch/riscv/semihosting.cc` (churn=193)
  - `src/arch/generic/BaseSemihosting.py` (churn=67)
  - `src/arch/riscv/RiscvSemihosting.py` (churn=46)
- 复现: `git show 1bb5d3b99e74da0a144e14d7eaab7cfd04764a50`

### #1077 cpu: Indirect predictor track conditional indirect

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1077
- 代表 commit: `17cbbd84aee7` (2024-04-29)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=cpu/pred; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/simple_indirect.hh` (churn=3)
- 复现: `git show 17cbbd84aee765f9c3be45d8abe3bb29c2c14dc2`

### #1078 arch-riscv: Add Integer Conditional operations extension (Zicond) instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1078
- 代表 commit: `666d1dd9a2d7` (2024-04-30)
- 变更规模: commits=1, files=1, +6/-0 (churn=6)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=6)
- 复现: `git show 666d1dd9a2d78db54e164a675882fe97161e2740`

### #1090 arch-generic: More reliable special file name handling in semihosting

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1090
- 代表 commit: `e7566448fa44` (2024-05-01)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=generic
- 主要改动文件（Top 8 by churn）:
  - `src/arch/generic/semihosting.cc` (churn=3)
- 复现: `git show e7566448fa442d090b4458fcc5f11611b9c1229e`

### #1091 arch-riscv: Fix interrupt and status CSR behavoir

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1091
- 代表 commit: `8b885222b148` (2024-05-02)
- 变更规模: commits=1, files=1, +1/-13 (churn=14)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/formats/standard.isa` (churn=14)
- 复现: `git show 8b885222b14818b2dd7c655f03765db0058a9d6c`

### #1076 arch-riscv: Fix VCSR read behavoir

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1076
- 代表 commit: `3a2a917a53f9` (2024-05-03)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.cc` (churn=2)
- 复现: `git show 3a2a917a53f9e95848336a3996256210bdde948a`

### #1092 misc: bump mypy from 1.9.0 to 1.10.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1092
- 代表 commit: `d834e8bf4e48` (2024-05-03)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show d834e8bf4e48731d5e51f925ba06c0268822b558`

### #1089 arch-generic: Fix reading from special :semihosting-features file

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1089
- 代表 commit: `7c9925bafacf` (2024-05-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7c9925bafacfb0901d6385544b024d828a144355`

### #1101 mem-ruby: Implement MakeReadUnique in CHI

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1101
- 代表 commit: `36c1ea9c61ef` (2024-05-06)
- 变更规模: commits=1, files=5, +123/-3 (churn=126)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` (churn=73)
  - `src/mem/ruby/protocol/chi/CHI-cache-transitions.sm` (churn=39)
  - `src/mem/ruby/protocol/chi/CHI-cache-funcs.sm` (churn=8)
  - `src/mem/ruby/protocol/chi/CHI-cache.sm` (churn=4)
  - `src/mem/ruby/protocol/chi/CHI-msg.sm` (churn=2)
- 复现: `git show 36c1ea9c61ef7d826d9faedadcd6f73b786d0192`

### #1093 gpu-compute: Invalidate Scalar cache when SQC invalidates

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1093
- 代表 commit: `0d3d456894b5` (2024-05-06)
- 变更规模: commits=1, files=3, +50/-16 (churn=66)
- 影响范围: topdirs=src, configs; subsys=gpu-compute, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/gpu-compute/scalar_memory_pipeline.cc` (churn=44)
  - `configs/ruby/GPU_VIPER.py` (churn=11)
  - `src/gpu-compute/compute_unit.cc` (churn=11)
- 复现: `git show 0d3d456894b546bd37450abb7e42f65c2f070871`

### #1103 gpu: Consolidated fixes for v24.0

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1103
- 代表 commit: `cb47755e15b9` (2024-05-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show cb47755e15b90e05f47b9480f1a3683c150804b6`

### #1040 arch-x86: Add XCR0 register and add to X86KvmCPU

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1040
- 代表 commit: `6ed446e5466c` (2024-05-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6ed446e5466c14fecca267bec8c55b05a54a6ee3`

### #1109 misc: Update version in optional-requirements

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1109
- 代表 commit: `06ab3f9b182c` (2024-05-07)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=optional-requirements.txt; subsys=optional-requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `optional-requirements.txt` (churn=2)
- 复现: `git show 06ab3f9b182cd239bebe8f95577cb282317604a1`

### #1115 util: Update gem5-resource-manager requirements

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1115
- 代表 commit: `bc0f38831619` (2024-05-07)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=4)
- 复现: `git show bc0f3883161978f73335749027288c913cab80f0`

### #1100 mem-ruby: Implement NS bit for CHI transactions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1100
- 代表 commit: `0df5635bdf45` (2024-05-08)
- 变更规模: commits=1, files=6, +31/-10 (churn=41)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/slicc_interface/RubyRequest.hh` (churn=15)
  - `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` (churn=14)
  - `src/mem/ruby/protocol/RubySlicc_Types.sm` (churn=3)
  - `src/mem/ruby/protocol/chi/CHI-cache-funcs.sm` (churn=3)
  - `src/mem/ruby/protocol/chi/CHI-cache.sm` (churn=3)
  - `src/mem/ruby/protocol/chi/CHI-msg.sm` (churn=3)
- 复现: `git show 0df5635bdf45faed12cb2dafe385263b2ae571d6`

### #1118 mem-ruby: Fix NullPointerException in RubyRequest

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1118
- 代表 commit: `233135da8184` (2024-05-09)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/slicc_interface/RubyRequest.hh` (churn=2)
- 复现: `git show 233135da81842d99ef5ee1e32e4eaff266e685b6`

### #1097 util: Bump gpu-fs docker to ROCm 6.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1097
- 代表 commit: `e4ebe29f432b` (2024-05-09)
- 变更规模: commits=1, files=2, +3/-3 (churn=6)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/gpu-fs/Dockerfile` (churn=4)
  - `util/dockerfiles/gpu-fs/README.md` (churn=2)
- 复现: `git show e4ebe29f432b6e185bf373f705f3799399ddb860`

### #1110 misc: Add resource versions to examples

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1110
- 代表 commit: `5c82447653b2` (2024-05-09)
- 变更规模: commits=1, files=20, +57/-25 (churn=82)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gem5_library/arm-ubuntu-run-with-kvm.py` (churn=10)
  - `configs/example/lupv/run_lupv.py` (churn=8)
  - `configs/example/gem5_library/riscv-fs.py` (churn=6)
  - `configs/example/gem5_library/x86-parsec-benchmarks.py` (churn=6)
  - `configs/example/gem5_library/arm-ubuntu-run.py` (churn=4)
  - `configs/example/gem5_library/caches/octopi-cache-example.py` (churn=4)
  - `configs/example/gem5_library/checkpoints/riscv-hello-restore-checkpoint.py` (churn=4)
  - `configs/example/gem5_library/checkpoints/simpoints-se-restore.py` (churn=4)
- 复现: `git show 5c82447653b2f4babaea7b5fe0e6cd70ed4494b2`

### #1079 arch-riscv: Fix narrowing/widening type-convert instructions

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1079
- 代表 commit: `8c4d5f8e27c8` (2024-05-09)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=4)
- 复现: `git show 8c4d5f8e27c81551f2ed5cb88f06bc73e1f05404`

### #1120 arch-vega: Fix SDWA dst select

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1120
- 代表 commit: `e3c2a322a1f9` (2024-05-10)
- 变更规模: commits=1, files=1, +3/-3 (churn=6)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/inst_util.hh` (churn=6)
- 复现: `git show e3c2a322a1f940c03bb15d32a0c7fc6fb49d22b4`

### #1099 arch-riscv: Fix CSR instruction behavior 2nd attempts

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1099
- 代表 commit: `53245fa0e8fc` (2024-05-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 53245fa0e8fc0625fdf851a87ed7e4a9c18e47a5`

### #1082 arch-arm: Implement FEAT_MPAM in CPU

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1082
- 代表 commit: `10b24dc9a477` (2024-05-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 10b24dc9a477d9c3b82c1a4c86c7462d65d3a64d`

### #1085 stdlib: change default exit event for SIMPOINT_BEGIN

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1085
- 代表 commit: `6b427a84f71a` (2024-05-13)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/simulator.py` (churn=3)
- 复现: `git show 6b427a84f71a48b4d7c5d6012d99e3f3d1c73dfe`

### #1114 configs: nvm sweep fix

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1114
- 代表 commit: `b279e40cb788` (2024-05-13)
- 变更规模: commits=1, files=2, +3/-3 (churn=6)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/nvm/sweep.py` (churn=4)
  - `configs/nvm/sweep_hybrid.py` (churn=2)
- 复现: `git show b279e40cb788087bab9926fa92484f57ab1e8978`

### #1116 util: Add GNU non executable line to x86 m5

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1116
- 代表 commit: `65976e4c6de6` (2024-05-14)
- 变更规模: commits=1, files=2, +12/-0 (churn=12)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/m5/src/abi/x86/m5op.S` (churn=6)
  - `util/m5/src/abi/x86/m5op_addr.S` (churn=6)
- 复现: `git show 65976e4c6de69c2545e547dc87580a4761d39b95`

### #1123 arch-riscv: Add RVV FP16 support (Zvfh & Zvfhmin)

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1123
- 代表 commit: `d48191d6088c` (2024-05-16)
- 变更规模: commits=1, files=5, +124/-21 (churn=145)
- 影响范围: topdirs=src, ext; subsys=arch, ext; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/utility.hh` (churn=80)
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=42)
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=15)
  - `src/arch/riscv/regs/float.hh` (churn=7)
  - `ext/softfloat/softfloat_types.h` (churn=1)
- 复现: `git show d48191d6088c0b20638e5ff98606eccc39fb4904`

### #1039 cpu: Don't change to suspend if the thread status is halted

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1039
- 代表 commit: `321bd0716392` (2024-05-16)
- 变更规模: commits=1, files=2, +5/-2 (churn=7)
- 影响范围: topdirs=src; subsys=cpu/o3, cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/thread_context.cc` (churn=4)
  - `src/cpu/simple_thread.cc` (churn=3)
- 复现: `git show 321bd0716392c0e11c4cfb9aac9a4d774d8d36f3`

### #990 util: Fixed gem5img.py script

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/990
- 代表 commit: `97a87a7c849b` (2024-05-16)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5img.py` (churn=3)
- 复现: `git show 97a87a7c849b46b1303b7bc7ad5271e4e3ba4fd0`

### #1134 arch-riscv: Fix vrgather instruction

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1134
- 代表 commit: `adb177dab68c` (2024-05-16)
- 变更规模: commits=1, files=1, +6/-5 (churn=11)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=11)
- 复现: `git show adb177dab68c954d3bf91d1eda3e5dfac740eb13`

### #1143 arch-generic: Avoid out-of-memory errors for bad semihosting calls

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1143
- 代表 commit: `6b34765d5d0f` (2024-05-16)
- 变更规模: commits=1, files=2, +31/-11 (churn=42)
- 影响范围: topdirs=src; subsys=arch; arch=generic
- 主要改动文件（Top 8 by churn）:
  - `src/arch/generic/semihosting.cc` (churn=37)
  - `src/arch/generic/semihosting.hh` (churn=5)
- 复现: `git show 6b34765d5d0fdc0deb6a35034fa56cbb5165cdce`

### #1142 arch-arm: Fix 32-bit semihosting ABI

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1142
- 代表 commit: `716fe6d31dec` (2024-05-16)
- 变更规模: commits=1, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/semihosting.hh` (churn=8)
- 复现: `git show 716fe6d31decd2b5bb45cc193a143cea516d0009`

### #1141 dev-amdgpu,gpu-compute,configs: MI300X

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1141
- 代表 commit: `2b3beb92ff88` (2024-05-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2b3beb92ff88d689df10a125ac60c2d0c76426ea`

### #1138 arch-x86: Improve KVM set XCR

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1138
- 代表 commit: `82318e85af80` (2024-05-20)
- 变更规模: commits=1, files=2, +21/-12 (churn=33)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/kvm/x86_cpu.cc` (churn=29)
  - `src/arch/x86/isa.cc` (churn=4)
- 复现: `git show 82318e85af804c3466b6cfdd5a688901f0fa77f2`

### #1137 arch-riscv: Fix viota instruction

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1137
- 代表 commit: `13924336b1d1` (2024-05-20)
- 变更规模: commits=1, files=3, +27/-26 (churn=53)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=24)
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=23)
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=6)
- 复现: `git show 13924336b1d10b83ebdeeb559b15b96e74240e46`

### #1147 arch-arm: Add missing outer-shareable TLBIs to the list

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1147
- 代表 commit: `6f4ba0b422c2` (2024-05-20)
- 变更规模: commits=1, files=1, +4/-1 (churn=5)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/insts/misc64.cc` (churn=5)
- 复现: `git show 6f4ba0b422c2e2e77c5ae37be5c619cc00967cce`

### #1127 Revert "cpu-kvm: Support perf counters on hybrid host architectures"

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1127
- 代表 commit: `0824d7f2cd9a` (2024-05-21)
- 变更规模: commits=1, files=5, +74/-267 (churn=341)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/kvm/perfevent.cc` (churn=165)
  - `src/cpu/kvm/perfevent.hh` (churn=157)
  - `src/cpu/kvm/base.cc` (churn=7)
  - `src/cpu/kvm/BaseKvmCPU.py` (churn=6)
  - `src/cpu/kvm/base.hh` (churn=6)
- 复现: `git show 0824d7f2cd9a413998c373284955fb6fcbd5f15a`

### #1152 arch-riscv: Fix GDB connection failed after #1099

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1152
- 代表 commit: `5e20438c1cbe` (2024-05-21)
- 变更规模: commits=1, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/remote_gdb.cc` (churn=8)
- 复现: `git show 5e20438c1cbe9bbba77c85520ca14fc246a49a94`

### #1153 arch-riscv: add exception code to DPRINTFS msg

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1153
- 代表 commit: `688f8fb03b2b` (2024-05-21)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/faults.cc` (churn=4)
- 复现: `git show 688f8fb03b2bf62234611b0e5f36a02d8f268497`

### #1149 dev: add reset wrap mode to mouse.cc

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1149
- 代表 commit: `33cebe937613` (2024-05-21)
- 变更规模: commits=1, files=1, +3/-1 (churn=4)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/ps2/mouse.cc` (churn=4)
- 复现: `git show 33cebe9376132d2f80f39440ac01a31d0bb7a355`

### #1145 misc: Remove gcc 8 support, gem5 support GCC >= v10

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1145
- 代表 commit: `6adb7a86373b` (2024-05-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6adb7a86373b95b9c257ad6bc598e952d51b95e1`

### #1146 misc: Revert Dramsys Ubuntu to 22.04 to compile in gcc <13

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1146
- 代表 commit: `52fbc8ebcf99` (2024-05-21)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/weekly-tests.yaml` (churn=2)
- 复现: `git show 52fbc8ebcf9920451bdf77a8651658e69ec85d7b`

### #1155 misc: Sync stable .github dir with develop

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/1155
- 代表 commit: `0b2243bb0a26` (2024-05-21)
- 变更规模: commits=1, files=2, +3/-3 (churn=6)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/compiler-tests.yaml` (churn=4)
  - `.github/workflows/weekly-tests.yaml` (churn=2)
- 复现: `git show 0b2243bb0a262bf97f45948b579ccf8b1a8ee35d`

### #1154 util: Update gem5-resource-manager requirements

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1154
- 代表 commit: `1a68d71f0730` (2024-05-22)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=2)
- 复现: `git show 1a68d71f07305b434427b063c762b93c56a47af3`

### #1128 arch-vega: Template MFMA instructions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1128
- 代表 commit: `1616d34003aa` (2024-05-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1616d34003aa0d62ba0365a1f3932116188802d8`

### #1105 util, ext: Fix building TLM

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1105
- 代表 commit: `96fbc2068ab1` (2024-05-24)
- 变更规模: commits=1, files=2, +17/-7 (churn=24)
- 影响范围: topdirs=ext, util; subsys=ext, util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/tlm/SConstruct` (churn=19)
  - `ext/systemc/SConscript` (churn=5)
- 复现: `git show 96fbc2068ab1f47dece87c136ebce7cc27847532`

### #1163 arch-riscv: Fix c.jalr and c.jr instruction

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1163
- 代表 commit: `4f6fdbf8bfc3` (2024-05-25)
- 变更规模: commits=1, files=3, +47/-10 (churn=57)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/formats/compressed.isa` (churn=41)
  - `src/arch/riscv/isa/decoder.isa` (churn=8)
  - `src/arch/riscv/isa/formats/standard.isa` (churn=8)
- 复现: `git show 4f6fdbf8bfc38c7d8f3a6281d676d73b9b8a097f`

### #1166 arch-arm: Rewrite performTlbi to use map instead of switch

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/1166
- 代表 commit: `10dbfb8bb771` (2024-05-28)
- 变更规模: commits=1, files=4, +931/-676 (churn=1607)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/insts/misc64.cc` (churn=1554)
  - `src/arch/arm/insts/misc64.hh` (churn=42)
  - `src/arch/arm/utility.cc` (churn=9)
  - `src/arch/arm/utility.hh` (churn=2)
- 复现: `git show 10dbfb8bb771beb0319b84d68e6861708df9f63b`

### #1162 arch-vega: Fix GCC 13 build errors

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1162
- 代表 commit: `1dfaa224ffb6` (2024-05-28)
- 变更规模: commits=1, files=1, +25/-12 (churn=37)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/operand.hh` (churn=37)
- 复现: `git show 1dfaa224ffb6b62b7a1e72a022f36db40c09c599`

### #1176 arch-arm: TLBIs targeting EL2 regime are executable from S state

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1176
- 代表 commit: `5ec1acaf5fdb` (2024-05-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5ec1acaf5fdb767d187b8e6de5c516c2775bdb16`

### #1156 mem-ruby: Remove VIPER StoreThrough temp cache storage

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1156
- 代表 commit: `e82cf20150da` (2024-05-28)
- 变更规模: commits=1, files=1, +37/-5 (churn=42)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/GPU_VIPER-TCP.sm` (churn=42)
- 复现: `git show e82cf20150daffbc06ccb9308267eff4517f728c`

### #1178 misc,tests: Download all gem5 bins via one artifact

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1178
- 代表 commit: `4acc20dac197` (2024-05-28)
- 变更规模: commits=1, files=1, +25/-48 (churn=73)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=73)
- 复现: `git show 4acc20dac197d0e47a66bb9d80955c26dc3bf89c`

### #1175 arch-arm: Implement HCR_EL2 force broadcast for EL1&0 TLBIs

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1175
- 代表 commit: `c4ed23a10b51` (2024-05-29)
- 变更规模: commits=1, files=5, +201/-48 (churn=249)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/insts/misc.cc` (churn=132)
  - `src/arch/arm/insts/misc64.cc` (churn=63)
  - `src/arch/arm/isa/insts/data64.isa` (churn=36)
  - `src/arch/arm/tlbi_op.hh` (churn=14)
  - `src/arch/arm/isa/formats/aarch64.isa` (churn=4)
- 复现: `git show c4ed23a10b51f2f4f3c40fac060e8b34f5b18848`

### #1157 dev-amdgpu: Fix pending PCI RLC doorbell

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1157
- 代表 commit: `07f6b7c59c75` (2024-05-29)
- 变更规模: commits=1, files=3, +12/-1 (churn=13)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/amdgpu/amdgpu_device.cc` (churn=11)
  - `src/dev/amdgpu/amdgpu_device.hh` (churn=1)
  - `src/dev/amdgpu/sdma_engine.cc` (churn=1)
- 复现: `git show 07f6b7c59c7554778f4b17e3844bf7f9e128abd4`

### #1172 arch-x86: break 32/64-bit mov's input dependency on prior dest value

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1172
- 代表 commit: `a54d3198a820` (2024-05-29)
- 变更规模: commits=1, files=1, +37/-7 (churn=44)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/isa/microops/regop.isa` (churn=44)
- 复现: `git show a54d3198a820e1f6ae74d8b0eda47f112e4afe6f`

### #1170 util-docker,gpu,gpu-compute: Improve GCN-GPU Dockerfile

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1170
- 代表 commit: `ce0bb4655c9e` (2024-05-29)
- 变更规模: commits=1, files=1, +133/-81 (churn=214)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/gcn-gpu/Dockerfile` (churn=214)
- 复现: `git show ce0bb4655c9e881f1037b28980a041a585da5eed`

### #1165 util: allow to override ARCH in cxx config's Makefile

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1165
- 代表 commit: `7d339ee79bae` (2024-05-29)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/cxx_config/Makefile` (churn=2)
- 复现: `git show 7d339ee79bae1ab51b9987be7f0d9d8a631c8638`

### #1171 arch-x86: set AF=0 when logical instructions execute

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1171
- 代表 commit: `9027d5c3e258` (2024-05-29)
- 变更规模: commits=1, files=3, +45/-45 (churn=90)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/isa/insts/general_purpose/logical.py` (churn=72)
  - `src/arch/x86/isa/insts/general_purpose/compare_and_test/test.py` (churn=12)
  - `src/arch/x86/isa/microops/regop.isa` (churn=6)
- 复现: `git show 9027d5c3e258c93694bc876f942bd8c5e9da58db`

### #1180 arch-arm: Fix memory attributes of table walks

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1180
- 代表 commit: `b161172f6583` (2024-05-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b161172f65836246e18ad5ad7b51c0a4b2d08952`

### #1179 mem-cache: Fix maybe-uninitialized warning

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1179
- 代表 commit: `7fa0342a7c79` (2024-05-29)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=mem/cache/rp; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/replacement_policies/replaceable_entry.hh` (churn=2)
- 复现: `git show 7fa0342a7c7982967c948b346ba7077dce54efd2`

### #1181 misc: Sync .github develop -> stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/1181
- 代表 commit: `8404ae276bf3` (2024-05-29)
- 变更规模: commits=1, files=1, +25/-48 (churn=73)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=73)
- 复现: `git show 8404ae276bf3eed701aeeaec3cef0ed3b4f5de5b`

### #1184 misc: Fix daily tests merge-artifacts

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1184
- 代表 commit: `65b86cfac983` (2024-05-30)
- 变更规模: commits=1, files=1, +3/-3 (churn=6)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=6)
- 复现: `git show 65b86cfac98338153936b6b308368f8edf961d3d`

### #1188 misc: Another attempt to fix the merge-upload in for daily

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1188
- 代表 commit: `7c1207d5c449` (2024-05-30)
- 变更规模: commits=1, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=8)
- 复现: `git show 7c1207d5c449dfa8639558a5449ca014e9c8705a`

### #1117 mem-ruby: Reduce handshaking between CorePair and dir

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1117
- 代表 commit: `efbfdeabd785` (2024-05-30)
- 变更规模: commits=1, files=2, +19/-42 (churn=61)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm` (churn=34)
  - `src/mem/ruby/protocol/MOESI_AMD_Base-CorePair.sm` (churn=27)
- 复现: `git show efbfdeabd785c3b01ae66094a4d3801a390c3635`

### #1183 arch-vega: Fix clang comp error due to constant exp

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1183
- 代表 commit: `a0de33110b81` (2024-05-30)
- 变更规模: commits=1, files=1, +26/-3 (churn=29)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/instructions.hh` (churn=29)
- 复现: `git show a0de33110b81a59d750f8c622b2b0940b66cf206`

### #1185 misc: Sync .github dir to stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/1185
- 代表 commit: `bbdaae540c37` (2024-05-30)
- 变更规模: commits=1, files=1, +3/-3 (churn=6)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=6)
- 复现: `git show bbdaae540c3745d1bc0833059ac15e0dfa388f14`

### #1189 misc: Merge .github dir develop -> stable

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/1189
- 代表 commit: `ef2a9110b741` (2024-05-30)
- 变更规模: commits=1, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=8)
- 复现: `git show ef2a9110b7412cd526867dadd686e65fb3e4a5ac`

### #1190 arch-vega: More scratch, accvgpr instructions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1190
- 代表 commit: `fe8daa85d6f0` (2024-06-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fe8daa85d6f00cab9312879b89353fa6ecf388f3`

### #1173 arch-riscv: Add rvZext to BranchTarget

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1173
- 代表 commit: `5d3f1c3316f8` (2024-06-03)
- 变更规模: commits=1, files=2, +4/-3 (churn=7)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/formats/standard.isa` (churn=5)
  - `src/arch/riscv/isa/formats/compressed.isa` (churn=2)
- 复现: `git show 5d3f1c3316f8588fd332a1a936ba184499117918`

### #1080 arch-x86: Fix TLB Assertion Error on CFLUSH

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1080
- 代表 commit: `dad5c7b6f743` (2024-06-03)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/translation.hh` (churn=2)
- 复现: `git show dad5c7b6f7434ec7668192edffad83bc31a1d5f7`

### #1193 misc: bump pre-commit from 3.7.0 to 3.7.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1193
- 代表 commit: `8c98dcb7cf50` (2024-06-04)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 8c98dcb7cf50c0602e3f4091469848c77601e740`

### #1192 misc: bump tqdm from 4.66.3 to 4.66.4

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1192
- 代表 commit: `500bdc53022b` (2024-06-04)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=optional-requirements.txt; subsys=optional-requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `optional-requirements.txt` (churn=2)
- 复现: `git show 500bdc53022bc504197e060f6d9fd28e8eb6d0ae`

### #1191 dev: Remove an extra file in virtio

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1191
- 代表 commit: `40ef8f3afbb1` (2024-06-04)
- 变更规模: commits=1, files=1, +0/-49 (churn=49)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/virtio/VirtIORng 2.py` (churn=49)
- 复现: `git show 40ef8f3afbb164dcf8cf52d339a7ce1e0692575f`

### #1196 Revert "arch-x86: Fix TLB Assertion Error on CFLUSH"

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1196
- 代表 commit: `a764b9be1c0c` (2024-06-04)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/translation.hh` (churn=2)
- 复现: `git show a764b9be1c0c4b701c8ebb8812e0eaf7a2eecec9`

### #1197 dev-arm: Fix -Wdeprecated-copy warning

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1197
- 代表 commit: `abbb94af8bf6` (2024-06-05)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/arm/smmu_v3_transl.hh` (churn=1)
- 复现: `git show abbb94af8bf6aa31e2ee3571ed723ab01960fe8f`

### #1202 arch-generic: flush streams after semihosting write calls

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1202
- 代表 commit: `8e5fbcbbbb8f` (2024-06-06)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=generic
- 主要改动文件（Top 8 by churn）:
  - `src/arch/generic/semihosting.cc` (churn=2)
- 复现: `git show 8e5fbcbbbb8fb3b1d3ab37dba5d08fb09c9f0b27`

### #1198 arch-arm: avoid using an uninitialized variable use in MMU walks

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1198
- 代表 commit: `ec5881ec4e65` (2024-06-07)
- 变更规模: commits=1, files=2, +49/-34 (churn=83)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/table_walker.cc` (churn=64)
  - `src/arch/arm/table_walker.hh` (churn=19)
- 复现: `git show ec5881ec4e65120a388ed431149f6de9724909aa`

### #1187 arch-riscv: correctly set dynamic VLEN for all arith instructions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1187
- 代表 commit: `5cfad84a988b` (2024-06-07)
- 变更规模: commits=1, files=2, +16/-0 (churn=16)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=12)
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=4)
- 复现: `git show 5cfad84a988bd3df7dedbfb858eb69906b22eaf9`

### #1200 arch-arm,mem: Don't hardcode secure mode accesses for semihosting

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1200
- 代表 commit: `3cfc550fc05e` (2024-06-09)
- 变更规模: commits=1, files=5, +71/-62 (churn=133)
- 影响范围: topdirs=src; subsys=mem, arch; arch=arm, generic
- 主要改动文件（Top 8 by churn）:
  - `src/mem/translation_gen.test.cc` (churn=115)
  - `src/mem/translating_port_proxy.cc` (churn=6)
  - `src/arch/arm/mmu.cc` (churn=5)
  - `src/arch/generic/mmu.cc` (churn=4)
  - `src/mem/translation_gen.hh` (churn=3)
- 复现: `git show 3cfc550fc05ef6c96962b2efb60f8da07399c457`

### #1221 base: Fix uninitialized variable warning in symtab.test.cc

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1221
- 代表 commit: `d198380489f7` (2024-06-11)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/loader/symtab.test.cc` (churn=4)
- 复现: `git show d198380489f764f9cd4da553584f2e9e421bd52a`

### #1213 gpu-compute: Added functions to choose replacement policies for GPU

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1213
- 代表 commit: `8a44e97a1001` (2024-06-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 8a44e97a10019c4d509a9b13f1869931660db7a3`

### #996 stdlib: Improve gem5 PyStats

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/996
- 代表 commit: `f9abf6bb0871` (2024-06-12)
- 变更规模: commits=3, files=2, +23/-11 (churn=34)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/ext/pystats/__init__.py` (churn=27)
  - `src/python/m5/stats/gem5stats.py` (churn=7)
- commits 列表（按 topo-order，Top 12）：
  - 2024-05-27 `8f0ed4606184` stdlib: Move `_m5.stats.processDumpQueue` to call-once
  - 2024-06-11 `7e45ec0ff03b` stdlib: Fix m5.ext.pystats __init__.py
  - 2024-06-12 `f9abf6bb0871` stdlib: Improve gem5 PyStats
- 复现: `git show f9abf6bb08713f4799e0b729ff0cc3076be751fb`

### #1225 cpu: Revert "Don't change to suspend if the thread status is halted"

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1225
- 代表 commit: `74afea471d6f` (2024-06-12)
- 变更规模: commits=1, files=2, +2/-5 (churn=7)
- 影响范围: topdirs=src; subsys=cpu/o3, cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/thread_context.cc` (churn=4)
  - `src/cpu/simple_thread.cc` (churn=3)
- 复现: `git show 74afea471d6f3763b726cf3459b401fa62b74b54`

### #1216 mem-ruby: Fix deadlock in GPU_VIPER when issuing atomic requests

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1216
- 代表 commit: `be0a7937c1ca` (2024-06-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show be0a7937c1cac8998ede2806b7fae09a90133b0d`

### #1230 configs: Add replacement policy options for GPUFS

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1230
- 代表 commit: `b3d9dc42d43c` (2024-06-13)
- 变更规模: commits=1, files=1, +22/-0 (churn=22)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gpufs/runfs.py` (churn=22)
- 复现: `git show b3d9dc42d43c6e135dd61ab274a5634562ad438b`

### #1071 cpu,arch: Add IsInvalid flag to Unknown insts

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1071
- 代表 commit: `21ffd915297f` (2024-06-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 21ffd915297ff4708121dd096bd176c6e22e8903`

### #1217 gpu-compute, util-m5: add GPU kernel exit events

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1217
- 代表 commit: `3cf638e21730` (2024-06-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3cf638e2173017e99b834dac4ef3a1bcb69e415e`

### #1182 cpu-o3: Do not set Executed on load instruction to be replayed

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1182
- 代表 commit: `b8e21a2d32f2` (2024-06-14)
- 变更规模: commits=1, files=1, +6/-0 (churn=6)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/lsq_unit.cc` (churn=6)
- 复现: `git show b8e21a2d32f2485c74ccd5587719d87a2da237fc`

### #1248 gpu-compute: Add MFMA stats

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1248
- 代表 commit: `f91d14fe4697` (2024-06-15)
- 变更规模: commits=1, files=8, +67/-0 (churn=67)
- 影响范围: topdirs=src; subsys=gpu-compute, arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/gpu-compute/wavefront.cc` (churn=20)
  - `src/arch/amdgpu/vega/insts/instructions.hh` (churn=12)
  - `src/gpu-compute/gpu_dyn_inst.cc` (churn=12)
  - `src/gpu-compute/compute_unit.cc` (churn=10)
  - `src/gpu-compute/compute_unit.hh` (churn=6)
  - `src/gpu-compute/GPUStaticInstFlags.py` (churn=3)
  - `src/gpu-compute/gpu_dyn_inst.hh` (churn=2)
  - `src/gpu-compute/gpu_static_inst.hh` (churn=2)
- 复现: `git show f91d14fe4697eee7f2338bd52a9045a37136004c`

### #1249 arch-vega: Various MI300 fixes for PyTorch tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1249
- 代表 commit: `50e4209a4ad8` (2024-06-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 50e4209a4ad8a48689220ae1eeb04b7899f48656`

### #1226 gpu-compute,mem-ruby: Add RubyHitMiss flag for TCP and TCC cache

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1226
- 代表 commit: `6776bebbf642` (2024-06-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6776bebbf6423d5e0ec4571e8f277de21a4f4458`

### #1251 cpu-o3: Revert "Do not set Executed on load instruction to be replayed"

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1251
- 代表 commit: `2804311f7beb` (2024-06-17)
- 变更规模: commits=1, files=1, +0/-6 (churn=6)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/lsq_unit.cc` (churn=6)
- 复现: `git show 2804311f7beb05a78aaa57d002b99031643d0a20`

### #1247 arch: Mark FailUnimplemented instructions as Invalid instructions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1247
- 代表 commit: `500da4306bd0` (2024-06-17)
- 变更规模: commits=1, files=5, +8/-1 (churn=9)
- 影响范围: topdirs=src; subsys=arch; arch=arm, mips, power, sparc, x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/sparc/insts/unimp.hh` (churn=4)
  - `src/arch/arm/insts/pseudo.cc` (churn=2)
  - `src/arch/mips/isa/formats/unimp.isa` (churn=1)
  - `src/arch/power/isa/formats/unimp.isa` (churn=1)
  - `src/arch/x86/isa/formats/unimp.isa` (churn=1)
- 复现: `git show 500da4306bd0130848f7d4d1929242eafa2e92dc`

### #1236 mem-ruby: This commit fixes MI_example protocol

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1236
- 代表 commit: `fef6a97f935d` (2024-06-17)
- 变更规模: commits=1, files=1, +6/-1 (churn=7)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/MI_example-dir.sm` (churn=7)
- 复现: `git show fef6a97f935d7cc1c980c6f941b7454eddab56d5`

### #1234 arch,cpu,sim: Add mechanism to partially print vector regs

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1234
- 代表 commit: `15e0236a8b48` (2024-06-17)
- 变更规模: commits=1, files=11, +108/-13 (churn=121)
- 影响范围: topdirs=src; subsys=arch, cpu, sim; arch=arm, generic, riscv
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/reg_class.hh` (churn=29)
  - `src/sim/insttracer.hh` (churn=28)
  - `src/arch/generic/vec_reg.hh` (churn=20)
  - `src/cpu/exetrace.cc` (churn=11)
  - `src/arch/generic/isa.hh` (churn=9)
  - `src/arch/riscv/insts/vector.cc` (churn=9)
  - `src/cpu/inst_res.hh` (churn=7)
  - `src/arch/arm/isa.hh` (churn=2)
- 复现: `git show 15e0236a8b48a2a0a55167386a8b0fcdfcb96f3e`

### #1136 cpu,stdlib: Adding Spatter

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1136
- 代表 commit: `36f73f671db7` (2024-06-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 36f73f671db7de463e86902ad1fb9a29dfd84a60`

### #1254 gpu-compute,mem-ruby: Revert "Add RubyHitMiss flag for TCP and TCC cache"

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1254
- 代表 commit: `3138c8a8b16a` (2024-06-18)
- 变更规模: commits=1, files=5, +26/-44 (churn=70)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/GPUCoalescer.cc` (churn=29)
  - `src/mem/ruby/system/GPUCoalescer.hh` (churn=17)
  - `src/mem/ruby/protocol/GPU_VIPER-msg.sm` (churn=12)
  - `src/mem/ruby/protocol/GPU_VIPER-TCP.sm` (churn=10)
  - `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` (churn=2)
- 复现: `git show 3138c8a8b16a962ee8fe4edd22936dce59689862`

### #1167 stdlib,configs,tests: Add gem5 MultiSim (MultiProcessing for gem5)

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1167
- 代表 commit: `1a00ecfaf926` (2024-06-18)
- 变更规模: commits=1, files=28, +936/-65 (churn=1001)
- 影响范围: topdirs=tests, src, configs; subsys=tests, python, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/utils/multisim/multisim.py` (churn=300)
  - `configs/example/gem5_library/multisim/multisim-fs-x86-npb.py` (churn=138)
  - `src/python/gem5/simulate/simulator.py` (churn=130)
  - `configs/example/gem5_library/multisim/multisim-print-this.py` (churn=87)
  - `tests/gem5/gem5_library_example_tests/test_gem5_library_examples.py` (churn=80)
  - `src/python/gem5/utils/multisim/__main__.py` (churn=60)
  - `src/python/gem5/utils/multisim/__init__.py` (churn=33)
  - `src/python/gem5/utils/multiprocessing/README.md` (churn=30)
- 复现: `git show 1a00ecfaf926bc605e75709c4d93ae71fb2fd52c`

### #1256 util-docker: Update devcontainer to use Ubuntu 24.04

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1256
- 代表 commit: `9fe2bc9edcad` (2024-06-18)
- 变更规模: commits=1, files=1, +4/-31 (churn=35)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/devcontainer/Dockerfile` (churn=35)
- 复现: `git show 9fe2bc9edcad3eae5f3a448112f88b36ffe3d89e`

### #1257 util: Bump urllib3 in gem5-resource-manager

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1257
- 代表 commit: `e88f0944e309` (2024-06-18)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=2)
- 复现: `git show e88f0944e309e04800eefe4f03ce23e35a3c7fe5`

### #1267 tests: Fix x86_boot_exit_run.py 'set_max_ticks' typo

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1267
- 代表 commit: `25d614e4cef0` (2024-06-20)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/x86_boot_tests/configs/x86_boot_exit_run.py` (churn=2)
- 复现: `git show 25d614e4cef0b3db544ac3ae75fbff8c662a500e`

### #1262 stdlib: Add function to append kernel args

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1262
- 代表 commit: `943daeb603aa` (2024-06-20)
- 变更规模: commits=1, files=1, +8/-0 (churn=8)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/kernel_disk_workload.py` (churn=8)
- 复现: `git show 943daeb603aac2270618ba16d5c9ce19be6806e5`

### #1263 gpu-compute,mem,systemc: This commit corrects typos of 'cache'

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1263
- 代表 commit: `9fb0b1886376` (2024-06-20)
- 变更规模: commits=1, files=4, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=gpu-compute, mem/cache, mem, src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/gpu-compute/compute_unit.hh` (churn=2)
  - `src/mem/cache/cache.cc` (churn=2)
  - `src/mem/probes/stack_dist.cc` (churn=2)
  - `src/systemc/tests/systemc/utils/sc_report/cached/cached.cpp` (churn=2)
- 复现: `git show 9fb0b188637608281cf2d917c24d46f3e4ba8168`

### #1261 configs: Check before use replacement policy options

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1261
- 代表 commit: `ed860dfe5489` (2024-06-20)
- 变更规模: commits=1, files=1, +6/-3 (churn=9)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/ruby/GPU_VIPER.py` (churn=9)
- 复现: `git show ed860dfe5489457df1d8631944d33e5b918deb2f`

### #1258 cpu,stdlib: Fix Access Trace for Accessing Indices in SpatterGen

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1258
- 代表 commit: `7ff1e381c98f` (2024-06-20)
- 变更规模: commits=1, files=4, +50/-8 (churn=58)
- 影响范围: topdirs=src; subsys=cpu, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/testers/spatter_gen/utility_structs.hh` (churn=41)
  - `src/python/gem5/components/processors/spatter_gen/spatter_kernel.py` (churn=13)
  - `src/cpu/testers/spatter_gen/spatter_gen.cc` (churn=2)
  - `src/cpu/testers/spatter_gen/spatter_gen.hh` (churn=2)
- 复现: `git show 7ff1e381c98faeafd02a90269969d3c59cee94df`

### #1266 cpu: Fix `std::min` type mismatch in reg_class.hh

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1266
- 代表 commit: `7137b73ca062` (2024-06-20)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/reg_class.hh` (churn=2)
- 复现: `git show 7137b73ca0625f3245736138c91f3627226fba56`

### #1264 arch-riscv: Fix TLB lookup with vaddrs

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1264
- 代表 commit: `013f773d3145` (2024-06-20)
- 变更规模: commits=1, files=5, +112/-33 (churn=145)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/tlb.cc` (churn=84)
  - `src/arch/riscv/tlb.hh` (churn=22)
  - `src/arch/riscv/pagetable_walker.cc` (churn=16)
  - `src/arch/riscv/pagetable.cc` (churn=15)
  - `src/arch/riscv/pagetable.hh` (churn=8)
- 复现: `git show 013f773d3145a2a8985b30158935ecbb27a186cf`

### #1273 stdlib: Getter method to get monolith range.

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1273
- 代表 commit: `30bfdc8e525a` (2024-06-21)
- 变更规模: commits=2, files=2, +24/-0 (churn=24)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/memory/abstract_memory_system.py` (churn=16)
  - `src/python/gem5/components/memory/memory.py` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2024-06-21 `18bc5227f60a` stdlib: Getter method to get monolith range.
  - 2024-06-21 `30bfdc8e525a` stdlib: Getter method to get monolith range.
- 复现: `git show 30bfdc8e525a7106f66c200b57e5a1d16f573844`

### #1272 Adding an example for Spatter

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1272
- 代表 commit: `21bd1c28abf4` (2024-06-21)
- 变更规模: commits=2, files=4, +638/-88 (churn=726)
- 影响范围: topdirs=configs, src; subsys=configs, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/processors/spatter_gen/spatter_kernel.py` (churn=526)
  - `configs/example/gem5_library/spatter_gen/spatter-gen-test.py` (churn=194)
  - `src/python/gem5/components/processors/spatter_gen/__init__.py` (churn=4)
  - `configs/example/gem5_library/spatter_gen/traces/amg.json` (churn=2)
- commits 列表（按 topo-order，Top 12）：
  - 2024-06-21 `590bb1fbbb4b` Adding an example for Spatter
  - 2024-06-21 `21bd1c28abf4` Adding an example for Spatter
- 复现: `git show 21bd1c28abf4684e8ac3bad1e550cc431e0a42a9`

### #1284 resources: Update client_query to trim gem5 version

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1284
- 代表 commit: `241b8a09df1e` (2024-06-25)
- 变更规模: commits=2, files=1, +8/-2 (churn=10)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/client_api/client_query.py` (churn=10)
- commits 列表（按 topo-order，Top 12）：
  - 2024-06-25 `52fde944a573` resources: Update client_query to trim gem5 version
  - 2024-06-25 `241b8a09df1e` resources: Update client_query to trim gem5 version
- 复现: `git show 241b8a09df1e09e4686c25079c04465e212e0091`

### #1290 stdlib,tests: Update resources to v24.0 in Pyunit test

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1290
- 代表 commit: `b471d5f38265` (2024-06-27)
- 变更规模: commits=1, files=6, +55/-28 (churn=83)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/pyunit/stdlib/resources/refs/resources.json` (churn=33)
  - `tests/pyunit/stdlib/resources/refs/suite-checks.json` (churn=17)
  - `tests/pyunit/stdlib/resources/refs/obtain-resource.json` (churn=12)
  - `tests/pyunit/stdlib/resources/refs/workload-checks.json` (churn=12)
  - `tests/pyunit/stdlib/resources/refs/mongo-mock.json` (churn=6)
  - `tests/pyunit/stdlib/resources/refs/resource-specialization.json` (churn=3)
- 复现: `git show b471d5f3826505c05904b7694fbd61ccd2beea2f`

### #1289 resources: Update elfie.py to work with obtain_resources

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1289
- 代表 commit: `3acb6e59cf83` (2024-06-27)
- 变更规模: commits=1, files=2, +29/-2 (churn=31)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/elfie.py` (churn=29)
  - `src/python/gem5/resources/resource.py` (churn=2)
- 复现: `git show 3acb6e59cf8325be3e2bd9f6a70a922f2399e9fa`

### #1274 misc: Merge v24.0 release staging branch to stable

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1274
- 代表 commit: `43769abaf051` (2024-06-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 43769abaf05120fed1e4e0cfbb34619edbc10f3f`

## v24.0.0.1 (2024-08-08)

- PR 数：4

### #1306 misc, tests: Fix missing 's' in GPU tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1306
- 代表 commit: `a7645cdf20ef` (2024-07-01)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/weekly-tests.yaml` (churn=4)
- 复现: `git show a7645cdf20effbef03e0ec0401807b63e3282c87`

### #1308 misc: Add scheduler.yaml

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1308
- 代表 commit: `bb418d41eb6d` (2024-07-01)
- 变更规模: commits=1, files=1, +91/-0 (churn=91)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/scheduler.yaml` (churn=91)
- 复现: `git show bb418d41eb6d87c0a0591869097005c16420c6aa`

### #1361 tests,misc: Sync .github dir develop -> stable

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1361
- 代表 commit: `b88f814e633f` (2024-07-18)
- 变更规模: commits=1, files=3, +11/-98 (churn=109)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=66)
  - `.github/workflows/weekly-tests.yaml` (churn=30)
  - `.github/workflows/compiler-tests.yaml` (churn=13)
- 复现: `git show b88f814e633f879b71c636a6631f92b944017d5d`

### #1425 misc: v24.0.0.1 Hotfix release

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1425
- 代表 commit: `b1a44b89c7ba` (2024-08-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b1a44b89c7bae73fae2dc547bc1f871452075b85`

## v24.1.0.0 (2024-12-07)

- PR 数：242

### #1287 resources: fix check for additional_params for workloads

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1287
- 代表 commit: `f68f4dd390f1` (2024-06-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f68f4dd390f194fa2c1d9ab38846c36d9869214c`

### #1279 arch-arm: This commit fixes a typo in the ARM ldaddalx instruction

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1279
- 代表 commit: `3ce5e0584af9` (2024-06-26)
- 变更规模: commits=1, files=2, +8/-8 (churn=16)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/formats/aarch64.isa` (churn=8)
  - `src/arch/arm/isa/insts/amo64.isa` (churn=8)
- 复现: `git show 3ce5e0584af9019693aff72b2b6b12c76e3bc3df`

### #1295 misc: Merge stable into develop (v24.0 release)

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1295
- 代表 commit: `ca4897897c36` (2024-06-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ca4897897c3605d517c7936efdd49122eb6c0efb`

### #1260 gpu-compute,mem-ruby: Add RubyHitMiss flag for TCP and TCC cache

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1260
- 代表 commit: `04a3fd5b5ded` (2024-06-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 04a3fd5b5ded988d3c1d2e7fe91e62fadc3a98b9`

### #1305 misc, tests: Fix missing 's' in GPU tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1305
- 代表 commit: `e5414a80a39f` (2024-07-01)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/weekly-tests.yaml` (churn=4)
- 复现: `git show e5414a80a39fa90cb9afe9b8213775c735bad76e`

### #1307 misc: Add 'scheduler.yaml' workflow

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/1307
- 代表 commit: `3142464ff7bc` (2024-07-01)
- 变更规模: commits=1, files=4, +94/-73 (churn=167)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/scheduler.yaml` (churn=91)
  - `.github/workflows/daily-tests.yaml` (churn=33)
  - `.github/workflows/weekly-tests.yaml` (churn=30)
  - `.github/workflows/compiler-tests.yaml` (churn=13)
- 复现: `git show 3142464ff7bcd7b0fe05d728a55590e6c7edd74d`

### #1310 misc,github,tests: Attempt Fixes for flakey Daily Tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1310
- 代表 commit: `093e3afc81b6` (2024-07-01)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 093e3afc81b62aabc39e86b93e3e27fbb46a3b95`

### #1303 arch-arm: Implement FEAT_XS

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1303
- 代表 commit: `b28659d4f9f7` (2024-07-02)
- 变更规模: commits=1, files=14, +2113/-454 (churn=2567)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/insts/misc64.cc` (churn=1425)
  - `src/arch/arm/regs/misc.cc` (churn=354)
  - `src/arch/arm/regs/misc.hh` (churn=326)
  - `src/arch/arm/isa/formats/aarch64.isa` (churn=172)
  - `src/arch/arm/tlbi_op.hh` (churn=106)
  - `src/arch/arm/table_walker.cc` (churn=56)
  - `src/arch/arm/tlbi_op.cc` (churn=50)
  - `src/arch/arm/insts/misc64.hh` (churn=24)
- 复现: `git show b28659d4f9f7c1cf67aedf81649d6a4745401e30`

### #1304 arch-arm: support 64-bit PMCCNTR from AArch32

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1304
- 代表 commit: `d5c038388748` (2024-07-02)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/regs/misc.cc` (churn=1)
- 复现: `git show d5c0383887483f59c339545e26fe62868d0fb5dd`

### #1313 arch-arm: Properly implement IPASpace in the MMU

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1313
- 代表 commit: `6ebc6dd99872` (2024-07-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6ebc6dd99872370c90d588758424b62bf1065a8b`

### #1309 misc: bump mypy from 1.10.0 to 1.10.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1309
- 代表 commit: `baf2a9b9175f` (2024-07-03)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show baf2a9b9175fc4d0332ba2e7286664b8a4edcb1f`

### #1322 arch-arm: MISCREG_AT_S1E2R/W are executable from S state

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1322
- 代表 commit: `c9d910897812` (2024-07-04)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/regs/misc.cc` (churn=4)
- 复现: `git show c9d91089781233d91c0d8ecc8e2b2ec89b91ed1e`

### #1323 arch-arm: Implement FEAT_TTST

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1323
- 代表 commit: `d825103df20f` (2024-07-04)
- 变更规模: commits=1, files=5, +58/-21 (churn=79)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/table_walker.cc` (churn=65)
  - `src/arch/arm/table_walker.hh` (churn=7)
  - `src/arch/arm/ArmSystem.py` (churn=3)
  - `src/arch/arm/pagetable.cc` (churn=3)
  - `src/arch/arm/regs/misc.cc` (churn=1)
- 复现: `git show d825103df20fd7a9fe0ad9587945c145b7cff31d`

### #1328 systemc: Use headerDelay in timing annotation

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1328
- 代表 commit: `77528d192885` (2024-07-05)
- 变更规模: commits=1, files=4, +4/-4 (churn=8)
- 影响范围: topdirs=src, util; subsys=src, util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/tlm_bridge/gem5_to_tlm.cc` (churn=2)
  - `src/systemc/tlm_bridge/tlm_to_gem5.cc` (churn=2)
  - `util/tlm/src/sc_master_port.cc` (churn=2)
  - `util/tlm/src/sc_slave_port.cc` (churn=2)
- 复现: `git show 77528d192885d2778e65cbabb97faef83b68b2b0`

### #1135 arch-riscv: add agnostic option to vector tail/mask policy for mem and arith instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1135
- 代表 commit: `d20512c291c5` (2024-07-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d20512c291c54b8d0d366b040ebd9b076deacfaa`

### #1291 arch-riscv: Fix setRegs from GDB failed after #1099

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1291
- 代表 commit: `d54dcac3933b` (2024-07-09)
- 变更规模: commits=1, files=1, +6/-8 (churn=14)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/remote_gdb.cc` (churn=14)
- 复现: `git show d54dcac3933bcf6c4d9e7fd1236e9cba058578e1`

### #1285 cpu: Add cpuIdlePins to indicate the threadContext of CPU is idle

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1285
- 代表 commit: `ce8db858677d` (2024-07-10)
- 变更规模: commits=1, files=3, +29/-0 (churn=29)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/base.cc` (churn=18)
  - `src/cpu/BaseCPU.py` (churn=8)
  - `src/cpu/base.hh` (churn=3)
- 复现: `git show ce8db858677d8e66f46ba76f0637aee67f550c22`

### #1312 arch-riscv: add rv32 option to FS Linux config file

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1312
- 代表 commit: `2b902b0aec25` (2024-07-10)
- 变更规模: commits=1, files=1, +10/-0 (churn=10)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/riscv/fs_linux.py` (churn=10)
- 复现: `git show 2b902b0aec25c71119699ad58d8e260263e8195b`

### #1340 arch-riscv: fix initialization for some vector reduction insts

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1340
- 代表 commit: `8dde32d2dca1` (2024-07-10)
- 变更规模: commits=1, files=1, +3/-3 (churn=6)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=6)
- 复现: `git show 8dde32d2dca1eb7bf9618cf2f1d5bfa62cf5585b`

### #1343 util: Update gem5-resources-manager

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1343
- 代表 commit: `ebfb8999cb2c` (2024-07-11)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=4)
- 复现: `git show ebfb8999cb2c6a3ef87146515dd2cc5d097bc7bc`

### #1347 arch-riscv: Update local interrupts citation

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1347
- 代表 commit: `5e5e8fb9c6ce` (2024-07-12)
- 变更规模: commits=1, files=1, +12/-11 (churn=23)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/RiscvInterrupts.py` (churn=23)
- 复现: `git show 5e5e8fb9c6ceaade4bc3f89d37d5f1b922008064`

### #1346 arch-riscv: Overwrite getEMI() for timing expr

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1346
- 代表 commit: `9b8c84cb5d2f` (2024-07-12)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/insts/static_inst.hh` (churn=2)
- 复现: `git show 9b8c84cb5d2f71b5af4329f9ff6cdad380a6225c`

### #1275 mem: Change long in src/mem/physical.cc to int64_t

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1275
- 代表 commit: `aaa6566548f2` (2024-07-18)
- 变更规模: commits=1, files=1, +4/-18 (churn=22)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/physical.cc` (churn=22)
- 复现: `git show aaa6566548f2e45b14e4a6384a94b2f579ee7739`

### #1327 arch,arch-arm: Fix remaining implicit float conversion warnings in .isa

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1327
- 代表 commit: `fc5910942907` (2024-07-18)
- 变更规模: commits=1, files=3, +21/-5 (churn=26)
- 影响范围: topdirs=src; subsys=arch; arch=arm, isa_parser
- 主要改动文件（Top 8 by churn）:
  - `src/arch/isa_parser/isa_parser.py` (churn=15)
  - `src/arch/arm/isa/insts/fp.isa` (churn=9)
  - `src/arch/arm/isa/insts/neon.isa` (churn=2)
- 复现: `git show fc59109429079c2a7bfa0e030a5a80ecc3920af0`

### #1360 python: move cache coherence protocol check above imports

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/1360
- 代表 commit: `b6f8ecb1beba` (2024-07-22)
- 变更规模: commits=1, files=4, +16/-14 (churn=30)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/cachehierarchies/ruby/mesi_three_level_cache_hierarchy.py` (churn=9)
  - `src/python/gem5/components/cachehierarchies/chi/private_l1_cache_hierarchy.py` (churn=7)
  - `src/python/gem5/components/cachehierarchies/ruby/mesi_two_level_cache_hierarchy.py` (churn=7)
  - `src/python/gem5/components/cachehierarchies/ruby/mi_example_cache_hierarchy.py` (churn=7)
- 复现: `git show b6f8ecb1beba40c1085d75b52a1476fdfc2cdd21`

### #1331 arch-riscv: Improve widening/narrowing vectors overlap check

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1331
- 代表 commit: `82c91e8edbea` (2024-07-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 82c91e8edbea56d146b6ce28b8cc541c9bc16768`

### #1369 misc,tests: Attempt fix daily downloads

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1369
- 代表 commit: `97f6f3c4dafe` (2024-07-22)
- 变更规模: commits=1, files=1, +10/-4 (churn=14)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=14)
- 复现: `git show 97f6f3c4dafec239258be82a0e8d6eb23b741ec7`

### #1373 misc,tests: Second attempt at fixing Daily test

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1373
- 代表 commit: `e7d1c90aeb50` (2024-07-23)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=2)
- 复现: `git show e7d1c90aeb5097ea3c70ffc3f25dbede663f98b4`

### #1366 arch-vega: Multiple SOPC fixes

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1366
- 代表 commit: `7dae1a1d25c0` (2024-07-23)
- 变更规模: commits=1, files=1, +5/-5 (churn=10)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/sopc.cc` (churn=10)
- 复现: `git show 7dae1a1d25c01a9a56743222f2985d76d6e8f244`

### #1375 misc,tests: Third attempt at fixing Daily test

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1375
- 代表 commit: `7722f84d1e5a` (2024-07-24)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=2)
- 复现: `git show 7722f84d1e5a45228d359800ea4566896c4ecd43`

### #1379 arch-vega: Fix unconditional clamps in VOP3

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1379
- 代表 commit: `a7bc4ca19a11` (2024-07-25)
- 变更规模: commits=1, files=1, +9/-3 (churn=12)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/vop3.cc` (churn=12)
- 复现: `git show a7bc4ca19a11dee74332669a62c9a010484b38f9`

### #1382 Revert daily test changes

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1382
- 代表 commit: `b3b289ae8146` (2024-07-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b3b289ae81469df226ed4c19b83179b0d5e9c64d`

### #1378 arch-vega: Improve SDWA, SDWAB, and DPP

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1378
- 代表 commit: `37ca94450a6a` (2024-07-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 37ca94450a6a5314ec5998fbf4adb7d9b37c83fb`

### #1383 misc,tests: Rm gem5 binary pre-build from dailys

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1383
- 代表 commit: `b11718536e88` (2024-07-26)
- 变更规模: commits=1, files=1, +5/-73 (churn=78)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=78)
- 复现: `git show b11718536e886506b23103020ae2d2d1ff0fe337`

### #1386 tests: remove dependant job

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1386
- 代表 commit: `679000f91dd5` (2024-07-26)
- 变更规模: commits=1, files=1, +0/-2 (churn=2)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=2)
- 复现: `git show 679000f91dd50d67632cfbd46ff2886458c4f3ea`

### #1390 arch-arm: return 64-bit cycle counter for MISCREG_PMCCNTR

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1390
- 代表 commit: `b51927e7a8fd` (2024-07-29)
- 变更规模: commits=1, files=1, +1/-3 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/pmu.cc` (churn=4)
- 复现: `git show b51927e7a8fd88feb2c3959538928c8cf20deeb6`

### #1388 arch-arm: Add support for AArch32 PMEVCNTR*/PMEVTYPER*/PMCCFILTR

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1388
- 代表 commit: `b23a4c7806b6` (2024-07-29)
- 变更规模: commits=1, files=4, +109/-29 (churn=138)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/regs/misc.cc` (churn=85)
  - `src/arch/arm/regs/misc.hh` (churn=24)
  - `src/arch/arm/pmu.cc` (churn=17)
  - `src/arch/arm/tracers/tarmac_parser.cc` (churn=12)
- 复现: `git show b23a4c7806b673513fda1a71e921e2803f0725e6`

### #1391 configs: GPUFS: Disable KVM perf counters by default

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1391
- 代表 commit: `ddc9a1853633` (2024-07-29)
- 变更规模: commits=1, files=2, +11/-7 (churn=18)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gpufs/system/system.py` (churn=14)
  - `configs/example/gpufs/runfs.py` (churn=4)
- 复现: `git show ddc9a18536337455cc83a4328c724217e835b2cd`

### #1389 arch: Dump semihosting write buffer in debug output

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1389
- 代表 commit: `b64aa0b9b312` (2024-07-30)
- 变更规模: commits=1, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=generic
- 主要改动文件（Top 8 by churn）:
  - `src/arch/generic/semihosting.cc` (churn=3)
- 复现: `git show b64aa0b9b3122baa23900d411666d0b1e3c5544d`

### #1342 arch,cpu: Implement generic reset method for MMU

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1342
- 代表 commit: `c13f895af0de` (2024-07-30)
- 变更规模: commits=1, files=3, +11/-2 (churn=13)
- 影响范围: topdirs=src; subsys=arch, cpu; arch=generic
- 主要改动文件（Top 8 by churn）:
  - `src/arch/generic/mmu.cc` (churn=7)
  - `src/cpu/base.cc` (churn=4)
  - `src/arch/generic/mmu.hh` (churn=2)
- 复现: `git show c13f895af0ded0d4782baaec55d13d0826a88baf`

### #1395 misc: Remove GCN3 from maintainers

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1395
- 代表 commit: `7d4febcce342` (2024-07-30)
- 变更规模: commits=1, files=1, +0/-6 (churn=6)
- 影响范围: topdirs=MAINTAINERS.yaml; subsys=MAINTAINERS.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `MAINTAINERS.yaml` (churn=6)
- 复现: `git show 7d4febcce342244bdffd3706b98429a969bf428b`

### #1329 sim: Add error message for kernel exceeding memory size

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1329
- 代表 commit: `2bfafa726fed` (2024-07-30)
- 变更规模: commits=1, files=1, +14/-4 (churn=18)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/kernel_workload.cc` (churn=18)
- 复现: `git show 2bfafa726fedd5ac588bdc85a9ef6b4813dd038f`

### #1319 arch-riscv: Fix implicit int-to-float conversion in .isa files

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1319
- 代表 commit: `267817eaa1c1` (2024-07-31)
- 变更规模: commits=1, files=2, +7/-5 (churn=12)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=10)
  - `src/arch/riscv/isa/formats/vector_conf.isa` (churn=2)
- 复现: `git show 267817eaa1c12a005a8370f63d993cf7c029a4b4`

### #1408 misc: bump pre-commit from 3.7.1 to 3.8.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1408
- 代表 commit: `217def7bf972` (2024-08-02)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 217def7bf972f66693ae188e77af80973c4c5c73`

### #1410 mem: Fix name() helper for DRAM rank

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1410
- 代表 commit: `d2c8754ab3b7` (2024-08-03)
- 变更规模: commits=1, files=1, +5/-1 (churn=6)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/dram_interface.hh` (churn=6)
- 复现: `git show d2c8754ab3b72e52a07d62662de0120b941f1ae5`

### #1411 misc: update GPU maintainters

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1411
- 代表 commit: `a4cb466457fe` (2024-08-05)
- 变更规模: commits=1, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=MAINTAINERS.yaml; subsys=MAINTAINERS.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `MAINTAINERS.yaml` (churn=3)
- 复现: `git show a4cb466457fe866e50c57c1418aebe51e0baf6a8`

### #1407 misc: bump mypy from 1.10.1 to 1.11.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1407
- 代表 commit: `7b1948c18c78` (2024-08-05)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 7b1948c18c78350451317ad0a82a8dad46c97c92`

### #1413 gpu-compute: update GPUKernelInfo print to print WG number

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1413
- 代表 commit: `ba455e202537` (2024-08-05)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=gpu-compute; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/gpu-compute/dispatcher.cc` (churn=3)
- 复现: `git show ba455e202537834dc25d3d8181268c8a443eb63f`

### #1412 gpu-compute: fix typo in GPUMem debug print

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1412
- 代表 commit: `edd73bd33040` (2024-08-05)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=gpu-compute; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/gpu-compute/gpu_dyn_inst.cc` (churn=2)
- 复现: `git show edd73bd330401df78f4a0bae6b8dbf0332236224`

### #1406 arch-riscv: Move pmpReset implementation to MMU::reset()

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/1406
- 代表 commit: `5df08fdb0813` (2024-08-05)
- 变更规模: commits=1, files=2, +11/-6 (churn=17)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/faults.cc` (churn=9)
  - `src/arch/riscv/mmu.hh` (churn=8)
- 复现: `git show 5df08fdb0813ebf32c89a71adba10f6b90525921`

### #1374 mem: Fix "Need is_secure arg" prefetcher crash

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1374
- 代表 commit: `bd53bad5cf32` (2024-08-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bd53bad5cf32b4abbdec6062609e5ae9b4cf47bd`

### #1417 misc: Fix typo in multisim code snippet

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1417
- 代表 commit: `ba704a01b21c` (2024-08-06)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=2)
- 复现: `git show ba704a01b21c7b80a1ddcd268a7214aa46fd72fd`

### #1282 misc: Change devcontainer for isca tutorial and bootcamp

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/1282
- 代表 commit: `bb290aaff5c1` (2024-08-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bb290aaff5c197e3755fc51404089fe7d820e716`

### #1424 misc: Stable merge to dev

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1424
- 代表 commit: `bbc49aa914d2` (2024-08-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bbc49aa914d206323fcdcacf43d3209b12267c89`

### #1415 util-docker,tests: Up clang support: >=v10

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1415
- 代表 commit: `811e8c0fb4da` (2024-08-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 811e8c0fb4dafc575015e76b1458fab84f9a6f2b`

### #1385 Updating hex addr printing

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1385
- 代表 commit: `bd228af5cf86` (2024-08-07)
- 变更规模: commits=1, files=7, +7/-7 (churn=14)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/testers/traffic_gen/dram_gen.cc` (churn=2)
  - `src/cpu/testers/traffic_gen/dram_rot_gen.cc` (churn=2)
  - `src/cpu/testers/traffic_gen/hybrid_gen.cc` (churn=2)
  - `src/cpu/testers/traffic_gen/linear_gen.cc` (churn=2)
  - `src/cpu/testers/traffic_gen/nvm_gen.cc` (churn=2)
  - `src/cpu/testers/traffic_gen/random_gen.cc` (churn=2)
  - `src/cpu/testers/traffic_gen/strided_gen.cc` (churn=2)
- 复现: `git show bd228af5cf8618e4c8da2d6ca7f1f7da8bc590c3`

### #1431 gpu-compute: fix GPU TLB outstandingReqs vs. associativity

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1431
- 代表 commit: `86f7fae86bb0` (2024-08-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 86f7fae86bb02a6e3f227db9887570e8b5e575ff`

### #1316 arch-riscv: use sign-extend for all address generation

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1316
- 代表 commit: `ce07203c5fb2` (2024-08-08)
- 变更规模: commits=1, files=6, +70/-52 (churn=122)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=76)
  - `src/arch/riscv/tlb.cc` (churn=28)
  - `src/arch/riscv/isa/formats/amo.isa` (churn=6)
  - `src/arch/riscv/isa/formats/mem.isa` (churn=6)
  - `src/arch/riscv/isa/formats/standard.isa` (churn=4)
  - `src/arch/riscv/isa/formats/compressed.isa` (churn=2)
- 复现: `git show ce07203c5fb26ef30d4e70cf1d8c69846905def7`

### #1428 misc: Update GitHub badge links

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/1428
- 代表 commit: `ba0c3cc29a61` (2024-08-08)
- 变更规模: commits=1, files=1, +3/-3 (churn=6)
- 影响范围: topdirs=README.md; subsys=README.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `README.md` (churn=6)
- 复现: `git show ba0c3cc29a61d554659ac89a2522e6489996d0a9`

### #1430 dev-amdgpu: Fix issues found by address sanitizer

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1430
- 代表 commit: `85c48a36ec3e` (2024-08-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 85c48a36ec3e8cd8b9534c7c2d9cc9c33034623a`

### #1416 Updating Traffic Generators

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1416
- 代表 commit: `33e3bc4ff15e` (2024-08-08)
- 变更规模: commits=1, files=8, +81/-14 (churn=95)
- 影响范围: topdirs=src; subsys=python, cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/processors/strided_generator_core.py` (churn=30)
  - `src/python/gem5/components/processors/strided_generator.py` (churn=29)
  - `src/python/gem5/components/processors/linear_generator.py` (churn=8)
  - `src/python/gem5/components/processors/linear_generator_core.py` (churn=8)
  - `src/python/gem5/components/processors/random_generator.py` (churn=8)
  - `src/python/gem5/components/processors/random_generator_core.py` (churn=8)
  - `src/cpu/testers/traffic_gen/linear_gen.cc` (churn=2)
  - `src/cpu/testers/traffic_gen/strided_gen.cc` (churn=2)
- 复现: `git show 33e3bc4ff15e9b1c40cf287c41b87dd3ced661f6`

### #1426 util: Fix MongoDB script requirements.txt

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1426
- 代表 commit: `8593f69f0a48` (2024-08-08)
- 变更规模: commits=1, files=1, +1/-2 (churn=3)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/helper_scripts_for_mongodb/requirements.txt` (churn=3)
- 复现: `git show 8593f69f0a489dfd8d32cc95d88ccd063d5ee39d`

### #1404 mem-ruby,sim-se: Clear LL/SC locks after functional writes

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1404
- 代表 commit: `b8001a861b83` (2024-08-09)
- 变更规模: commits=1, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/Sequencer.cc` (churn=3)
- 复现: `git show b8001a861b8398d9288807dbec7eef269a72954e`

### #1364 arch-riscv: Extend wfi behavior

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1364
- 代表 commit: `e980780efd69` (2024-08-09)
- 变更规模: commits=1, files=4, +43/-5 (churn=48)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=21)
  - `src/arch/riscv/isa.hh` (churn=12)
  - `src/arch/riscv/RiscvISA.py` (churn=11)
  - `src/arch/riscv/isa.cc` (churn=4)
- 复现: `git show e980780efd690477666afc13b6633c8348dbcf59`

### #1445 arch-vega: Swizzle multi-dword scratch requests

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1445
- 代表 commit: `7d46c5066356` (2024-08-12)
- 变更规模: commits=1, files=4, +143/-22 (churn=165)
- 影响范围: topdirs=src; subsys=arch, gpu-compute; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/op_encodings.hh` (churn=61)
  - `src/arch/amdgpu/vega/gpu_mem_helpers.hh` (churn=49)
  - `src/arch/amdgpu/vega/insts/flat.cc` (churn=48)
  - `src/gpu-compute/gpu_dyn_inst.hh` (churn=7)
- 复现: `git show 7d46c5066356074ccd9f9e1fe9e7aee277cfb15f`

### #1451 arch-vega: Update microscaling format scaling and denorm handling

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1451
- 代表 commit: `c359b53a19bb` (2024-08-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c359b53a19bb969f11300a33f1f547364c02da7b`

### #1420 arch-arm: Fix incorrect behaviour of VFNMS and VFNMA

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1420
- 代表 commit: `f6f547fb6241` (2024-08-13)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/formats/fp.isa` (churn=2)
- 复现: `git show f6f547fb6241cdc80d26fee772f7afd31ab909ea`

### #1459 misc,tests: Fix compiler tests (add missing `,`)

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1459
- 代表 commit: `3640559a129f` (2024-08-13)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/compiler-tests.yaml` (churn=2)
- 复现: `git show 3640559a129fbde16d6687b9689a2bde2f319479`

### #1449 mem: Stride Prefetcher Fix

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1449
- 代表 commit: `629bf84e10c0` (2024-08-14)
- 变更规模: commits=1, files=3, +28/-7 (churn=35)
- 影响范围: topdirs=src; subsys=mem/cache/prefetch; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/prefetch/stride.cc` (churn=21)
  - `src/mem/cache/prefetch/Prefetcher.py` (churn=7)
  - `src/mem/cache/prefetch/stride.hh` (churn=7)
- 复现: `git show 629bf84e10c04f62a2358b067667820ad5b9173f`

### #1325 arch-arm: Fix incorrect operation of VRINT* instructions

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1325
- 代表 commit: `646f994efbc4` (2024-08-15)
- 变更规模: commits=1, files=1, +29/-28 (churn=57)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/insts/fp.isa` (churn=57)
- 复现: `git show 646f994efbc444b40e7a90a90ecc5158777cc554`

### #1472 arch-arm: Redirect VHE for ZCR_EL1

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1472
- 代表 commit: `280871245b81` (2024-08-16)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa.cc` (churn=2)
- 复现: `git show 280871245b812ff6090ddfbc7a8b33987454d24b`

### #1471 arch-riscv: Sign-extend the address in newPCState

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1471
- 代表 commit: `aa4fe362a582` (2024-08-19)
- 变更规模: commits=1, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.hh` (churn=3)
- 复现: `git show aa4fe362a582ca52cff0ccf83b764cc109bbc5c4`

### #1470 arch-riscv: fix GDB breakpoint issue for RV32

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1470
- 代表 commit: `b0d81ec8a2bc` (2024-08-19)
- 变更规模: commits=1, files=3, +20/-4 (churn=24)
- 影响范围: topdirs=src; subsys=arch, base; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/remote_gdb.cc` (churn=14)
  - `src/base/remote_gdb.hh` (churn=8)
  - `src/arch/riscv/remote_gdb.hh` (churn=2)
- 复现: `git show b0d81ec8a2bcece1a510b20d40bd47314b24a89e`

### #1270 gpu-compute,tests: Move GPU tests to testlib

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1270
- 代表 commit: `f600db4a98d4` (2024-08-19)
- 变更规模: commits=1, files=8, +255/-97 (churn=352)
- 影响范围: topdirs=.github, tests, configs, ext; subsys=.github, tests, configs, ext; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/gpu/test_gpu_apu_se.py` (churn=145)
  - `.github/workflows/weekly-tests.yaml` (churn=94)
  - `.github/workflows/daily-tests.yaml` (churn=48)
  - `configs/example/apu_se.py` (churn=28)
  - `.github/workflows/ci-tests.yaml` (churn=27)
  - `tests/gem5/gpu/test_gpu_ruby_random.py` (churn=4)
  - `tests/gem5/gpu/test_gpu_ruby_random_wbL2.py` (churn=4)
  - `ext/testlib/configuration.py` (churn=2)
- 复现: `git show f600db4a98d4aee4e9543d6e62e5aeeb53146126`

### #1460 docs,misc: RELEASE-NOTES.md updates for v24.1

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/1460
- 代表 commit: `7413d3217c1b` (2024-08-19)
- 变更规模: commits=1, files=1, +9/-0 (churn=9)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=9)
- 复现: `git show 7413d3217c1b926ead7cc19ea8784cf82ac4d37d`

### #1447 dev,arch-x86: Added softstrobe mode to intel8254 timer

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1447
- 代表 commit: `ce4c2c649566` (2024-08-19)
- 变更规模: commits=1, files=1, +4/-1 (churn=5)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/intel_8254_timer.cc` (churn=5)
- 复现: `git show ce4c2c649566d57c7b5c78d29f2199880b9d5a4c`

### #1292 util-docker: Cleanup, refactor, better document Dockerfiles

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/1292
- 代表 commit: `0857442e44e9` (2024-08-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0857442e44e9f46dda77982ca422e36a4c38d99c`

### #1477 misc: Update on-create.sh

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1477
- 代表 commit: `1512eddd43df` (2024-08-20)
- 变更规模: commits=1, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=.devcontainer; subsys=.devcontainer; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.devcontainer/on-create.sh` (churn=3)
- 复现: `git show 1512eddd43dfd37d22e0109ba155def4b9163db6`

### #1485 tests,gpu-compute: Fix Daily/Weekly GPU tests failures

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1485
- 代表 commit: `e7442036a57a` (2024-08-20)
- 变更规模: commits=1, files=2, +2/-2 (churn=4)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=2)
  - `.github/workflows/weekly-tests.yaml` (churn=2)
- 复现: `git show e7442036a57afd4f570aedd4021fb9a8c1908ab2`

### #1403 mem: Fixed implementation of Best Offset Prefetcher

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1403
- 代表 commit: `f6010439fe54` (2024-08-21)
- 变更规模: commits=1, files=3, +122/-48 (churn=170)
- 影响范围: topdirs=src; subsys=mem/cache/prefetch; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/prefetch/bop.cc` (churn=152)
  - `src/mem/cache/prefetch/Prefetcher.py` (churn=10)
  - `src/mem/cache/prefetch/bop.hh` (churn=8)
- 复现: `git show f6010439fe54bf39b44892e0884013f28de29642`

### #1467 stdlib: Give user's disk_device priority when setting root val

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1467
- 代表 commit: `868e287e713b` (2024-08-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 868e287e713bb26a4faa30eb92ecd30bfb45a16b`

### #1496 tests,gpu-compute: Fix gpu tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1496
- 代表 commit: `30866376d35e` (2024-08-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 30866376d35e7f99e35fdb1f718ddc6ec80b5c45`

### #1475 resources: update filtering of resources by gem5 versions

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1475
- 代表 commit: `1773001dd6b0` (2024-08-22)
- 变更规模: commits=1, files=1, +14/-9 (churn=23)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/client_api/atlasclient.py` (churn=23)
- 复现: `git show 1773001dd6b028c41b75935fd8f6a68972d75b81`

### #1446 base, mem-cache: Make the AssociativeCache more generic

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1446
- 代表 commit: `fec28e466e5d` (2024-08-23)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fec28e466e5d6a8093214879e7252d285e011940`

### #1495 arch-arm: Add place holder of registers.

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1495
- 代表 commit: `fc391cb9e876` (2024-08-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fc391cb9e8767bd51b1937d615ece5cfbfcb2f4e`

### #1512 arch-arm: Use .f32/.f64 suffixes for vfp mnemonics

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1512
- 代表 commit: `a679b9e8a337` (2024-08-25)
- 变更规模: commits=1, files=2, +11/-12 (churn=23)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/insts/fp.isa` (churn=20)
  - `src/arch/arm/insts/vfp.cc` (churn=3)
- 复现: `git show a679b9e8a337be51569e55190239c8da882a89d0`

### #1510 arch-arm: when programming an invalid PMU ID detach the counter

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1510
- 代表 commit: `ff1282260692` (2024-08-26)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/pmu.cc` (churn=1)
- 复现: `git show ff1282260692ff8373969fa7dd179790d1f84ce9`

### #1438 arch-arm: downgrade a warning to a DPRINTF

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1438
- 代表 commit: `3e288305c1e3` (2024-08-26)
- 变更规模: commits=1, files=1, +3/-2 (churn=5)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/pmu.cc` (churn=5)
- 复现: `git show 3e288305c1e3d8aa3567ad37745901000e398493`

### #1326 arch-arm: Fix implicit int-to-float conversion in VCMP

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1326
- 代表 commit: `b9eafdb19006` (2024-08-26)
- 变更规模: commits=1, files=1, +16/-16 (churn=32)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/insts/fp.isa` (churn=32)
- 复现: `git show b9eafdb190064326075109ebae8d1d3887b0b45c`

### #1515 tests: Fix gpu-tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1515
- 代表 commit: `9bd79bc1605d` (2024-08-26)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/gpu/test_gpu_apu_se.py` (churn=2)
- 复现: `git show 9bd79bc1605dd7f11f00c995c257e71387b83cb3`

### #1350 arch-vega: Pass s_memtime through smem pipe

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1350
- 代表 commit: `a8447b7fc01d` (2024-08-26)
- 变更规模: commits=1, files=25, +397/-56 (churn=453)
- 影响范围: topdirs=src, configs; subsys=mem, arch, gpu-compute, configs; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/VIPERSequencer.cc` (churn=109)
  - `src/mem/ruby/system/VIPERSequencer.hh` (churn=76)
  - `src/mem/ruby/system/Sequencer.cc` (churn=43)
  - `src/mem/ruby/system/VIPERSequencer.py` (churn=37)
  - `src/arch/amdgpu/vega/tlb.cc` (churn=32)
  - `src/mem/ruby/system/Sequencer.hh` (churn=28)
  - `src/arch/amdgpu/vega/insts/smem.cc` (churn=25)
  - `src/arch/amdgpu/common/tlb.cc` (churn=24)
- 复现: `git show a8447b7fc01d2ddbef6c078cdb15916af0b6bf36`

### #1514 base: Allow DPRINTF debugging of AssociativeCache

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1514
- 代表 commit: `d78a571660e4` (2024-08-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d78a571660e45dcdd72948f6810df3a8f72a4097`

### #1521 arch-vega: Revert incorrect SOPC compare

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1521
- 代表 commit: `bb9539ad4d03` (2024-08-29)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/sopc.cc` (churn=2)
- 复现: `git show bb9539ad4d037e83e2f7a9ae61318d467af602ac`

### #1502 arch-arm: Fix Execution Permission in Stage2 Direct Permission.

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1502
- 代表 commit: `29d6b46f1f8d` (2024-08-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 29d6b46f1f8d6e5d75a15fce5f6b03b3fbc974ec`

### #1481 dev-amdgpu: Implement UNMAP_QUEUES queue_sel==2

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1481
- 代表 commit: `403622f37646` (2024-08-31)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 403622f37646764c2857af9d0f58028df4f0b405`

### #1528 util-docker: Add labels to Dockerfiles

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1528
- 代表 commit: `a5a3810ac9e0` (2024-09-01)
- 变更规模: commits=1, files=10, +46/-1 (churn=47)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/devcontainer/Dockerfile` (churn=6)
  - `util/dockerfiles/sst/Dockerfile` (churn=5)
  - `util/dockerfiles/systemc/Dockerfile` (churn=5)
  - `util/dockerfiles/ubuntu-22.04_all-dependencies/Dockerfile` (churn=5)
  - `util/dockerfiles/ubuntu-24.04_all-dependencies/Dockerfile` (churn=5)
  - `util/dockerfiles/ubuntu-24.04_min-dependencies/Dockerfile` (churn=5)
  - `util/dockerfiles/clang-compiler/Dockerfile` (churn=4)
  - `util/dockerfiles/gcc-compiler/Dockerfile` (churn=4)
- 复现: `git show a5a3810ac9e0cf76f874ce71ef356c1cc635bf8b`

### #1482 sim-se, arch: Fix syscall parametre sizes for 32-bit OSs

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1482
- 代表 commit: `57d82fdbb45b` (2024-09-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 57d82fdbb45bd66482553dfeb92c3359ccff9790`

### #1531 misc: bump mypy from 1.11.1 to 1.11.2

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1531
- 代表 commit: `f014092fc27c` (2024-09-05)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show f014092fc27c48328e1382c1f71800d8e7315656`

### #1532 misc: bump tqdm from 4.66.4 to 4.66.5

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1532
- 代表 commit: `4d6e968b04fe` (2024-09-05)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=optional-requirements.txt; subsys=optional-requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `optional-requirements.txt` (churn=2)
- 复现: `git show 4d6e968b04fe66f895f1100d2deaa10256ff49ed`

### #1530 gpu-compute: Reuse RP list in GPU_VIPER

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1530
- 代表 commit: `51863d322fcc` (2024-09-09)
- 变更规模: commits=1, files=3, +10/-28 (churn=38)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/ruby/GPU_VIPER.py` (churn=31)
  - `configs/example/apu_se.py` (churn=4)
  - `configs/example/gpufs/runfs.py` (churn=3)
- 复现: `git show 51863d322fcc9a6c09ed1d6f2e29d6a05e4a5286`

### #1501 ext,tests,misc: Suppress incorrect GCC 12 error in Pybind

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1501
- 代表 commit: `da6ce1d9c2a1` (2024-09-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show da6ce1d9c2a1feedfd9165ca186ab41049d6374e`

### #1535 python: Ignore *args and **kwargs when generating cxxMethod pybinding script

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1535
- 代表 commit: `0da65b31c2c7` (2024-09-09)
- 变更规模: commits=1, files=1, +6/-0 (churn=6)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/SimObject.py` (churn=6)
- 复现: `git show 0da65b31c2c72884cf039ab526500a3c115dd0f7`

### #1516 cpu-o3: Panic if no FU exists for an instruction needing to issue

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1516
- 代表 commit: `ba5886aee7a5` (2024-09-11)
- 变更规模: commits=1, files=4, +60/-2 (churn=62)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/fu_pool.hh` (churn=23)
  - `src/cpu/o3/inst_queue.cc` (churn=16)
  - `src/cpu/o3/dyn_inst.hh` (churn=12)
  - `src/cpu/o3/commit.cc` (churn=11)
- 复现: `git show ba5886aee7a590f69bfe024c45c6afd08f21bec6`

### #1556 cpu-o3: Replace integral constants by named constants in FU pool

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1556
- 代表 commit: `e970acb9d25c` (2024-09-12)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/fu_pool.cc` (churn=4)
- 复现: `git show e970acb9d25cfd5d5e657611900f8b2742e4ffe4`

### #1552 arch-riscv: Change the packed data of GdbRegCache to protected

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1552
- 代表 commit: `f94cac6f6502` (2024-09-12)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/remote_gdb.hh` (churn=4)
- 复现: `git show f94cac6f6502ddb95d0a1e4b09e9ef05fad7f8ce`

### #1548 util-docker: Move `LABEL` to after image import

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/1548
- 代表 commit: `4126035f88d2` (2024-09-13)
- 变更规模: commits=1, files=10, +18/-26 (churn=44)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/devcontainer/Dockerfile` (churn=9)
  - `util/dockerfiles/systemc/Dockerfile` (churn=5)
  - `util/dockerfiles/clang-compiler/Dockerfile` (churn=4)
  - `util/dockerfiles/gcc-compiler/Dockerfile` (churn=4)
  - `util/dockerfiles/sst/Dockerfile` (churn=4)
  - `util/dockerfiles/ubuntu-22.04_all-dependencies/Dockerfile` (churn=4)
  - `util/dockerfiles/ubuntu-24.04_all-dependencies/Dockerfile` (churn=4)
  - `util/dockerfiles/ubuntu-24.04_min-dependencies/Dockerfile` (churn=4)
- 复现: `git show 4126035f88d23e51ce45ce63e16e9d2b268c7d84`

### #1486 misc,github,tests: Remove gerrit change ID requirement

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1486
- 代表 commit: `a1105cf23414` (2024-09-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a1105cf2341488770c4c157e040fbeddae9dfed3`

### #1563 misc: Fix lone header bug

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1563
- 代表 commit: `ad481167fa54` (2024-09-14)
- 变更规模: commits=1, files=1, +11/-5 (churn=16)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/git-commit-msg.py` (churn=16)
- 复现: `git show ad481167fa549a77bed6bb895c9f165014e9b5da`

### #1551 python: Redirect into correct subdirectory when using -re with multisim

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1551
- 代表 commit: `5aa7b1ce3e81` (2024-09-14)
- 变更规模: commits=1, files=4, +65/-24 (churn=89)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/main.py` (churn=52)
  - `src/python/m5/core.py` (churn=30)
  - `src/python/gem5/utils/multisim/multisim.py` (churn=4)
  - `src/python/gem5/utils/multiprocessing/_command_line.py` (churn=3)
- 复现: `git show 5aa7b1ce3e811e753e43effc2e3692671ee34d2a`

### #1499 stdlib: Issue warn if func is a gen for exit_event

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1499
- 代表 commit: `3feeb5724f23` (2024-09-17)
- 变更规模: commits=1, files=1, +14/-0 (churn=14)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/simulator.py` (churn=14)
- 复现: `git show 3feeb5724f236c3e0611c95e12ac61bf764f13dd`

### #1564 mem-ruby: Fix replacement policy in GPU_VIPER

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1564
- 代表 commit: `6d49130b0b0c` (2024-09-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6d49130b0b0c517f7e10d43bbd73c77d48ff219a`

### #1479 stdlib, python: Add warning message and clarify binary vs metric units

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1479
- 代表 commit: `f2f86a3e42bf` (2024-09-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f2f86a3e42bf4e9d52cafb412f682d08379b02d1`

### #1569 arch-arm: Fix DC IVAC for Secure EL2

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1569
- 代表 commit: `77dff262a1b4` (2024-09-18)
- 变更规模: commits=1, files=1, +1/-3 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/insts/data64.isa` (churn=4)
- 复现: `git show 77dff262a1b465a64c7dcc75b05a34276e368268`

### #1567 misc: Remove Serialize-related code in Random

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1567
- 代表 commit: `e564561d4167` (2024-09-19)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e564561d4167a2b1779a228a782ba8a47681b5ce`

### #1557 mem-cache: Do not require p.size and p.entry_size in IP template

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1557
- 代表 commit: `fee603fd842e` (2024-09-19)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fee603fd842ec6deb1903293c60868f4a049eb48`

### #1586 util-docker: Minor docker improvements/fixes

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1586
- 代表 commit: `473a37be0499` (2024-09-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 473a37be0499476dc39e16f93f501f10db3b085d`

### #1575 ext,util-docker: updated SST to v.14.0.0

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1575
- 代表 commit: `51b5279671d5` (2024-09-21)
- 变更规模: commits=1, files=5, +17/-38 (churn=55)
- 影响范围: topdirs=ext, util; subsys=ext, util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `ext/sst/Makefile` (churn=21)
  - `util/dockerfiles/sst/Dockerfile` (churn=16)
  - `ext/sst/INSTALL.md` (churn=14)
  - `ext/sst/Makefile.linux` (churn=2)
  - `ext/sst/Makefile.mac` (churn=2)
- 复现: `git show 51b5279671d5bda0b4db335eef1bd4df5b484d50`

### #1588 misc: Fix docker-build.yaml

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1588
- 代表 commit: `cae485260674` (2024-09-21)
- 变更规模: commits=1, files=1, +18/-33 (churn=51)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/docker-build.yaml` (churn=51)
- 复现: `git show cae48526067434f7f463455d48e9bdc2a6001711`

### #1592 util-docker: Minor housekeeping to Dockerfiles

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1592
- 代表 commit: `688268d22d24` (2024-09-23)
- 变更规模: commits=1, files=11, +21/-29 (churn=50)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/docker-bake.hcl` (churn=39)
  - `util/dockerfiles/ubuntu-24.04_min-dependencies/Dockerfile` (churn=2)
  - `util/dockerfiles/clang-compiler/Dockerfile` (churn=1)
  - `util/dockerfiles/devcontainer/Dockerfile` (churn=1)
  - `util/dockerfiles/gcc-compiler/Dockerfile` (churn=1)
  - `util/dockerfiles/gcn-gpu/Dockerfile` (churn=1)
  - `util/dockerfiles/gpu-fs/Dockerfile` (churn=1)
  - `util/dockerfiles/sst/Dockerfile` (churn=1)
- 复现: `git show 688268d22d24e86d672675bf7cad4cb3402b602c`

### #1587 scons: Fix scons 'readCommand' non-zero exits

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1587
- 代表 commit: `e85592da143c` (2024-09-23)
- 变更规模: commits=1, files=1, +10/-6 (churn=16)
- 影响范围: topdirs=site_scons; subsys=scons; arch=-
- 主要改动文件（Top 8 by churn）:
  - `site_scons/gem5_scons/util.py` (churn=16)
- 复现: `git show e85592da143caada133245937d856f438be23ab3`

### #1560 arch-arm: Move generateTrap from MiscRegOp to ArmStaticInst

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/1560
- 代表 commit: `c3d356b43dc5` (2024-09-23)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c3d356b43dc5eed31d88dd3d60ec5caa48ee6a5e`

### #1594 gpu-compute: Fix '64kB' to '64KiB' in gpu-compute

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1594
- 代表 commit: `2fc44a50f83e` (2024-09-23)
- 变更规模: commits=1, files=2, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=gpu-compute; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/gpu-compute/GPU.py` (churn=2)
  - `src/gpu-compute/LdsState.py` (churn=2)
- 复现: `git show 2fc44a50f83e6ceed08abd1494eb6bda398cf8fa`

### #1590 arch-riscv: Move static GDB methods to RemoteGDB virtual methods

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/1590
- 代表 commit: `e9ea18000d23` (2024-09-24)
- 变更规模: commits=1, files=2, +42/-20 (churn=62)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/remote_gdb.cc` (churn=56)
  - `src/arch/riscv/remote_gdb.hh` (churn=6)
- 复现: `git show e9ea18000d235814cd9a7cc9be5a2d0606ccf494`

### #1538 arch-riscv: add VLEN/ELEN as class attributes for all vec insts

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1538
- 代表 commit: `d1ce4fb6c7cb` (2024-09-24)
- 变更规模: commits=1, files=11, +481/-468 (churn=949)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=323)
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=266)
  - `src/arch/riscv/insts/vector.hh` (churn=206)
  - `src/arch/riscv/insts/vector.cc` (churn=48)
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=45)
  - `src/arch/riscv/isa/formats/vector_conf.isa` (churn=32)
  - `src/arch/riscv/pcstate.hh` (churn=14)
  - `src/arch/riscv/isa/formats/vector_mem.isa` (churn=6)
- 复现: `git show d1ce4fb6c7cb129652bb4d9cb6de3a925f9a5db7`

### #1580 misc: Make random gen portable across compilers.

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1580
- 代表 commit: `36264938dbe6` (2024-09-25)
- 变更规模: commits=1, files=3, +198/-6 (churn=204)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/random.test.cc` (churn=187)
  - `src/base/random.hh` (churn=16)
  - `src/base/SConscript` (churn=1)
- 复现: `git show 36264938dbe6172d0ae683c37ce4aac00185d42c`

### #1603 misc: Correctly display build information

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1603
- 代表 commit: `e17875b7c769` (2024-09-25)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/main.py` (churn=4)
- 复现: `git show e17875b7c7695bb543021f3bfd83c99e698e6723`

### #1604 util: Update gem5-resources-manager

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1604
- 代表 commit: `6bb1c9638c60` (2024-09-26)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=2)
- 复现: `git show 6bb1c9638c601ad4a095b09473060a066d1af3db`

### #1576 ext: Fix GCC v13+ comp of systemc due to problematic overloaded-virtual warn

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1576
- 代表 commit: `054790ad47ca` (2024-09-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 054790ad47ca59453fffe6f5b6a9caeb5625eb53`

### #1584 tests: Add Pannotia GPU Tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1584
- 代表 commit: `e987c60a4c14` (2024-09-26)
- 变更规模: commits=1, files=2, +347/-3 (churn=350)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/gpu/test_gpu_pannotia.py` (churn=344)
  - `tests/gem5/gpu/README.md` (churn=6)
- 复现: `git show e987c60a4c14203619703249b824bcf7af79724f`

### #1610 arch-arm: Add a method to determine External Abort

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1610
- 代表 commit: `277b5be4ddef` (2024-09-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 277b5be4ddefd5668c34ca7b5bcf5599d9337d5f`

### #1609 mem-cache: Helper functions to allow dynamic configuration of partitioning policies

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1609
- 代表 commit: `8381e1c5d3ee` (2024-09-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 8381e1c5d3ee25e494cb08456114d87a53979afd`

### #1574 arch-x86,stdlib: added MADT entries on the X86Board

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1574
- 代表 commit: `d57208c61568` (2024-10-01)
- 变更规模: commits=1, files=1, +34/-0 (churn=34)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/x86_board.py` (churn=34)
- 复现: `git show d57208c615683a7f20742d95b45e1c629fc134c9`

### #1605 tests, configs, util, mem, python, systemc: Change base 10 units to base 2

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1605
- 代表 commit: `c10feed524a8` (2024-10-01)
- 变更规模: commits=1, files=45, +135/-129 (churn=264)
- 影响范围: topdirs=configs, tests, util, src; subsys=configs, tests, util, mem, python, src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/fs/linux/arm/configs/base_config.py` (churn=22)
  - `tests/gem5/kvm_fork_tests/configs/boot_kvm_fork_run.py` (churn=14)
  - `tests/gem5/kvm_switch_tests/configs/boot_kvm_switch_exit.py` (churn=14)
  - `tests/gem5/x86_boot_tests/configs/x86_boot_exit_run.py` (churn=14)
  - `util/gem5img.py` (churn=14)
  - `util/tlm/examples/tlm_elastic_slave_with_l2.py` (churn=14)
  - `configs/common/Options.py` (churn=10)
  - `tests/gem5/arm_boot_tests/configs/arm_boot_exit_run.py` (churn=10)
- 复现: `git show c10feed524a8720864b5e3a6205e050d8d9fb190`

### #1571 stdlib: Add warning message for set_workload being called twice

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1571
- 代表 commit: `d5dfe03eb17e` (2024-10-01)
- 变更规模: commits=1, files=3, +16/-0 (churn=16)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/abstract_board.py` (churn=7)
  - `src/python/gem5/components/boards/kernel_disk_workload.py` (churn=5)
  - `src/python/gem5/components/boards/se_binary_workload.py` (churn=4)
- 复现: `git show d5dfe03eb17e2a6313bcf6cd78670ea5a864102e`

### #1559 arch-riscv: fix viota

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1559
- 代表 commit: `93313b3daac3` (2024-10-01)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=2)
- 复现: `git show 93313b3daac31dfe443912b9c6a25ffaacec2b07`

### #1616 arch-arm: Add recursive reduce in Neon instruction.

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1616
- 代表 commit: `bdd10069b1c1` (2024-10-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show bdd10069b1c1427b59afff9ec25e2f86cea6bd41`

### #1619 configs: Deprecate Vega10

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1619
- 代表 commit: `c8c75959addc` (2024-10-02)
- 变更规模: commits=1, files=6, +0/-651 (churn=651)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gpufs/vega10.py` (churn=155)
  - `configs/example/gpufs/hip_rodinia.py` (churn=150)
  - `configs/example/gpufs/hip_cookbook.py` (churn=142)
  - `configs/example/gpufs/hip_samples.py` (churn=140)
  - `configs/example/gpufs/vega10_atomic.py` (churn=32)
  - `configs/example/gpufs/vega10_kvm.py` (churn=32)
- 复现: `git show c8c75959addc2f3262d2aa636b9456ed42af6ba4`

### #1621 dev-amdgpu: Use GPU specific cache line size

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1621
- 代表 commit: `24504c9a3eeb` (2024-10-03)
- 变更规模: commits=1, files=5, +7/-4 (churn=11)
- 影响范围: topdirs=src, configs; subsys=dev, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/amdgpu/amdgpu_device.cc` (churn=3)
  - `configs/example/gpufs/system/system.py` (churn=2)
  - `src/dev/amdgpu/AMDGPU.py` (churn=2)
  - `src/dev/amdgpu/memory_manager.cc` (churn=2)
  - `src/dev/amdgpu/memory_manager.hh` (churn=2)
- 复现: `git show 24504c9a3eebff840c7f97c654c4a0b1f0f884a8`

### #1617 arch-riscv: Implement CLINT reset feature

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1617
- 代表 commit: `5b5f7afc1ba9` (2024-10-03)
- 变更规模: commits=1, files=3, +48/-4 (churn=52)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/riscv/clint.cc` (churn=31)
  - `src/dev/riscv/clint.hh` (churn=13)
  - `src/dev/riscv/Clint.py` (churn=8)
- 复现: `git show 5b5f7afc1ba9ccaf821528ca3891a36934454592`

### #1627 util-docker: Fix gpu dpcker images

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1627
- 代表 commit: `7117b1399b30` (2024-10-04)
- 变更规模: commits=1, files=3, +6/-4 (churn=10)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/docker-bake.hcl` (churn=8)
  - `util/dockerfiles/gcn-gpu/Dockerfile` (churn=1)
  - `util/dockerfiles/gpu-fs/Dockerfile` (churn=1)
- 复现: `git show 7117b1399b30589401bd204804aa52827addd1a0`

### #1628 misc,tests: Increase Weekly and Daily GPU test timeout

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1628
- 代表 commit: `6a24b69a9726` (2024-10-04)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6a24b69a972684544385ef7abd1e96c3644c1bb4`

### #1633 dev-amdgpu: Deprecate rom and mmio trace params

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1633
- 代表 commit: `f5858fe81f85` (2024-10-07)
- 变更规模: commits=1, files=4, +1/-29 (churn=30)
- 影响范围: topdirs=configs, src; subsys=configs, dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gpufs/runfs.py` (churn=16)
  - `src/dev/amdgpu/amdgpu_device.cc` (churn=10)
  - `configs/example/gpufs/system/amdgpu.py` (churn=2)
  - `src/dev/amdgpu/AMDGPU.py` (churn=2)
- 复现: `git show f5858fe81f8537a708293ebc4d430da3549f5e2f`

### #1625 python: clarify SimObject error message

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1625
- 代表 commit: `1ee924a0677b` (2024-10-07)
- 变更规模: commits=1, files=1, +3/-1 (churn=4)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/SimObject.py` (churn=4)
- 复现: `git show 1ee924a0677b1917e705990f72668cbb4601ed38`

### #1620 arch-riscv: Enable clone3 syscall in riscv64

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1620
- 代表 commit: `6ff3821c9dbe` (2024-10-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6ff3821c9dbe93dd009fc2759e5687fe43bc2836`

### #1635 learning-gem5,tests: Update learning-gem5 Ruby Test ref

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/1635
- 代表 commit: `3fc21da13c70` (2024-10-07)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/learning_gem5/ref/test` (churn=2)
- 复现: `git show 3fc21da13c70273e51744083c90a82b1cd428e23`

### #1639 cpu-o3: Add Crypto OpDesc to the O3 Default FU

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1639
- 代表 commit: `440999e447a1` (2024-10-08)
- 变更规模: commits=1, files=1, +9/-1 (churn=10)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/FuncUnitConfig.py` (churn=10)
- 复现: `git show 440999e447a19db88eeff9d4df4d8f7ba1ddf4e6`

### #1640 cpu-o3: Add Matrix OpDesc to the O3 Default FU

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1640
- 代表 commit: `4a3e2633d2d9` (2024-10-08)
- 变更规模: commits=1, files=2, +11/-1 (churn=12)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/FuncUnitConfig.py` (churn=9)
  - `src/cpu/o3/FUPool.py` (churn=3)
- 复现: `git show 4a3e2633d2d90d44b0fe7306f35774b865217fa5`

### #1453 mem-ruby: Remove static methods from RubySystem

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1453
- 代表 commit: `4f7b3ed82741` (2024-10-08)
- 变更规模: commits=1, files=123, +1066/-399 (churn=1465)
- 影响范围: topdirs=src, configs; subsys=mem, python, configs, cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/slicc/symbols/Type.py` (churn=124)
  - `src/mem/ruby/common/DataBlock.cc` (churn=91)
  - `src/mem/ruby/common/NetDest.cc` (churn=57)
  - `src/mem/ruby/slicc_interface/AbstractController.cc` (churn=56)
  - `src/mem/slicc/symbols/StateMachine.py` (churn=49)
  - `src/mem/ruby/system/Sequencer.cc` (churn=43)
  - `src/mem/ruby/slicc_interface/RubyRequest.hh` (churn=41)
  - `src/mem/ruby/system/VIPERCoalescer.cc` (churn=38)
- 复现: `git show 4f7b3ed82741a6adc198d1b0cf818f6fa2c93bde`

### #1638 arch-riscv: Fix CLINT mtime reset handling

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1638
- 代表 commit: `67edf6432678` (2024-10-08)
- 变更规模: commits=1, files=2, +13/-4 (churn=17)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/riscv/clint.cc` (churn=10)
  - `src/dev/riscv/clint.hh` (churn=7)
- 复现: `git show 67edf64326788e0152a0f6adcf71aacd885c92f5`

### #1641 cpu,arch,arch-riscv: Check wake up signal when post interrupt

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1641
- 代表 commit: `402a030ce159` (2024-10-08)
- 变更规模: commits=1, files=3, +16/-1 (churn=17)
- 影响范围: topdirs=src; subsys=arch, cpu; arch=generic, riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/generic/interrupts.hh` (churn=6)
  - `src/cpu/base.cc` (churn=6)
  - `src/arch/riscv/interrupts.hh` (churn=5)
- 复现: `git show 402a030ce1590dafe6721e6eed42de3ae5245983`

### #1595 misc,tests: Add cache of ALL/gem5.opt to ci-test.yaml

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1595
- 代表 commit: `cc0eb12e9a5f` (2024-10-09)
- 变更规模: commits=1, files=1, +57/-6 (churn=63)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/ci-tests.yaml` (churn=63)
- 复现: `git show cc0eb12e9a5fbc250ab14b71e2876f1f3ef6e6d1`

### #1637 systemc: Disable 'overloaded-virtual' warn for systemc bind funcs

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1637
- 代表 commit: `ee9135663283` (2024-10-09)
- 变更规模: commits=1, files=4, +38/-10 (churn=48)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/ext/tlm_core/2/sockets/initiator_socket.hh` (churn=18)
  - `src/systemc/ext/core/sc_port.hh` (churn=17)
  - `src/systemc/ext/core/sc_export.hh` (churn=12)
  - `src/systemc/ext/tlm_core/2/sockets/target_socket.hh` (churn=1)
- 复现: `git show ee91356632835b54aefecdfa143601a9a6a996ce`

### #1509 Use board get_mem_ports consistently

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1509
- 代表 commit: `f03dddb458e5` (2024-10-09)
- 变更规模: commits=1, files=5, +12/-8 (churn=20)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/arm_board.py` (churn=12)
  - `src/python/gem5/components/cachehierarchies/classic/no_cache.py` (churn=2)
  - `src/python/gem5/components/cachehierarchies/classic/private_l1_cache_hierarchy.py` (churn=2)
  - `src/python/gem5/components/cachehierarchies/classic/private_l1_private_l2_cache_hierarchy.py` (churn=2)
  - `src/python/gem5/components/cachehierarchies/classic/private_l1_shared_l2_cache_hierarchy.py` (churn=2)
- 复现: `git show f03dddb458e5a9f0a0279ce053f478417a4666eb`

### #1642 misc: pre-commit autoupdate

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1642
- 代表 commit: `965da9ea79ad` (2024-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 965da9ea79ade729aed3c548677f24d40c6e0b0e`

### #1647 misc: Add "src/python" to vscode Python Analysis Paths

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1647
- 代表 commit: `34437880138d` (2024-10-09)
- 变更规模: commits=1, files=1, +5/-0 (churn=5)
- 影响范围: topdirs=.vscode; subsys=.vscode; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.vscode/settings.json` (churn=5)
- 复现: `git show 34437880138d44fb5de3b10a7c65079a2831a89f`

### #1615 cpu: fix simInsts and simOps not resetting

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1615
- 代表 commit: `feeb3b2d6725` (2024-10-09)
- 变更规模: commits=1, files=3, +29/-5 (churn=34)
- 影响范围: topdirs=src; subsys=cpu, cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/base.hh` (churn=28)
  - `src/cpu/base.cc` (churn=4)
  - `src/cpu/o3/probe/elastic_trace.cc` (churn=2)
- 复现: `git show feeb3b2d672557b1470e02a502a5f334a795eaab`

### #1533 arch-arm: Add support of AArch32 VCVTA/P/N/M instructions.

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1533
- 代表 commit: `1c8ab47a5484` (2024-10-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1c8ab47a54847f9110dfe582dc076a780a1507ac`

### #1537 Implement BTB using the cache library

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1537
- 代表 commit: `50f652a2ee19` (2024-10-10)
- 变更规模: commits=1, files=8, +394/-133 (churn=527)
- 影响范围: topdirs=src, configs; subsys=cpu/pred, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/btb_entry.hh` (churn=288)
  - `src/cpu/pred/simple_btb.cc` (churn=91)
  - `src/cpu/pred/simple_btb.hh` (churn=72)
  - `src/cpu/pred/BranchPredictor.py` (churn=47)
  - `configs/common/cores/arm/HPI.py` (churn=9)
  - `configs/common/cores/arm/O3_ARM_v7a.py` (churn=9)
  - `configs/common/cores/arm/ex5_big.py` (churn=9)
  - `src/cpu/pred/SConscript` (churn=2)
- 复现: `git show 50f652a2ee19c57f0d41541d6d8e73a59c5b62d8`

### #1649 stdlib,ruby: Enable resetting version numbers

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1649
- 代表 commit: `3f42ab4ca915` (2024-10-10)
- 变更规模: commits=1, files=6, +61/-0 (churn=61)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/cachehierarchies/ruby/caches/prebuilt/octopi_cache/octopi.py` (churn=14)
  - `src/python/gem5/components/cachehierarchies/ruby/abstract_ruby_cache_hierarchy.py` (churn=12)
  - `src/python/gem5/components/cachehierarchies/ruby/mesi_three_level_cache_hierarchy.py` (churn=10)
  - `src/python/gem5/components/cachehierarchies/ruby/mesi_two_level_cache_hierarchy.py` (churn=9)
  - `src/python/gem5/components/cachehierarchies/chi/private_l1_cache_hierarchy.py` (churn=8)
  - `src/python/gem5/components/cachehierarchies/ruby/mi_example_cache_hierarchy.py` (churn=8)
- 复现: `git show 3f42ab4ca915f8db0d929b032e0851b37e8256dc`

### #1654 tests,misc: Remove `edited` from PR Action trigger list

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1654
- 代表 commit: `c1c5147e530c` (2024-10-10)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/ci-tests.yaml` (churn=2)
- 复现: `git show c1c5147e530ce05e0f1c8f60dbb769c809f5194b`

### #1646 util-docker,tests: Add compiler tests & Dockerfiles for GCC 14

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1646
- 代表 commit: `6195b33960c4` (2024-10-10)
- 变更规模: commits=1, files=3, +16/-5 (churn=21)
- 影响范围: topdirs=util, .github; subsys=util, .github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/docker-bake.hcl` (churn=13)
  - `.github/workflows/compiler-tests.yaml` (churn=6)
  - `util/dockerfiles/gcc-compiler/Dockerfile` (churn=2)
- 复现: `git show 6195b33960c4821cb21d671b32fa10bb7944557a`

### #1653 tests: Refactor downloading of pannotia tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1653
- 代表 commit: `65ba2dcae51c` (2024-10-10)
- 变更规模: commits=1, files=1, +9/-7 (churn=16)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/gpu/test_gpu_pannotia.py` (churn=16)
- 复现: `git show 65ba2dcae51c97010439d07b5d0618b13b83cc2a`

### #1652 misc: Add 'ext' & 'tests' to vscode pythin extraPaths

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1652
- 代表 commit: `a8f88abfb12f` (2024-10-10)
- 变更规模: commits=1, files=1, +3/-1 (churn=4)
- 影响范围: topdirs=.vscode; subsys=.vscode; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.vscode/settings.json` (churn=4)
- 复现: `git show a8f88abfb12f772f496aad2b9866476f7ebb575c`

### #1657 dev: Make unknown PCI device writes a warning

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1657
- 代表 commit: `1edeeda88156` (2024-10-14)
- 变更规模: commits=1, files=1, +8/-3 (churn=11)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/pci/host.cc` (churn=11)
- 复现: `git show 1edeeda88156f552a7de482f3e1a04fcb9866332`

### #1525 arch-riscv: Add support for riscv hardware probing syscall

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1525
- 代表 commit: `652a72d122ff` (2024-10-14)
- 变更规模: commits=1, files=2, +482/-0 (churn=482)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/linux/se_workload.cc` (churn=386)
  - `src/arch/riscv/linux/linux.hh` (churn=96)
- 复现: `git show 652a72d122ff2c9404c9122e98d035ed6efb7d36`

### #1497 stdlib: Extend `AbstractBoard` pre_instantiation functionality

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1497
- 代表 commit: `20965f571bef` (2024-10-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 20965f571bef5d9a61e3d108e5b6844f1ad6ff0d`

### #1664 arch-vega: Fix multi-dword setElem in PackedReg

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1664
- 代表 commit: `deb8f983a1cb` (2024-10-14)
- 变更规模: commits=1, files=1, +6/-3 (churn=9)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/operand.hh` (churn=9)
- 复现: `git show deb8f983a1cb8bd94885efa290f1f293eea63711`

### #1643 arch-x86,arch-arm: Remove static variables in decoders

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1643
- 代表 commit: `f55a4ce98960` (2024-10-17)
- 变更规模: commits=1, files=4, +13/-20 (churn=33)
- 影响范围: topdirs=src; subsys=arch; arch=arm, x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/decoder.hh` (churn=24)
  - `src/arch/x86/decoder.cc` (churn=5)
  - `src/arch/arm/decoder.cc` (churn=2)
  - `src/arch/arm/decoder.hh` (churn=2)
- 复现: `git show f55a4ce98960d4d624fb211a2b85567f0abe2bef`

### #1542 SE script and tests for risc-v's vector extension

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1542
- 代表 commit: `0341c5a50229` (2024-10-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0341c5a502290216204efcd7792ac11dddef5f7b`

### #1618 stdlib,arch-x86: Update X86Demoboard

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1618
- 代表 commit: `d454e421d231` (2024-10-17)
- 变更规模: commits=1, files=1, +65/-20 (churn=85)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/prebuilt/demo/x86_demo_board.py` (churn=85)
- 复现: `git show d454e421d231246a443231c7d94e0761feabf0ec`

### #1678 tests: Fix compiler tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1678
- 代表 commit: `7591f2a84378` (2024-10-17)
- 变更规模: commits=1, files=4, +6/-6 (churn=12)
- 影响范围: topdirs=src; subsys=arch, base, dev, mem/cache; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/base/stats/units.hh` (churn=4)
  - `src/dev/virtio/base.hh` (churn=4)
  - `src/arch/arm/faults.hh` (churn=2)
  - `src/mem/cache/cache_blk.hh` (churn=2)
- 复现: `git show 7591f2a84378c4810f58459b4e39d74457045405`

### #1432 arch-riscv: Implement Zcmp instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1432
- 代表 commit: `cb5d14f75335` (2024-10-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show cb5d14f7533522972b42a3152bad5cae5b1f2f8e`

### #1478 arch-arm: Add arm demo board

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1478
- 代表 commit: `946bf83b7520` (2024-10-18)
- 变更规模: commits=1, files=3, +205/-0 (churn=205)
- 影响范围: topdirs=src, configs; subsys=python, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/prebuilt/demo/arm_demo_board.py` (churn=112)
  - `configs/example/gem5_library/arm-demo-ubuntu-run.py` (churn=92)
  - `src/python/SConscript` (churn=1)
- 复现: `git show 946bf83b75205f11c3c6cdaba274caa4a9e16046`

### #1662 systemc: Disable 'overloaded-virtual' warn for clang

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1662
- 代表 commit: `ae0cee66ed3c` (2024-10-18)
- 变更规模: commits=1, files=4, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/ext/core/sc_export.hh` (churn=2)
  - `src/systemc/ext/core/sc_port.hh` (churn=2)
  - `src/systemc/ext/tlm_core/2/sockets/initiator_socket.hh` (churn=2)
  - `src/systemc/ext/tlm_core/2/sockets/target_socket.hh` (churn=2)
- 复现: `git show ae0cee66ed3c09d25b8e73e349d9b0800b3c30da`

### #1679 sim: Make SignalSinkPort::set virtual

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1679
- 代表 commit: `3fc6cc7763e6` (2024-10-18)
- 变更规模: commits=1, files=1, +3/-2 (churn=5)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/signal.hh` (churn=5)
- 复现: `git show 3fc6cc7763e67ebab76358845b06b94060dd19be`

### #1454 mem-cache: Implementation of SMS prefetcher

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1454
- 代表 commit: `2e271459d098` (2024-10-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2e271459d09879d4fa52fde4c4693f1f3906057f`

### #1631 tests: update input sizes for pannotia tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1631
- 代表 commit: `b836a3f239c1` (2024-10-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b836a3f239c10db46ffc4fafc67183486db92033`

### #1666 scons,misc: Portable debug flag generation

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1666
- 代表 commit: `3e83f3ce4f52` (2024-10-18)
- 变更规模: commits=1, files=1, +14/-10 (churn=24)
- 影响范围: topdirs=build_tools; subsys=build_tools; arch=-
- 主要改动文件（Top 8 by churn）:
  - `build_tools/debugflaghh.py` (churn=24)
- 复现: `git show 3e83f3ce4f5283f78e754eca566c42b90957c57b`

### #1685 mem-ruby,misc: Remove redundant assignment

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1685
- 代表 commit: `db47d203718c` (2024-10-20)
- 变更规模: commits=1, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/common/DataBlock.cc` (churn=1)
- 复现: `git show db47d203718c0f3c6523608daaea58972401a607`

### #1686 learning-gem5: Add `ruby_system` param set to `RubyPortProxy`

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/1686
- 代表 commit: `b705629b83ac` (2024-10-20)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/learning_gem5/part3/test_caches.py` (churn=2)
- 复现: `git show b705629b83acbc3c968a1fb64ceb283aca54af72`

### #1695 tests: Fix replacement_policies tests' refs

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1695
- 代表 commit: `2c679bfa04c0` (2024-10-21)
- 变更规模: commits=1, files=42, +498/-498 (churn=996)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/replacement_policies/ref/lfu_test3_ld` (churn=50)
  - `tests/gem5/replacement_policies/ref/lfu_test3_st` (churn=50)
  - `tests/gem5/replacement_policies/ref/lru_test1_ld` (churn=32)
  - `tests/gem5/replacement_policies/ref/lru_test1_st` (churn=32)
  - `tests/gem5/replacement_policies/ref/lip_test1_ld` (churn=30)
  - `tests/gem5/replacement_policies/ref/lip_test1_st` (churn=30)
  - `tests/gem5/replacement_policies/ref/second_chance_test3_ld` (churn=30)
  - `tests/gem5/replacement_policies/ref/second_chance_test3_st` (churn=30)
- 复现: `git show 2c679bfa04c0bb01701b292fa6ed446b000a2db8`

### #1690 mem-ruby: Fix issues in protocols due to multi-RubySystem

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1690
- 代表 commit: `16217f843fb2` (2024-10-21)
- 变更规模: commits=1, files=7, +10/-2 (churn=12)
- 影响范围: topdirs=src, configs; subsys=mem, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/slicc/symbols/StateMachine.py` (churn=5)
  - `configs/learning_gem5/part3/msi_caches.py` (churn=2)
  - `src/mem/ruby/common/WriteMask.hh` (churn=1)
  - `src/mem/ruby/protocol/chi/CHI-cache-funcs.sm` (churn=1)
  - `src/mem/ruby/protocol/chi/CHI-mem.sm` (churn=1)
  - `src/mem/ruby/system/RubySystem.cc` (churn=1)
  - `src/mem/slicc/symbols/Type.py` (churn=1)
- 复现: `git show 16217f843fb23de431c7645ed6bc75e9bcff6db0`

### #1684 dev: move dprint of reg name before register read/write

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/1684
- 代表 commit: `fce42880b945` (2024-10-22)
- 变更规模: commits=1, files=1, +14/-17 (churn=31)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/reg_bank.hh` (churn=31)
- 复现: `git show fce42880b945edde627cae42cdab41a84b91105b`

### #1697 arch-arm: Implement AT as standalone instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1697
- 代表 commit: `0f75c39d30a8` (2024-10-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0f75c39d30a8402e9e6cedb3c6784d36b53907eb`

### #1683 arch-x86: break 32/64-bit LEA's input dependency on prior dest value

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1683
- 代表 commit: `faf764e66854` (2024-10-22)
- 变更规模: commits=1, files=1, +8/-1 (churn=9)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/isa/microops/ldstop.isa` (churn=9)
- 复现: `git show faf764e66854179decd8773770c8159c82cc54aa`

### #1655 arch-arm: Add support of AArch32 VRINTN/X/A/Z/M/P instructions.

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1655
- 代表 commit: `3a14a73982d1` (2024-10-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3a14a73982d1682fee280a1df4b6494118a16b99`

### #1490 stdlib, configs: Add RiscvDemoBoard

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1490
- 代表 commit: `f01d68bf9676` (2024-10-22)
- 变更规模: commits=1, files=4, +319/-8 (churn=327)
- 影响范围: topdirs=src, configs; subsys=python, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/prebuilt/demo/riscv_demo_board.py` (churn=188)
  - `configs/example/gem5_library/riscv-demo-board-run.py` (churn=112)
  - `src/python/gem5/components/boards/riscv_board.py` (churn=26)
  - `src/python/SConscript` (churn=1)
- 复现: `git show f01d68bf967643d35b4fa449d80acac3ab96938a`

### #1526 arch-riscv: Fix the bug of vsetivli frequently flushing the pipeline

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1526
- 代表 commit: `35db93ada4dd` (2024-10-23)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 35db93ada4ddc3494f2faff348a66c3c9e255d1c`

### #1651 mem-ruby,tests: Add CHI with ISA tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1651
- 代表 commit: `709f2c769534` (2024-10-23)
- 变更规模: commits=1, files=4, +237/-0 (churn=237)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/chi_protocol/configs/chi-with-isa.py` (churn=144)
  - `tests/gem5/chi_protocol/test_chi_per_isa.py` (churn=77)
  - `tests/gem5/chi_protocol/README.md` (churn=8)
  - `tests/gem5/chi_protocol/refs/matrix-multiply-stdout.txt` (churn=8)
- 复现: `git show 709f2c769534936e0b154fdbb78bc535de9ae04e`

### #1698 tests: move weekly gpu tests to have separate jobs

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1698
- 代表 commit: `c91af552d469` (2024-10-24)
- 变更规模: commits=1, files=2, +236/-78 (churn=314)
- 影响范围: topdirs=.github, tests; subsys=.github, tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/weekly-tests.yaml` (churn=173)
  - `tests/gem5/gpu/test_gpu_pannotia.py` (churn=141)
- 复现: `git show c91af552d4698473afd4f83c943436ddadf11efe`

### #1713 arch-arm: Replace translateAtomic with translateFunctional in AT

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1713
- 代表 commit: `c9f94f4e0639` (2024-10-25)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/insts/misc64.cc` (churn=2)
- 复现: `git show c9f94f4e06391d956b3f43b2f7940bf6cbcc7347`

### #1716 util-docker: Add RISCV to Ubuntu all-deps Docker platforms

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1716
- 代表 commit: `dde1c7d3a105` (2024-10-26)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/docker-bake.hcl` (churn=1)
- 复现: `git show dde1c7d3a105845929b0e972b5e3ffcc27b448ba`

### #1399 mem-ruby: Prevent LL/SC livelock in MESI protocols (#1384)

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1399
- 代表 commit: `7bddc764cc23` (2024-10-28)
- 变更规模: commits=1, files=3, +334/-13 (churn=347)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/MESI_Two_Level-L1cache.sm` (churn=181)
  - `src/mem/ruby/protocol/MESI_Three_Level-L0cache.sm` (churn=162)
  - `src/mem/ruby/system/Sequencer.cc` (churn=4)
- 复现: `git show 7bddc764cc23be8efdb66ac02faed9814fb5143c`

### #1693 configs,scons: Update scripts and build_opts to make GPU-FS simulations more configurable

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1693
- 代表 commit: `853f2ea0127d` (2024-10-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 853f2ea0127d38e616c30d16b715a64cb8655c5a`

### #1694 mem-ruby: Re-enable assign with implicit_ctor structures

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1694
- 代表 commit: `1442a4dccd43` (2024-10-29)
- 变更规模: commits=1, files=3, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/slicc/ast/AssignStatementAST.py` (churn=4)
  - `src/mem/slicc/ast/LocalVariableAST.py` (churn=2)
  - `src/mem/slicc/ast/MemberExprAST.py` (churn=2)
- 复现: `git show 1442a4dccd43ebe32fda9272325bc4e6f1925a35`

### #1650 mem-ruby: Remove unused variables/mark [maybe unused]

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1650
- 代表 commit: `d8e7c91127ee` (2024-10-29)
- 变更规模: commits=1, files=2, +1/-6 (churn=7)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/slicc_interface/Message.hh` (churn=6)
  - `src/mem/ruby/structures/BankedArray.hh` (churn=1)
- 复现: `git show d8e7c91127ee288d5c49d94cc0119ba5c6c911f7`

### #1731 util-docker: Add qemu-riscv-env Dockerfile

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1731
- 代表 commit: `d5d788084015` (2024-10-29)
- 变更规模: commits=1, files=2, +50/-0 (churn=50)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/qemu-riscv-env/Dockerfile` (churn=41)
  - `util/dockerfiles/docker-bake.hcl` (churn=9)
- 复现: `git show d5d7880840150420e0bade6df9513a6d9f02d474`

### #1702 Add SE mode to X86Board and RiscvBoard

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1702
- 代表 commit: `2c6de97ea1a0` (2024-10-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2c6de97ea1a0df1e507fccb7d91a1c09e4e087c6`

### #1736 tests: update timout on pannotia fw gpu test

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1736
- 代表 commit: `24b672ab01dd` (2024-10-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 24b672ab01dd0640e084e2bebd0b2cf26d18ffb3`

### #1727 arch-riscv: Fix Zcmp implement typos

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1727
- 代表 commit: `757b272a2590` (2024-10-30)
- 变更规模: commits=1, files=1, +5/-5 (churn=10)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/formats/zcmp.isa` (churn=10)
- 复现: `git show 757b272a2590f595ea8edbebfd9f959337639a0c`

### #1737 sim: Add include guards in simulate.hh

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1737
- 代表 commit: `b5a73b59eff5` (2024-10-31)
- 变更规模: commits=1, files=1, +5/-0 (churn=5)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/simulate.hh` (churn=5)
- 复现: `git show b5a73b59eff553963f8d9939ee8eb9640311a074`

### #1724 base: Remove DPRINTF_UNCONDITIONAL

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1724
- 代表 commit: `ad17fa040ad4` (2024-10-31)
- 变更规模: commits=1, files=2, +0/-39 (churn=39)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/trace.test.cc` (churn=28)
  - `src/base/trace.hh` (churn=11)
- 复现: `git show ad17fa040ad4d1f9638b562d6be0fcb888b4d8a8`

### #1732 arch-x86: Update MTRR defType register

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1732
- 代表 commit: `df6a318a8646` (2024-11-01)
- 变更规模: commits=1, files=1, +8/-0 (churn=8)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/isa.cc` (churn=8)
- 复现: `git show df6a318a8646b5a9b197dfe27a3b4c36ac31741c`

### #1739 arch-arm: Do not compute purifyTaggedAddr in checkPermissions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1739
- 代表 commit: `a2476373c9ec` (2024-11-01)
- 变更规模: commits=1, files=1, +1/-4 (churn=5)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/mmu.cc` (churn=5)
- 复现: `git show a2476373c9ec3aca79acbe750eaa93c9c8718225`

### #1723 util: Bumps werkzeug in gem5-resources-manager

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1723
- 代表 commit: `cc4f466e1e87` (2024-11-01)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=2)
- 复现: `git show cc4f466e1e87e742599a940cb5777fa45b5e7f0d`

### #1661 arch-arm: Rewrite the ArmTLB storage to use an AssociativeCache

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/1661
- 代表 commit: `d37636025544` (2024-11-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d37636025544160e9fb123896bbfa96d4d37d495`

### #1744 Add Python interface to get port actual name

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1744
- 代表 commit: `956b164a43a1` (2024-11-02)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/python.cc` (churn=1)
- 复现: `git show 956b164a43a12fc4566235dacc12400abfb6d5e6`

### #1746 mem-ruby: Fix two NetDest locals using default constructor

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1746
- 代表 commit: `2ed724b670bc` (2024-11-02)
- 变更规模: commits=1, files=2, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/network/garnet/NetworkInterface.cc` (churn=2)
  - `src/mem/ruby/slicc_interface/AbstractController.cc` (churn=2)
- 复现: `git show 2ed724b670bcbf20ed0e8f8d5afc18ed24e1e5d3`

### #1751 arch-arm: Use the cached release object instead of HaveExt

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1751
- 代表 commit: `4f74c3a949d2` (2024-11-03)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/mmu.cc` (churn=4)
- 复现: `git show 4f74c3a949d259cec9b68c0709d25d0b08c53072`

### #1747 misc: bump tqdm from 4.66.5 to 4.66.6

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1747
- 代表 commit: `dba9a9e56476` (2024-11-04)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=optional-requirements.txt; subsys=optional-requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `optional-requirements.txt` (churn=2)
- 复现: `git show dba9a9e564763daf61fa024c604530898a19eea3`

### #1710 arch-riscv: Add support for Zicbop extension

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1710
- 代表 commit: `2e998c9fc007` (2024-11-04)
- 变更规模: commits=1, files=2, +76/-5 (churn=81)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/formats/mem.isa` (churn=46)
  - `src/arch/riscv/isa/decoder.isa` (churn=35)
- 复现: `git show 2e998c9fc007a334ea890037f7aed7d1aecd4aec`

### #1752 arch-arm: Cache a pointer to previously matched TLB entry

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1752
- 代表 commit: `3e628dd1c0e8` (2024-11-05)
- 变更规模: commits=1, files=2, +35/-3 (churn=38)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/tlb.cc` (churn=23)
  - `src/arch/arm/tlb.hh` (churn=15)
- 复现: `git show 3e628dd1c0e81906bdb0aea9d5c66761b64fd78d`

### #1734 base: Make BaseGdbRegCache::data() non constant

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1734
- 代表 commit: `940f49b63b6d` (2024-11-05)
- 变更规模: commits=1, files=7, +12/-12 (churn=24)
- 影响范围: topdirs=src; subsys=arch, base; arch=arm, mips, power, riscv, sparc, x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/remote_gdb.hh` (churn=4)
  - `src/arch/power/remote_gdb.hh` (churn=4)
  - `src/arch/riscv/remote_gdb.hh` (churn=4)
  - `src/arch/sparc/remote_gdb.hh` (churn=4)
  - `src/arch/x86/remote_gdb.hh` (churn=4)
  - `src/arch/mips/remote_gdb.hh` (churn=2)
  - `src/base/remote_gdb.hh` (churn=2)
- 复现: `git show 940f49b63b6d6462226598110d3f8a578cf87875`

### #1692 dev-amdgpu, gpu-compute, mem-ruby: Add support for writeback L2 in GPU

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1692
- 代表 commit: `d463868f28f1` (2024-11-05)
- 变更规模: commits=1, files=11, +286/-23 (churn=309)
- 影响范围: topdirs=src; subsys=mem, gpu-compute, dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm` (churn=118)
  - `src/mem/ruby/protocol/GPU_VIPER-TCC.sm` (churn=54)
  - `src/gpu-compute/gpu_command_processor.cc` (churn=46)
  - `src/dev/amdgpu/amdgpu_device.cc` (churn=23)
  - `src/gpu-compute/gpu_command_processor.hh` (churn=15)
  - `src/mem/request.hh` (churn=15)
  - `src/gpu-compute/compute_unit.cc` (churn=13)
  - `src/gpu-compute/compute_unit.hh` (churn=10)
- 复现: `git show d463868f28f1c9cf439f8b3cf784a5b3c069828a`

### #1735 misc: Add v24.1 release notes for RubySystem changes

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1735
- 代表 commit: `6881534bd2a3` (2024-11-05)
- 变更规模: commits=1, files=1, +20/-0 (churn=20)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=20)
- 复现: `git show 6881534bd2a3e11125046638044c0ceb9e4c20cd`

### #1753 configs: Update legacy RISC-V FS Linux script

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1753
- 代表 commit: `7f5037297997` (2024-11-05)
- 变更规模: commits=1, files=1, +31/-12 (churn=43)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/riscv/fs_linux.py` (churn=43)
- 复现: `git show 7f5037297997ce1f0592db296f3cbdc1816c2f5b`

### #1759 arch-riscv: fix vrgather pin count

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1759
- 代表 commit: `63ea52de5627` (2024-11-05)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=2)
- 复现: `git show 63ea52de56277db01effdf93625ed23011f202d9`

### #1756 arch-riscv: sign-extend the PC when enter/leave trap handler

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1756
- 代表 commit: `70c211236ac4` (2024-11-05)
- 变更规模: commits=1, files=3, +10/-8 (churn=18)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.hh` (churn=10)
  - `src/arch/riscv/isa/decoder.isa` (churn=6)
  - `src/arch/riscv/faults.cc` (churn=2)
- 复现: `git show 70c211236ac4e273a2bfec903dbdce5d8658cbc2`

### #1749 misc: bump pre-commit from 3.8.0 to 4.0.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1749
- 代表 commit: `ecde7d9fa978` (2024-11-05)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show ecde7d9fa9789558d388b05fbdc2977f089bf325`

### #1750 misc: update RELEASE-NOTES.md for simInsts and simOps

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1750
- 代表 commit: `f2892fd5bc3c` (2024-11-06)
- 变更规模: commits=1, files=1, +6/-0 (churn=6)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=6)
- 复现: `git show f2892fd5bc3c8468ac1d40f449a988c2670397b6`

### #1748 misc: bump mypy from 1.11.2 to 1.13.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1748
- 代表 commit: `ca07a068936f` (2024-11-06)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show ca07a068936fd1fd840c3fa2c330f7e3dc8ca2aa`

### #1583 arch-arm,util-m5: Change arm64's default m5 call type to addr

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1583
- 代表 commit: `ad8bd6b5c7a6` (2024-11-07)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/m5/src/abi/arm64/SConsopts` (churn=4)
- 复现: `git show ad8bd6b5c7a6f72060d4079604a8a9a8f61df9e6`

### #1758 arch, cpu: Add generic getValidAddr to correct exetrace symbol table

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1758
- 代表 commit: `8b1075b792e3` (2024-11-08)
- 变更规模: commits=1, files=7, +45/-1 (churn=46)
- 影响范围: topdirs=src; subsys=arch, cpu; arch=arm, generic, riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/mmu.cc` (churn=14)
  - `src/arch/riscv/mmu.hh` (churn=9)
  - `src/arch/riscv/tlb.hh` (churn=9)
  - `src/arch/generic/mmu.cc` (churn=6)
  - `src/cpu/exetrace.cc` (churn=4)
  - `src/arch/arm/mmu.hh` (churn=2)
  - `src/arch/generic/mmu.hh` (churn=2)
- 复现: `git show 8b1075b792e34482c84d721f034234b425bd51f5`

### #1763 misc: Fix typo in README.md

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/1763
- 代表 commit: `665d32cba254` (2024-11-11)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=README.md; subsys=README.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `README.md` (churn=2)
- 复现: `git show 665d32cba254f35fa26aee962bd9848fc215794c`

### #1743 stdlib: Add interface to set binary in fs mode

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1743
- 代表 commit: `5ae26c0f0906` (2024-11-18)
- 变更规模: commits=2, files=1, +92/-30 (churn=122)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/kernel_disk_workload.py` (churn=122)
- commits 列表（按 topo-order，Top 12）：
  - 2024-11-12 `951df78278b2` stdlib: Add interface to set binary in fs mode
  - 2024-11-18 `5ae26c0f0906` stdlib: Add interface to set binary in fs mode
- 复现: `git show 5ae26c0f090648ba6801c1241736036705d0170b`

### #1534 misc: Do not share the random number generator across components

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1534
- 代表 commit: `b82ab5ac8956` (2024-11-18)
- 变更规模: commits=2, files=88, +896/-376 (churn=1272)
- 影响范围: topdirs=src; subsys=cpu, cpu/pred, dev, mem, mem/cache/rp, arch; arch=arm, riscv
- 主要改动文件（Top 8 by churn）:
  - `src/base/random.test.cc` (churn=312)
  - `src/base/random.hh` (churn=148)
  - `src/base/random.cc` (churn=58)
  - `src/cpu/testers/rubytest/Check.cc` (churn=50)
  - `src/cpu/testers/memtest/memtest.cc` (churn=30)
  - `src/cpu/testers/traffic_gen/hybrid_gen.cc` (churn=30)
  - `src/cpu/testers/gpu_ruby_test/address_manager.cc` (churn=28)
  - `src/dev/arm/smmu_v3_caches.cc` (churn=28)
- commits 列表（按 topo-order，Top 12）：
  - 2024-11-18 `0b0e9d0c2fcc` misc: Do not share the random number generator across components
  - 2024-11-18 `b82ab5ac8956` misc: Do not share the random number generator across components
- 复现: `git show b82ab5ac8956765682529a9ad5cd630193025364`

### #1789 arch-arm,misc: Fix build errors

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1789
- 代表 commit: `5f01a03bde89` (2024-11-19)
- 变更规模: commits=2, files=2, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/mmu.hh` (churn=4)
  - `src/arch/arm/pagetable.hh` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2024-11-18 `f4140a1b5320` arch-arm,misc: Fix build errors
  - 2024-11-19 `5f01a03bde89` arch-arm,misc: Fix build errors
- 复现: `git show 5f01a03bde8968902dc2998df274adc898ac0396`

### #1790 python: modify comment for ExitEvent.WORKEND

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1790
- 代表 commit: `75c4003a7e16` (2024-11-19)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/simulator.py` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2024-11-18 `d06216de06d5` python: modify comment for ExitEvent.WORKEND
  - 2024-11-19 `75c4003a7e16` python: modify comment for ExitEvent.WORKEND
- 复现: `git show 75c4003a7e16c7931da14015ccff2a537e9c404d`

### #1782 arch-riscv: fix reg dep autoref on vslide with vcpy micro

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1782
- 代表 commit: `c54132bdd9e4` (2024-11-19)
- 变更规模: commits=2, files=2, +18/-2 (churn=20)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=16)
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2024-11-19 `a25d07e2585f` arch-riscv: fix reg dep autoref on vslide with vcpy micro
  - 2024-11-19 `c54132bdd9e4` arch-riscv: fix reg dep autoref on vslide with vcpy micro
- 复现: `git show c54132bdd9e459ac05eecd5b951fdf0e93bb631a`

### #1810 arch-x86, sim-se: move mmap end downward in case of large stacks

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/1810
- 代表 commit: `25523e73a4f3` (2024-12-02)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/process.cc` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2024-11-27 `3a17c5abfecd` arch-x86, sim-se: move mmap end downward in case of large stacks
  - 2024-12-02 `25523e73a4f3` arch-x86, sim-se: move mmap end downward in case of large stacks
- 复现: `git show 25523e73a4f3af5dcbe237c8f0211f43648ac589`

### #1795 tests: modify gem5/learning-gem5 ref file to fix failure

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/1795
- 代表 commit: `1e5021c2e316` (2024-12-02)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/learning_gem5/ref/test` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2024-11-27 `22aad1165e98` tests: modify gem5/learning-gem5 ref file to fix failure
  - 2024-12-02 `1e5021c2e316` tests: modify gem5/learning-gem5 ref file to fix failure
- 复现: `git show 1e5021c2e3160c90cf22b4be93cafaf357625383`

### #1822 misc: Add ArmISA section to the RELEASE-NOTES.md file

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1822
- 代表 commit: `c64a807f94e2` (2024-12-02)
- 变更规模: commits=1, files=1, +52/-0 (churn=52)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=52)
- 复现: `git show c64a807f94e22f142c9e6cdf054846f8f1ee267f`

### #1833 misc: Add CHI section to the RELEASE-NOTES.md

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1833
- 代表 commit: `8a9f61c546ba` (2024-12-03)
- 变更规模: commits=1, files=1, +19/-0 (churn=19)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=19)
- 复现: `git show 8a9f61c546bad8330f868c135a7744fce7f77453`

### #1817 ruby-chi: fix wrong ruby-CHI base class name

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1817
- 代表 commit: `f799d9130916` (2024-12-04)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/ruby/CHI_config.py` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-04 `7e1713df977b` ruby-chi: fix wrong ruby-CHI base class name
  - 2024-12-04 `f799d9130916` ruby-chi: fix wrong ruby-CHI base class name
- 复现: `git show f799d9130916136884ae1827237825f5a532fe66`

### #1835 arch-riscv: Remove CPU_SET use for non-linux host

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1835
- 代表 commit: `dee42f1867c4` (2024-12-04)
- 变更规模: commits=2, files=1, +16/-2 (churn=18)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/linux/se_workload.cc` (churn=18)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-04 `2625d238e21f` arch-riscv: Remove CPU_SET use for non-linux host
  - 2024-12-04 `dee42f1867c4` arch-riscv: Remove CPU_SET use for non-linux host
- 复现: `git show dee42f1867c4aa8b08bf71647bee5eb1a9448783`

### #1838 mem-ruby: Fix functional access in MI_example

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1838
- 代表 commit: `5672d63ae46d` (2024-12-04)
- 变更规模: commits=2, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/MI_example-cache.sm` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-04 `f1e1d4c44d34` mem-ruby: Fix functional access in MI_example
  - 2024-12-04 `5672d63ae46d` mem-ruby: Fix functional access in MI_example
- 复现: `git show 5672d63ae46dcee82e263c6bc0b023d0c91bd44d`

### #1834 base,arch-arm: Add GEM5_NO_OPTIMIZE; use in ARM's vfp.hh

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1834
- 代表 commit: `3711bf8a7a38` (2024-12-04)
- 变更规模: commits=2, files=2, +24/-4 (churn=28)
- 影响范围: topdirs=src; subsys=arch, base; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/base/compiler.hh` (churn=20)
  - `src/arch/arm/insts/vfp.hh` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-04 `22254a5254c9` base,arch-arm: Add GEM5_NO_OPTIMIZE; use in ARM's vfp.hh
  - 2024-12-04 `3711bf8a7a38` base,arch-arm: Add GEM5_NO_OPTIMIZE; use in ARM's vfp.hh
- 复现: `git show 3711bf8a7a3892a2f68ad32cf03cd8d92c8cfdc3`

### #1832 arch-riscv: fix tlb stats in timming mode

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1832
- 代表 commit: `2b645ed38c9a` (2024-12-06)
- 变更规模: commits=2, files=3, +14/-14 (churn=28)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/tlb.cc` (churn=12)
  - `src/arch/riscv/pagetable_walker.cc` (churn=8)
  - `src/arch/riscv/tlb.hh` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-06 `2984f06e9d71` arch-riscv: fix tlb stats in timming mode
  - 2024-12-06 `2b645ed38c9a` arch-riscv: fix tlb stats in timming mode
- 复现: `git show 2b645ed38c9a9df1ae9fa40d0fe301d7519ea18f`

### #1840 misc: v24.1 release notes update

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1840
- 代表 commit: `8f37677c9bae` (2024-12-06)
- 变更规模: commits=2, files=1, +609/-472 (churn=1081)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=1081)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-06 `8e1b06e15b8f` misc: v24.1 release notes update
  - 2024-12-06 `8f37677c9bae` misc: v24.1 release notes update
- 复现: `git show 8f37677c9bae9aabfdbf031553ee987996ea0c92`

### #1842 mem-ruby,misc: Fix RNG range

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1842
- 代表 commit: `ae60062a9e95` (2024-12-06)
- 变更规模: commits=2, files=1, +8/-6 (churn=14)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/testers/gpu_ruby_test/address_manager.cc` (churn=14)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-06 `4b5c43b66188` mem-ruby,misc: Fix RNG range
  - 2024-12-06 `ae60062a9e95` mem-ruby,misc: Fix RNG range
- 复现: `git show ae60062a9e95aecb46d353508d5bd227083ef36f`

### #1844 misc: Add GPU info to release notes

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1844
- 代表 commit: `93b58fbf642d` (2024-12-06)
- 变更规模: commits=2, files=1, +34/-0 (churn=34)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=34)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-06 `776d72b5ef01` misc: Add GPU info to release notes
  - 2024-12-06 `93b58fbf642d` misc: Add GPU info to release notes
- 复现: `git show 93b58fbf642ddc14a9f5dc2deb6336df8675f2e1`

### #1843 tests: Update pyunit tests references to include 24.1

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1843
- 代表 commit: `63d25922a2db` (2024-12-07)
- 变更规模: commits=2, files=6, +110/-56 (churn=166)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/pyunit/stdlib/resources/refs/resources.json` (churn=66)
  - `tests/pyunit/stdlib/resources/refs/suite-checks.json` (churn=34)
  - `tests/pyunit/stdlib/resources/refs/obtain-resource.json` (churn=24)
  - `tests/pyunit/stdlib/resources/refs/workload-checks.json` (churn=24)
  - `tests/pyunit/stdlib/resources/refs/mongo-mock.json` (churn=12)
  - `tests/pyunit/stdlib/resources/refs/resource-specialization.json` (churn=6)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-07 `c1cd887b462a` tests: Update pyunit tests references to include 24.1
  - 2024-12-07 `63d25922a2db` tests: Update pyunit tests references to include 24.1
- 复现: `git show 63d25922a2db14643f132c0f62a224c7f7991c49`

## v24.1.0.1 (2024-12-19)

- PR 数：5

### #1865 mem-ruby: Add missing option in ProtocolInfo

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1865
- 代表 commit: `0fe31664f3ed` (2024-12-18)
- 变更规模: commits=2, files=5, +34/-18 (churn=52)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/Sequencer.cc` (churn=26)
  - `src/mem/ruby/slicc_interface/ProtocolInfo.hh` (churn=16)
  - `src/mem/ruby/protocol/MESI_Three_Level.slicc` (churn=4)
  - `src/mem/ruby/protocol/MESI_Two_Level.slicc` (churn=4)
  - `src/mem/slicc/parser.py` (churn=2)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-18 `a71cff88ff1e` mem-ruby: Add missing option in ProtocolInfo
  - 2024-12-18 `0fe31664f3ed` mem-ruby: Add missing option in ProtocolInfo
- 复现: `git show 0fe31664f3ed5779fb52974bd366eb3e8f944e9a`

### #1864 mem-ruby: Fix missing RubySystem in PerfectCacheMemory's entries

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1864
- 代表 commit: `b6c941c9cabd` (2024-12-18)
- 变更规模: commits=2, files=2, +40/-6 (churn=46)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/structures/PerfectCacheMemory.hh` (churn=38)
  - `src/mem/slicc/symbols/StateMachine.py` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-18 `d34d1d51d285` mem-ruby: Fix missing RubySystem in PerfectCacheMemory's entries
  - 2024-12-18 `b6c941c9cabd` mem-ruby: Fix missing RubySystem in PerfectCacheMemory's entries
- 复现: `git show b6c941c9cabdf6bd98be09addc6225c8a18d003b`

### #335 misc: Add sphinx stdlib documentation

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/335
- 代表 commit: `e146f1b2bcfe` (2024-12-18)
- 变更规模: commits=2, files=6, +230/-0 (churn=230)
- 影响范围: topdirs=docs, .gitignore; subsys=docs, .gitignore; arch=-
- 主要改动文件（Top 8 by churn）:
  - `docs/conf.py` (churn=84)
  - `docs/README` (churn=74)
  - `docs/Makefile` (churn=40)
  - `docs/gem5-sphinx-apidoc` (churn=28)
  - `.gitignore` (churn=2)
  - `docs/gem5-sphinx` (churn=2)
- commits 列表（按 topo-order，Top 12）：
  - 2024-11-12 `69ab29a556b6` misc: Add sphinx stdlib documentation
  - 2024-12-18 `e146f1b2bcfe` misc: Add sphinx stdlib documentation
- 复现: `git show e146f1b2bcfe07c8095c6529ac32d88c44f7c3cd`

### #1851 configs: Generalize class types in CHI RNF/MN generators

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1851
- 代表 commit: `b5e27f5ed873` (2024-12-18)
- 变更规模: commits=2, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/ruby/CHI_config.py` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-12 `437ae9e0ff18` configs: Generalize class types in CHI RNF/MN generators
  - 2024-12-18 `b5e27f5ed873` configs: Generalize class types in CHI RNF/MN generators
- 复现: `git show b5e27f5ed873c9fec53afa8925c7d578f77deaa3`

### #1875 v24.1.0.1 Hotfix Release

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1875
- 代表 commit: `c9625ce9cc5b` (2024-12-19)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c9625ce9cc5b5a90a38327de5ac0e1870974af5e`

## v24.1.0.2 (2025-02-12)

- PR 数：2

### #1930 mem-ruby: set RubySystem pointer during TBE alloc

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1930
- 代表 commit: `dc448c953074` (2025-02-01)
- 变更规模: commits=2, files=2, +46/-18 (churn=64)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/structures/TBETable.hh` (churn=38)
  - `src/mem/slicc/symbols/StateMachine.py` (churn=26)
- commits 列表（按 topo-order，Top 12）：
  - 2025-01-17 `77e787e199a4` mem-ruby: set RubySystem pointer during TBE alloc
  - 2025-02-01 `dc448c953074` mem-ruby: set RubySystem pointer during TBE alloc
- 复现: `git show dc448c953074a70cd04c4344c8b6ebc5a30f8c3a`

### #1964 misc: Hotfix v24.1.0.2

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1964
- 代表 commit: `186a913a48f1` (2025-02-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 186a913a48f13bdd484fcdef17ac3e28d2b8b4c9`

## v24.1.0.3 (2025-04-11)

- PR 数：2

### #1793 base: Fix failing compiler tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1793
- 代表 commit: `837a9a5c54ee` (2025-04-10)
- 变更规模: commits=2, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/random.cc` (churn=2)
- commits 列表（按 topo-order，Top 12）：
  - 2024-11-22 `c7137ee371db` base: Fix failing compiler tests
  - 2025-04-10 `837a9a5c54ee` base: Fix failing compiler tests
- 复现: `git show 837a9a5c54ee4ee80ff1b356454dd18e2df0cf20`

### #2177 misc: Hotfix v24.1.0.3

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2177
- 代表 commit: `b9da2bfe1e21` (2025-04-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b9da2bfe1e21f6efa2730edd4e80e58c5d965458`

## v25.0.0.0 (2025-06-18)

- PR 数：274

### #1762 base: Introduce registerExtraLog() in Logger

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1762
- 代表 commit: `c38f109b5229` (2024-11-12)
- 变更规模: commits=1, files=3, +58/-9 (churn=67)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/logging.hh` (churn=34)
  - `src/base/logging.test.cc` (churn=21)
  - `src/base/logging.cc` (churn=12)
- 复现: `git show c38f109b5229a626340c185e4c883185cbf60449`

### #1765 arch-riscv: Fix incomplete copy-constructor

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1765
- 代表 commit: `17df0c0c3ca4` (2024-11-12)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/pcstate.hh` (churn=1)
- 复现: `git show 17df0c0c3ca4af678241885284470fd3fea0e3f8`

### #1711 arch-riscv: Fix vector instruction assertion caused by speculative execution

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1711
- 代表 commit: `0a4fedc71f0e` (2024-11-14)
- 变更规模: commits=1, files=2, +14/-13 (churn=27)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=19)
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=8)
- 复现: `git show 0a4fedc71f0ec6f7d373263d22af6d563af00135`

### #1775 mem,ruby: Adding SimObject name to SLICC errors.

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1775
- 代表 commit: `fe890742d8cf` (2024-11-14)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/slicc/ast/AST.py` (churn=2)
- 复现: `git show fe890742d8cfad763da2cbee8b31933f1b6fae85`

### #1780 tests: update gpu weekly test to test mis hip instead of fw hip

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1780
- 代表 commit: `017ce337d278` (2024-11-15)
- 变更规模: commits=1, files=2, +31/-31 (churn=62)
- 影响范围: topdirs=.github, tests; subsys=.github, tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/gpu/test_gpu_pannotia.py` (churn=54)
  - `.github/workflows/weekly-tests.yaml` (churn=8)
- 复现: `git show 017ce337d278be5a4fd813e5878d09207066e9f9`

### #1489 mem-ruby, sim-se: Add support for Maybe_Stale blocks in functional reads

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1489
- 代表 commit: `a74181ba2130` (2024-11-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a74181ba2130717d946f7b1507b9f3ff0e42767b`

### #1433 cpu: add GlobalInstTracker and LocalInstTracker

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1433
- 代表 commit: `70c157dc3ca2` (2024-11-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 70c157dc3ca248fa0e8ecb850b9f8be63bb0a55e`

### #1659 cpu-o3: Use the generic cache library to build store sets

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1659
- 代表 commit: `350470de7566` (2024-11-18)
- 变更规模: commits=1, files=8, +129/-111 (churn=240)
- 影响范围: topdirs=src, configs; subsys=cpu/o3, configs, mem/cache; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/store_set.cc` (churn=138)
  - `src/cpu/o3/store_set.hh` (churn=56)
  - `src/cpu/o3/BaseO3CPU.py` (churn=17)
  - `src/cpu/o3/mem_dep_unit.cc` (churn=16)
  - `src/cpu/o3/SConscript` (churn=5)
  - `src/mem/cache/tags/indexing_policies/base.hh` (churn=4)
  - `configs/common/cores/arm/O3_ARM_v7a.py` (churn=2)
  - `configs/common/cores/arm/ex5_big.py` (churn=2)
- 复现: `git show 350470de756671c62a2da69cf380d3d6b905f411`

### #117 ruby: Enable all protocols in a single gem5 build

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/117
- 代表 commit: `8c05375061ad` (2024-11-19)
- 变更规模: commits=3, files=6, +300/-286 (churn=586)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/tlm/utils.cc` (churn=414)
  - `src/mem/ruby/protocol/chi/tlm/controller.hh` (churn=60)
  - `src/mem/ruby/protocol/chi/tlm/utils.hh` (churn=44)
  - `src/mem/ruby/protocol/chi/generic/CHIGenericController.hh` (churn=34)
  - `src/mem/ruby/protocol/chi/tlm/controller.cc` (churn=24)
  - `src/mem/ruby/protocol/chi/generic/CHIGenericController.cc` (churn=10)
- commits 列表（按 topo-order，Top 12）：
  - 2024-11-19 `8c05375061ad` ruby: Enable all protocols in a single gem5 build
  - 2024-11-26 `53a39652e12d` mem-ruby: Fix conflict between 117 and 1084
  - 2024-12-02 `1b166970299a` mem-ruby: Fix conflict between 117 and 1084
- 复现: `git show 8c05375061addd797d9e9792442dbc771bcde37c`

### #1792 arch-arm: Make FEAT_SME visible in userspace for SE mode

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1792
- 代表 commit: `1cf64a36dffe` (2024-11-21)
- 变更规模: commits=1, files=1, +12/-0 (churn=12)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/process.cc` (churn=12)
- 复现: `git show 1cf64a36dffe961d1e3a5c6e28e103a8a1af76e2`

### #1700 Mfma matrix core timing

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1700
- 代表 commit: `5583723a7716` (2024-11-21)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5583723a77168a4314c1ec4cb8160006cc8a7155`

### #1084 mem-ruby, configs: Add a CHI-TLM controller + testing

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1084
- 代表 commit: `e3ccf2aab6fc` (2024-11-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e3ccf2aab6fcd826daa20c77dc0429e387dda559`

### #1768 arch-riscv: Use getValidAddr to get zero-extend address in RV32 mode

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1768
- 代表 commit: `aca709e677b5` (2024-11-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show aca709e677b532e64a85210fca8d300f4c0a1713`

### #1636 stdlib: Add viper board, viper cache, and gpu components

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1636
- 代表 commit: `ee92bdd04dd5` (2024-11-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ee92bdd04dd5b89ca4ec44625059f43739d65f67`

### #1804 arch-arm: Fix bug in VQRSHL.

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1804
- 代表 commit: `a058dc57c356` (2024-11-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a058dc57c356adab43450020323358173563ff5b`

### #1813 mem-ruby: Fix conflict between 117 and 1084

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1813
- 代表 commit: `b1a9fa183fb0` (2024-11-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b1a9fa183fb0169ee9ee1fe53ef06f7291ab0f9c`

### #1419 cpu: LoopPoint analysis object

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1419
- 代表 commit: `24ade2b1af18` (2024-11-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 24ade2b1af1803836df0c07e4f1e36e1dc4014e1`

### #1812 arch-arm: Print the ESR_ELx register when generating a fault

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1812
- 代表 commit: `ce4033e36fa8` (2024-11-27)
- 变更规模: commits=1, files=1, +8/-7 (churn=15)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/faults.cc` (churn=15)
- 复现: `git show ce4033e36fa854b41fa566c27de3c24bd5e7ff5f`

### #1823 arch-arm: Fix inconsistency of rint().

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1823
- 代表 commit: `eef64d6aab83` (2024-11-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show eef64d6aab83909c44db38f3293e668e62825894`

### #1827 misc: bump tqdm from 4.66.6 to 4.67.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1827
- 代表 commit: `9a9e1cdc816b` (2024-12-01)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=optional-requirements.txt; subsys=optional-requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `optional-requirements.txt` (churn=2)
- 复现: `git show 9a9e1cdc816b146a5b71705a5d9adb05e30b4230`

### #1491 misc: update fs examples to use ubuntu 24.04 boot workloads

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1491
- 代表 commit: `3553d55ae56f` (2024-12-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3553d55ae56f1fe93a0857325926731535e1bca0`

### #1770 tests: add x86-ubuntu-run-with-kvm-no-perf to tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1770
- 代表 commit: `ec2a0cb0c2d7` (2024-12-02)
- 变更规模: commits=1, files=1, +21/-0 (churn=21)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/gem5_library_example_tests/test_gem5_library_examples.py` (churn=21)
- 复现: `git show ec2a0cb0c2d761022d7bc7563070b12a13b282e8`

### #1819 arch-arm: Make ESR_ELx a 64 bit register

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1819
- 代表 commit: `9effe971b5b9` (2024-12-02)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/regs/misc_types.hh` (churn=3)
- 复现: `git show 9effe971b5b98dca671436bab48ebc4703aa2cd7`

### #1808 sim-se, arch-x86: implement/ignore sched_get* syscalls

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1808
- 代表 commit: `60c5c6f6ce81` (2024-12-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 60c5c6f6ce8123f8964baa4a554ff223cfad0861`

### #1829 mem-ruby, scons: Add ProtocolInfo.hh files in build targets

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1829
- 代表 commit: `2a36428d26d8` (2024-12-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2a36428d26d8d18e428f14e1e20def066f33df60`

### #1841 arch-arm: Fix trapping for ZCR_EL12 register

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1841
- 代表 commit: `23df1934de13` (2024-12-06)
- 变更规模: commits=1, files=1, +16/-58 (churn=74)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/regs/misc.cc` (churn=74)
- 复现: `git show 23df1934de13d338e16230cfee5aefb850b89c76`

### #1831 arch-riscv: Add senvcfg CSR

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1831
- 代表 commit: `a547558df15a` (2024-12-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a547558df15afcd528d5bb3f3797d768a31aeee3`

### #1769 gpu-compute: Reverting L1 TLB entries and L1, L2, L3 TLB assoc back to 32 from 64

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1769
- 代表 commit: `393bf85f983f` (2024-12-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 393bf85f983f4f267ff41a4c1dad72ca5d34f4b4`

### #1828 cpu: Incorrect BP update for atomic core

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1828
- 代表 commit: `1e6b9fd2f021` (2024-12-06)
- 变更规模: commits=1, files=1, +6/-5 (churn=11)
- 影响范围: topdirs=src; subsys=cpu/simple; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/simple/base.cc` (churn=11)
- 复现: `git show 1e6b9fd2f021b96f6d9c34eab619d688899dad5d`

### #1845 mem-ruby: Fix Atomic transitions in VIPER protocol

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1845
- 代表 commit: `6806254d8f98` (2024-12-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6806254d8f98e531e6bf56fbea7dfb91c80d9409`

### #1704 arch-riscv: Implement resumable non-maskable interrupt(Smrnmi)

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1704
- 代表 commit: `6ebbc3c8cf6a` (2024-12-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6ebbc3c8cf6aec93ba59581e968450c81b6f110c`

### #1818 arch-arm: Simplify FEAT_PAN implementation

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1818
- 代表 commit: `b3744574a2b7` (2024-12-12)
- 变更规模: commits=1, files=2, +10/-64 (churn=74)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/mmu.cc` (churn=68)
  - `src/arch/arm/mmu.hh` (churn=6)
- 复现: `git show b3744574a2b7d50f4a713b03e79d1427eef46d71`

### #1859 misc: edit pre-commit.ci yaml to target develop for autoupdate

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/1859
- 代表 commit: `d980bbffd6e3` (2024-12-16)
- 变更规模: commits=1, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=.pre-commit-config.yaml; subsys=.pre-commit-config.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.pre-commit-config.yaml` (churn=3)
- 复现: `git show d980bbffd6e341eb4b05700dd2bff5aac9a29ab8`

### #1850 misc: [pre-commit.ci] pre-commit autoupdate

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/1850
- 代表 commit: `f3bd73a8f9a4` (2024-12-17)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.pre-commit-config.yaml; subsys=.pre-commit-config.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.pre-commit-config.yaml` (churn=2)
- 复现: `git show f3bd73a8f9a47aa91dc8a90d59a107b7188e5ec7`

### #1866 resources: Update error message when wrong category is passed

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1866
- 代表 commit: `50d6460441ac` (2024-12-17)
- 变更规模: commits=1, files=1, +4/-1 (churn=5)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/resource.py` (churn=5)
- 复现: `git show 50d6460441ac9e56beff5494f2e43456915d62b6`

### #1869 mem: fix warning message for when memory size > 4GiB

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1869
- 代表 commit: `ece85d20e7c1` (2024-12-18)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/mem_interface.hh` (churn=2)
- 复现: `git show ece85d20e7c14f8795f4f15975f9b8d2d563f4df`

### #1861 cpu: Add EpisodeCount debug flag

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1861
- 代表 commit: `478e76a5092f` (2024-12-19)
- 变更规模: commits=1, files=2, +3/-1 (churn=4)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/testers/gpu_ruby_test/episode.cc` (churn=3)
  - `src/cpu/testers/gpu_ruby_test/SConscript` (churn=1)
- 复现: `git show 478e76a5092ffeb20fa17ebdfdb70e2d7ee938ec`

### #1882 util: bump Jinja2 from 3.1.4 to 3.1.5

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1882
- 代表 commit: `83af07434e5d` (2024-12-26)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=2)
- 复现: `git show 83af07434e5d0043f0de5905a0c62394dc0cd74d`

### #1883 misc: bump precommit asottile/pyupgrade to v3.19.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1883
- 代表 commit: `474f2cb50ece` (2024-12-26)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.pre-commit-config.yaml; subsys=.pre-commit-config.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.pre-commit-config.yaml` (churn=2)
- 复现: `git show 474f2cb50ece935682e79b3f9d9808e9db53322c`

### #1876 util: update checkpoint upgrader for MISCREG_SENVCFG

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1876
- 代表 commit: `47f022a80e3d` (2024-12-27)
- 变更规模: commits=1, files=2, +81/-1 (churn=82)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/cpt_upgraders/riscv-senvcfg.py` (churn=80)
  - `util/cpt_upgraders/riscv-vext.py` (churn=2)
- 复现: `git show 47f022a80e3d0ecf911ddc84c347dc9484acf2aa`

### #1878 arch-riscv: Change RISC-V Interrupts::nmi_cause to protected

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1878
- 代表 commit: `99655b833058` (2024-12-27)
- 变更规模: commits=1, files=1, +3/-1 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/interrupts.hh` (churn=4)
- 复现: `git show 99655b8330587baa29d292db96b500e7c5689636`

### #1890 misc: bump mypy from 1.13.0 to 1.14.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1890
- 代表 commit: `60a9bd6bf3ef` (2025-01-02)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 60a9bd6bf3ef494b1a0a298cfe05b2c9ad2e6d3c`

### #1893 base: Print debug message when interrupt change

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1893
- 代表 commit: `8ea768eab09c` (2025-01-03)
- 变更规模: commits=1, files=2, +11/-2 (churn=13)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/intpin.hh` (churn=12)
  - `src/dev/SConscript` (churn=1)
- 复现: `git show 8ea768eab09c14ce1e96db18be7195abb04de602`

### #1894 scons: Fix -Werror=undef err with homebrew's protobuf on macOS

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1894
- 代表 commit: `f7a84d1f85d3` (2025-01-06)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/proto/SConsopts` (churn=1)
- 复现: `git show f7a84d1f85d3a8b9e647f17758fc7a215d9cd628`

### #1870 resources: Update error handling for JSONClient

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1870
- 代表 commit: `fc0bdcb038b6` (2025-01-06)
- 变更规模: commits=1, files=3, +45/-14 (churn=59)
- 影响范围: topdirs=src, tests; subsys=python, tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/client_api/jsonclient.py` (churn=53)
  - `src/python/gem5/resources/client_api/abstract_client.py` (churn=4)
  - `tests/pyunit/stdlib/resources/pyunit_json_client_checks.py` (churn=2)
- 复现: `git show fc0bdcb038b629cbd52efcac73d93fa3bc37135a`

### #1785 misc: Fix scons tags

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1785
- 代表 commit: `c16407e21a5c` (2025-01-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c16407e21a5cf30b9f528b14dd500832c2c0db61`

### #1902 mem-cache: Fix use-after-free in MSHR handling

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1902
- 代表 commit: `fed674114607` (2025-01-07)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=mem/cache; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/cache.cc` (churn=2)
- 复现: `git show fed674114607415fb88b287a8a56596ed161e427`

### #1901 fastmodel: Add tag for fastmodel

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1901
- 代表 commit: `286af09cf535` (2025-01-07)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/fastmodel/iris/isa.hh` (churn=2)
- 复现: `git show 286af09cf5358c25704f69b22385e02420a59e96`

### #1905 arch-arm: setMiscRegs: Show CSPR.d in debug log on writes to CPSR

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1905
- 代表 commit: `0e1b1dce6b7e` (2025-01-07)
- 变更规模: commits=1, files=1, +3/-2 (churn=5)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa.cc` (churn=5)
- 复现: `git show 0e1b1dce6b7e6dc8d5382338f443db2267ee481a`

### #1858 arch-arm: Implement FEAT_S1PIE

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1858
- 代表 commit: `8bac08a153a2` (2025-01-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 8bac08a153a2d706b0efb84df468ca6f9e08417a`

### #1906 dev: Use move constructor as default in RegisterBank

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/1906
- 代表 commit: `43a69da20e5f` (2025-01-10)
- 变更规模: commits=1, files=2, +58/-0 (churn=58)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/reg_bank.test.cc` (churn=56)
  - `src/dev/reg_bank.hh` (churn=2)
- 复现: `git show 43a69da20e5f65cfb9bc2a41a1b5b1f22bfcff48`

### #1913 arch-arm: Add syscall 435 to arm64

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1913
- 代表 commit: `24b5615721e7` (2025-01-10)
- 变更规模: commits=1, files=2, +17/-0 (churn=17)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/linux/linux.hh` (churn=16)
  - `src/arch/arm/linux/se_workload.cc` (churn=1)
- 复现: `git show 24b5615721e71ab0794cbb2e0dd737060069cce4`

### #1907 arch-arm: gic_v3_redistributor: Fix GICR_IGRPMODR0 update bug

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1907
- 代表 commit: `408e40b29110` (2025-01-10)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/arm/gic_v3_redistributor.cc` (churn=2)
- 复现: `git show 408e40b29110b78067e320c49347598aeca26ac5`

### #1916 arch-vega: Fix LDS/buffer load/store x2,x3,x4

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1916
- 代表 commit: `33ff4a34488e` (2025-01-10)
- 变更规模: commits=1, files=2, +11/-11 (churn=22)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/ds.cc` (churn=12)
  - `src/arch/amdgpu/vega/insts/mubuf.cc` (churn=10)
- 复现: `git show 33ff4a34488e78f5c81ba13e9b1e682546fac029`

### #1915 arch-vega: Add SDWA to v_cmp_ne_u32

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1915
- 代表 commit: `a5f66cd4b652` (2025-01-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a5f66cd4b6524871de91bc703fa026fc6df8c36d`

### #1918 scons: Fix missing and/or filter name

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1918
- 代表 commit: `051c2ff829a3` (2025-01-13)
- 变更规模: commits=1, files=1, +4/-2 (churn=6)
- 影响范围: topdirs=site_scons; subsys=scons; arch=-
- 主要改动文件（Top 8 by churn）:
  - `site_scons/gem5_scons/sources.py` (churn=6)
- 复现: `git show 051c2ff829a34f0b10bf2f8db1867932422bf8b4`

### #1853 arch-arm: Do FEAT_VHE redirection for TCR2_EL1 register

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1853
- 代表 commit: `d4e7d9d799d2` (2025-01-13)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa.cc` (churn=2)
- 复现: `git show d4e7d9d799d23e0ee37f15d3bb5ed7eb8471afb3`

### #1911 misc: Fixup optional libs being required

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1911
- 代表 commit: `d2a45e027581` (2025-01-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d2a45e027581e06f9bb6985ecb3ddf4e4f716820`

### #1787 sim: Remove cyclic dependency of SimObjects and probes

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1787
- 代表 commit: `75b8a2effd95` (2025-01-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 75b8a2effd95cdaa3314da756d9de3812ad96321`

### #1794 util: Adding TargetNamedBreakpoint class for gdb.

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1794
- 代表 commit: `e87e90459a77` (2025-01-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e87e90459a775e3c15a2bccab9cc73b91de86d55`

### #1910 arch-x86: Fix typo for readlinkat syscall

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1910
- 代表 commit: `e059f93609e8` (2025-01-14)
- 变更规模: commits=1, files=2, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/linux/syscall_tbl32.cc` (churn=2)
  - `src/arch/x86/linux/syscall_tbl64.cc` (churn=2)
- 复现: `git show e059f93609e8360a138c4a73c14b6bedf855245c`

### #1928 sim-se: Fix "<bad format>" in ioctl call

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1928
- 代表 commit: `d9aee8e8e434` (2025-01-15)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/syscall_emul.hh` (churn=2)
- 复现: `git show d9aee8e8e434d9f3268c59f515a7a841fd4c0bac`

### #1929 Fix missing python tags

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1929
- 代表 commit: `87574cb82341` (2025-01-15)
- 变更规模: commits=1, files=3, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=arch, mem, sim; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/tlm/SConscript` (churn=4)
  - `src/arch/arm/fastmodel/SConscript` (churn=2)
  - `src/sim/SConscript` (churn=2)
- 复现: `git show 87574cb82341837c5edb896d52c75612367b5821`

### #1904 dev: Fix memory leaks

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1904
- 代表 commit: `15f5dfdc7d34` (2025-01-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 15f5dfdc7d3442537a6f4e105fa92df09e55ac33`

### #1931 mem-ruby: Init block_size_bits in Message and TBEs objects

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1931
- 代表 commit: `bb35cfcf7525` (2025-01-16)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/slicc/symbols/Type.py` (churn=2)
- 复现: `git show bb35cfcf752512ce25ddd6b3230f621a3d1b1ff3`

### #1872 cpu-o3, stats: Stats Added to O3 CPU

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1872
- 代表 commit: `cf88f7dad1f0` (2025-01-16)
- 变更规模: commits=1, files=4, +28/-2 (churn=30)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/rename.cc` (churn=17)
  - `src/cpu/o3/lsq_unit.cc` (churn=6)
  - `src/cpu/o3/rename.hh` (churn=4)
  - `src/cpu/o3/lsq_unit.hh` (churn=3)
- 复现: `git show cf88f7dad1f0e388fa9425d4f2c7edd9d9a7b26e`

### #1933 systemc, python: Export into converters of sc_time to Python

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1933
- 代表 commit: `79375a9fe701` (2025-01-16)
- 变更规模: commits=1, files=1, +6/-1 (churn=7)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/core/sc_time_python.cc` (churn=7)
- 复现: `git show 79375a9fe7019ea90c97ef2dd67664abdcf3b8d2`

### #1891 util-docker: Devcontainer fixes

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1891
- 代表 commit: `80f3aa063313` (2025-01-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 80f3aa0633137702fb91383e82bf7feaaacc1afc`

### #1932 scons: Fix build failures with ASAN enabled.

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1932
- 代表 commit: `9ca82725ec15` (2025-01-17)
- 变更规模: commits=1, files=1, +19/-19 (churn=38)
- 影响范围: topdirs=SConstruct; subsys=SConstruct; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=38)
- 复现: `git show 9ca82725ec158317cfad8ec268a8a2272b8f5525`

### #1681 cpu-o3: Clear thread state in time buffers on thread exit

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1681
- 代表 commit: `8cbbaf88b1a4` (2025-01-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 8cbbaf88b1a4d34b407f5fb50b49798fb080dff8`

### #1940 mem-cache: fixed incorrect x-o lookup address in BOP

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1940
- 代表 commit: `3ed0729aef20` (2025-01-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3ed0729aef206cb4e9f2d0b11c5caa28b3af515d`

### #1939 arch-arm: Do not panic if invalid TG is programmed in MMU

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1939
- 代表 commit: `e7ac96f5e7ae` (2025-01-20)
- 变更规模: commits=1, files=1, +7/-2 (churn=9)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/table_walker.cc` (churn=9)
- 复现: `git show e7ac96f5e7ae5674f6c7aa20bb78e8060bc00c40`

### #1938 Clear mstatus.mprv when xret leaving M-mode

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1938
- 代表 commit: `0e4f9f2ffd6e` (2025-01-21)
- 变更规模: commits=1, files=1, +4/-0 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=4)
- 复现: `git show 0e4f9f2ffd6edcfa2fe2ed33ded619a2b2833b03`

### #1944 mem-ruby: remove incorrect actions in MOESI_CMP_directory

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1944
- 代表 commit: `3655562f7574` (2025-01-23)
- 变更规模: commits=1, files=1, +0/-3 (churn=3)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/MOESI_CMP_directory-L2cache.sm` (churn=3)
- 复现: `git show 3655562f7574f5c2390302e33063d6a51aa1c86f`

### #1943 arch-riscv: Implement SVNAPOT Extension

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1943
- 代表 commit: `cb4a69661088` (2025-01-23)
- 变更规模: commits=1, files=5, +22/-2 (churn=24)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/pagetable_walker.cc` (churn=19)
  - `src/arch/riscv/pagetable.hh` (churn=2)
  - `src/arch/riscv/RiscvISA.py` (churn=1)
  - `src/arch/riscv/page_size.hh` (churn=1)
  - `src/arch/riscv/pagetable_walker.hh` (churn=1)
- 复现: `git show cb4a69661088399398754cabe86dfab539a9e2ca`

### #1947 arch-vega,gpu-compute: Fix architected flat scratch

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1947
- 代表 commit: `d7d2efb8770d` (2025-01-26)
- 变更规模: commits=1, files=3, +45/-29 (churn=74)
- 影响范围: topdirs=src; subsys=arch, gpu-compute; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/op_encodings.hh` (churn=63)
  - `src/gpu-compute/wavefront.cc` (churn=6)
  - `src/arch/amdgpu/vega/gpu_mem_helpers.hh` (churn=5)
- 复现: `git show d7d2efb8770dc26c2a16c278f47a3effa4fa64de`

### #1948 util-docker: Update README.md and fix bugs in devcontainer Dockerfile

- 动作（heuristic）: 文档/示例
- PR 链接: https://github.com/gem5/gem5/pull/1948
- 代表 commit: `f99218d822d7` (2025-01-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f99218d822d7ae9ec8b551c66a98046cba98f687`

### #1923 arch-arm, arch-x86: Only build X86/ARM KVM obj when they are a target ISA

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1923
- 代表 commit: `86e08e87d391` (2025-01-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 86e08e87d3917602db2fef0b8052df61237cc2ab`

### #1936 Fix-append-kernel-arg-risv

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1936
- 代表 commit: `5c75cc5ef5d0` (2025-01-28)
- 变更规模: commits=1, files=1, +11/-8 (churn=19)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/riscv_board.py` (churn=19)
- 复现: `git show 5c75cc5ef5d0996849714ea11fbabb2ec1796fd9`

### #1952 arch-x86: fix pack micro-op implementation

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1952
- 代表 commit: `17f4c844265a` (2025-01-28)
- 变更规模: commits=1, files=1, +8/-5 (churn=13)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/isa/microops/mediaop.isa` (churn=13)
- 复现: `git show 17f4c844265aded267866aaeb7e3beb2ff74a1c4`

### #1958 arch-arm: Fix ARM KVM build problems

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1958
- 代表 commit: `b8601644b1ee` (2025-01-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b8601644b1eec491d2e8137b14364ffac79c9f2f`

### #1761 arch-riscv: Implement Zcmt

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1761
- 代表 commit: `6d3434a8b352` (2025-01-30)
- 变更规模: commits=1, files=17, +449/-5 (churn=454)
- 影响范围: topdirs=src, configs, util; subsys=arch, configs, util; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/formats/zcmt.isa` (churn=119)
  - `src/arch/riscv/insts/zcmt.cc` (churn=93)
  - `util/cpt_upgraders/riscv-jvt.py` (churn=74)
  - `src/arch/riscv/insts/zcmt.hh` (churn=68)
  - `src/arch/riscv/decoder.cc` (churn=27)
  - `src/arch/riscv/pcstate.hh` (churn=21)
  - `src/arch/riscv/regs/misc.hh` (churn=14)
  - `src/arch/riscv/faults.cc` (churn=10)
- 复现: `git show 6d3434a8b3526c6b67461c244fa968e463613bd9`

### #1954 arch-vega: Fix assertion error at VIPERSequencer.cc

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1954
- 代表 commit: `0afbbedc310b` (2025-01-30)
- 变更规模: commits=1, files=1, +4/-0 (churn=4)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/VIPERSequencer.cc` (churn=4)
- 复现: `git show 0afbbedc310bed3d2dcb1e08d7528d03eb53f815`

### #1955 arch-riscv: fix vector reduction instructions when LMUL>1

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1955
- 代表 commit: `7814d3ad2080` (2025-01-30)
- 变更规模: commits=1, files=3, +55/-28 (churn=83)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=43)
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=36)
  - `src/arch/riscv/isa/decoder.isa` (churn=4)
- 复现: `git show 7814d3ad208012919452f643335147dada9e1037`

### #1963 misc: bump pre-commit from 4.0.1 to 4.1.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1963
- 代表 commit: `d1980b7c3ab8` (2025-02-02)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show d1980b7c3ab824104906bb17c4a2c69890e98027`

### #947 python,stdlib: Improve exit events

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/947
- 代表 commit: `eccf319125d8` (2025-02-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show eccf319125d8b96ef6a85f7a92b7195958397581`

### #1957 arch-x86: Remove decodePages decode cache

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/1957
- 代表 commit: `d2300df6323b` (2025-02-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d2300df6323bbd4bbbbc4d097d57d28165470d5d`

### #1971 misc: bump precommit black and isort versions

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1971
- 代表 commit: `b2aa0e53ee39` (2025-02-04)
- 变更规模: commits=1, files=29, +44/-48 (churn=92)
- 影响范围: topdirs=configs, src, util, tests, .pre-commit-config.yaml; subsys=configs, python, util, tests, .pre-commit-config.yaml, sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/helper_scripts_for_mongodb/update-gem5-versions.py` (churn=14)
  - `util/helper_scripts_for_mongodb/create-new-collection.py` (churn=10)
  - `util/helper_scripts_for_mongodb/helper.py` (churn=6)
  - `.pre-commit-config.yaml` (churn=4)
  - `configs/common/cpu2000.py` (churn=4)
  - `tests/gem5/to_tick/configs/tick-exit.py` (churn=4)
  - `src/python/gem5/components/boards/mem_mode.py` (churn=3)
  - `src/python/gem5/components/memory/dram_interfaces/ddr5.py` (churn=3)
- 复现: `git show b2aa0e53ee39d195cd3929f05c81f24ada41bd31`

### #1480 gem5 Bridge Driver

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1480
- 代表 commit: `10a648a9fd91` (2025-02-04)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 10a648a9fd91f2f89c253748813f32f06120f96f`

### #1968 scons: Improve codegen for build/../params/*.hh

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1968
- 代表 commit: `4dd808bfc044` (2025-02-04)
- 变更规模: commits=1, files=4, +38/-37 (churn=75)
- 影响范围: topdirs=build_tools, src; subsys=build_tools, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/params.py` (churn=62)
  - `build_tools/sim_object_param_struct_hh.py` (churn=6)
  - `src/python/m5/SimObject.py` (churn=4)
  - `build_tools/code_formatter.py` (churn=3)
- 复现: `git show 4dd808bfc04450916a30656f519fe728bfde02e6`

### #1953 configs: `exec` -> `importlib` for Ruby.py  dynamic imports

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1953
- 代表 commit: `04cf65f4ec6e` (2025-02-06)
- 变更规模: commits=1, files=1, +11/-10 (churn=21)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/ruby/Ruby.py` (churn=21)
- 复现: `git show 04cf65f4ec6ef00e711aba970642af4935d4b86a`

### #1927 sim,tests: Add a tag for SimObjects

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1927
- 代表 commit: `298993dc73db` (2025-02-06)
- 变更规模: commits=1, files=4, +6/-4 (churn=10)
- 影响范围: topdirs=src; subsys=base, sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/SConscript` (churn=4)
  - `src/base/SConscript` (churn=2)
  - `src/base/stats/SConscript` (churn=2)
  - `src/sim/probe/SConscript` (churn=2)
- 复现: `git show 298993dc73db127b66444790688cf96d2ac79c03`

### #1979 arch: Return reference type for BaseISA::getIsaName()

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1979
- 代表 commit: `bf3d40992082` (2025-02-07)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=generic
- 主要改动文件（Top 8 by churn）:
  - `src/arch/generic/isa.hh` (churn=2)
- 复现: `git show bf3d4099208238e8f20eb207886b61f19ebdee59`

### #1975 scons: Use relative paths in codegen to improve build caching

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1975
- 代表 commit: `e04a218238f3` (2025-02-07)
- 变更规模: commits=1, files=2, +4/-3 (churn=7)
- 影响范围: topdirs=build_tools, src; subsys=build_tools, mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `build_tools/code_formatter.py` (churn=4)
  - `src/mem/ruby/SConscript` (churn=3)
- 复现: `git show e04a218238f3b8fa5c173209af7267a724bf00b8`

### #1983 dev,arch-riscv: Fix CLINT pio_size sanity check

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1983
- 代表 commit: `e184d45920be` (2025-02-11)
- 变更规模: commits=1, files=2, +12/-4 (churn=16)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/riscv/clint.cc` (churn=13)
  - `src/dev/riscv/clint.hh` (churn=3)
- 复现: `git show e184d45920be83f9d974b44acf12e8506502edac`

### #1993 arch-arm: Fix off-by-one when initializing PMU counters

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1993
- 代表 commit: `922763a0813e` (2025-02-12)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/pmu.cc` (churn=2)
- 复现: `git show 922763a0813e703cf60db06be7f859745aafe6da`

### #1439 sim,mem,cpu,arch-arm: Add support for cache PMU events

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1439
- 代表 commit: `a6a8f7d787ba` (2025-02-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a6a8f7d787ba05a20afe3fbe07808a6006e36e51`

### #1976 gpu-compute: GPU progress prints and debug tracing

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1976
- 代表 commit: `62359c8101d8` (2025-02-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 62359c8101d8ab39a68288351b4eeec1f131b913`

### #1981 util-docker: Fix missing gfortran in gcn-gnu

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1981
- 代表 commit: `c829d5b3b4be` (2025-02-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c829d5b3b4bef52af1a16e924f7a2d5007162024`

### #1767 arch-riscv: Add support for Zfa extension

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1767
- 代表 commit: `f5eb43dae8e4` (2025-02-13)
- 变更规模: commits=1, files=3, +350/-17 (churn=367)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=362)
  - `src/arch/riscv/regs/float.hh` (churn=4)
  - `src/arch/riscv/linux/se_workload.cc` (churn=1)
- 复现: `git show f5eb43dae8e4bae710b584e8aa2ab3b1c7cec11d`

### #2000 arch-riscv: Fix mip and sip

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2000
- 代表 commit: `f56eabd58dda` (2025-02-13)
- 变更规模: commits=1, files=2, +37/-22 (churn=59)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/regs/misc.hh` (churn=55)
  - `src/arch/riscv/isa.cc` (churn=4)
- 复现: `git show f56eabd58ddaa7d47c7863d92538b57fc205cc09`

### #2007 arch-riscv: Follow up mip and sip fixes

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2007
- 代表 commit: `fd8151994de6` (2025-02-14)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.cc` (churn=3)
- 复现: `git show fd8151994de6e69b60b95c7ec22bf79ed7c663fe`

### #2002 arch-arm: Split the decodeFp function in subfunctions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2002
- 代表 commit: `32ca62ea4c2e` (2025-02-17)
- 变更规模: commits=1, files=1, +617/-558 (churn=1175)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/formats/aarch64.isa` (churn=1175)
- 复现: `git show 32ca62ea4c2ec673a5ef6796cdf8e31b4bd51edb`

### #2006 Fix Stats::Vecotr::zero()

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2006
- 代表 commit: `ed9a1de58d80` (2025-02-17)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/statistics.hh` (churn=2)
- 复现: `git show ed9a1de58d80b439cb98be3eb0b0ff48cf7bed1b`

### #1849 sim: Update multisim to use Process instead of Pool

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1849
- 代表 commit: `2905241c70c7` (2025-02-17)
- 变更规模: commits=1, files=1, +58/-14 (churn=72)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/utils/multisim/multisim.py` (churn=72)
- 复现: `git show 2905241c70c7556376df5ca3e24011c262382033`

### #2013 python: Fix python 3.8 TypeError error in gem5stats

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2013
- 代表 commit: `b417270d845b` (2025-02-18)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/stats/gem5stats.py` (churn=2)
- 复现: `git show b417270d845b884c37711eeee2062a42c0282753`

### #2017 mem-ruby: Update txnId in Send_CompI_Stale

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2017
- 代表 commit: `155260cae2cc` (2025-02-19)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` (churn=1)
- 复现: `git show 155260cae2cceee6e79437ee7220528b92989145`

### #2015 arch-riscv: Fix mnepc lower bits

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2015
- 代表 commit: `61e9078b1b72` (2025-02-19)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.cc` (churn=1)
- 复现: `git show 61e9078b1b72c85957b7301adfe348718aa0195a`

### #2012 cpu: Fix postInterrupt wakeup

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2012
- 代表 commit: `909400f50432` (2025-02-19)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/base.cc` (churn=2)
- 复现: `git show 909400f504328c8b10bb24fcfbb0e285e76185fb`

### #2019 resources: Update search to filter resources with minor gem5_version

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2019
- 代表 commit: `94c0cb68937e` (2025-02-20)
- 变更规模: commits=1, files=3, +11/-12 (churn=23)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/client_api/atlasclient.py` (churn=10)
  - `src/python/gem5/resources/client_api/jsonclient.py` (churn=8)
  - `src/python/gem5/resources/client_api/client_query.py` (churn=5)
- 复现: `git show 94c0cb68937e4b84cd4e62c770738ba7c03375bc`

### #2024 scons: use --git-common-dir to detect hooks

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2024
- 代表 commit: `deffd395e285` (2025-02-24)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=site_scons; subsys=scons; arch=-
- 主要改动文件（Top 8 by churn）:
  - `site_scons/site_tools/git.py` (churn=2)
- 复现: `git show deffd395e285deb068774753813acc40c4e19fa6`

### #1982 sim,stdlib: Fixes for external signal

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1982
- 代表 commit: `599dc98e4695` (2025-02-25)
- 变更规模: commits=1, files=4, +315/-44 (churn=359)
- 影响范围: topdirs=src, util; subsys=sim, python, util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/init_signals.cc` (churn=192)
  - `util/hypercall_external_signal/transmitter.py` (churn=153)
  - `src/python/gem5/simulate/simulator.py` (churn=13)
  - `src/sim/SConscript` (churn=1)
- 复现: `git show 599dc98e46956776db4dccfd0be3caa660a740da`

### #2033 base: Add print when accessing an AssociativeCache entry

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2033
- 代表 commit: `4035a24c67b7` (2025-02-26)
- 变更规模: commits=1, files=1, +8/-1 (churn=9)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/cache/associative_cache.hh` (churn=9)
- 复现: `git show 4035a24c67b73db4c67e5a7be1dd291b6d0a603e`

### #2029 stdlib: Fix simloop exit hypercall

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2029
- 代表 commit: `8d69a8e8d109` (2025-02-26)
- 变更规模: commits=1, files=1, +5/-2 (churn=7)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/simulate.py` (churn=7)
- 复现: `git show 8d69a8e8d10904a0839f3f714fed786bf323aca4`

### #2040 misc: remove my-config directory

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2040
- 代表 commit: `c2984a445b7f` (2025-02-27)
- 变更规模: commits=1, files=1, +0/-83 (churn=83)
- 影响范围: topdirs=my-configs; subsys=my-configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `my-configs/x86-ubuntu-hypercall-test.py` (churn=83)
- 复现: `git show c2984a445b7f763c3af6c1fedbaadc87a316f828`

### #1995 stdlib: Improvements to exit events

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1995
- 代表 commit: `66b70d7a6a7f` (2025-02-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 66b70d7a6a7f605000821bb5cca40d80cc2a408e`

### #1961 stdlib: Add SE mode support to multi-program workloads

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1961
- 代表 commit: `2824a0a36e78` (2025-02-27)
- 变更规模: commits=1, files=1, +172/-40 (churn=212)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/se_binary_workload.py` (churn=212)
- 复现: `git show 2824a0a36e7858e08dcac949f50d5538dd4ff969`

### #1999 gpu-compute: Add RLC queues to checkpoint

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1999
- 代表 commit: `d8baccc15134` (2025-02-27)
- 变更规模: commits=1, files=2, +119/-0 (churn=119)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/amdgpu/sdma_engine.cc` (churn=113)
  - `src/dev/amdgpu/sdma_engine.hh` (churn=6)
- 复现: `git show d8baccc1513410b8f2f638eae9ab528da36df1ad`

### #2044 base, arch-arm: New version of decoding for AdvSIMD

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2044
- 代表 commit: `96f22ee2960f` (2025-02-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 96f22ee2960f827e5778ecb9b6f5d69bd0ea8b3f`

### #2039 gpu-compute: Added MFMA insts, check if inst exists

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2039
- 代表 commit: `ba7209b2bf3f` (2025-02-28)
- 变更规模: commits=1, files=2, +11/-5 (churn=16)
- 影响范围: topdirs=src; subsys=gpu-compute; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/gpu-compute/scoreboard_check_stage.cc` (churn=10)
  - `src/gpu-compute/compute_unit.cc` (churn=6)
- 复现: `git show ba7209b2bf3f428d269bfc8844ec68e711fb599c`

### #1988 Add a bi-directional communication orchestrator to gem5's stdlib

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1988
- 代表 commit: `535993eb5415` (2025-03-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 535993eb5415132e550dfa7ee831c8cfb300969e`

### #2048 dev, arch-riscv: Fix Clint msip register read/write

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2048
- 代表 commit: `fc30fa736121` (2025-03-03)
- 变更规模: commits=1, files=2, +23/-25 (churn=48)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/riscv/clint.cc` (churn=42)
  - `src/dev/riscv/clint.hh` (churn=6)
- 复现: `git show fc30fa7361215bf5a88bb3dc3ec785dac2d7f4b3`

### #1945 base: Refactor base/random to avoid memory leak

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/1945
- 代表 commit: `70ea8182d768` (2025-03-05)
- 变更规模: commits=1, files=5, +61/-34 (churn=95)
- 影响范围: topdirs=src; subsys=base, mem, sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/random.hh` (churn=58)
  - `src/base/random.cc` (churn=31)
  - `src/base/random.test.cc` (churn=2)
  - `src/mem/ruby/network/MessageBuffer.cc` (churn=2)
  - `src/sim/syscall_emul.hh` (churn=2)
- 复现: `git show 70ea8182d76830d1b110d4634eb32bf742e3497a`

### #2047 misc: bump mypy from 1.14.1 to 1.15.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2047
- 代表 commit: `377097f7b6c1` (2025-03-05)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 377097f7b6c1fee6a5f1323dbb14fccc1e671d56`

### #2051 arch-riscv: Use generic ISA resetThread for RISC-V workloads

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2051
- 代表 commit: `72960bf77d82` (2025-03-05)
- 变更规模: commits=1, files=2, +3/-3 (churn=6)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/linux/fs_workload.cc` (churn=4)
  - `src/arch/riscv/bare_metal/fs_workload.cc` (churn=2)
- 复现: `git show 72960bf77d8289a356f2eeff2a99437908204256`

### #1912 resources: Add exceptions if the resource JSON has schema issues

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1912
- 代表 commit: `dc55377210bc` (2025-03-06)
- 变更规模: commits=1, files=6, +556/-75 (churn=631)
- 影响范围: topdirs=src, tests; subsys=python, tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/resource.py` (churn=297)
  - `tests/pyunit/stdlib/resources/pyunit_obtain_resources_check.py` (churn=127)
  - `tests/pyunit/stdlib/resources/refs/obtain-resource.json` (churn=107)
  - `src/python/gem5/resources/downloader.py` (churn=65)
  - `src/python/gem5/resources/client_api/abstract_client.py` (churn=20)
  - `src/python/gem5/resources/client_api/jsonclient.py` (churn=15)
- 复现: `git show dc55377210bc412fa716407f229e57de02ff123c`

### #1949 dev: rework PCI to add type1 header

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1949
- 代表 commit: `13eb3bd72083` (2025-03-06)
- 变更规模: commits=1, files=26, +799/-224 (churn=1023)
- 影响范围: topdirs=src, configs, tests, util; subsys=dev, configs, python, tests, util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/pci/device.cc` (churn=458)
  - `src/dev/pci/pcireg.h` (churn=193)
  - `src/dev/pci/device.hh` (churn=103)
  - `src/dev/pci/PciDevice.py` (churn=77)
  - `util/cpt_upgraders/pci-config-pxcap.py` (churn=52)
  - `src/dev/amdgpu/amdgpu_device.cc` (churn=28)
  - `src/dev/storage/ide_ctrl.cc` (churn=20)
  - `src/dev/net/sinic.cc` (churn=16)
- 复现: `git show 13eb3bd72083f5afb0b1c37b97f8dcf0adccc588`

### #2060 misc: bump isort version to 6.0.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2060
- 代表 commit: `998fcbc87323` (2025-03-06)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.pre-commit-config.yaml; subsys=.pre-commit-config.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.pre-commit-config.yaml` (churn=2)
- 复现: `git show 998fcbc87323376de7e0591bbd1e050c292c8c39`

### #1991 sim: Add `switch_processor` function in `simulator.py`

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1991
- 代表 commit: `561fe9d31892` (2025-03-06)
- 变更规模: commits=1, files=3, +18/-0 (churn=18)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/processors/abstract_processor.py` (churn=9)
  - `src/python/gem5/simulate/simulator.py` (churn=7)
  - `src/python/gem5/components/processors/simple_switchable_processor.py` (churn=2)
- 复现: `git show 561fe9d31892cf741177dc601aff778476ebf28f`

### #2055 misc: modify comment to remove non-ASCII characters

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2055
- 代表 commit: `472fa530beda` (2025-03-06)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/testers/spatter_gen/SpatterGen.py` (churn=4)
- 复现: `git show 472fa530beda1b728e866b62270862792e659cff`

### #2059 mem-ruby: Implement CHI ReadNoSnp Request

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2059
- 代表 commit: `a92a442b738b` (2025-03-07)
- 变更规模: commits=1, files=4, +53/-1 (churn=54)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/CHI-cache-actions.sm` (churn=32)
  - `src/mem/ruby/protocol/chi/CHI-cache-transitions.sm` (churn=18)
  - `src/mem/ruby/protocol/chi/CHI-cache-funcs.sm` (churn=3)
  - `src/mem/ruby/protocol/chi/CHI-cache.sm` (churn=1)
- 复现: `git show a92a442b738b003fbda4cd42303e69d4f1bea5ab`

### #2056 python: remove duplicate exit handler for hypercall 5

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2056
- 代表 commit: `5a4b1dc0defd` (2025-03-07)
- 变更规模: commits=1, files=1, +0/-10 (churn=10)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/exit_handler.py` (churn=10)
- 复现: `git show 5a4b1dc0defd4d668675ed9bd119f5b758a35f7b`

### #1926 cpu-o3: add retry resp to LSQ with throttling params

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1926
- 代表 commit: `b0a782ceceba` (2025-03-07)
- 变更规模: commits=1, files=3, +207/-2 (churn=209)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/lsq.cc` (churn=155)
  - `src/cpu/o3/lsq.hh` (churn=43)
  - `src/cpu/o3/BaseO3CPU.py` (churn=11)
- 复现: `git show b0a782cecebaec50b5a7bc4928ee5ba9e8da9149`

### #2063 cpu: Fix incorrect return address after flush

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2063
- 代表 commit: `55d22fd271a4` (2025-03-07)
- 变更规模: commits=1, files=2, +28/-13 (churn=41)
- 影响范围: topdirs=src; subsys=cpu/pred; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/bpred_unit.cc` (churn=37)
  - `src/cpu/pred/bpred_unit.hh` (churn=4)
- 复现: `git show 55d22fd271a48e841a9f59d0ec39cac68f1759b4`

### #1994 sim: Add option to print exit event information

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1994
- 代表 commit: `047941d1aae1` (2025-03-07)
- 变更规模: commits=1, files=3, +57/-1 (churn=58)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/exit_handler.py` (churn=28)
  - `src/python/gem5/simulate/simulator.py` (churn=23)
  - `src/python/m5/main.py` (churn=7)
- 复现: `git show 047941d1aae18ea68a3e7f8ee1d99c2558016e4e`

### #2067 misc: bump Jinja2 version to 3.1.6

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2067
- 代表 commit: `987c6e02db9e` (2025-03-10)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=2)
- 复现: `git show 987c6e02db9e9c940626a9685dc9997109cb0dcf`

### #499 cpu: BPU support for decoupled front-end

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/499
- 代表 commit: `a82b658bc7ca` (2025-03-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a82b658bc7ca96c3284fe4f9e17a8c8d9936cda1`

### #2035 mem-ruby: Move GPU L1 cache MSHR to the coalescer

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/2035
- 代表 commit: `7c75a88c3bbd` (2025-03-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7c75a88c3bbd7eef0f232d37d4d38f80364ba94e`

### #2080 scons: Explicitly define call arg in `CheckLibWithHeader`

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2080
- 代表 commit: `eec9622e5738` (2025-03-17)
- 变更规模: commits=1, files=7, +11/-9 (churn=20)
- 影响范围: topdirs=src, SConstruct; subsys=base, SConstruct, cpu, mem, src, sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=6)
  - `src/base/stats/SConsopts` (churn=4)
  - `src/base/SConsopts` (churn=2)
  - `src/cpu/kvm/SConsopts` (churn=2)
  - `src/mem/SConsopts` (churn=2)
  - `src/proto/SConsopts` (churn=2)
  - `src/sim/SConsopts` (churn=2)
- 复现: `git show eec9622e5738920087cd2e57b80d9036a9971b71`

### #2071 arch-arm: Implement FEAT_FP16

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2071
- 代表 commit: `4d57cf2073d0` (2025-03-19)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4d57cf2073d0e264c1a01516cc10ff81173ab5cc`

### #2090 python: assert if max processes not set for multisim

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2090
- 代表 commit: `20a3dec1374d` (2025-03-19)
- 变更规模: commits=1, files=1, +5/-0 (churn=5)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/utils/multisim/multisim.py` (churn=5)
- 复现: `git show 20a3dec1374d92d9fe4e5b97f9cea4e5253d2b06`

### #1825 sim-se: Implement free-list-based physical page allocator for SE mode

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1825
- 代表 commit: `81256852e498` (2025-03-20)
- 变更规模: commits=1, files=11, +460/-52 (churn=512)
- 影响范围: topdirs=src; subsys=sim, base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/free_list.hh` (churn=194)
  - `src/base/free_list.test.cc` (churn=136)
  - `src/sim/mem_pool.cc` (churn=79)
  - `src/sim/process.cc` (churn=39)
  - `src/sim/mem_state.cc` (churn=23)
  - `src/sim/mem_pool.hh` (churn=16)
  - `src/sim/process.hh` (churn=12)
  - `src/sim/se_workload.cc` (churn=6)
- 复现: `git show 81256852e4984466b44996f9a7f7880b9c880633`

### #2093 ext,stdlib: Update integration of DRAMSys

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2093
- 代表 commit: `887e29dfd64e` (2025-03-21)
- 变更规模: commits=1, files=12, +26/-60 (churn=86)
- 影响范围: topdirs=src, ext, configs, .github; subsys=ext, mem, configs, .github, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/memory/dramsys.py` (churn=27)
  - `src/mem/dramsys_wrapper.cc` (churn=13)
  - `ext/dramsys/cmake/FindSystemCLanguage.cmake` (churn=9)
  - `ext/dramsys/CMakeLists.txt` (churn=8)
  - `ext/dramsys/README` (churn=8)
  - `src/mem/dramsys_wrapper.hh` (churn=7)
  - `ext/dramsys/SConscript` (churn=4)
  - `src/mem/dramsys.cc` (churn=4)
- 复现: `git show 887e29dfd64e7444fb1941ad24543e4a64c79d82`

### #2095 systemc: Set response state in transport_dbg as well

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2095
- 代表 commit: `77acf0875d89` (2025-03-21)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/tlm_bridge/tlm_to_gem5.cc` (churn=1)
- 复现: `git show 77acf0875d8981d58cd63a14e2780097f45be81a`

### #2107 scons: add option --debug-fission

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2107
- 代表 commit: `21f41b356a35` (2025-03-25)
- 变更规模: commits=1, files=1, +9/-0 (churn=9)
- 影响范围: topdirs=SConstruct; subsys=SConstruct; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=9)
- 复现: `git show 21f41b356a35af042b1899bdd81c88910e8515eb`

### #2103 stdlib: Add warning message for multisim processor switching

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2103
- 代表 commit: `a1545728edb1` (2025-03-26)
- 变更规模: commits=1, files=1, +5/-1 (churn=6)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/processors/switchable_processor.py` (churn=6)
- 复现: `git show a1545728edb15d75f095c4908baa5ae584bbefe1`

### #2091 gpu-compute: GPU protocol tester bug

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2091
- 代表 commit: `84da74837cf7` (2025-03-26)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 84da74837cf7dd7f5b0569b32e86e25696fca9c7`

### #1942 scons: Add a target to generate a compilation database

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1942
- 代表 commit: `bbc98a626e56` (2025-03-27)
- 变更规模: commits=1, files=1, +27/-0 (churn=27)
- 影响范围: topdirs=SConstruct; subsys=SConstruct; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=27)
- 复现: `git show bbc98a626e56213d4831869a29ece453bc5b0b84`

### #2119 cpu-o3: Replace C++03 boilerplate with range-based for loops

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2119
- 代表 commit: `ef486ff89383` (2025-03-28)
- 变更规模: commits=1, files=12, +79/-304 (churn=383)
- 影响范围: topdirs=src; subsys=cpu/o3, cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/lsq.cc` (churn=112)
  - `src/cpu/o3/commit.cc` (churn=55)
  - `src/cpu/o3/iew.cc` (churn=43)
  - `src/cpu/o3/fu_pool.cc` (churn=39)
  - `src/cpu/o3/fetch.cc` (churn=31)
  - `src/cpu/o3/rob.cc` (churn=28)
  - `src/cpu/o3/rename.cc` (churn=27)
  - `src/cpu/o3/decode.cc` (churn=20)
- 复现: `git show ef486ff89383e245f5f2dc8bdd77ccd414a7afff`

### #2084 gpu-compute: Implement kernarg preload

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2084
- 代表 commit: `cd691c56cf0f` (2025-03-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show cd691c56cf0fa1d27ccf5acf1e2eb9b607cc7de8`

### #2126 sim-se: Add ArmLinux32 support for clone3 syscall

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2126
- 代表 commit: `ecd390c657c2` (2025-03-31)
- 变更规模: commits=1, files=2, +18/-1 (churn=19)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/linux/linux.hh` (churn=16)
  - `src/arch/arm/linux/se_workload.cc` (churn=3)
- 复现: `git show ecd390c657c2c4dc52cea043944176cc35656a65`

### #2127 cpu: Fix bug exposed by clang 18's -Woverloaded-virtual

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2127
- 代表 commit: `1a73c8b63d9d` (2025-03-31)
- 变更规模: commits=1, files=2, +5/-3 (churn=8)
- 影响范围: topdirs=src; subsys=cpu/pred; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/tage_sc_l.cc` (churn=6)
  - `src/cpu/pred/tage_sc_l.hh` (churn=2)
- 复现: `git show 1a73c8b63d9dfbef712cad074396196abefd5fd9`

### #1935 util: Add Python implementation of terminal client (gem5term)

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1935
- 代表 commit: `53b3727e2baf` (2025-03-31)
- 变更规模: commits=1, files=2, +412/-0 (churn=412)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/term/gem5term` (churn=328)
  - `util/term/README.md` (churn=84)
- 复现: `git show 53b3727e2bafb9fd77392cd628ff977242fe140e`

### #2130 arch-arm: Update bootloader to set SCR_EL3.HXEN bit to 1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2130
- 代表 commit: `e56775df1b3c` (2025-04-01)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=system; subsys=system; arch=-
- 主要改动文件（Top 8 by churn）:
  - `system/arm/bootloader/arm64/boot.S` (churn=1)
- 复现: `git show e56775df1b3c5549a5877fa08aca560418a6c343`

### #2128 scons,arch-arm: Fix clang 18 build issues

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2128
- 代表 commit: `adb149915fbe` (2025-04-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show adb149915fbe59aedbaed7407bce7531ac7c6813`

### #2120 arch-riscv: Fix SVNAPOT PPN encoding

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2120
- 代表 commit: `ecc6d3e5958c` (2025-04-02)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/pagetable_walker.cc` (churn=3)
- 复现: `git show ecc6d3e5958c6d570d0198a940642e4c97d1d946`

### #2131 mem: Make PortTerminator create resp_ports as well as req_ports

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2131
- 代表 commit: `96b1a3c95580` (2025-04-02)
- 变更规模: commits=1, files=2, +26/-1 (churn=27)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/port_terminator.hh` (churn=25)
  - `src/mem/port_terminator.cc` (churn=2)
- 复现: `git show 96b1a3c955805e891c4c6ea388a643e084802531`

### #2140 base, cpu-o3: Use string_view in Named constructor

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2140
- 代表 commit: `b5e76272afcd` (2025-04-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b5e76272afcd404e630f4215c8a7d1951a411008`

### #1997 sim: Fix MemoryError at system call

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1997
- 代表 commit: `2c6cac9e3d7d` (2025-04-03)
- 变更规模: commits=1, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/syscall_emul_buf.hh` (churn=8)
- 复现: `git show 2c6cac9e3d7da64983ffc16bead4f49a9220aa2e`

### #1709 arch-riscv: Fix misprediction of control flow instruction caused by vset{i}vl{i}

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1709
- 代表 commit: `afd31c741664` (2025-04-03)
- 变更规模: commits=1, files=9, +118/-59 (churn=177)
- 影响范围: topdirs=src, configs, util; subsys=arch, configs, util; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `util/cpt_upgraders/riscv-vconf.py` (churn=76)
  - `src/arch/riscv/isa/formats/vector_conf.isa` (churn=58)
  - `src/arch/riscv/decoder.cc` (churn=17)
  - `src/arch/riscv/pcstate.hh` (churn=15)
  - `src/arch/riscv/decoder.hh` (churn=4)
  - `src/arch/riscv/isa/decoder.isa` (churn=3)
  - `configs/example/gem5_library/checkpoints/riscv-hello-restore-checkpoint.py` (churn=2)
  - `src/arch/riscv/insts/vector.cc` (churn=1)
- 复现: `git show afd31c7416647f661d421f60686ba422e34a16ce`

### #2141 scons: Fix compilation error with SCons < 4.0.0

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2141
- 代表 commit: `056e7b0f4308` (2025-04-03)
- 变更规模: commits=1, files=1, +9/-3 (churn=12)
- 影响范围: topdirs=SConstruct; subsys=SConstruct; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=12)
- 复现: `git show 056e7b0f4308dbeecb2b0f7576dc97383e644e22`

### #2132 misc: bump pre-commit from 4.1.0 to 4.2.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2132
- 代表 commit: `198c773a7530` (2025-04-03)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 198c773a7530a3bb0fa41ee2eb9ed17699fd57e0`

### #2124 use mkdtemp for tmpfile in SerializationFixture

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2124
- 代表 commit: `7c9d9662cdc6` (2025-04-04)
- 变更规模: commits=1, files=2, +9/-23 (churn=32)
- 影响范围: topdirs=src; subsys=base, sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/gtest/serialization_fixture.hh` (churn=24)
  - `src/sim/serialize.test.cc` (churn=8)
- 复现: `git show 7c9d9662cdc667b77b468d64acdc828195f19f38`

### #2144 mem: Remove deprecated method

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2144
- 代表 commit: `72718e3380e2` (2025-04-04)
- 变更规模: commits=1, files=1, +0/-8 (churn=8)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/qos/QoSPolicy.py` (churn=8)
- 复现: `git show 72718e3380e282325bac66174c758f67f280e980`

### #2143 arch-riscv: Fix atomic ops on big endian hosts

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2143
- 代表 commit: `b526038c5e81` (2025-04-04)
- 变更规模: commits=1, files=1, +80/-16 (churn=96)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=96)
- 复现: `git show b526038c5e81311a62d081eceab84b622a48f796`

### #2139 mem-ruby: Make SLICC enum FooType_to_string handle the _NUM case

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2139
- 代表 commit: `32a5efe8386b` (2025-04-04)
- 变更规模: commits=1, files=1, +7/-5 (churn=12)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/slicc/symbols/Type.py` (churn=12)
- 复现: `git show 32a5efe8386b07bd455f2909b72dfa4b53213048`

### #2158 mem-ruby: Add link name to each throttle stat

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2158
- 代表 commit: `89bc2d7b9f63` (2025-04-08)
- 变更规模: commits=1, files=5, +29/-20 (churn=49)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/network/simple/Throttle.cc` (churn=22)
  - `src/mem/ruby/network/simple/Throttle.hh` (churn=11)
  - `src/mem/ruby/network/simple/Switch.cc` (churn=7)
  - `src/mem/ruby/network/simple/SimpleNetwork.cc` (churn=6)
  - `src/mem/ruby/network/simple/Switch.hh` (churn=3)
- 复现: `git show 89bc2d7b9f6342e1b65e81b48ad881cfb847a92e`

### #2152 sim-se: execve: make new process inherit max stack size

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2152
- 代表 commit: `88626aef6ea0` (2025-04-09)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/syscall_emul.hh` (churn=1)
- 复现: `git show 88626aef6ea090659c80f562df7182338916f9fb`

### #2155 ext,tests: Fix weekly DRAMSys tests failing

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2155
- 代表 commit: `901b8c228ac8` (2025-04-10)
- 变更规模: commits=1, files=1, +6/-2 (churn=8)
- 影响范围: topdirs=ext; subsys=ext; arch=-
- 主要改动文件（Top 8 by churn）:
  - `ext/dramsys/SConscript` (churn=8)
- 复现: `git show 901b8c228ac81c75b9caf7851ad68a9b75a0b848`

### #2157 mem-ruby: Fix Ruby MemCtrl functionalRead

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2157
- 代表 commit: `f0ee053bca9d` (2025-04-10)
- 变更规模: commits=1, files=3, +4/-1 (churn=5)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/common/WriteMask.hh` (churn=2)
  - `src/mem/ruby/protocol/RubySlicc_MemControl.sm` (churn=2)
  - `src/mem/ruby/protocol/RubySlicc_Exports.sm` (churn=1)
- 复现: `git show f0ee053bca9ddd9b32bf6e3a588a380b743d9c93`

### #2174 mem-ruby: return early response for software prefetches

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2174
- 代表 commit: `b1f142368c7f` (2025-04-10)
- 变更规模: commits=1, files=2, +35/-0 (churn=35)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/RubyPort.cc` (churn=19)
  - `src/mem/ruby/system/Sequencer.cc` (churn=16)
- 复现: `git show b1f142368c7fc7653872d60fb7304e5197759705`

### #2184 arch-riscv: Fix read permission for mstatus.mxr

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2184
- 代表 commit: `3b5df5df8714` (2025-04-16)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/tlb.cc` (churn=2)
- 复现: `git show 3b5df5df87148f9398843ae7322f4b13041df41d`

### #2187 mem-ruby: Revert "mem-ruby: return early response for software prefetches"

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2187
- 代表 commit: `8d0a8e2d60a9` (2025-04-16)
- 变更规模: commits=1, files=2, +0/-35 (churn=35)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/RubyPort.cc` (churn=19)
  - `src/mem/ruby/system/Sequencer.cc` (churn=16)
- 复现: `git show 8d0a8e2d60a9cda722a4080e391684ed30a9b8d1`

### #2194 arch-x86,tests: Reduce the number of X86 boot-tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2194
- 代表 commit: `84285001c96a` (2025-04-17)
- 变更规模: commits=1, files=1, +2/-35 (churn=37)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/x86_boot_tests/test_linux_boot.py` (churn=37)
- 复现: `git show 84285001c96a1f4f71b89465e4c135e003ebaea5`

### #2179 arch-riscv: Fix interupt delegation

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2179
- 代表 commit: `386362455d4c` (2025-04-18)
- 变更规模: commits=1, files=3, +69/-54 (churn=123)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/regs/misc.hh` (churn=46)
  - `src/arch/riscv/interrupts.cc` (churn=41)
  - `src/arch/riscv/isa.cc` (churn=36)
- 复现: `git show 386362455d4c8b7af393bd54b2b902a1737a4da6`

### #2190 arch-riscv: Raise PF when execute user page in S-mode

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2190
- 代表 commit: `35b4b91f4bcf` (2025-04-18)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/tlb.cc` (churn=3)
- 复现: `git show 35b4b91f4bcf6c530df2ba163e95d1b711f704f2`

### #2199 mem-ruby: Fix misprint in RubyRequest sequencer ostream<<

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2199
- 代表 commit: `31211b0d2ea8` (2025-04-21)
- 变更规模: commits=1, files=2, +4/-7 (churn=11)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/Sequencer.cc` (churn=9)
  - `src/mem/ruby/system/Sequencer.hh` (churn=2)
- 复现: `git show 31211b0d2ea84bd5b8c8c1a22cbcf07014ee34f6`

### #2165 gpu-compute: Fix kernarg preload address

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2165
- 代表 commit: `6fb51291e5a3` (2025-04-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6fb51291e5a378440a84ffddcd07a8c2ed1aae73`

### #2202 python: Fix assert conditions and fix spelling error

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2202
- 代表 commit: `b54f0ac00cff` (2025-04-23)
- 变更规模: commits=1, files=1, +3/-3 (churn=6)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/proxy.py` (churn=6)
- 复现: `git show b54f0ac00cffc23908b31a39a1b55977fe37dc0a`

### #2163 arch-vega: Improved dispatch scheduler

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2163
- 代表 commit: `cdfec4534c52` (2025-04-23)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show cdfec4534c521ea4fe9ac42f71732739defdb50c`

### #2160 configs,mem-ruby: Update ruby configs for ALL target

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2160
- 代表 commit: `a23dac9028c0` (2025-04-24)
- 变更规模: commits=1, files=9, +35/-29 (churn=64)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/ruby/MESI_Three_Level.py` (churn=10)
  - `configs/ruby/MESI_Three_Level_HTM.py` (churn=10)
  - `configs/ruby/MESI_Two_Level.py` (churn=8)
  - `configs/ruby/MOESI_CMP_directory.py` (churn=8)
  - `configs/ruby/MOESI_CMP_token.py` (churn=8)
  - `configs/ruby/MI_example.py` (churn=6)
  - `configs/ruby/MOESI_hammer.py` (churn=6)
  - `configs/ruby/Ruby.py` (churn=6)
- 复现: `git show a23dac9028c0ede337dc95548c5f648964775d9f`

### #2171 sim: Use heap allocs instead of VLAs

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2171
- 代表 commit: `86156d61d542` (2025-04-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 86156d61d542ba419661517d33bb7dd9c27bf301`

### #2170 mem: Use heap allocs instead of VLAs

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2170
- 代表 commit: `4d75fcc7dfaf` (2025-04-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4d75fcc7dfaf2573a06f42f4683b7518c4b5ca95`

### #2167 cpu: Replace uses of variable length arrays with heap allocs

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2167
- 代表 commit: `21cf5074c81e` (2025-04-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 21cf5074c81eb2f696371a1a5193a092aac37fd8`

### #2168 arm: Replace variable length arrays with strings/heap allocs

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2168
- 代表 commit: `6fb59a253247` (2025-04-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6fb59a2532471fbd903a3da3bba36609be2ce396`

### #2169 base,systemc,python: Replace use of VLAs with heap allocs/strings

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2169
- 代表 commit: `314a4f9a01e8` (2025-04-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 314a4f9a01e836f1416389d3920b0f3fea0b8c8f`

### #2206 arch: addr_range: Use heap allocs instead of VLAs

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2206
- 代表 commit: `c22742b69b08` (2025-04-24)
- 变更规模: commits=1, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/addr_range.hh` (churn=8)
- 复现: `git show c22742b69b08f2831c1d68f74839f89841cefe7e`

### #2208 Use heap allocations instead of VLAs in virtio device code

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2208
- 代表 commit: `1282c9a44268` (2025-04-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1282c9a442684227e16f6bc76b1ff5b3b51024ea`

### #2189 sim: Add warning for non-default create function

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2189
- 代表 commit: `4de3b6e781a0` (2025-04-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 4de3b6e781a05c6dd36c08a78c757489786a6065`

### #2209 arch-x86: apply fix for pack micro-op on duplicated code

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2209
- 代表 commit: `dbd84a108261` (2025-04-25)
- 变更规模: commits=1, files=1, +8/-5 (churn=13)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/isa/microops/mediaop.isa` (churn=13)
- 复现: `git show dbd84a108261f69ee3c47101c96ec319321a5b34`

### #2210 configs,mem-ruby: Fix Directory_Controller fallback logic

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2210
- 代表 commit: `662cef0ea78e` (2025-04-26)
- 变更规模: commits=1, files=1, +17/-4 (churn=21)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/ruby/Ruby.py` (churn=21)
- 复现: `git show 662cef0ea78e407dbdfa5c5bd930464e86098c42`

### #2214 cpu-o3: break Request::NO_ACCESS reference cycle

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2214
- 代表 commit: `7ec75d0ba433` (2025-04-28)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/lsq.cc` (churn=4)
- 复现: `git show 7ec75d0ba43379b54e783fd3f19310ce963307bd`

### #2216 mem-ruby,scons: Disable verbose SLICC output unless flag set

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2216
- 代表 commit: `fa482ec67798` (2025-04-28)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/SConscript` (churn=4)
- 复现: `git show fa482ec67798ddfbaa522d9a17904a6201465cf7`

### #2220 cpu: fix memory leak on indirect branch prediction

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2220
- 代表 commit: `bfb6c54e45de` (2025-04-28)
- 变更规模: commits=1, files=1, +3/-3 (churn=6)
- 影响范围: topdirs=src; subsys=cpu/pred; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/bpred_unit.cc` (churn=6)
- 复现: `git show bfb6c54e45de98deb02f78336ad6422640e2cf6d`

### #2223 arch-riscv: Fix CMO decoding

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2223
- 代表 commit: `85ef2cf6ca5d` (2025-04-28)
- 变更规模: commits=1, files=1, +78/-74 (churn=152)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=152)
- 复现: `git show 85ef2cf6ca5dc2cd0e341097f15524124f812ce1`

### #2026 arch-riscv: fix narrowing instructions with pin µop

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2026
- 代表 commit: `fb80ec811947` (2025-04-29)
- 变更规模: commits=1, files=2, +83/-20 (churn=103)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=75)
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=28)
- 复现: `git show fb80ec811947ec00e8821357d370cc4245a0a506`

### #2226 systemc: Export simple arithmetic operations in sc_time pybind

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2226
- 代表 commit: `d06f9416f4f1` (2025-04-29)
- 变更规模: commits=1, files=1, +4/-0 (churn=4)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/core/sc_time_python.cc` (churn=4)
- 复现: `git show d06f9416f4f10dae0576d9f04821ec3d35212ef7`

### #2193 Fix, improve, refactor gpu-tests to improve runner testing stability

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2193
- 代表 commit: `13d116f3925e` (2025-04-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 13d116f3925e0437db29753918a4d87dedab1c96`

### #2230 dev: Make sid, ssid optional

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2230
- 代表 commit: `0f1a747fcb70` (2025-04-30)
- 变更规模: commits=1, files=2, +21/-13 (churn=34)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/dma_device.hh` (churn=20)
  - `src/dev/dma_device.cc` (churn=14)
- 复现: `git show 0f1a747fcb7055f1491a7904297626a51cec20f5`

### #2161 util: Add feature to update debug flags in external hypercall util

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2161
- 代表 commit: `c548cd7eed5d` (2025-04-30)
- 变更规模: commits=1, files=2, +55/-6 (churn=61)
- 影响范围: topdirs=src, util; subsys=python, util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/exit_handler.py` (churn=34)
  - `util/hypercall_external_signal/orchestrator-request.py` (churn=27)
- 复现: `git show c548cd7eed5d49a4bd9b0b6a5812df60386c79e1`

### #2231 configs: Remove riscv/fs_linux.py duplicate --bootloader option

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2231
- 代表 commit: `4672ca9c97a7` (2025-04-30)
- 变更规模: commits=1, files=1, +0/-6 (churn=6)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/riscv/fs_linux.py` (churn=6)
- 复现: `git show 4672ca9c97a7546095e6473c2df95052a5b98d83`

### #2236 arch-power: Avoid incidental use of VLA in sizeof expression.

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2236
- 代表 commit: `9745d9c0f320` (2025-05-01)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=power
- 主要改动文件（Top 8 by churn）:
  - `src/arch/power/tlb.cc` (churn=4)
- 复现: `git show 9745d9c0f32047d001a81c1af466c4fab46f831c`

### #2237 arch-mips: Avoid incidental use of VLA in sizeof expression

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2237
- 代表 commit: `bd9fe3d078cb` (2025-05-01)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=mips
- 主要改动文件（Top 8 by churn）:
  - `src/arch/mips/tlb.cc` (churn=4)
- 复现: `git show bd9fe3d078cb5dc444ecd26da64a26b1a5dd8e45`

### #2239 cpu: Prevent re-executing load instruction

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2239
- 代表 commit: `ef7f48c6c465` (2025-05-01)
- 变更规模: commits=1, files=1, +5/-0 (churn=5)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/lsq_unit.cc` (churn=5)
- 复现: `git show ef7f48c6c465fa81e50fe8bc5b59d5d5591e47b8`

### #2235 arch-x86: Don't use non ISO C++ variable length arrays

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2235
- 代表 commit: `3e770ea533ff` (2025-05-01)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3e770ea533ffb32574873e4e692a118366cad8b7`

### #2225 sim, util: changes to the external signal handler

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2225
- 代表 commit: `406827c3427a` (2025-05-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 406827c3427a87c1fe5c2fda494babb4894b9211`

### #2207 misc: Increase Clang support to v19; Drop GCC v10 support

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2207
- 代表 commit: `e94fcac16f8e` (2025-05-03)
- 变更规模: commits=1, files=4, +38/-37 (churn=75)
- 影响范围: topdirs=.github, SConstruct, util; subsys=.github, SConstruct, util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=40)
  - `util/dockerfiles/docker-bake.hcl` (churn=29)
  - `.github/workflows/compiler-tests.yaml` (churn=4)
  - `.github/workflows/ci-tests.yaml` (churn=2)
- 复现: `git show e94fcac16f8e7f0a909fecae4c0249704594f142`

### #2246 tests: Reduce the gpu_ruby_rand Daily tests to 1h

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2246
- 代表 commit: `8fb258eeaec7` (2025-05-04)
- 变更规模: commits=1, files=2, +4/-4 (churn=8)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/gpu/test_gpu_ruby_random.py` (churn=4)
  - `tests/gem5/gpu/test_gpu_ruby_random_wbL2.py` (churn=4)
- 复现: `git show 8fb258eeaec70ebb8d3431f3b8905072869b195e`

### #2215 sim: convert entering event queue message to debug flag

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2215
- 代表 commit: `23b6885d9c6f` (2025-05-05)
- 变更规模: commits=1, files=2, +5/-1 (churn=6)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/simulate.cc` (churn=5)
  - `src/sim/SConscript` (churn=1)
- 复现: `git show 23b6885d9c6f5a12e4bb99cf1ab39bf2a0830dc3`

### #2243 arch-vega: Ignore EXEC SGPR dest for VOP3 V_CMPX_*

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2243
- 代表 commit: `88ec168de394` (2025-05-05)
- 变更规模: commits=1, files=1, +6/-2 (churn=8)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/op_encodings.cc` (churn=8)
- 复现: `git show 88ec168de39447f32736fa3895629fc23688613a`

### #2242 gpu: Remove SDMA header heap allocations

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2242
- 代表 commit: `12fbd8ebc000` (2025-05-05)
- 变更规模: commits=1, files=3, +83/-57 (churn=140)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/amdgpu/sdma_engine.cc` (churn=83)
  - `src/dev/amdgpu/sdma_packets.hh` (churn=41)
  - `src/dev/amdgpu/sdma_engine.hh` (churn=16)
- 复现: `git show 12fbd8ebc0005d7dc88ac1321326a643612a36ae`

### #2241 mem-ruby: Fix DMA sequencer request size above 64

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2241
- 代表 commit: `41b9c344d760` (2025-05-05)
- 变更规模: commits=1, files=2, +47/-9 (churn=56)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/DMASequencer.cc` (churn=53)
  - `src/mem/ruby/system/DMASequencer.hh` (churn=3)
- 复现: `git show 41b9c344d7603e2f4dad43d3248e6b9a23ac75cb`

### #2247 dev: Make sid, ssid option in dmaRead, dmaWrite

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2247
- 代表 commit: `cf0d66767aa4` (2025-05-06)
- 变更规模: commits=1, files=1, +4/-2 (churn=6)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/dma_device.hh` (churn=6)
- 复现: `git show cf0d66767aa4d30e18cafa3bb4e31e67b8c66ba4`

### #2252 python: Add OptionalParam to support std::optional.

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2252
- 代表 commit: `362a5622cdc9` (2025-05-07)
- 变更规模: commits=1, files=1, +63/-0 (churn=63)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/params.py` (churn=63)
- 复现: `git show 362a5622cdc9d10d4078a222e89774449d35a7fb`

### #2255 mem-ruby: fix deadlocks when running ruby_random_test with -n > 18

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2255
- 代表 commit: `9a391e5f2c3f` (2025-05-07)
- 变更规模: commits=1, files=4, +59/-29 (churn=88)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/testers/rubytest/RubyTester.cc` (churn=48)
  - `src/cpu/testers/rubytest/Check.cc` (churn=25)
  - `src/cpu/testers/rubytest/Check.hh` (churn=10)
  - `src/cpu/testers/rubytest/RubyTester.hh` (churn=5)
- 复现: `git show 9a391e5f2c3f24402a1b9f1d16b958bc5984c7a5`

### #2256 configs: Enable bootloader argument for RISCV simulations

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2256
- 代表 commit: `ae355c2df6fd` (2025-05-07)
- 变更规模: commits=1, files=1, +5/-5 (churn=10)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/common/Options.py` (churn=10)
- 复现: `git show ae355c2df6fd06b4d8e377b8f05554e7217fefb4`

### #2257 arch-sparc: Fix SyntaxWarnings in sparc/isa/base.isa

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2257
- 代表 commit: `3ba0be51b575` (2025-05-07)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=sparc
- 主要改动文件（Top 8 by churn）:
  - `src/arch/sparc/isa/base.isa` (churn=4)
- 复现: `git show 3ba0be51b575d0ee32570b17f165068c3c12129b`

### #2249 resources: return a copy of resource JSON to avoid mutations

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2249
- 代表 commit: `19a5bb3a3a00` (2025-05-07)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/client_api/jsonclient.py` (churn=3)
- 复现: `git show 19a5bb3a3a0094344138c67de8eaa0970665beae`

### #2234 gpu: Don't use VLA in device (de)serialization

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2234
- 代表 commit: `8a869c8407ce` (2025-05-07)
- 变更规模: commits=1, files=4, +220/-216 (churn=436)
- 影响范围: topdirs=src; subsys=dev, sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/amdgpu/pm4_packet_processor.cc` (churn=186)
  - `src/dev/amdgpu/sdma_engine.cc` (churn=153)
  - `src/dev/amdgpu/amdgpu_device.cc` (churn=80)
  - `src/sim/serialize.hh` (churn=17)
- 复现: `git show 8a869c8407ce4ad9c3d2b68b783a381b426ad52c`

### #2217 scons: Remove -Wno-vla-cxx-extension. gem5 is now VLA-clean

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2217
- 代表 commit: `45f60e2f0604` (2025-05-08)
- 变更规模: commits=1, files=1, +0/-2 (churn=2)
- 影响范围: topdirs=SConstruct; subsys=SConstruct; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=2)
- 复现: `git show 45f60e2f0604d4d99cc2a9106334b1a5250edd99`

### #2245 tests,misc: Improve dir finding for GH Actions; merge separate jobs

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2245
- 代表 commit: `23e674502bd6` (2025-05-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 23e674502bd69861810f4153084e96fbdb3e25c2`

### #2259 arch-x86: Return early from MWAIT if address monitor is not armed

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2259
- 代表 commit: `85d30451706a` (2025-05-08)
- 变更规模: commits=1, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/isa/formats/monitor_mwait.isa` (churn=3)
- 复现: `git show 85d30451706a4ecfc19b027498188818fdd80699`

### #2263 arch-vega,gpu-compute: Opcode overrides based on gfx version

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2263
- 代表 commit: `973a11167b54` (2025-05-09)
- 变更规模: commits=1, files=9, +118/-0 (churn=118)
- 影响范围: topdirs=src; subsys=gpu-compute, arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/vop2.cc` (churn=37)
  - `src/arch/amdgpu/vega/insts/instructions.hh` (churn=34)
  - `src/arch/amdgpu/vega/gpu_decoder.cc` (churn=26)
  - `src/arch/amdgpu/vega/gpu_decoder.hh` (churn=6)
  - `src/gpu-compute/gpu_command_processor.cc` (churn=6)
  - `src/gpu-compute/shader.cc` (churn=6)
  - `src/gpu-compute/fetch_unit.cc` (churn=1)
  - `src/gpu-compute/gpu_command_processor.hh` (churn=1)
- 复现: `git show 973a11167b54c63e753b9994030521239a0180fb`

### #2260 dev: Adopt OptionalParam for DmaDevice.

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2260
- 代表 commit: `a06dac9560aa` (2025-05-12)
- 变更规模: commits=1, files=1, +2/-4 (churn=6)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/Device.py` (churn=6)
- 复现: `git show a06dac9560aa0471ba9d6590294c3a7a5421f435`

### #2264 misc: Introduce dictionary parameters (DictParam) in gem5

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2264
- 代表 commit: `187793e80da5` (2025-05-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 187793e80da535b3d7625ca4a7d422d4efb324eb`

### #1854 Speculative update for TAGE-SC-L

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/1854
- 代表 commit: `0da730d6e86c` (2025-05-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0da730d6e86c1147eb0353f2bb0b394c467692aa`

### #2275 scons: Add cxx_config support for OptionalParams

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2275
- 代表 commit: `cbce413a0e39` (2025-05-15)
- 变更规模: commits=1, files=1, +18/-3 (churn=21)
- 影响范围: topdirs=build_tools; subsys=build_tools; arch=-
- 主要改动文件（Top 8 by churn）:
  - `build_tools/cxx_config_cc.py` (churn=21)
- 复现: `git show cbce413a0e39fd7a75f163bd23dc77b1f71d1ad9`

### #2276 scons: Use two return booleans for DictParams in cxx_config

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2276
- 代表 commit: `d1d6daf4a769` (2025-05-15)
- 变更规模: commits=1, files=1, +7/-2 (churn=9)
- 影响范围: topdirs=build_tools; subsys=build_tools; arch=-
- 主要改动文件（Top 8 by churn）:
  - `build_tools/cxx_config_cc.py` (churn=9)
- 复现: `git show d1d6daf4a76900e556f3c46a31a4a2c2de0cf8d6`

### #2162 arch-vega: Add pagetable walker buffer

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2162
- 代表 commit: `5d5ea63e7948` (2025-05-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5d5ea63e7948738f71a88f6c544fb14b4b33a509`

### #2280 dev: add value check for debug flag

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2280
- 代表 commit: `0c6c0f6c4b69` (2025-05-17)
- 变更规模: commits=1, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/reg_bank.hh` (churn=8)
- 复现: `git show 0c6c0f6c4b69460f5ec187374691f893aa6a7fa3`

### #2278 stdlib, tests: Fix ruby cache init signature

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2278
- 代表 commit: `b480d4e2d1af` (2025-05-19)
- 变更规模: commits=1, files=5, +16/-16 (churn=32)
- 影响范围: topdirs=src, tests; subsys=python, tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/traffic_gen/configs/simple_traffic_run.py` (churn=14)
  - `src/python/gem5/components/cachehierarchies/ruby/mesi_three_level_cache_hierarchy.py` (churn=8)
  - `src/python/gem5/components/cachehierarchies/ruby/mesi_two_level_cache_hierarchy.py` (churn=6)
  - `src/python/gem5/components/cachehierarchies/ruby/mi_example_cache_hierarchy.py` (churn=2)
  - `tests/gem5/replacement_policies/configs/cache_hierarchies.py` (churn=2)
- 复现: `git show b480d4e2d1afc98fbc890714dbe8b77f6f7256f9`

### #2173 cpu-o3: put unsent mem req to retry queue

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2173
- 代表 commit: `d86d164d9757` (2025-05-20)
- 变更规模: commits=1, files=5, +28/-2 (churn=30)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/lsq_unit.cc` (churn=12)
  - `src/cpu/o3/iew.cc` (churn=6)
  - `src/cpu/o3/inst_queue.cc` (churn=6)
  - `src/cpu/o3/iew.hh` (churn=3)
  - `src/cpu/o3/inst_queue.hh` (churn=3)
- 复现: `git show d86d164d97571dc1e2d6388d5098de59fcba90d1`

### #2285 arch-arm: Initialize the BaseMMU::Mode in TLBTypes::KeyType

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2285
- 代表 commit: `94cc9aa8b790` (2025-05-20)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/pagetable.cc` (churn=3)
- 复现: `git show 94cc9aa8b7909c13b25cdb0a89d471fb1582a44d`

### #2284 dev: Update MI300X model to use real firmware

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2284
- 代表 commit: `ffbfe65b2512` (2025-05-21)
- 变更规模: commits=1, files=15, +359/-39 (churn=398)
- 影响范围: topdirs=src, configs; subsys=dev, configs, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/gpufs/system/system.py` (churn=79)
  - `src/dev/amdgpu/amdgpu_vm.cc` (churn=78)
  - `src/python/gem5/components/devices/gpus/amdgpu.py` (churn=71)
  - `src/dev/amdgpu/amdgpu_device.cc` (churn=43)
  - `src/dev/amdgpu/amdgpu_vm.hh` (churn=28)
  - `src/dev/amdgpu/sdma_engine.cc` (churn=23)
  - `src/dev/amdgpu/amdgpu_nbio.cc` (churn=20)
  - `configs/example/gpufs/mi300.py` (churn=17)
- 复现: `git show ffbfe65b25126fc7451cf32c5e8a4162dedaa10a`

### #2272 gpu-compute,arch-vega: Included two new DS instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2272
- 代表 commit: `6f07f07de5d3` (2025-05-21)
- 变更规模: commits=1, files=3, +128/-4 (churn=132)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/ds.cc` (churn=119)
  - `src/arch/amdgpu/vega/insts/instructions.hh` (churn=10)
  - `src/arch/amdgpu/vega/gpu_registers.cc` (churn=3)
- 复现: `git show 6f07f07de5d3d2a1d744d3c773f5da1fbda1a118`

### #2289 dev: Print warn_access message in IsaFake

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2289
- 代表 commit: `306fb839c982` (2025-05-22)
- 变更规模: commits=1, files=1, +8/-4 (churn=12)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/isa_fake.cc` (churn=12)
- 复现: `git show 306fb839c9829ce55774366b6a3c8f4877803e6a`

### #2291 mem: fixed bug with prefetcher probes

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2291
- 代表 commit: `a5bc885b0d49` (2025-05-22)
- 变更规模: commits=1, files=1, +3/-3 (churn=6)
- 影响范围: topdirs=src; subsys=mem/cache/prefetch; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/prefetch/base.cc` (churn=6)
- 复现: `git show a5bc885b0d492a81446046f91b674f8e26765489`

### #2294 mem: Add port name in port_wrapper panic message

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2294
- 代表 commit: `222f29e02669` (2025-05-22)
- 变更规模: commits=1, files=1, +13/-7 (churn=20)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/port_wrapper.cc` (churn=20)
- 复现: `git show 222f29e026696b8d26815a31d0bdd5062d4e6f0f`

### #2288 arch: Fix isa_parser Format initial

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2288
- 代表 commit: `82dd7efa8fd9` (2025-05-22)
- 变更规模: commits=1, files=1, +10/-4 (churn=14)
- 影响范围: topdirs=src; subsys=arch; arch=isa_parser
- 主要改动文件（Top 8 by churn）:
  - `src/arch/isa_parser/isa_parser.py` (churn=14)
- 复现: `git show 82dd7efa8fd9bbee74fba490b77a914a7fbb0ef9`

### #2274 stdlib: correct memory range check in dramsim3

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2274
- 代表 commit: `dd43f9d1bd52` (2025-05-22)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/memory/dramsim_3.py` (churn=2)
- 复现: `git show dd43f9d1bd521bad4fd807e85d45ace37b6120bd`

### #2286 arch-riscv: Fix move scalar to vector tail agnostic behaviour

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/2286
- 代表 commit: `a095b5ddc044` (2025-05-27)
- 变更规模: commits=1, files=2, +23/-0 (churn=23)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=13)
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=10)
- 复现: `git show a095b5ddc0443fdb34e6725a1ef0d6560b56b438`

### #1855 cpu: Add taken-only history

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1855
- 代表 commit: `e95bad48181e` (2025-05-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e95bad48181ee2760d5b4e07fc6aadd43895a0a7`

### #2271 arch-arm: Add stats to track PMU events

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2271
- 代表 commit: `d73af2932bbe` (2025-05-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d73af2932bbe87aa52c3f74df74a8daf788889fe`

### #2287 arch-arm: Implement FEAT_FHM and FEAT_FRINTTS.

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2287
- 代表 commit: `72dcf3177d6b` (2025-05-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 72dcf3177d6bfe9becfd406c53cbd238f8351494`

### #2295 mem: Set default panic callback in port_wrapper

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2295
- 代表 commit: `8c3e9f4a5aea` (2025-05-27)
- 变更规模: commits=1, files=2, +25/-20 (churn=45)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/port_wrapper.hh` (churn=32)
  - `src/mem/port_wrapper.cc` (churn=13)
- 复现: `git show 8c3e9f4a5aeaa468f9261a992d8a83e47f809b93`

### #2240 resources: Update backend resources API

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2240
- 代表 commit: `9d219455f3d5` (2025-05-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 9d219455f3d5d0960c8f0271f9e0c8cffc91938e`

### #2023 arch-riscv: Add support for fault-only-first unit-stride segment load instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2023
- 代表 commit: `a2de450019c7` (2025-05-27)
- 变更规模: commits=1, files=6, +335/-48 (churn=383)
- 影响范围: topdirs=src; subsys=arch, cpu, cpu/o3; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=364)
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=11)
  - `src/arch/riscv/insts/vector.hh` (churn=3)
  - `src/cpu/o3/FuncUnitConfig.py` (churn=2)
  - `src/cpu/op_class.hh` (churn=2)
  - `src/cpu/FuncUnit.py` (churn=1)
- 复现: `git show a2de450019c74eb1522dd11ad91350c21450c63b`

### #1387 arch-riscv: Add Hypervisor (H) extension

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/1387
- 代表 commit: `1e0ab7184f72` (2025-05-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 1e0ab7184f722f23fe4cb7824fc36011ed8183b2`

### #2323 util: Update upstream_msg_filter.sed to support macOS

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2323
- 代表 commit: `0eea4d1a5202` (2025-05-29)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/maint/upstream_msg_filter.sed` (churn=4)
- 复现: `git show 0eea4d1a52024cf874c01e594243ee517ecf4a71`

### #2330 util: Bump GPUFS application builder dockerfile

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2330
- 代表 commit: `fb91fc46c664` (2025-05-31)
- 变更规模: commits=2, files=2, +18/-20 (churn=38)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/gpu-fs/Dockerfile` (churn=26)
  - `util/dockerfiles/gpu-fs/README.md` (churn=12)
- commits 列表（按 topo-order，Top 12）：
  - 2025-05-31 `36046bf33271` util: Bump GPUFS application builder dockerfile
  - 2025-05-31 `fb91fc46c664` util: Bump GPUFS application builder dockerfile
- 复现: `git show fb91fc46c6643611d002976420c930ee4617bf66`

### #2326 mem-garnet,mem-ruby: Fixed Garnet_standalone simulation

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2326
- 代表 commit: `3dc9dccd0fe3` (2025-05-31)
- 变更规模: commits=2, files=2, +18/-2 (churn=20)
- 影响范围: topdirs=configs, src; subsys=configs, mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/ruby/Garnet_standalone.py` (churn=12)
  - `src/mem/slicc/symbols/StateMachine.py` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2025-05-31 `34fd83520839` mem-garnet,mem-ruby: Fixed Garnet_standalone simulation
  - 2025-05-31 `3dc9dccd0fe3` mem-garnet,mem-ruby: Fixed Garnet_standalone simulation
- 复现: `git show 3dc9dccd0fe3c592c07ba5afe9b7f4712420287c`

### #2324 scons: Fix gcc version compare

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2324
- 代表 commit: `6ce1d99472c0` (2025-05-31)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=SConstruct; subsys=SConstruct; arch=-
- 主要改动文件（Top 8 by churn）:
  - `SConstruct` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-05-31 `f06f51f02ac4` scons: Fix gcc version compare
  - 2025-05-31 `6ce1d99472c0` scons: Fix gcc version compare
- 复现: `git show 6ce1d99472c036fa6a75a7d5a6d757fecb1049ce`

### #2333 misc: bump mypy from 1.15.0 to 1.16.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2333
- 代表 commit: `5e05c83daaa1` (2025-06-02)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-02 `e0ff3cafbfda` misc: bump mypy from 1.15.0 to 1.16.0
  - 2025-06-02 `5e05c83daaa1` misc: bump mypy from 1.15.0 to 1.16.0
- 复现: `git show 5e05c83daaa1aa96dffb5f79c4d9443464cc4e8d`

### #2331 arch-riscv: Allow load-reserved accesses to use the TLB

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2331
- 代表 commit: `fc2880e4ae97` (2025-06-02)
- 变更规模: commits=2, files=3, +34/-76 (churn=110)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/pagetable_walker.cc` (churn=58)
  - `src/arch/riscv/tlb.cc` (churn=40)
  - `src/arch/riscv/tlb.hh` (churn=12)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-02 `db1a08d8c864` arch-riscv: Allow load-reserved accesses to use the TLB
  - 2025-06-02 `fc2880e4ae97` arch-riscv: Allow load-reserved accesses to use the TLB
- 复现: `git show fc2880e4ae97c40feff87f17de5ca097efd67a81`

### #2022 arch-riscv: Add support for vector stride segment load/store instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2022
- 代表 commit: `6f0574b91c1f` (2025-06-02)
- 变更规模: commits=2, files=8, +1456/-804 (churn=2260)
- 影响范围: topdirs=src; subsys=arch, cpu, cpu/o3; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=1414)
  - `src/arch/riscv/isa/decoder.isa` (churn=496)
  - `src/arch/riscv/insts/vector.hh` (churn=188)
  - `src/arch/riscv/insts/vector.cc` (churn=88)
  - `src/arch/riscv/isa/formats/vector_mem.isa` (churn=54)
  - `src/cpu/o3/FuncUnitConfig.py` (churn=8)
  - `src/cpu/op_class.hh` (churn=8)
  - `src/cpu/FuncUnit.py` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-02 `b7c10c1a3783` arch-riscv: Add support for vector stride segment load/store instructions
  - 2025-06-02 `6f0574b91c1f` arch-riscv: Add support for vector stride segment load/store instructions
- 复现: `git show 6f0574b91c1f36e26cbbe91b1db3f102fabf4d8b`

### #2319 misc: bump pyupgrade from v3.19.1 to v3.20.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2319
- 代表 commit: `86de5dcce443` (2025-06-02)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=.pre-commit-config.yaml; subsys=.pre-commit-config.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.pre-commit-config.yaml` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-05-29 `f938ff5200f1` misc: bump pyupgrade from v3.19.1 to v3.20.0
  - 2025-06-02 `86de5dcce443` misc: bump pyupgrade from v3.19.1 to v3.20.0
- 复现: `git show 86de5dcce4437b862fbe8457ddeaf02b89822f11`

### #2348 sim: remove cerr and improve macOS support in init_signals.cc

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2348
- 代表 commit: `9fafef35af83` (2025-06-09)
- 变更规模: commits=2, files=1, +10/-6 (churn=16)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/init_signals.cc` (churn=16)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-06 `bf1af96e1aa1` sim: remove cerr and improve macOS support in init_signals.cc
  - 2025-06-09 `9fafef35af83` sim: remove cerr and improve macOS support in init_signals.cc
- 复现: `git show 9fafef35af83ff2370551f2bbb8b67cb67ffc62c`

### #2349 util, stdlib: changes to hypercall external signal

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2349
- 代表 commit: `bd4a543cc443` (2025-06-09)
- 变更规模: commits=2, files=3, +30/-4 (churn=34)
- 影响范围: topdirs=util, src; subsys=util, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/hypercall_external_signal/transmitter.py` (churn=26)
  - `src/python/gem5/simulate/exit_handler.py` (churn=4)
  - `util/hypercall_external_signal/orchestrator-request.py` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-06 `669be59d36ba` util, stdlib: changes to hypercall external signal
  - 2025-06-09 `bd4a543cc443` util, stdlib: changes to hypercall external signal
- 复现: `git show bd4a543cc4436066e6481c9489bee7abcbaf1603`

### #2355 arch-riscv: Fix timing walk exit path on early fault

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2355
- 代表 commit: `34571cc20ffa` (2025-06-09)
- 变更规模: commits=2, files=2, +88/-18 (churn=106)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/tlb.cc` (churn=72)
  - `src/arch/riscv/pagetable_walker.cc` (churn=34)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-09 `358531dd1694` arch-riscv: Fix timing walk exit path on early fault
  - 2025-06-09 `34571cc20ffa` arch-riscv: Fix timing walk exit path on early fault
- 复现: `git show 34571cc20ffaf55892605761a8a26b272e81da9e`

### #2361 build(deps): bump requests from 2.32.0 to 2.32.4 in /util/gem5-resources-manager

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2361
- 代表 commit: `fe30ac74ef14` (2025-06-10)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-10 `117b52f3e1ff` build(deps): bump requests from 2.32.0 to 2.32.4 in /util/gem5-resources-manager
  - 2025-06-10 `fe30ac74ef14` build(deps): bump requests from 2.32.0 to 2.32.4 in /util/gem5-resources-manager
- 复现: `git show fe30ac74ef14431d0b0719d025f1621691525cf5`

### #2021 arch-riscv: Fix incorrect vector unit-stride segment load instructions

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2021
- 代表 commit: `9222fe71493a` (2025-06-10)
- 变更规模: commits=2, files=3, +128/-18 (churn=146)
- 影响范围: topdirs=src; subsys=arch, cpu/o3; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/insts/vector.cc` (churn=90)
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=48)
  - `src/cpu/o3/FuncUnitConfig.py` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-03 `27bb878c3d47` arch-riscv: Fix incorrect vector unit-stride segment load instructions
  - 2025-06-10 `9222fe71493a` arch-riscv: Fix incorrect vector unit-stride segment load instructions
- 复现: `git show 9222fe71493a5a7ba53f4fb03b0c9be5824fb2a6`

### #2307 arch-riscv: fix vcpyvs µops for vector indexed loads

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2307
- 代表 commit: `c36e041c7467` (2025-06-10)
- 变更规模: commits=2, files=1, +18/-10 (churn=28)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/templates/vector_mem.isa` (churn=28)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-10 `480492088978` arch-riscv: fix vcpyvs µops for vector indexed loads
  - 2025-06-10 `c36e041c7467` arch-riscv: fix vcpyvs µops for vector indexed loads
- 复现: `git show c36e041c7467f39810d69016ae5a81632363093d`

### #2343 tests: Update pyunit tests to work with v25.0

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2343
- 代表 commit: `ac32d2abab75` (2025-06-10)
- 变更规模: commits=2, files=6, +112/-84 (churn=196)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/pyunit/stdlib/resources/refs/resource-specialization.json` (churn=72)
  - `tests/pyunit/stdlib/resources/refs/obtain-resource.json` (churn=66)
  - `tests/pyunit/stdlib/resources/refs/suite-checks.json` (churn=34)
  - `tests/pyunit/stdlib/resources/pyunit_obtain_resources_check.py` (churn=8)
  - `tests/pyunit/stdlib/resources/pyunit_resource_specialization.py` (churn=8)
  - `tests/pyunit/stdlib/resources/pyunit_suite_checks.py` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-10 `b6ac11a03313` tests: Update pyunit tests to work with v25.0
  - 2025-06-10 `ac32d2abab75` tests: Update pyunit tests to work with v25.0
- 复现: `git show ac32d2abab75de84a970110397c8fbf0d9eaee7c`

### #2363 tests: Fix x86 boot test by removing overriding of kernel args

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2363
- 代表 commit: `99bd5fee3a6a` (2025-06-11)
- 变更规模: commits=2, files=1, +2/-4 (churn=6)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/x86_boot_tests/configs/x86_boot_exit_run.py` (churn=6)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-10 `fff836a0ca0e` tests: Fix x86 boot test by removing overriding of kernel args
  - 2025-06-11 `99bd5fee3a6a` tests: Fix x86 boot test by removing overriding of kernel args
- 复现: `git show 99bd5fee3a6a7ad232ad530d07ff80d3f582166a`

### #2366 tests: CHI_ prefix needed after ALL protocol PR

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2366
- 代表 commit: `929e5deed12f` (2025-06-11)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/chi_tlm_tests/configs/ruby_mem_test.py` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-11 `47049e790a3e` tests: CHI_ prefix needed after ALL protocol PR
  - 2025-06-11 `929e5deed12f` tests: CHI_ prefix needed after ALL protocol PR
- 复现: `git show 929e5deed12f7a44e2261d5b983fccba9e71f489`

### #2367 stdlib: Return HTTP error for Azure Func API

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2367
- 代表 commit: `3ac583d4a527` (2025-06-11)
- 变更规模: commits=2, files=1, +46/-10 (churn=56)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/client_api/azure_functions_client.py` (churn=56)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-11 `53053eb9fa19` stdlib: Return HTTP error for Azure Func API
  - 2025-06-11 `3ac583d4a527` stdlib: Return HTTP error for Azure Func API
- 复现: `git show 3ac583d4a52732d87895134be5531671602367b3`

### #2360 stdlib: change multisim stderr.txt to simerr.txt

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2360
- 代表 commit: `cd210807b2dd` (2025-06-11)
- 变更规模: commits=2, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/core.py` (churn=8)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-11 `ced7f124fe62` stdlib: change multisim stderr.txt to simerr.txt
  - 2025-06-11 `cd210807b2dd` stdlib: change multisim stderr.txt to simerr.txt
- 复现: `git show cd210807b2dd1e5d99e12bfa7ba3474e845ec781`

### #2365 tests: Reduce Nightly/Weekly X86/ARM Boot tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2365
- 代表 commit: `d90d9e77de44` (2025-06-11)
- 变更规模: commits=2, files=3, +55/-16 (churn=71)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/arm_boot_tests/configs/arm_boot_exit_run.py` (churn=32)
  - `tests/gem5/arm_boot_tests/test_linux_boot.py` (churn=21)
  - `tests/gem5/x86_boot_tests/test_linux_boot.py` (churn=18)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-11 `b866941f4f65` tests: Reduce Nightly/Weekly X86/ARM Boot tests
  - 2025-06-11 `d90d9e77de44` tests: Reduce Nightly/Weekly X86/ARM Boot tests
- 复现: `git show d90d9e77de44b879d25e69dc7cf5da81c85b164f`

### #2336 misc: v25.0.0.0 release notes

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2336
- 代表 commit: `0a90390078a2` (2025-06-17)
- 变更规模: commits=1, files=1, +227/-1 (churn=228)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=228)
- 复现: `git show 0a90390078a2d476d5808dd27b990bbaa6863eb3`

### #2371 arch-arm: Automatically enable stat probe listeners

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2371
- 代表 commit: `1a849a5beea4` (2025-06-17)
- 变更规模: commits=2, files=1, +28/-4 (churn=32)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/pmu.cc` (churn=32)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-16 `a4174dd7abf8` arch-arm: Automatically enable stat probe listeners
  - 2025-06-17 `1a849a5beea4` arch-arm: Automatically enable stat probe listeners
- 复现: `git show 1a849a5beea48a684132b90828b314f633d9716f`

### #2082 tests: add tests for restoring from checkpoints using multisim

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2082
- 代表 commit: `4b61b826a678` (2025-06-17)
- 变更规模: commits=2, files=3, +466/-0 (churn=466)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/multisim/configs/hello-restore-checkpoint.py` (churn=162)
  - `tests/gem5/multisim/test-multisim.py` (churn=160)
  - `tests/gem5/multisim/configs/riscv-hello-restore-checkpoints.py` (churn=144)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-17 `ba21b2fe6eb5` tests: add tests for restoring from checkpoints using multisim
  - 2025-06-17 `4b61b826a678` tests: add tests for restoring from checkpoints using multisim
- 复现: `git show 4b61b826a678c3b72a99972eac8994d83b52cd96`

### #2362 arch-riscv: Remove unneeded flushAll

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2362
- 代表 commit: `098bf6a33a21` (2025-06-17)
- 变更规模: commits=2, files=3, +0/-96 (churn=96)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.cc` (churn=82)
  - `src/arch/riscv/faults.cc` (churn=10)
  - `src/arch/riscv/isa/decoder.isa` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-11 `408e6ebfb325` arch-riscv: Remove unneeded flushAll
  - 2025-06-17 `098bf6a33a21` arch-riscv: Remove unneeded flushAll
- 复现: `git show 098bf6a33a21ac60d581393c361828898f502f3d`

### #2341 arch-riscv: Remove N extension

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2341
- 代表 commit: `6856aee02cf9` (2025-06-17)
- 变更规模: commits=2, files=11, +60/-1044 (churn=1104)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/regs/misc.hh` (churn=602)
  - `src/arch/riscv/remote_gdb.cc` (churn=164)
  - `src/arch/riscv/interrupts.cc` (churn=112)
  - `src/arch/riscv/isa.cc` (churn=60)
  - `src/arch/riscv/faults.cc` (churn=50)
  - `src/arch/riscv/remote_gdb.hh` (churn=40)
  - `src/arch/riscv/isa/decoder.isa` (churn=26)
  - `src/arch/riscv/gdb-xml/riscv-32bit-csr.xml` (churn=20)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-11 `6f9fda33a1a4` arch-riscv: Remove N extension
  - 2025-06-17 `6856aee02cf9` arch-riscv: Remove N extension
- 复现: `git show 6856aee02cf9e277815e0c7acc8a9f7087fa532f`

### #2322 misc: Release v25.0.0.0

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2322
- 代表 commit: `d22064c1c05f` (2025-06-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d22064c1c05f5857431219da888de7e29a268c16`

## v25.0.0.1 (2025-08-11)

- PR 数：9

### #2415 stdlib: remove duplicate ClassicGeneratorExitHandler

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2415
- 代表 commit: `4e2718f50d93` (2025-07-30)
- 变更规模: commits=2, files=1, +0/-126 (churn=126)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/exit_handler.py` (churn=126)
- commits 列表（按 topo-order，Top 12）：
  - 2025-07-01 `f93c153f2c2d` stdlib: remove duplicate ClassicGeneratorExitHandler
  - 2025-07-30 `4e2718f50d93` stdlib: remove duplicate ClassicGeneratorExitHandler
- 复现: `git show 4e2718f50d931294b7cdf787828bc90b6a74b00b`

### #2397 util: bump urllib3 to 2.5.0 in util/gem5-resources-manager

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2397
- 代表 commit: `a5ffb90272b2` (2025-07-30)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-06-24 `3373a5a01256` util: bump urllib3 to 2.5.0 in util/gem5-resources-manager
  - 2025-07-30 `a5ffb90272b2` util: bump urllib3 to 2.5.0 in util/gem5-resources-manager
- 复现: `git show a5ffb90272b27053210f1e75bd45b67dcea002d7`

### #2399 cpu: Fix looppoint analysis v25

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2399
- 代表 commit: `35de1ef5d3dc` (2025-07-30)
- 变更规模: commits=2, files=2, +10/-8 (churn=18)
- 影响范围: topdirs=src; subsys=cpu/simple; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/simple/probes/looppoint_analysis.hh` (churn=14)
  - `src/cpu/simple/probes/looppoint_analysis.cc` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-07-16 `831c4990b7da` cpu: Fix looppoint analysis v25
  - 2025-07-30 `35de1ef5d3dc` cpu: Fix looppoint analysis v25
- 复现: `git show 35de1ef5d3dc736f5da77e8457a1ea1b9db83bd2`

### #2492 arch-arm: fix writeback type for AArch32 FP16 instructions

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2492
- 代表 commit: `3a6f3c16b696` (2025-07-30)
- 变更规模: commits=2, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/insts/fp.isa` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-07-30 `357519f981fe` arch-arm: fix writeback type for AArch32 FP16 instructions
  - 2025-07-30 `3a6f3c16b696` arch-arm: fix writeback type for AArch32 FP16 instructions
- 复现: `git show 3a6f3c16b696c3a84059604049ce767d515c757c`

### #2464 arch-riscv: populate logBytes/paddr after functional pt walk

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2464
- 代表 commit: `415cbec52985` (2025-07-30)
- 变更规模: commits=2, files=1, +10/-2 (churn=12)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/pagetable_walker.cc` (churn=12)
- commits 列表（按 topo-order，Top 12）：
  - 2025-07-24 `538a022b9714` arch-riscv: populate logBytes/paddr after functional pt walk
  - 2025-07-30 `415cbec52985` arch-riscv: populate logBytes/paddr after functional pt walk
- 复现: `git show 415cbec5298535dc1402cac360ff3812c436e308`

### #2441 arch-arm: Add FEAT_FP16 FP instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2441
- 代表 commit: `0dd766800279` (2025-07-30)
- 变更规模: commits=2, files=2, +580/-114 (churn=694)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/insts/fp64.isa` (churn=388)
  - `src/arch/arm/isa/formats/aarch64.isa` (churn=306)
- commits 列表（按 topo-order，Top 12）：
  - 2025-07-14 `82af83a1f4a5` arch-arm: Add FEAT_FP16 FP instructions
  - 2025-07-30 `0dd766800279` arch-arm: Add FEAT_FP16 FP instructions
- 复现: `git show 0dd76680027988cd530988cef834e0055af81b8d`

### #2502 cpu: Adapt simpoint listener to new probe structure

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2502
- 代表 commit: `e82a08d080db` (2025-08-06)
- 变更规模: commits=2, files=2, +8/-8 (churn=16)
- 影响范围: topdirs=src; subsys=cpu/simple; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/simple/probes/simpoint.cc` (churn=12)
  - `src/cpu/simple/probes/simpoint.hh` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-08-04 `4f7550bc54d7` cpu: Adapt simpoint listener to new probe structure
  - 2025-08-06 `e82a08d080db` cpu: Adapt simpoint listener to new probe structure
- 复现: `git show e82a08d080db2d1fc7fb09d8b56fd9804c75c3ec`

### #2512 cpu-o3: properly index time buffer when clearing states

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2512
- 代表 commit: `d03208b6aa21` (2025-08-06)
- 变更规模: commits=2, files=1, +8/-6 (churn=14)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/commit.cc` (churn=14)
- commits 列表（按 topo-order，Top 12）：
  - 2025-08-06 `6bdff4fe6b64` cpu-o3: properly index time buffer when clearing states
  - 2025-08-06 `d03208b6aa21` cpu-o3: properly index time buffer when clearing states
- 复现: `git show d03208b6aa212c93d6de648b5cacd96e37ec49b1`

### #2496 misc: Hotfix 25.0.0.1

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2496
- 代表 commit: `ddd4ae35adb0` (2025-08-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ddd4ae35adb0a3df1f1ba11e9a973a5c2f8c2944`

## v25.1.0.0 (2025-12-31)

- PR 数：274

### #2316 arch-arm: Add L2D_TLB_REFILL and L2I_TLB_REFILL in PMU

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2316
- 代表 commit: `046c684b4f44` (2025-05-29)
- 变更规模: commits=1, files=4, +25/-9 (churn=34)
- 影响范围: topdirs=src, configs; subsys=arch, configs; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/tlb.cc` (churn=15)
  - `src/arch/arm/ArmPMU.py` (churn=13)
  - `src/arch/arm/tlb.hh` (churn=5)
  - `configs/example/arm/devices.py` (churn=1)
- 复现: `git show 046c684b4f444c48a1b7d6210cbde872041d1740`

### #2328 Bug fix for configs/ruby/Ruby.py

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2328
- 代表 commit: `9495e0e381f2` (2025-05-31)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 9495e0e381f2a03402b93dec4a2b26719cc5a760`

### #1712 arch-riscv: Fix incorrect vector slide instructions and statically filter redundant uops

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/1712
- 代表 commit: `635ac5d0da02` (2025-05-31)
- 变更规模: commits=1, files=5, +755/-323 (churn=1078)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=719)
  - `src/arch/riscv/isa/templates/vector_arith.isa` (churn=228)
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=89)
  - `src/arch/riscv/insts/vector.cc` (churn=25)
  - `src/arch/riscv/insts/vector.hh` (churn=17)
- 复现: `git show 635ac5d0da0247af5a56ece43adfba3065ebbd0a`

### #2317 arch-arm: Add read/write function to FPCR/FPSR.

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2317
- 代表 commit: `a535ff3cad38` (2025-06-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a535ff3cad38ea26918416d8df4468a972c061a0`

### #2335 misc tests: Set specific versions in GH Action workflows

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2335
- 代表 commit: `61de99c435a6` (2025-06-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 61de99c435a671bf93df8b28e8587b3c0a1991f2`

### #2337 arch-riscv: Add RVV support for riscv32 ISA

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2337
- 代表 commit: `178735413b34` (2025-06-03)
- 变更规模: commits=1, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.cc` (churn=8)
- 复现: `git show 178735413b343387425267f3414d8898a245824e`

### #2346 Add missing operands to disassembly for common arm instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2346
- 代表 commit: `ff18a3dae9e7` (2025-06-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ff18a3dae9e7bd5af9bfe688098ee067333dfa78`

### #2356 tests: Update fs tests to use the 24.04 disk images

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2356
- 代表 commit: `efdfe1ab5f12` (2025-06-06)
- 变更规模: commits=1, files=4, +37/-48 (churn=85)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/kvm_switch_tests/configs/boot_kvm_switch_exit.py` (churn=43)
  - `tests/gem5/x86_boot_tests/configs/x86_boot_exit_run.py` (churn=20)
  - `tests/gem5/arm_boot_tests/configs/arm_boot_exit_run.py` (churn=18)
  - `tests/gem5/riscv_boot_tests/configs/riscv_boot_exit_run.py` (churn=4)
- 复现: `git show efdfe1ab5f12db432256721ed78a030be1461a99`

### #2350 arch-vega: Included modifiers support in vop3_cmp instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2350
- 代表 commit: `00aae5865693` (2025-06-09)
- 变更规模: commits=1, files=1, +308/-0 (churn=308)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/vop3_cmp.cc` (churn=308)
- 复现: `git show 00aae58656938fa1f4aaf0803740f9920181cbf9`

### #2357 mem: Skip redundant test in AddrRange::isSubset in 1-byte ranges.

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2357
- 代表 commit: `825cae5691c5` (2025-06-09)
- 变更规模: commits=1, files=1, +11/-2 (churn=13)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/addr_range.hh` (churn=13)
- 复现: `git show 825cae5691c5da2401a8b560bc73682901bb5565`

### #2368 stdlib: Append resource version to cached resource filename

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2368
- 代表 commit: `3091cee1be93` (2025-06-16)
- 变更规模: commits=1, files=5, +33/-9 (churn=42)
- 影响范围: topdirs=tests, configs, src; subsys=tests, configs, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/gpu/test_gpu_apu_se.py` (churn=20)
  - `configs/example/apu_se.py` (churn=8)
  - `tests/gem5/se_mode/rvv_intrinsic_tests/test.py` (churn=5)
  - `tests/pyunit/stdlib/resources/pyunit_resource_specialization.py` (churn=5)
  - `src/python/gem5/resources/resource.py` (churn=4)
- 复现: `git show 3091cee1be931f51f7430b2242f7225e62db9ba2`

### #2380 arch-arm,configs: Add option to populate PMU statCounters in baremetal.py

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2380
- 代表 commit: `cb2c8ffa9d43` (2025-06-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show cb2c8ffa9d4321cdb1891d1835f2bed9658cb173`

### #2378 arch-arm: Enable new FP extensions in SE mode

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2378
- 代表 commit: `e2438a6eab32` (2025-06-17)
- 变更规模: commits=1, files=2, +6/-2 (churn=8)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/ArmISA.py` (churn=5)
  - `src/arch/arm/process.cc` (churn=3)
- 复现: `git show e2438a6eab3213017d8daeec3a0b33cb9d1de79c`

### #2376 arch-riscv: Fix PMP checking

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2376
- 代表 commit: `5286276b9ed3` (2025-06-17)
- 变更规模: commits=1, files=2, +33/-41 (churn=74)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/pmp.cc` (churn=49)
  - `src/arch/riscv/pmp.hh` (churn=25)
- 复现: `git show 5286276b9ed388b2a9bf6b64e054f1a9a7b69376`

### #2379 arch-arm: Generate disable PMU ExitEvent when local disable

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2379
- 代表 commit: `cbef540d2cf9` (2025-06-18)
- 变更规模: commits=1, files=2, +39/-9 (churn=48)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/pmu.cc` (churn=35)
  - `src/arch/arm/pmu.hh` (churn=13)
- 复现: `git show cbef540d2cf99f7bc10e9c731f5bc2db08e1da0c`

### #2386 arch-riscv: Fix mstatus.mprv check

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2386
- 代表 commit: `f86f0102774d` (2025-06-23)
- 变更规模: commits=1, files=1, +5/-1 (churn=6)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/tlb.cc` (churn=6)
- 复现: `git show f86f0102774d91a818d6714fffe67e495a37f7eb`

### #2312 cpu-o3: stall fetch from commit when trap event is pending

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2312
- 代表 commit: `a30d641f484b` (2025-06-25)
- 变更规模: commits=1, files=3, +15/-0 (churn=15)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/fetch.cc` (churn=11)
  - `src/cpu/o3/comm.hh` (churn=2)
  - `src/cpu/o3/commit.cc` (churn=2)
- 复现: `git show a30d641f484b4915c6b34a57e34c25deac148d9a`

### #2396 configs: Fix bootloader argument parsing in legacy RISC-V script

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2396
- 代表 commit: `3cfe63181c61` (2025-06-25)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/riscv/fs_linux.py` (churn=3)
- 复现: `git show 3cfe63181c61704617a622d49ed5b13949e599d0`

### #2400 arch-riscv: Fix bootloader & kernel load address

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2400
- 代表 commit: `6656a34ac8b1` (2025-06-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6656a34ac8b11fdf349a250965179c6274ef7a9f`

### #2412 mem-cache: Fix FIFO RP invalidation

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2412
- 代表 commit: `477508aee400` (2025-06-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 477508aee400c603c8881b7a73c590552d39f8d2`

### #2394 arch-vega: Implement CDNA4 (MI350) instructions, part 1

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2394
- 代表 commit: `332756a06478` (2025-07-01)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 332756a064780328777cc196f18d73255293243f`

### #2419 misc: bump mypy from 1.16.0 to 1.16.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2419
- 代表 commit: `66c4b574c34c` (2025-07-02)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 66c4b574c34ce19a6e49aa2ef4c7677a69dd6bc8`

### #2417 arch-riscv: Make PLIC output latency configurable

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2417
- 代表 commit: `120625cb9b43` (2025-07-02)
- 变更规模: commits=1, files=3, +11/-1 (churn=12)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/riscv/plic.hh` (churn=5)
  - `src/dev/riscv/Plic.py` (churn=4)
  - `src/dev/riscv/plic.cc` (churn=3)
- 复现: `git show 120625cb9b439658930b349ca0ec29bd0c901e0c`

### #2401 configs: deprecate configs/example/riscv/fs_linux.py

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2401
- 代表 commit: `41e27089b394` (2025-07-03)
- 变更规模: commits=1, files=1, +0/-0 (churn=0)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/{ => deprecated}/example/riscv/fs_linux.py` (churn=0)
- 复现: `git show 41e27089b39409588d0ae76a971c78846218c360`

### #2418 arch-vega: MI350 microscaling updates

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2418
- 代表 commit: `f4000aa15cb5` (2025-07-06)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f4000aa15cb542aecaaf25d00838604df6b23294`

### #2308 cpu: Add user-mode stats

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2308
- 代表 commit: `9a4feaaa4868` (2025-07-07)
- 变更规模: commits=1, files=8, +75/-10 (churn=85)
- 影响范围: topdirs=src; subsys=cpu, cpu/o3, cpu/minor, cpu/simple; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/minor/execute.cc` (churn=18)
  - `src/cpu/simple/base.cc` (churn=13)
  - `src/cpu/base.cc` (churn=12)
  - `src/cpu/kvm/base.cc` (churn=10)
  - `src/cpu/o3/commit.cc` (churn=10)
  - `src/cpu/base.hh` (churn=8)
  - `src/cpu/o3/cpu.hh` (churn=8)
  - `src/cpu/o3/cpu.cc` (churn=6)
- 复现: `git show 9a4feaaa4868bf38646fa98f9e911ec8a333430b`

### #362 misc: Add git-clang-format to pre-commit with wrapper script

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/362
- 代表 commit: `6e287f3da539` (2025-07-08)
- 变更规模: commits=1, files=3, +467/-1 (churn=468)
- 影响范围: topdirs=.clang-format, .pre-commit-config.yaml, util; subsys=.clang-format, .pre-commit-config.yaml, util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/run-git-clang-format.py` (churn=365)
  - `.clang-format` (churn=93)
  - `.pre-commit-config.yaml` (churn=10)
- 复现: `git show 6e287f3da5398f37fec7bad139c0241d5d446440`

### #2420 dev-amdgpu: Update GPU SDMA model to support non-zero VMID packets, MMIOs, and fix SDMA poll livelock

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2420
- 代表 commit: `954dc0effc28` (2025-07-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 954dc0effc28d5cec7fec006e8536577e79e7a70`

### #2422 arch-vega: Fix incorrect address translation caused by tlb

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2422
- 代表 commit: `6f1b62349c06` (2025-07-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6f1b62349c06f340f03191620bf3a118399f0cbb`

### #2432 cpu-o3: Specify unit of measurement for LSQ loadToUse stat

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2432
- 代表 commit: `d7ae428ed509` (2025-07-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d7ae428ed509283edd389b2f0305cd8d7911c29d`

### #2421 misc: fix include paths

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2421
- 代表 commit: `9080de78b022` (2025-07-10)
- 变更规模: commits=1, files=3, +4/-2 (churn=6)
- 影响范围: topdirs=src, .gitignore; subsys=.gitignore, arch, mem/cache; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `.gitignore` (churn=2)
  - `src/arch/riscv/utility.hh` (churn=2)
  - `src/mem/cache/tags/partitioning_policies/way_pp.cc` (churn=2)
- 复现: `git show 9080de78b022bf0a6a441a0beb4435cc60eb483f`

### #2425 configs: Allow GPU-FS to use HBMCtrl memory controller

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2425
- 代表 commit: `654fa090c17c` (2025-07-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 654fa090c17c82eeaefff7435929528e858afcf6`

### #2427 arch-vega: Implement all MI355X conversion instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2427
- 代表 commit: `dbad721361ae` (2025-07-10)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show dbad721361ae31287d3f33e326d87b564c6a83f5`

### #2314 Introduce git-clang-format to gem5 with wrapper script (Option 2 of 2)

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2314
- 代表 commit: `31f0800acfe8` (2025-07-11)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 31f0800acfe8d22383f4d50b5458452582fc060c`

### #2409 scons: Update build_tools to enable importing

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2409
- 代表 commit: `da3597cf5dbc` (2025-07-11)
- 变更规模: commits=1, files=8, +980/-777 (churn=1757)
- 影响范围: topdirs=build_tools; subsys=build_tools; arch=-
- 主要改动文件（Top 8 by churn）:
  - `build_tools/cxx_config_cc.py` (churn=459)
  - `build_tools/sim_object_param_struct_hh.py` (churn=376)
  - `build_tools/sim_object_param_struct_cc.py` (churn=316)
  - `build_tools/enum_cc.py` (churn=181)
  - `build_tools/cxx_config_hh.py` (churn=131)
  - `build_tools/debugflaghh.py` (churn=131)
  - `build_tools/enum_hh.py` (churn=129)
  - `build_tools/debugflagcc.py` (churn=34)
- 复现: `git show da3597cf5dbca00e90d3ec1418793c3f3ee94207`

### #2436 arch-vega: GPU TLB Multi Page Size Support

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2436
- 代表 commit: `8d3f84e3b488` (2025-07-11)
- 变更规模: commits=1, files=2, +37/-18 (churn=55)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/tlb.cc` (churn=46)
  - `src/arch/amdgpu/vega/tlb.hh` (churn=9)
- 复现: `git show 8d3f84e3b48861968bfa46f676526c204a5e1bfb`

### #2408 misc: Add gitignore for ext/sst

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2408
- 代表 commit: `bab3ceef48ad` (2025-07-11)
- 变更规模: commits=1, files=1, +6/-0 (churn=6)
- 影响范围: topdirs=ext; subsys=ext; arch=-
- 主要改动文件（Top 8 by churn）:
  - `ext/sst/.gitignore` (churn=6)
- 复现: `git show bab3ceef48ad82b774c2efc995cdd9499e1fc502`

### #2435 arch-vega: Add Large Pagesize Support to GPU TLB Coalescer

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2435
- 代表 commit: `56aca813700a` (2025-07-12)
- 变更规模: commits=1, files=3, +136/-23 (churn=159)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/tlb_coalescer.cc` (churn=145)
  - `src/arch/amdgpu/vega/tlb_coalescer.hh` (churn=13)
  - `src/arch/amdgpu/vega/VegaGPUTLB.py` (churn=1)
- 复现: `git show 56aca813700a6b107f2282f15af5e4d2cad0d0e2`

### #2444 cpu-o3: Print OpClass name instead of number for debug purposes

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2444
- 代表 commit: `8625ae636559` (2025-07-14)
- 变更规模: commits=1, files=1, +6/-6 (churn=12)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/commit.cc` (churn=12)
- 复现: `git show 8625ae6365598b4a10cf177b2bfc01353465b7fd`

### #2445 stdlib: Fix orchestrator exit handler workload id

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2445
- 代表 commit: `888f06dd2868` (2025-07-15)
- 变更规模: commits=1, files=1, +5/-1 (churn=6)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/exit_handler.py` (churn=6)
- 复现: `git show 888f06dd286866acd7c68489399cf2bdae2ef853`

### #2431 mem-cache: Report Blocked_NoWBBuffers as cache blocking cause

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2431
- 代表 commit: `b9d490612524` (2025-07-16)
- 变更规模: commits=1, files=1, +4/-1 (churn=5)
- 影响范围: topdirs=src; subsys=mem/cache; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/base.cc` (churn=5)
- 复现: `git show b9d4906125249ad9dbc7c4c929c3f4dfb2d348a5`

### #2416 dev: Export reg bank's offsetMap

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2416
- 代表 commit: `87a9aea67215` (2025-07-16)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/reg_bank.hh` (churn=2)
- 复现: `git show 87a9aea67215921ad69ff365816bc1cf993481ff`

### #2451 arch-vega: Fix compile errors on clang 17

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2451
- 代表 commit: `afe83fb616a7` (2025-07-16)
- 变更规模: commits=1, files=2, +42/-43 (churn=85)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/vop3_cvt.hh` (churn=77)
  - `src/arch/amdgpu/common/dtype/mxfp_convert.hh` (churn=8)
- 复现: `git show afe83fb616a76eb206b174121f16253b50ca4f9b`

### #2447 systemc: Fix tlm::tlm_dmi usage

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2447
- 代表 commit: `ee39a57b1766` (2025-07-17)
- 变更规模: commits=1, files=1, +4/-1 (churn=5)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/tlm_bridge/gem5_to_tlm.cc` (churn=5)
- 复现: `git show ee39a57b176603d82554efa046515577683d075d`

### #2393 arch-arm: Add FEAT_AFP and enable in AdvSimd instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2393
- 代表 commit: `7693819af6aa` (2025-07-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7693819af6aa9179d2397336358e64d88ad1655a`

### #2452 configs: deprecate configs/example/gem5_library/riscv-fs.py

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2452
- 代表 commit: `b2eeb9c68ad2` (2025-07-23)
- 变更规模: commits=1, files=1, +0/-0 (churn=0)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/{ => deprecated}/example/gem5_library/riscv-fs.py` (churn=0)
- 复现: `git show b2eeb9c68ad28e2d7c568fffcfb9d89fd9db8536`

### #2468 misc: Modify clang-format to no break in method declarations

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2468
- 代表 commit: `a06c50a27dcc` (2025-07-24)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=.clang-format; subsys=.clang-format; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.clang-format` (churn=3)
- 复现: `git show a06c50a27dcc31d10ac36e5ca8ff80ab0e45859b`

### #2459 stdlib: add Simulator API to schedule hypercall 6 exits

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2459
- 代表 commit: `3388cd42866a` (2025-07-24)
- 变更规模: commits=1, files=1, +53/-0 (churn=53)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/simulator.py` (churn=53)
- 复现: `git show 3388cd42866a9f3cb6557cd697e088664713f68b`

### #2467 misc: define default hypercall ids in new header

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2467
- 代表 commit: `d7d0a73e7041` (2025-07-24)
- 变更规模: commits=1, files=1, +42/-0 (churn=42)
- 影响范围: topdirs=include; subsys=include; arch=-
- 主要改动文件（Top 8 by churn）:
  - `include/gem5/hypercall_ids.h` (churn=42)
- 复现: `git show d7d0a73e70412d93724d7bdc3376c4a1753c8427`

### #2469 sim: Add map to sim_exit.hh includes

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2469
- 代表 commit: `9bc48c873b2c` (2025-07-24)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/sim_exit.hh` (churn=1)
- 复现: `git show 9bc48c873b2c63507e10031081a111bd3975bbce`

### #2472 stdlib: return ticks to seconds conversion in exit handler

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2472
- 代表 commit: `a83f643dbbd0` (2025-07-24)
- 变更规模: commits=1, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/simulate/exit_handler.py` (churn=3)
- 复现: `git show a83f643dbbd0db7fa8fa026ee67dde80cfa7edd3`

### #2407 python: Remove m5 internal

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2407
- 代表 commit: `5a24d3733a49` (2025-07-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5a24d3733a49d9a5e027c14b7d328128e01d96f7`

### #2478 arch-vega: Improve MFMA precision to match MI300X hardware

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2478
- 代表 commit: `f8d05ce80c15` (2025-07-25)
- 变更规模: commits=1, files=1, +3/-2 (churn=5)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/instructions.hh` (churn=5)
- 复现: `git show f8d05ce80c15b3d366fef9410670d6a32f20519b`

### #2228 misc: Add useful workspace settings for GitHub and PR extensions

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/2228
- 代表 commit: `250bba75f90f` (2025-07-28)
- 变更规模: commits=1, files=1, +37/-0 (churn=37)
- 影响范围: topdirs=.vscode; subsys=.vscode; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.vscode/settings.json` (churn=37)
- 复现: `git show 250bba75f90f7947c35eec6be4dc81c9dda3573e`

### #2485 arch-arm: Fix inverted RAO check of HCPTR.TCP10

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2485
- 代表 commit: `5dc014d57371` (2025-07-29)
- 变更规模: commits=1, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/insts/static_inst.cc` (churn=8)
- 复现: `git show 5dc014d573711379ec4cbfdf38108f472ec6155d`

### #2483 arch-arm: Remove unnecessary operand type CntrlRegNC

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2483
- 代表 commit: `838342a7bc82` (2025-07-29)
- 变更规模: commits=1, files=1, +14/-17 (churn=31)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/operands.isa` (churn=31)
- 复现: `git show 838342a7bc82a49f92e9ba323b183495819e2ee8`

### #2465 arch-arm: Split decodeBranchExcSys into multiple sub-functions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2465
- 代表 commit: `355e3a88cece` (2025-07-29)
- 变更规模: commits=1, files=1, +421/-353 (churn=774)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/formats/aarch64.isa` (churn=774)
- 复现: `git show 355e3a88cece87f99999fde0a5144e6bb1e1013d`

### #2489 misc: Use Right PointerAlignment in clang-format

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2489
- 代表 commit: `f6dfce5b9dfb` (2025-07-29)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.clang-format; subsys=.clang-format; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.clang-format` (churn=2)
- 复现: `git show f6dfce5b9dfb7c0f3cda15dd1b542c295cf74ec2`

### #2487 systemc: Improve SystemC stability

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2487
- 代表 commit: `f0aad778249f` (2025-07-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f0aad778249f24bc1392ceb17995a2999e69dd84`

### #2474 arch-riscv: fix bug where next PTW never starts

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2474
- 代表 commit: `121eb2bbc497` (2025-07-30)
- 变更规模: commits=1, files=1, +10/-2 (churn=12)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/pagetable_walker.cc` (churn=12)
- 复现: `git show 121eb2bbc497a680b97cbc26be4bcc1f43db240e`

### #2477 cpu: move conditional pred out of bpred_unit

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/2477
- 代表 commit: `23f4be207519` (2025-07-31)
- 变更规模: commits=1, files=22, +347/-141 (churn=488)
- 影响范围: topdirs=src, configs; subsys=cpu/pred, configs, cpu/minor, cpu/o3, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/conditional.hh` (churn=149)
  - `src/cpu/pred/bpred_unit.hh` (churn=97)
  - `src/cpu/pred/conditional.cc` (churn=65)
  - `src/cpu/pred/bpred_unit.cc` (churn=34)
  - `src/cpu/pred/BranchPredictor.py` (churn=30)
  - `configs/common/cores/arm/HPI.py` (churn=18)
  - `src/python/gem5/prebuilt/riscvmatched/riscvmatched_core.py` (churn=18)
  - `configs/common/cores/arm/O3_ARM_v7a.py` (churn=12)
- 复现: `git show 23f4be207519b1b847414fd01b3d85d5cb45419f`

### #1786 tests: Add a unit test for bloom filters

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1786
- 代表 commit: `a56499a75d1e` (2025-07-31)
- 变更规模: commits=1, files=2, +396/-0 (churn=396)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/filters/base.test.cc` (churn=394)
  - `src/base/filters/SConscript` (churn=2)
- 复现: `git show a56499a75d1e2d9b1e586b44dec0779f04c7bd82`

### #2497 arch-riscv: Fix CSR writing mask

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2497
- 代表 commit: `1fc38c9b2c4c` (2025-07-31)
- 变更规模: commits=1, files=2, +61/-45 (churn=106)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/regs/misc.hh` (churn=97)
  - `src/arch/riscv/isa.cc` (churn=9)
- 复现: `git show 1fc38c9b2c4caee533bc2fb0620da19789d7c0d7`

### #2498 systemc: Fix a bug when length is 0 in get_direct_mem_ptr

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2498
- 代表 commit: `a8cbbb52b5a8` (2025-07-31)
- 变更规模: commits=1, files=1, +9/-0 (churn=9)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/tlm_bridge/tlm_to_gem5.cc` (churn=9)
- 复现: `git show a8cbbb52b5a8d9bc9c127189dbfe546f080f17b9`

### #2499 cpu: Workaround missing IsUnconditional flag

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2499
- 代表 commit: `c0dc383ff7d5` (2025-08-01)
- 变更规模: commits=1, files=2, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=cpu/pred; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/bpred_unit.cc` (churn=2)
  - `src/cpu/pred/bpred_unit.hh` (churn=2)
- 复现: `git show c0dc383ff7d506bde9c3a9aaa9fd82ac02736781`

### #2442 tests: fix kernel panic on kvm switch tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2442
- 代表 commit: `3a5cea007731` (2025-08-01)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/kvm_switch_tests/configs/boot_kvm_switch_exit.py` (churn=4)
- 复现: `git show 3a5cea0077312859e83f2d5a33211dfd50240008`

### #2450 tests: Add tests for running scripts via readfile

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2450
- 代表 commit: `618fbfbcff71` (2025-08-01)
- 变更规模: commits=1, files=4, +201/-0 (churn=201)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/readfile_tests/configs/ubuntu-run-with-readfile.py` (churn=116)
  - `tests/gem5/readfile_tests/test-readfile.py` (churn=72)
  - `tests/gem5/readfile_tests/README.md` (churn=11)
  - `tests/gem5/readfile_tests/configs/test_script.sh` (churn=2)
- 复现: `git show 618fbfbcff71fca18d5db06767c09c59b6a17d0d`

### #2510 cpu-o3: Bundle some RenameStats into a vector

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2510
- 代表 commit: `0525337c970f` (2025-08-05)
- 变更规模: commits=1, files=2, +45/-41 (churn=86)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/rename.cc` (churn=63)
  - `src/cpu/o3/rename.hh` (churn=23)
- 复现: `git show 0525337c970f2c0cc2e0b1d6a2e4d12a475da868`

### #2503 misc: bump mypy from 1.16.1 to 1.17.1

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2503
- 代表 commit: `44e0b18fb411` (2025-08-05)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 44e0b18fb411c476fded84b03c5a29b764b6cc74`

### #2507 mem: refactor Bridge to allow derived class

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/2507
- 代表 commit: `6bd546c40936` (2025-08-06)
- 变更规模: commits=1, files=4, +118/-59 (churn=177)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/bridge.cc` (churn=88)
  - `src/mem/bridge.hh` (churn=73)
  - `src/mem/Bridge.py` (churn=14)
  - `src/mem/SConscript` (churn=2)
- 复现: `git show 6bd546c40936e0df67d189f2da94e7ada6fa58dc`

### #2506 arch-riscv: Fix interrupt delegation 2nd attempt

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2506
- 代表 commit: `a3159398311f` (2025-08-06)
- 变更规模: commits=1, files=1, +13/-77 (churn=90)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/interrupts.cc` (churn=90)
- 复现: `git show a3159398311fb0fb2a65a11cca666a05cc279ccc`

### #2517 cpu-o3: Bundle some DecodeStats into a vector

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2517
- 代表 commit: `be8708d4d1a4` (2025-08-07)
- 变更规模: commits=1, files=2, +41/-34 (churn=75)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/decode.cc` (churn=52)
  - `src/cpu/o3/decode.hh` (churn=23)
- 复现: `git show be8708d4d1a49f8a66e2e3b0bff841938055659b`

### #2443 misc: Update ci-tests with clang format

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2443
- 代表 commit: `3ae55aab557c` (2025-08-07)
- 变更规模: commits=1, files=1, +33/-0 (churn=33)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/ci-tests.yaml` (churn=33)
- 复现: `git show 3ae55aab557cd188feef141a98ee6f7bf1e6c20b`

### #2494 misc: split single line if statements and add braces in .clang-format

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2494
- 代表 commit: `17c2dc8c071b` (2025-08-07)
- 变更规模: commits=1, files=1, +6/-1 (churn=7)
- 影响范围: topdirs=.clang-format; subsys=.clang-format; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.clang-format` (churn=7)
- 复现: `git show 17c2dc8c071b80f959e3a9f7b5aefd5ad35929df`

### #2481 python: Prevent SimObject from sharing reference of class member.

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2481
- 代表 commit: `e6b0a0223c32` (2025-08-08)
- 变更规模: commits=1, files=1, +3/-0 (churn=3)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/SimObject.py` (churn=3)
- 复现: `git show e6b0a0223c32e859c30e7103c92a556e44db586f`

### #2519 misc: improvement to clang-format

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2519
- 代表 commit: `86cf5fb969ef` (2025-08-11)
- 变更规模: commits=1, files=1, +10/-1 (churn=11)
- 影响范围: topdirs=.clang-format; subsys=.clang-format; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.clang-format` (churn=11)
- 复现: `git show 86cf5fb969eff5bfad1668c71b69361ba362e460`

### #2515 cpu: BTB update at squash/commit option

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2515
- 代表 commit: `b4b111599abc` (2025-08-11)
- 变更规模: commits=1, files=3, +59/-40 (churn=99)
- 影响范围: topdirs=src; subsys=cpu/pred; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/bpred_unit.cc` (churn=79)
  - `src/cpu/pred/bpred_unit.hh` (churn=13)
  - `src/cpu/pred/BranchPredictor.py` (churn=7)
- 复现: `git show b4b111599abc555fa7a82625b8329b2407362188`

### #2516 cpu: Tage GHR out-of-bounds at rollover

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2516
- 代表 commit: `5829abd58a2e` (2025-08-11)
- 变更规模: commits=1, files=1, +18/-6 (churn=24)
- 影响范围: topdirs=src; subsys=cpu/pred; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/tage_base.cc` (churn=24)
- 复现: `git show 5829abd58a2ea916a1a338269e35e22b3c5312d2`

### #2311 mem-ruby: return early response for software prefetches

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2311
- 代表 commit: `d446055f5e1d` (2025-08-11)
- 变更规模: commits=1, files=2, +40/-0 (churn=40)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/RubyPort.cc` (churn=24)
  - `src/mem/ruby/system/Sequencer.cc` (churn=16)
- 复现: `git show d446055f5e1d3fb4345c87a86ccd98b9776a9dbb`

### #2520 cpu-o3: Mark some DynInst methods as const

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2520
- 代表 commit: `14992bd5ba6e` (2025-08-11)
- 变更规模: commits=1, files=1, +6/-2 (churn=8)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/dyn_inst.hh` (churn=8)
- 复现: `git show 14992bd5ba6e24bf855dbd8ee96a4913ac86f868`

### #2521 misc: [pre-commit.ci] pre-commit autoupdate

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/2521
- 代表 commit: `6f4ba3f38dab` (2025-08-12)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.pre-commit-config.yaml; subsys=.pre-commit-config.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.pre-commit-config.yaml` (churn=2)
- 复现: `git show 6f4ba3f38dab84b13411ffc02d6f27705ac86ae8`

### #2522 sim: Remove unneeded headers from debug.cc

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2522
- 代表 commit: `d597d1327516` (2025-08-12)
- 变更规模: commits=1, files=1, +0/-3 (churn=3)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/debug.cc` (churn=3)
- 复现: `git show d597d1327516ca11f092cb8b9394dcbd9f535de7`

### #2523 base, cpu: Add missing virtuals

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2523
- 代表 commit: `5def0734d6e8` (2025-08-12)
- 变更规模: commits=1, files=2, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=base, cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/loader/object_file.hh` (churn=1)
  - `src/cpu/pc_event.hh` (churn=1)
- 复现: `git show 5def0734d6e84a11b8455ab657e744cf86908007`

### #2404 arch,mem-ruby: Make parsers less verbose by default

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2404
- 代表 commit: `76d39bb7271e` (2025-08-12)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 76d39bb7271e32cb4ab34f5b62ecb96070130ea0`

### #2490 arch-arm, cpu, config: Implement Bfloat16 for Arm CPUs

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2490
- 代表 commit: `6d08fa288336` (2025-08-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 6d08fa28833630df6073bd3b2cc7937e0e7c0e2e`

### #2500 cpu-o3: remove inactive tail bytes from memory request

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2500
- 代表 commit: `f4cea64ba4c4` (2025-08-13)
- 变更规模: commits=1, files=2, +30/-3 (churn=33)
- 影响范围: topdirs=src; subsys=cpu/o3, cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/utils.hh` (churn=21)
  - `src/cpu/o3/lsq.cc` (churn=12)
- 复现: `git show f4cea64ba4c44579240f413b58fb248aadc9f2b1`

### #2484 misc: Update stats dump to have a optional message

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2484
- 代表 commit: `25829029e3c7` (2025-08-13)
- 变更规模: commits=2, files=6, +26/-13 (churn=39)
- 影响范围: topdirs=src; subsys=base, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/stats/text.cc` (churn=11)
  - `src/python/m5/stats/__init__.py` (churn=11)
  - `src/base/stats/hdf5.cc` (churn=5)
  - `src/base/stats/hdf5.hh` (churn=4)
  - `src/base/stats/output.hh` (churn=4)
  - `src/base/stats/text.hh` (churn=4)
- commits 列表（按 topo-order，Top 12）：
  - 2025-08-13 `25829029e3c7` misc: Update stats dump to have a optional message
  - 2025-08-15 `44818a24dacb` base: Make dump stats message optional
- 复现: `git show 25829029e3c73383f0fc837a1f03dd6400abfc3d`

### #2470 util: Modify hypercall-external-signal util to support gem5 dashboard

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2470
- 代表 commit: `9628cbe38651` (2025-08-13)
- 变更规模: commits=1, files=2, +31/-17 (churn=48)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/hypercall_external_signal/transmitter.py` (churn=31)
  - `util/hypercall_external_signal/{orchestrator-request.py => orchestrator_request.py}` (churn=17)
- 复现: `git show 9628cbe38651d8d0dc24008159b5aa6892e63be2`

### #2351 arch-x86: automatically exit X86 simulations on kernel panic

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2351
- 代表 commit: `f43934fd9178` (2025-08-14)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f43934fd9178f9278264dcc60d3dce3cc2f07935`

### #2460 mem-garnet:: Fixed uniform_random traffic

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2460
- 代表 commit: `ba89a1b0351b` (2025-08-15)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ba89a1b0351b2d39d9dac11533aca83768972e45`

### #2535 base: Make dump stats message optional

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2535
- 代表 commit: `a3ed436a07f3` (2025-08-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a3ed436a07f37558068bff96bf3696871f448e7e`

### #2532 misc: clang-format update

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2532
- 代表 commit: `ca0928ff936a` (2025-08-18)
- 变更规模: commits=1, files=1, +6/-0 (churn=6)
- 影响范围: topdirs=.clang-format; subsys=.clang-format; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.clang-format` (churn=6)
- 复现: `git show ca0928ff936a2286addf0cb193c517c99ceafd00`

### #2525 Ignore rseq syscall on riscv to fix SE workloads built with newer glibc

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2525
- 代表 commit: `19f5f626deec` (2025-08-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 19f5f626deecc344a777bd5eb712ee409638b4ff`

### #2531 misc: Some fixes from SPEC CPU development

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2531
- 代表 commit: `7d4505400b61` (2025-08-20)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7d4505400b61ad2b6dc9788433b0387116878c5c`

### #2542 misc: split single line loop

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2542
- 代表 commit: `df865d32c467` (2025-08-20)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.clang-format; subsys=.clang-format; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.clang-format` (churn=2)
- 复现: `git show df865d32c46795f65dc804d9edb82bbd131c4a56`

### #2403 misc: Update protobuf files to remove required

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2403
- 代表 commit: `2d0e8eaca574` (2025-08-21)
- 变更规模: commits=1, files=3, +20/-20 (churn=40)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/proto/inst.proto` (churn=14)
  - `src/proto/inst_dep_record.proto` (churn=14)
  - `src/proto/packet.proto` (churn=12)
- 复现: `git show 2d0e8eaca5740c540c4f03da8173ea01fa4da5a5`

### #2547 configs,stdlib: Ensure Resource ID arg passed to RISCV Demo Script

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2547
- 代表 commit: `94d46340fa50` (2025-08-22)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 94d46340fa50d35ad69ae09fb4b9a7c7da064b80`

### #2540 python: Fix RISC-V FDT stdout-path

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2540
- 代表 commit: `02b2af8c6597` (2025-08-22)
- 变更规模: commits=1, files=3, +3/-3 (churn=6)
- 影响范围: topdirs=src, configs; subsys=python, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/deprecated/example/riscv/fs_linux.py` (churn=2)
  - `src/python/gem5/components/boards/riscv_board.py` (churn=2)
  - `src/python/gem5/prebuilt/riscvmatched/riscvmatched_board.py` (churn=2)
- 复现: `git show 02b2af8c65974caa0895ebd9e13a6f54cafd680c`

### #2298 dev: reworks PCI to add a PCI host bridge

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2298
- 代表 commit: `2d2883c95f3f` (2025-08-26)
- 变更规模: commits=1, files=50, +1295/-550 (churn=1845)
- 影响范围: topdirs=src, configs; subsys=dev, python, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/pci/upstream.hh` (churn=317)
  - `src/dev/pci/host.hh` (churn=231)
  - `src/dev/pci/host.cc` (churn=150)
  - `src/dev/pci/device.hh` (churn=133)
  - `src/dev/pci/upstream.cc` (churn=130)
  - `src/dev/pci/one_way_bridge.hh` (churn=113)
  - `src/dev/pci/device.cc` (churn=112)
  - `src/dev/pci/one_way_bridge.cc` (churn=101)
- 复现: `git show 2d2883c95f3f06ef249b1a038667da0d9f6da1e9`

### #2552 arch-arm: Make fplub.cc unused attribtue 'maybe_unused'

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2552
- 代表 commit: `5cea7c1961c7` (2025-08-26)
- 变更规模: commits=1, files=1, +5/-1 (churn=6)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/insts/fplib.cc` (churn=6)
- 复现: `git show 5cea7c1961c77cf6602f5579000e9b3b7efd4a61`

### #2551 ext: Update Pybind11 to v3.0.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2551
- 代表 commit: `740177429a8a` (2025-08-28)
- 变更规模: commits=1, files=307, +27558/-7202 (churn=34760)
- 影响范围: topdirs=ext; subsys=ext; arch=-
- 主要改动文件（Top 8 by churn）:
  - `ext/pybind11/docs/changelog.md` (churn=3148)
  - `ext/pybind11/docs/changelog.rst` (churn=2696)
  - `ext/pybind11/include/pybind11/pybind11.h` (churn=1313)
  - `ext/pybind11/include/pybind11/detail/type_caster_base.h` (churn=898)
  - `ext/pybind11/include/pybind11/detail/internals.h` (churn=797)
  - `ext/pybind11/include/pybind11/cast.h` (churn=787)
  - `ext/pybind11/.github/workflows/ci.yml` (churn=731)
  - `ext/pybind11/tests/test_pytypes.py` (churn=540)
- 复现: `git show 740177429a8a17bc5900525e8bf14212eee89ff9`

### #2549 Add virtual destructors

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2549
- 代表 commit: `c705e082b43e` (2025-08-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c705e082b43e8001aa0d4d185874de8f33ff76b2`

### #2440 configs: Update configs in configs/example/gem5_library for v25.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2440
- 代表 commit: `2f402dc117f3` (2025-08-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2f402dc117f34fb8bbc15437a2127400032e7ac2`

### #2556 sim: Add minor updates to ThermalEntity

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2556
- 代表 commit: `ea2cccb564f4` (2025-08-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ea2cccb564f47bbe6c6c27950d9e511bff7572e6`

### #2555 arch-arm: bug fixes for generic timing registers in SE mode

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2555
- 代表 commit: `755053ae51f8` (2025-09-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 755053ae51f8b556098bca7a0fa632c570385299`

### #2563 misc: bump pre-commit from 4.2.0 to 4.3.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2563
- 代表 commit: `fdbc008e7ec9` (2025-09-04)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show fdbc008e7ec9d5ea748aae939e65d252f8617b08`

### #2569 arch-arm,base: Fix ramdisk loading

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2569
- 代表 commit: `de172e496d88` (2025-09-05)
- 变更规模: commits=1, files=3, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=base, arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/base/loader/image_file_data.cc` (churn=4)
  - `src/arch/arm/linux/fs_workload.cc` (churn=2)
  - `src/base/loader/image_file_data.hh` (churn=2)
- 复现: `git show de172e496d88b778902ee9c2fd1ad92dcf212864`

### #2568 arch-arm, dev-arm: Replace params inclusion with forward declaration

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2568
- 代表 commit: `ece4b9e84e0c` (2025-09-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show ece4b9e84e0cd86a4db72d20a9af6b09440cb3b3`

### #2567 cpu: Link tagBits to tag_bits in the SimpleBTB

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2567
- 代表 commit: `cc61e4433039` (2025-09-08)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=cpu/pred; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/BranchPredictor.py` (churn=2)
- 复现: `git show cc61e4433039fcf0ac191ae3ed78aed909d91dd2`

### #2570 dev: Fix PCI host bridge with no range from up

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2570
- 代表 commit: `723b231dcb01` (2025-09-16)
- 变更规模: commits=1, files=14, +293/-78 (churn=371)
- 影响范围: topdirs=src; subsys=dev, python, mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/pci/bus.cc` (churn=97)
  - `src/dev/pci/bus.hh` (churn=77)
  - `src/dev/pci/PciUpstream.py` (churn=50)
  - `src/dev/pci/{one_way_bridge.hh => up_down_bridge.hh}` (churn=34)
  - `src/dev/pci/upstream.cc` (churn=28)
  - `src/dev/pci/{one_way_bridge.cc => up_down_bridge.cc}` (churn=24)
  - `src/dev/pci/upstream.hh` (churn=22)
  - `src/python/gem5/prebuilt/riscvmatched/riscvmatched_board.py` (churn=11)
- 复现: `git show 723b231dcb0195ca2cfa3fed4c3d7e6ac4bfe19c`

### #2580 misc: Re-enable Address Sanitizer for GCC

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2580
- 代表 commit: `e7b5ec8da3bf` (2025-09-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e7b5ec8da3bfba58da76ac00ab8de05e76895a68`

### #2572 stdlib: Update types in AbstractMemorySystem

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2572
- 代表 commit: `9da2e6d8f1f9` (2025-09-16)
- 变更规模: commits=1, files=3, +14/-7 (churn=21)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/memory/abstract_memory_system.py` (churn=14)
  - `src/python/gem5/components/memory/hbm.py` (churn=4)
  - `src/python/gem5/components/memory/memory.py` (churn=3)
- 复现: `git show 9da2e6d8f1f948763157bce93bbdbc7566e912b4`

### #2581 Cleanup some headers, etc.

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/2581
- 代表 commit: `fa224163cb87` (2025-09-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fa224163cb870cfa9791d6142b93e9e40649b6cd`

### #2587 arch-arm: Fix struct to class for arm build

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2587
- 代表 commit: `e67862f31ff0` (2025-09-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e67862f31ff0fa5c9085971e6239dc5eeb059808`

### #2592 dev: Fix PCI build

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2592
- 代表 commit: `e0056fa5018c` (2025-09-16)
- 变更规模: commits=1, files=4, +4/-8 (churn=12)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/pci/upstream.hh` (churn=5)
  - `src/dev/pci/bus.hh` (churn=3)
  - `src/dev/pci/up_down_bridge.hh` (churn=3)
  - `src/dev/pci/upstream.cc` (churn=1)
- 复现: `git show e0056fa5018c39f5aa6b3ce7d877fbbe1dabf64b`

### #2546 Toward removing main.py as a dependence

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2546
- 代表 commit: `49837d64632a` (2025-09-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 49837d64632a02d440ba0925812f5ca1dab19c6f`

### #2574 Cleaning up m5.params

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2574
- 代表 commit: `c2881afddfe0` (2025-09-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c2881afddfe0cbf9b6973a689f9aef9a338079a5`

### #2594 dev: fix RISC-V sst PCI connection

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2594
- 代表 commit: `c493a051e625` (2025-09-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c493a051e625eba955c3ca40d6c7533b03fc5144`

### #2590 Cleanup imports in python and stdlib

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/2590
- 代表 commit: `d3853ba4f1e4` (2025-09-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d3853ba4f1e4ee944c426ab58e07273b8abc3426`

### #359 Decoupled front end

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/359
- 代表 commit: `12137b9be0b1` (2025-09-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 12137b9be0b1af120cc3d09f45052cea766f7edf`

### #2596 cpu,dev,mem: Cleanup some python imports

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/2596
- 代表 commit: `e35c6421ed58` (2025-09-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e35c6421ed589642769f19285c6af65c7c1f84c1`

### #2593 stdlib,python: Improve consistency of python imports

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2593
- 代表 commit: `c6bbdaa645f0` (2025-09-18)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c6bbdaa645f0fd750688d50389396d8adff57044`

### #2597 cpu: Remove unused parameter from FTQ to enable MacOS build

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2597
- 代表 commit: `c5276de5b0e8` (2025-09-18)
- 变更规模: commits=1, files=2, +2/-4 (churn=6)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/ftq.hh` (churn=4)
  - `src/cpu/o3/ftq.cc` (churn=2)
- 复现: `git show c5276de5b0e8a4aa410032415d33260741b6c8a6`

### #2600 mem-cache: Prefetch for all cache blocks in a Fetch Target

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2600
- 代表 commit: `38c7e348a5d4` (2025-09-19)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 38c7e348a5d444a353fb628932a168138167e531`

### #2601 python: Add missing escape in param_types cxx_ini-parse func

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2601
- 代表 commit: `8ae406e7af74` (2025-09-19)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/params/param_types.py` (churn=2)
- 复现: `git show 8ae406e7af749f040768d0bdf0047ff51ca4841c`

### #2598 mem-cache: Add cache_snoop parameter to Fetch Directed Prefetcher

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2598
- 代表 commit: `ed3841e7a5d5` (2025-09-19)
- 变更规模: commits=1, files=2, +7/-2 (churn=9)
- 影响范围: topdirs=src; subsys=mem/cache/prefetch; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/prefetch/Prefetcher.py` (churn=7)
  - `src/mem/cache/prefetch/fdp.cc` (churn=2)
- 复现: `git show ed3841e7a5d520ef514bb51601f95f907fc57cc7`

### #2606 stdlib: Wrong wiring of PTW ports with sequencers in CHI platform

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2606
- 代表 commit: `05b88610023f` (2025-09-23)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/cachehierarchies/chi/private_l1_cache_hierarchy.py` (churn=2)
- 复现: `git show 05b88610023f726272cb5b79ddb595af51813b0c`

### #2607 mem-cache: Add assert for extractTag function

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2607
- 代表 commit: `20907897a217` (2025-09-23)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=mem/cache; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/tags/tagged_entry.hh` (churn=2)
- 复现: `git show 20907897a2170b7732448ded9bc44381b29121bd`

### #2560 dev,arch-riscv: Improve `rdtime` instruction

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2560
- 代表 commit: `89dd30c1334c` (2025-09-23)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 89dd30c1334cbe0c87f820b69aa675cbbaab2fc8`

### #2610 python: Re-enable config.ini output

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2610
- 代表 commit: `7762c7c4e6a2` (2025-09-23)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/simulate.py` (churn=2)
- 复现: `git show 7762c7c4e6a2580dc7b5e19056f21c819a6c098e`

### #1108 cpu-minor: Integrate Minor's executeStats with int/fp/vec ALU Accesses

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/1108
- 代表 commit: `4b24d7ca9797` (2025-09-24)
- 变更规模: commits=2, files=1, +10/-9 (churn=19)
- 影响范围: topdirs=src; subsys=cpu/minor; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/minor/execute.cc` (churn=19)
- commits 列表（按 topo-order，Top 12）：
  - 2024-12-20 `841475619cf5` cpu-minor: Rm redundant if, move stats update
  - 2025-09-24 `4b24d7ca9797` cpu-minor: Integrate Minor's executeStats with int/fp/vec ALU Accesses
- 复现: `git show 4b24d7ca9797c41b947c89b56d1c4faf91549cdb`

### #2605 [pre-commit.ci] pre-commit autoupdate

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/2605
- 代表 commit: `33201585b9a5` (2025-09-25)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.pre-commit-config.yaml; subsys=.pre-commit-config.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.pre-commit-config.yaml` (churn=2)
- 复现: `git show 33201585b9a571932e41bd0657bf023b7008b158`

### #2608 mem-cache: Cleanup headers and python imports in mem/cache

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/2608
- 代表 commit: `fae7157b8b02` (2025-09-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fae7157b8b02f016015dbd9063fe028b32fb81fa`

### #2524 tests: IPC Regression Tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2524
- 代表 commit: `105485a61858` (2025-09-26)
- 变更规模: commits=1, files=4, +371/-0 (churn=371)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/regression_tests/configs/hello-world-binary.py` (churn=141)
  - `tests/gem5/regression_tests/configs/matrix-multiply.py` (churn=140)
  - `tests/gem5/regression_tests/test_regression_tests.py` (churn=79)
  - `tests/gem5/regression_tests/README.md` (churn=11)
- 复现: `git show 105485a61858ad77e6a147f0ace94b95a8d1d703`

### #1977 mem-cache: Unit test FIFO RP

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/1977
- 代表 commit: `13e943ee492a` (2025-09-27)
- 变更规模: commits=2, files=2, +340/-0 (churn=340)
- 影响范围: topdirs=src; subsys=mem/cache/rp; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/replacement_policies/tree_plru_rp.test.cc` (churn=338)
  - `src/mem/cache/replacement_policies/SConscript` (churn=2)
- commits 列表（按 topo-order，Top 12）：
  - 2025-09-27 `13e943ee492a` mem-cache: Unit test FIFO RP
  - 2025-08-21 `411c2ee8072b` tests, mem: Add SimObject unit tests for TreePLRU RP
- 复现: `git show 13e943ee492aa0b2cff077418fe7fb5559ea7afa`

### #2617 fastmodel: Fix compiler error when building fastmodels

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2617
- 代表 commit: `053ac08ea552` (2025-09-30)
- 变更规模: commits=1, files=2, +11/-7 (churn=18)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/fastmodel/iris/cpu.cc` (churn=10)
  - `src/arch/arm/fastmodel/iris/cpu.hh` (churn=8)
- 复现: `git show 053ac08ea55245c851ded4bea279f8415cd88f47`

### #2609 stdlib: Define a CHI PrivateL1PrivateL2CacheHierarchy

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2609
- 代表 commit: `11e77fb3345f` (2025-09-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 11e77fb3345f25d08140d14de35b9a33406b4a5f`

### #2303 cpu:Add gshare branch predictor model

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2303
- 代表 commit: `22e4e8331d62` (2025-09-30)
- 变更规模: commits=1, files=4, +266/-1 (churn=267)
- 影响范围: topdirs=src; subsys=cpu/pred; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/gshare.cc` (churn=164)
  - `src/cpu/pred/gshare.hh` (churn=91)
  - `src/cpu/pred/BranchPredictor.py` (churn=9)
  - `src/cpu/pred/SConscript` (churn=3)
- 复现: `git show 22e4e8331d62601d1cb14ada7eab2ad9f77fd639`

### #2624 misc: bump mypy from 1.17.1 to 1.18.2

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2624
- 代表 commit: `d24ee0d969f4` (2025-10-02)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show d24ee0d969f41d343177c5f44d3204d1ee651ff1`

### #2619 arch-riscv: new vector instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2619
- 代表 commit: `fa54fcaba685` (2025-10-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fa54fcaba685787e54fef3f3924525374745676c`

### #2627 stdlib: Decouple the AbstractCore from the chi L1CacheController

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2627
- 代表 commit: `2cdb524ffd6a` (2025-10-03)
- 变更规模: commits=1, files=3, +18/-7 (churn=25)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/cachehierarchies/chi/nodes/l1_cache.py` (churn=17)
  - `src/python/gem5/components/cachehierarchies/chi/private_l1_cache_hierarchy.py` (churn=4)
  - `src/python/gem5/components/cachehierarchies/chi/private_l1_private_l2_cache_hierarchy.py` (churn=4)
- 复现: `git show 2cdb524ffd6a79af57e66a8443bd493cd1a2ab93`

### #2613 tests: add tests for configuration related output files

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2613
- 代表 commit: `66d23ca867fe` (2025-10-03)
- 变更规模: commits=1, files=3, +308/-0 (churn=308)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/config_output_files/configs/arm-hello.py` (churn=152)
  - `tests/gem5/config_output_files/test-output-files.py` (churn=145)
  - `tests/gem5/config_output_files/README.md` (churn=11)
- 复现: `git show 66d23ca867fe65842358f2a6e0695dae1bc94055`

### #2599 arch-vega: Implement remaining non-MFMA CDNA4 instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2599
- 代表 commit: `3cf7da9e087f` (2025-10-05)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3cf7da9e087f1ab6f0bc2f117c8ae574ca26605b`

### #2618 base: Add CTAD deduction guide for Memoizer

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2618
- 代表 commit: `106ca81037f4` (2025-10-06)
- 变更规模: commits=1, files=1, +4/-0 (churn=4)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/memoizer.hh` (churn=4)
- 复现: `git show 106ca81037f47e74ef74be3bab4e2aa9f460902e`

### #2634 mem: Remove deprecated port names in mem

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2634
- 代表 commit: `e1c1e70e09c3` (2025-10-07)
- 变更规模: commits=1, files=11, +0/-62 (churn=62)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/system/Sequencer.py` (churn=13)
  - `src/mem/AddrMapper.py` (churn=6)
  - `src/mem/Bridge.py` (churn=6)
  - `src/mem/CommMonitor.py` (churn=6)
  - `src/mem/MemChecker.py` (churn=6)
  - `src/mem/MemDelay.py` (churn=6)
  - `src/mem/SerialLink.py` (churn=6)
  - `src/mem/XBar.py` (churn=6)
- 复现: `git show e1c1e70e09c35f66f54fe2925cbdd8b57726b15f`

### #2614 stdlib: Improvements for VIPER board

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2614
- 代表 commit: `c6051acf505b` (2025-10-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c6051acf505b9bc27e673bb67a8c3945a2daedaa`

### #2643 stdlib: Fix binding of table walker port

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2643
- 代表 commit: `1023451e689c` (2025-10-07)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/cachehierarchies/classic/private_l1_private_l2_cache_hierarchy.py` (churn=3)
- 复现: `git show 1023451e689c433e46493473fdfeb85853e21abe`

### #2641 configs: Fix ruby_fs.py platform after PCI change

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2641
- 代表 commit: `31b483f989b3` (2025-10-07)
- 变更规模: commits=1, files=2, +11/-10 (churn=21)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/example/arm/ruby_fs.py` (churn=13)
  - `configs/example/arm/devices.py` (churn=8)
- 复现: `git show 31b483f989b3cf9a9105924577579b306db84433`

### #2631 arch-riscv: hot fix in `vclmulh`

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2631
- 代表 commit: `ee72d2b810ce` (2025-10-07)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=4)
- 复现: `git show ee72d2b810ce5f680184131fd50cd6b155af84db`

### #2638 stdlib, python: Clean up some python imports

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2638
- 代表 commit: `fda964c2358e` (2025-10-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show fda964c2358e633663c034c815ef443bc5aa46ca`

### #2637 misc: [pre-commit.ci] pre-commit autoupdate

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/2637
- 代表 commit: `154b3b1b650c` (2025-10-07)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=.pre-commit-config.yaml; subsys=.pre-commit-config.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.pre-commit-config.yaml` (churn=4)
- 复现: `git show 154b3b1b650cdb6463e5d22a1cf8fe37e47c5b31`

### #2269 python: improve error message in SimObject __setattr__

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2269
- 代表 commit: `542c97a2feb2` (2025-10-08)
- 变更规模: commits=1, files=1, +5/-2 (churn=7)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/SimObject.py` (churn=7)
- 复现: `git show 542c97a2feb2e6744d8142c14b7ba0e3de334361`

### #2646 tests: fix failing FIFO RP daily tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2646
- 代表 commit: `00d666d5924e` (2025-10-08)
- 变更规模: commits=1, files=1, +4/-0 (churn=4)
- 影响范围: topdirs=src; subsys=mem/cache/rp; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/replacement_policies/fifo_rp.test.cc` (churn=4)
- 复现: `git show 00d666d5924e483bb8b590a70fbb4d276f9e55d3`

### #2639 misc: Add missing import to flag_tables

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2639
- 代表 commit: `4ecbc40848c6` (2025-10-08)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/kern/linux/flag_tables.hh` (churn=2)
- 复现: `git show 4ecbc40848c60796b0710594318f769605e3e711`

### #2645 arch-arm: Add missing break in decoder

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2645
- 代表 commit: `be1ddd0c1c65` (2025-10-08)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/formats/sve_2nd_level.isa` (churn=1)
- 复现: `git show be1ddd0c1c659a70e34f3c488ee01ce116da9c2a`

### #2625 arch-riscv: Fix `vslideup`

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2625
- 代表 commit: `29ca616c4172` (2025-10-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 29ca616c41726dbf77e6a8a7e96f44114a5efa85`

### #2640 arch-arm, dev-arm: Small cleanup in arch-arm and dev-arm

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/2640
- 代表 commit: `b46c49e3efae` (2025-10-08)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b46c49e3efaeba7d2b89e096ca553675eff2c3a5`

### #2648 stdlib: Support generation of multiple directories in CHI

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2648
- 代表 commit: `c29cadf246f8` (2025-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c29cadf246f89a55f2dcddb6ac485388747973c6`

### #2633 dev-amdgpu,stdlib: Allow for multiple GPUs in stdlib

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2633
- 代表 commit: `59f093c14609` (2025-10-09)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 59f093c146098ad5c72e657d57534c6a8d92ca41`

### #2653 misc, tests: Move fast unit tests to CI tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2653
- 代表 commit: `892c99355030` (2025-10-13)
- 变更规模: commits=1, files=2, +9/-6 (churn=15)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/ci-tests.yaml` (churn=11)
  - `.github/workflows/daily-tests.yaml` (churn=4)
- 复现: `git show 892c99355030661144566da9930b93552cea5390`

### #2660 misc: Fix licesnse typo in testlib

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2660
- 代表 commit: `31bfb109690b` (2025-10-13)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=ext; subsys=ext; arch=-
- 主要改动文件（Top 8 by churn）:
  - `ext/testlib/state.py` (churn=2)
- 复现: `git show 31bfb109690b5d09fc9c3e336e027361d8ee35ee`

### #2644 mem-cache: Remove unused header in prefetch

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2644
- 代表 commit: `0e523f74f0a2` (2025-10-13)
- 变更规模: commits=1, files=2, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=mem/cache/prefetch; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/prefetch/base.hh` (churn=1)
  - `src/mem/cache/prefetch/fdp.hh` (churn=1)
- 复现: `git show 0e523f74f0a24c0334cc9ac4bdc13279c8ce5253`

### #2655 base: Fix free-nonheap-object warning with Ubsan

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2655
- 代表 commit: `7a32ffa8182a` (2025-10-13)
- 变更规模: commits=1, files=1, +7/-7 (churn=14)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/refcnt.hh` (churn=14)
- 复现: `git show 7a32ffa8182a177c838fb32e5b8c1e643543fe24`

### #2663 misc: Fix license typo in testlib

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2663
- 代表 commit: `3c4dedb088e2` (2025-10-13)
- 变更规模: commits=1, files=2, +2/-2 (churn=4)
- 影响范围: topdirs=ext, src; subsys=ext, base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `ext/testlib/terminal.py` (churn=2)
  - `src/base/amo.test.cc` (churn=2)
- 复现: `git show 3c4dedb088e22d2a62dd4aa4d056d281a1310082`

### #2626 arch-riscv: fix `vm*` for `VLEN/EEW < 8` and `LMUL > 1`

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2626
- 代表 commit: `a8aa1f38ef53` (2025-10-14)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/insts/vector.cc` (churn=2)
- 复现: `git show a8aa1f38ef53c83b667147a8707361b7a3bf4cf6`

### #2662 misc: [pre-commit.ci] pre-commit autoupdate

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/2662
- 代表 commit: `49acdb2fdcaf` (2025-10-14)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=.pre-commit-config.yaml; subsys=.pre-commit-config.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.pre-commit-config.yaml` (churn=4)
- 复现: `git show 49acdb2fdcaf09efb6e0e4e6f34dfdc696090d33`

### #2595 Fdp-library

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2595
- 代表 commit: `87b05e832257` (2025-10-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 87b05e83225750af2137e2b3429412c629fa4833`

### #2656 arch-arm, configs, cpu: Implement FEAT_SVE2

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2656
- 代表 commit: `e26d7f8f2ecc` (2025-10-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e26d7f8f2ecc68dba7569b731a5ad802125d9905`

### #2616 arch-arm,cpu-o3: Fix out of bound with Ubsan

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2616
- 代表 commit: `f951cc657390` (2025-10-17)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show f951cc65739087b170972f7e039617bfcfe96538`

### #2671 dev: rename PCI devices base classes

- 动作（heuristic）: 重命名/迁移
- PR 链接: https://github.com/gem5/gem5/pull/2671
- 代表 commit: `024e373e81b7` (2025-10-19)
- 变更规模: commits=1, files=5, +57/-28 (churn=85)
- 影响范围: topdirs=src; subsys=dev; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/dev/pci/device.hh` (churn=45)
  - `src/dev/pci/device.cc` (churn=24)
  - `src/dev/pci/PciDevice.py` (churn=6)
  - `src/dev/pci/up_down_bridge.cc` (churn=6)
  - `src/dev/pci/SConscript` (churn=4)
- 复现: `git show 024e373e81b7e156e3b7bf00c60d94e98a3c05d8`

### #2665 cpu-minor: explicitly check if instruction in ROM during decoding

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2665
- 代表 commit: `56f810ec9ba6` (2025-10-22)
- 变更规模: commits=1, files=1, +13/-5 (churn=18)
- 影响范围: topdirs=src; subsys=cpu/minor; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/minor/decode.cc` (churn=18)
- 复现: `git show 56f810ec9ba624e874256206fb588bfc5d0f9ffd`

### #2687 misc: Add ASAN and UBSAN compilation tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2687
- 代表 commit: `5dc5b4ac2358` (2025-10-22)
- 变更规模: commits=1, files=1, +16/-0 (churn=16)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/compiler-tests.yaml` (churn=16)
- 复现: `git show 5dc5b4ac235859beb3eb6537a5127ecb46617e4a`

### #2686 base: Fix use-after-free warning

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2686
- 代表 commit: `ad4be8c2e8b8` (2025-10-23)
- 变更规模: commits=1, files=1, +3/-1 (churn=4)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/refcnt.hh` (churn=4)
- 复现: `git show ad4be8c2e8b86a7d3c34f91d1431597427d83e4d`

### #2651 cpu-o3: Make space for a pipeline tracer

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2651
- 代表 commit: `8fe12a1d8b65` (2025-10-23)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 8fe12a1d8b6590104a22e42fd1c6d4fe2afdd940`

### #2690 stdlib: Remove GPU ip_discovery.bin if exists

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2690
- 代表 commit: `13382d009146` (2025-10-23)
- 变更规模: commits=1, files=2, +3/-0 (churn=3)
- 影响范围: topdirs=configs, src; subsys=configs, python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/devices/gpus/amdgpu.py` (churn=2)
  - `configs/example/gpufs/mi300.py` (churn=1)
- 复现: `git show 13382d00914693c3430bc94b116ced81cdb653f6`

### #2691 arch-arm: Fix compile errors

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2691
- 代表 commit: `3250006a0fbb` (2025-10-24)
- 变更规模: commits=1, files=2, +1/-6 (churn=7)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/templates/sve_mem.isa` (churn=5)
  - `src/arch/arm/isa/insts/sve.isa` (churn=2)
- 复现: `git show 3250006a0fbba624093edd6ea0a33113c41697c4`

### #2696 python: Fix imports broken by PR 2595

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2696
- 代表 commit: `fef757967792` (2025-10-24)
- 变更规模: commits=1, files=2, +17/-18 (churn=35)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/simulate.py` (churn=34)
  - `src/python/m5/debug.py` (churn=1)
- 复现: `git show fef7579677929886fdde2a6eebe1b52099014fb1`

### #2650 arch-arm, stdlib: Rework the PTW to support a configurable number of outstanding TW

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2650
- 代表 commit: `d4831ee789dd` (2025-10-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d4831ee789ddbb9f44df67ae40f403d4996ebc52`

### #2697 mem-ruby: Cleanup imports and includes

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/2697
- 代表 commit: `dd9adbb445f3` (2025-10-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show dd9adbb445f3fd265321bdd9aa3f89746e9f89c0`

### #2683 tests: remove duplicate option on arm boot tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2683
- 代表 commit: `88a08406f060` (2025-10-27)
- 变更规模: commits=1, files=1, +0/-1 (churn=1)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/arm_boot_tests/test_linux_boot.py` (churn=1)
- 复现: `git show 88a08406f060c515da18a9db01277dfe201c301f`

### #2704 dev: Temporary fix for X86 board hanging forever & moved to board

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2704
- 代表 commit: `89da23b5464b` (2025-10-27)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 89da23b5464b9dea107667e81bfe4a0d0b5c036e`

### #2684 cpu: Enable PcCountTracker in NULL builds

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2684
- 代表 commit: `bb0f68259e40` (2025-10-27)
- 变更规模: commits=1, files=1, +13/-15 (churn=28)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/probes/SConscript` (churn=28)
- 复现: `git show bb0f68259e40c904c44c9435b2715cddaa86a715`

### #2647 stdlib: Define a new get_mem_ranges method

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2647
- 代表 commit: `273cc6594aa5` (2025-10-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 273cc6594aa5fd3190f58604818734546976b8ff`

### #2674 tests: Update resource links to point to azure

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2674
- 代表 commit: `71e168867da6` (2025-10-28)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 71e168867da6c8226d8bed0d36081af222110ab7`

### #2703 arch-riscv: Fix RISC-V `Decoder::reset()` not calling base class reset

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2703
- 代表 commit: `ba54641029de` (2025-10-29)
- 变更规模: commits=1, files=1, +1/-0 (churn=1)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/decoder.cc` (churn=1)
- 复现: `git show ba54641029de953739dce620a44a7663b2330c31`

### #2706 mem-ruby: Break dependence in CHI protocol

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2706
- 代表 commit: `0f476aabf35a` (2025-10-29)
- 变更规模: commits=1, files=2, +13/-8 (churn=21)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/structures/MN_TBETable.cc` (churn=11)
  - `src/mem/ruby/structures/MN_TBETable.hh` (churn=10)
- 复现: `git show 0f476aabf35a29bf840985e601f9f3896de83a03`

### #2713 python: Update RISC-V lupv_board.py

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2713
- 代表 commit: `503aed5ed437` (2025-10-29)
- 变更规模: commits=1, files=1, +25/-11 (churn=36)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/boards/experimental/lupv_board.py` (churn=36)
- 复现: `git show 503aed5ed43731bc9d638995a0d1948668edd60b`

### #2670 cpu-o3: Instant ROB squash

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2670
- 代表 commit: `f1bc35e26fd7` (2025-10-30)
- 变更规模: commits=1, files=3, +11/-5 (churn=16)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/rob.hh` (churn=8)
  - `src/cpu/o3/BaseO3CPU.py` (churn=5)
  - `src/cpu/o3/rob.cc` (churn=3)
- 复现: `git show f1bc35e26fd7f2076f9fc0a9675e6ceca005525c`

### #2664 base,arch-arm,cpu: Cleaning up more headers and tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2664
- 代表 commit: `0c5fe866a1ad` (2025-10-30)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 0c5fe866a1ada133823f386b3f6bff3967514a30`

### #2722 sim-se,arch: Initialize max stack size from parameter (all ISAs)

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2722
- 代表 commit: `514accd5051c` (2025-10-30)
- 变更规模: commits=1, files=6, +10/-9 (churn=19)
- 影响范围: topdirs=src; subsys=arch; arch=arm, mips, power, riscv, sparc, x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/sparc/process.hh` (churn=5)
  - `src/arch/arm/process.cc` (churn=4)
  - `src/arch/riscv/process.cc` (churn=4)
  - `src/arch/mips/process.cc` (churn=2)
  - `src/arch/power/process.cc` (churn=2)
  - `src/arch/x86/process.cc` (churn=2)
- 复现: `git show 514accd5051c458ba5ea0293a122e9774072a93d`

### #2548 mem, tests: Add SimObject unit tests for the TreePLRU replacement policy

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2548
- 代表 commit: `2a46a8f1a54f` (2025-10-31)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 2a46a8f1a54fd02cc6b46e49bf44582267d3014b`

### #2718 arch-arm: Ensure Stage2Walk port pointer is used

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2718
- 代表 commit: `d53336fb1775` (2025-10-31)
- 变更规模: commits=1, files=1, +1/-2 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/table_walker.cc` (churn=3)
- 复现: `git show d53336fb177507b4ed42e3a419cb5cb1122f5aef`

### #2708 resources: make workloads work with set_se_multi_binary_workload

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2708
- 代表 commit: `522452770957` (2025-10-31)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 522452770957e44205269eff298626027eb9c8c3`

### #2723 mem-ruby: Annotate CHI-TLM scheduling time in Transaction obj

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2723
- 代表 commit: `869315d4e195` (2025-10-31)
- 变更规模: commits=1, files=4, +36/-38 (churn=74)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/tlm/tlm_chi_gen.cc` (churn=55)
  - `src/mem/ruby/protocol/chi/tlm/generator.hh` (churn=10)
  - `src/mem/ruby/protocol/chi/tlm/generator.cc` (churn=5)
  - `src/mem/ruby/protocol/chi/tlm/TlmGenerator.py` (churn=4)
- 复现: `git show 869315d4e1955d5594a92f42f24ec8cadd7a48e7`

### #2721 tests,scons,arch-arm: Fix Address sanitizer compilation test failures

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2721
- 代表 commit: `d65257daf3f6` (2025-10-31)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show d65257daf3f60d000cb33e4b12e79bdbeb02b878`

### #2725 cpu-o3: Add LQ and SQ average occupancy stat

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2725
- 代表 commit: `548cb234238c` (2025-11-02)
- 变更规模: commits=1, files=2, +36/-2 (churn=38)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/lsq_unit.cc` (churn=21)
  - `src/cpu/o3/lsq_unit.hh` (churn=17)
- 复现: `git show 548cb234238c623d4df06c5f50d411fc30df6243`

### #2729 base: make RefCountingPtr's operator bool() explicit

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2729
- 代表 commit: `6c7c615db60f` (2025-11-02)
- 变更规模: commits=1, files=4, +12/-4 (churn=16)
- 影响范围: topdirs=src; subsys=base, cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/refcnt.hh` (churn=6)
  - `src/cpu/o3/lsq_unit.hh` (churn=6)
  - `src/base/refcnt.test.cc` (churn=2)
  - `src/cpu/o3/rob.cc` (churn=2)
- 复现: `git show 6c7c615db60f631b094ab32cf3d2157b9ade55c5`

### #2685 mem-cache: register TagExtractor for SectorTag

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2685
- 代表 commit: `db92c134575a` (2025-11-02)
- 变更规模: commits=1, files=1, +6/-0 (churn=6)
- 影响范围: topdirs=src; subsys=mem/cache; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/tags/sector_tags.cc` (churn=6)
- 复现: `git show db92c134575a636b6236a398b972d11efb8d0f12`

### #2666 base: Fix memleak in coroutine

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2666
- 代表 commit: `75d88aedff63` (2025-11-02)
- 变更规模: commits=1, files=1, +20/-5 (churn=25)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/coroutine.hh` (churn=25)
- 复现: `git show 75d88aedff630485ba8f45558974d3614a9afc68`

### #2727 Revert "mem-ruby: Break dependence in CHI protocol"

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2727
- 代表 commit: `2a6d443b4c80` (2025-11-03)
- 变更规模: commits=1, files=2, +8/-13 (churn=21)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/structures/MN_TBETable.cc` (churn=11)
  - `src/mem/ruby/structures/MN_TBETable.hh` (churn=10)
- 复现: `git show 2a6d443b4c8065230d0f96f8e6e101cac6d8895c`

### #2732 mem-ruby: Expose ARM::CHI::Payload to python

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2732
- 代表 commit: `1ede65b8ce64` (2025-11-03)
- 变更规模: commits=1, files=1, +2/-0 (churn=2)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/tlm/tlm_chi_gen.cc` (churn=2)
- 复现: `git show 1ede65b8ce6423f0595c62fa79f13190eeddfea9`

### #2726 sim-se: Fix DPRINTF format in syscall return

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2726
- 代表 commit: `52022e1053ff` (2025-11-03)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=src; subsys=sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/sim/syscall_desc.cc` (churn=2)
- 复现: `git show 52022e1053ff779db3d9e1bece2a64632cdd6be5`

### #2731 arch-x86: raise #DE on division overflow

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2731
- 代表 commit: `2870f0d67859` (2025-11-04)
- 变更规模: commits=1, files=3, +34/-10 (churn=44)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/isa/microops/regop.isa` (churn=21)
  - `src/arch/x86/insts/static_inst.cc` (churn=17)
  - `src/arch/x86/insts/static_inst.hh` (churn=6)
- 复现: `git show 2870f0d678592ce74a8b77afca828fdca8fad405`

### #2694 base: Fix GEM5_PUBLIC typo

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2694
- 代表 commit: `e7185b9975da` (2025-11-04)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show e7185b9975dad3d2b17b23c113ce2fab1f8f1234`

### #2714 arch-riscv: add initrd option to FsWorkload

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2714
- 代表 commit: `131e275d3b9d` (2025-11-05)
- 变更规模: commits=1, files=3, +34/-0 (churn=34)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/linux/fs_workload.cc` (churn=25)
  - `src/arch/riscv/RiscvFsWorkload.py` (churn=7)
  - `src/arch/riscv/linux/fs_workload.hh` (churn=2)
- 复现: `git show 131e275d3b9db3ce882fe8ead3d5f6833793d9be`

### #2736 tests: Update resource links to directly point to azure

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2736
- 代表 commit: `3a068babd2d2` (2025-11-05)
- 变更规模: commits=1, files=1, +5/-5 (churn=10)
- 影响范围: topdirs=tests; subsys=tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/gpu/test_gpu_pannotia.py` (churn=10)
- 复现: `git show 3a068babd2d245181265f2c4b4f01bb1a5e5b512`

### #2738 cpu-o3: Increase MaxWidth to 16

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2738
- 代表 commit: `ba604ef48fbc` (2025-11-07)
- 变更规模: commits=1, files=1, +13/-1 (churn=14)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/limits.hh` (churn=14)
- 复现: `git show ba604ef48fbca21688c7cf7ef7bb40654dada438`

### #2659 arch-riscv: fix bugs in pinned registers in `vred*` instructions

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2659
- 代表 commit: `dfa10abafd11` (2025-11-07)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show dfa10abafd11e26f7781452d154292e88009c7d5`

### #2739 mem-ruby: Fixes to CHIGeneric imports and headers

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2739
- 代表 commit: `b3b453ee57b7` (2025-11-09)
- 变更规模: commits=1, files=3, +3/-6 (churn=9)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/generic/CHIGenericController.hh` (churn=5)
  - `src/mem/ruby/protocol/chi/generic/CHIGenericController.cc` (churn=3)
  - `src/mem/ruby/protocol/chi/generic/CHIGeneric.py` (churn=1)
- 复现: `git show b3b453ee57b794869c16a5398bacdb9a8be1bfe6`

### #2743 misc: [pre-commit.ci] pre-commit autoupdate

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/2743
- 代表 commit: `955ea82667df` (2025-11-11)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=.pre-commit-config.yaml; subsys=.pre-commit-config.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.pre-commit-config.yaml` (churn=4)
- 复现: `git show 955ea82667df922d05ebb194f4b1cb9812e1736c`

### #2675 mem-ruby: Add support for CLFLUSH type instructions in MESI Three Level protocol

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2675
- 代表 commit: `8b263b8d9c01` (2025-11-11)
- 变更规模: commits=1, files=11, +519/-17 (churn=536)
- 影响范围: topdirs=src, configs; subsys=mem, configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/MESI_Two_Level-L2cache.sm` (churn=213)
  - `src/mem/ruby/protocol/MESI_Three_Level-L1cache.sm` (churn=167)
  - `src/mem/ruby/protocol/MESI_Three_Level-L0cache.sm` (churn=112)
  - `src/mem/ruby/slicc_interface/ProtocolInfo.hh` (churn=18)
  - `src/mem/ruby/protocol/MESI_Three_Level-msg.sm` (churn=6)
  - `src/mem/ruby/protocol/MESI_Two_Level-msg.sm` (churn=6)
  - `src/mem/ruby/system/Sequencer.cc` (churn=6)
  - `src/mem/ruby/system/RubyPort.cc` (churn=3)
- 复现: `git show 8b263b8d9c0135d7cdb57e626e59e1cd167c8958`

### #2630 mem-cache: Add unit test for MRU RP

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2630
- 代表 commit: `02787687a0bc` (2025-11-13)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 02787687a0bc2d6de21ff1058ee076c5e8a75b6e`

### #2707 stdlib: Clean up stdlib imports in CHI

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2707
- 代表 commit: `b05b556d3831` (2025-11-13)
- 变更规模: commits=1, files=6, +31/-27 (churn=58)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/components/cachehierarchies/chi/private_l1_cache_hierarchy.py` (churn=25)
  - `src/python/gem5/components/cachehierarchies/chi/nodes/dma_requestor.py` (churn=9)
  - `src/python/gem5/components/cachehierarchies/chi/nodes/l2_cache.py` (churn=9)
  - `src/python/gem5/components/cachehierarchies/chi/nodes/l1_cache.py` (churn=7)
  - `src/python/gem5/components/cachehierarchies/chi/nodes/abstract_node.py` (churn=6)
  - `src/python/gem5/components/cachehierarchies/chi/nodes/directory.py` (churn=2)
- 复现: `git show b05b556d3831751c42c99c1d74fe72a36294b27e`

### #2740 mem-ruby: Update Ruby Network to use new-style stats

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2740
- 代表 commit: `53123ba58a5f` (2025-11-13)
- 变更规模: commits=1, files=6, +107/-118 (churn=225)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/network/simple/Switch.cc` (churn=97)
  - `src/mem/ruby/network/simple/SimpleNetwork.cc` (churn=82)
  - `src/mem/ruby/network/simple/SimpleNetwork.hh` (churn=17)
  - `src/mem/ruby/network/simple/Switch.hh` (churn=17)
  - `src/mem/ruby/network/simple/PerfectSwitch.cc` (churn=10)
  - `src/mem/ruby/network/simple/PerfectSwitch.hh` (churn=2)
- 复现: `git show 53123ba58a5fd391a580eedb3572543ab175ce33`

### #2698 mem-ruby: Update SLICC to generate only shared files

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2698
- 代表 commit: `413273afd460` (2025-11-13)
- 变更规模: commits=1, files=2, +23/-17 (churn=40)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/slicc/symbols/SymbolTable.py` (churn=33)
  - `src/mem/slicc/parser.py` (churn=7)
- 复现: `git show 413273afd460686b5cb460b44db4cbf5e856af1d`

### #2737 cpu-kvm: added support for hosts with larger page size

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2737
- 代表 commit: `8733b87f3406` (2025-11-13)
- 变更规模: commits=1, files=2, +60/-9 (churn=69)
- 影响范围: topdirs=src; subsys=cpu; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/kvm/vm.cc` (churn=58)
  - `src/cpu/kvm/vm.hh` (churn=11)
- 复现: `git show 8733b87f3406fdc57cdc8a8a379d9349ee2e941d`

### #2705 mem-ruby: Add missing import to SLICC controllers

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2705
- 代表 commit: `776830ec7f1f` (2025-11-14)
- 变更规模: commits=1, files=1, +8/-0 (churn=8)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/slicc/symbols/StateMachine.py` (churn=8)
- 复现: `git show 776830ec7f1fd001728982082e4bf53e4fb94ffa`

### #2701 arch-riscv: add mask policy for `vslide1*`

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2701
- 代表 commit: `d5e9a6d30ef2` (2025-11-18)
- 变更规模: commits=1, files=1, +16/-6 (churn=22)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/decoder.isa` (churn=22)
- 复现: `git show d5e9a6d30ef24346cf085a9045296ba9bb3561a7`

### #2755 base: fix compile error in struct hash

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2755
- 代表 commit: `59a85c12f23f` (2025-11-19)
- 变更规模: commits=1, files=1, +4/-4 (churn=8)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/stl_helpers/hash_helpers.hh` (churn=8)
- 复现: `git show 59a85c12f23f079b5ae335ad59033da9e8c8282e`

### #2741 arch-arm,sim-se: Implement sigreturn for Arm64

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2741
- 代表 commit: `9d52f319d070` (2025-11-19)
- 变更规模: commits=1, files=5, +464/-341 (churn=805)
- 影响范围: topdirs=src; subsys=arch, sim; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/linux/se_workload.cc` (churn=682)
  - `src/arch/arm/linux/linux.cc` (churn=67)
  - `src/arch/arm/linux/linux.hh` (churn=47)
  - `src/sim/syscall_emul.hh` (churn=8)
  - `src/arch/arm/SConscript` (churn=1)
- 复现: `git show 9d52f319d0706ac8e1362d532639ec9dfb87d7da`

### #2623 arch-arm: Decouple insts from generated decoder

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2623
- 代表 commit: `189cbae7f971` (2025-11-19)
- 变更规模: commits=1, files=3, +336/-156 (churn=492)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/insts/sve_macromem.cc` (churn=331)
  - `src/arch/arm/insts/sve_macromem.hh` (churn=160)
  - `src/arch/arm/SConscript` (churn=1)
- 复现: `git show 189cbae7f971f063a663329773b34172ae83beaa`

### #2759 arch-riscv: Only show RVV message when we enable it

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2759
- 代表 commit: `8a3623425973` (2025-11-19)
- 变更规模: commits=1, files=1, +5/-5 (churn=10)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa.cc` (churn=10)
- 复现: `git show 8a3623425973740aa9f2b359999f000aecebeb34`

### #2761 base-stats: Add m5_stats.Group to stats dump

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2761
- 代表 commit: `8b9a7f3c7635` (2025-11-20)
- 变更规模: commits=1, files=1, +33/-2 (churn=35)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/stats/gem5stats.py` (churn=35)
- 复现: `git show 8b9a7f3c763581eee45421e3e52fa93e96ed5554`

### #2764 misc: multiple stats outputs with --stats-file

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2764
- 代表 commit: `91ee67ce8950` (2025-11-22)
- 变更规模: commits=1, files=1, +9/-4 (churn=13)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/m5/main.py` (churn=13)
- 复现: `git show 91ee67ce8950aa4dfae03b3856a881465a902982`

### #2689 sim,mem-ruby: Define a CHI-TLM port

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2689
- 代表 commit: `9bb35e27af99` (2025-11-23)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 9bb35e27af99a46f518cc1a1538e1f80653c5ef1`

### #2766 arch-vega: Support MUBUF to LDS instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2766
- 代表 commit: `37635a980cde` (2025-11-24)
- 变更规模: commits=1, files=2, +57/-75 (churn=132)
- 影响范围: topdirs=src; subsys=arch; arch=amdgpu
- 主要改动文件（Top 8 by churn）:
  - `src/arch/amdgpu/vega/insts/mubuf.cc` (churn=124)
  - `src/arch/amdgpu/vega/insts/op_encodings.hh` (churn=8)
- 复现: `git show 37635a980cde0c32376f86fd64b9a68a5cf982fa`

### #2768 mem-ruby: Do not wrap txn_ids

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2768
- 代表 commit: `f035207f9083` (2025-11-24)
- 变更规模: commits=1, files=1, +8/-8 (churn=16)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/ruby/protocol/chi/tlm/controller.cc` (churn=16)
- 复现: `git show f035207f90838a45e9091d987363f45ff06c69ec`

### #2765 arch-arm: Implement crypto instructions for FEAT_SVE/2

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2765
- 代表 commit: `a7d6efcf348b` (2025-11-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show a7d6efcf348b85da7a23388d21a0460ac72df86f`

### #2752 sim-se: implement sendfile syscall

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2752
- 代表 commit: `b01cb1cd032d` (2025-11-24)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show b01cb1cd032da46922f5a3659ab7d2ef3c90c5ad`

### #2763 systemc: Only acquire and release if tlm_payload has mm

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2763
- 代表 commit: `3f5e51bf23e4` (2025-11-24)
- 变更规模: commits=1, files=1, +21/-7 (churn=28)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/systemc/tlm_bridge/gem5_to_tlm.cc` (churn=28)
- 复现: `git show 3f5e51bf23e405d26fb6e478880ce2eff49d0185`

### #2770 misc: [pre-commit.ci] pre-commit autoupdate

- 动作（heuristic）: CI
- PR 链接: https://github.com/gem5/gem5/pull/2770
- 代表 commit: `391903f87382` (2025-11-24)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=.pre-commit-config.yaml; subsys=.pre-commit-config.yaml; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.pre-commit-config.yaml` (churn=2)
- 复现: `git show 391903f873829f48fe2eae22e8b554313770d6b6`

### #2769 dev-amdgpu: Update mmhub size

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2769
- 代表 commit: `5be8bd307563` (2025-11-25)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 5be8bd3075638da34276134429cec6dc2593ff02`

### #2652 configs, cpu-o3: Implement a distributed InstructionQueue

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2652
- 代表 commit: `2d4d1bf2bbd3` (2025-11-25)
- 变更规模: commits=1, files=14, +547/-210 (churn=757)
- 影响范围: topdirs=src, configs; subsys=cpu/o3, configs, arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/inst_queue.cc` (churn=359)
  - `src/cpu/o3/inst_queue.hh` (churn=145)
  - `src/cpu/o3/IQUnit.py` (churn=60)
  - `src/cpu/o3/SMT.py` (churn=51)
  - `src/cpu/o3/iew.cc` (churn=30)
  - `configs/common/cores/arm/ex5_big.py` (churn=21)
  - `src/cpu/o3/BaseO3CPU.py` (churn=21)
  - `src/cpu/o3/dyn_inst.hh` (churn=21)
- 复现: `git show 2d4d1bf2bbd3a06b9b915075ecd0d833eb39f437`

### #2632 arch-arm: Add support for LRCPC instructions

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2632
- 代表 commit: `f4dc759db4a3` (2025-11-27)
- 变更规模: commits=1, files=6, +82/-7 (churn=89)
- 影响范围: topdirs=src; subsys=arch, mem; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/formats/aarch64.isa` (churn=45)
  - `src/arch/arm/isa/insts/ldr64.isa` (churn=23)
  - `src/mem/request.hh` (churn=10)
  - `src/arch/arm/ArmISA.py` (churn=6)
  - `src/arch/arm/regs/misc.cc` (churn=3)
  - `src/arch/arm/ArmSystem.py` (churn=2)
- 复现: `git show f4dc759db4a3b8e128260b037880b18287e2cffc`

### #2788 scons: Enable option when gcc only

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2788
- 代表 commit: `227a259a8780` (2025-11-29)
- 变更规模: commits=1, files=1, +7/-4 (churn=11)
- 影响范围: topdirs=src; subsys=src; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/SConscript` (churn=11)
- 复现: `git show 227a259a87808f9db24f4418f600de675e52ae34`

### #2716 python: Match walker caches to available MMU ports

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2716
- 代表 commit: `c1819b3b9e9c` (2025-11-29)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c1819b3b9e9c451472c4d98a7d0df21ff0c86d23`

### #2767 arch-riscv: Fix an issue in generateDisassembly

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2767
- 代表 commit: `89a5786249fe` (2025-11-29)
- 变更规模: commits=1, files=1, +2/-3 (churn=5)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/insts/vector.cc` (churn=5)
- 复现: `git show 89a5786249fe94f7daef5664f6fe3f0b8786c09d`

### #2301 tests: Add processor switching tests

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2301
- 代表 commit: `3d0e51367552` (2025-12-01)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 3d0e5136755274744794df9ae3abfc02c6d83e53`

### #2783 arch-riscv: fix old vd index for vslideup.vi

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2783
- 代表 commit: `aed7f2b50980` (2025-12-01)
- 变更规模: commits=1, files=1, +17/-5 (churn=22)
- 影响范围: topdirs=src; subsys=arch; arch=riscv
- 主要改动文件（Top 8 by churn）:
  - `src/arch/riscv/isa/formats/vector_arith.isa` (churn=22)
- 复现: `git show aed7f2b5098017248f67b6e5899c6952b9ee5db3`

### #2779 arch-x86: implement big movfp micro-op variant

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2779
- 代表 commit: `7185da291c8f` (2025-12-01)
- 变更规模: commits=1, files=1, +35/-14 (churn=49)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/isa/microops/fpop.isa` (churn=49)
- 复现: `git show 7185da291c8fbc784c6798918efe33432e216307`

### #2777 arch-arm: Fix switcheroo long tests for Arm

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2777
- 代表 commit: `cf3a609c56ca` (2025-12-01)
- 变更规模: commits=1, files=1, +6/-1 (churn=7)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/table_walker.cc` (churn=7)
- 复现: `git show cf3a609c56ca26dcb27648201491c81c6ca8f43f`

### #2700 misc: Allow non-serializing behaviour for MiscRegs

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2700
- 代表 commit: `9982744ac6aa` (2025-12-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 9982744ac6aa27ed70a0c23cdb949d5897525b6a`

### #2780 mem-ruby, tests: Make the TlmGenerator a ClockedObject

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2780
- 代表 commit: `12f5edceb708` (2025-12-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 12f5edceb7087f4bcfc39f67437842775a3145c4`

### #2518 cpu-o3: Bundle some Fetch/IEW/Commit stats into a vector

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2518
- 代表 commit: `33ce67ae93ee` (2025-12-02)
- 变更规模: commits=1, files=6, +199/-168 (churn=367)
- 影响范围: topdirs=src; subsys=cpu/o3; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/o3/fetch.cc` (churn=157)
  - `src/cpu/o3/iew.cc` (churn=124)
  - `src/cpu/o3/fetch.hh` (churn=34)
  - `src/cpu/o3/commit.cc` (churn=29)
  - `src/cpu/o3/iew.hh` (churn=16)
  - `src/cpu/o3/commit.hh` (churn=7)
- 复现: `git show 33ce67ae93ee298cdd3bc7737894776f66c3509b`

### #2793 arch-arm: Fix macOS build after FEAT_SVE2 PR

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2793
- 代表 commit: `94c48147b5b8` (2025-12-02)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/isa/insts/sve.isa` (churn=4)
- 复现: `git show 94c48147b5b8d3afd560cba592c61019f621dcff`

### #2782 arch-arm: Do some OpClass retagging for some SIMD instructions

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2782
- 代表 commit: `203348b99d00` (2025-12-02)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 203348b99d0085e7806be9008af53b4a272b04f6`

### #2724 cpu: Add Arm Neoverse V2 config

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2724
- 代表 commit: `51759b538e1d` (2025-12-02)
- 变更规模: commits=1, files=3, +678/-0 (churn=678)
- 影响范围: topdirs=configs, tests; subsys=configs, tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/common/cores/arm/neoverse_v2.py` (churn=369)
  - `configs/example/arm/fdp_neoverse_v2.py` (churn=265)
  - `tests/gem5/example_configs/neoverse/test_neoverse_v2.py` (churn=44)
- 复现: `git show 51759b538e1d1268acf3b3a09057be13b85a3c3e`

### #2792 misc: build(deps): bump werkzeug from 3.0.6 to 3.1.4 in /util/gem5-resources-manager

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2792
- 代表 commit: `436b2df68356` (2025-12-02)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/gem5-resources-manager/requirements.txt` (churn=2)
- 复现: `git show 436b2df683563481bfbfb1249365cecc6d014f23`

### #2791 misc: bump mypy from 1.18.2 to 1.19.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2791
- 代表 commit: `79b732723e54` (2025-12-02)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 79b732723e54487cbb67d8de6510a5a17e1a8fe3`

### #2079 util: Add validator and tests for full system workloads (disk and kernels)

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2079
- 代表 commit: `d516a6397f57` (2025-12-02)
- 变更规模: commits=1, files=7, +1248/-0 (churn=1248)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/disk-image-validator/config_tester.py` (churn=389)
  - `util/disk-image-validator/disk-image-validate.py` (churn=218)
  - `util/disk-image-validator/run_config_tests.py` (churn=171)
  - `util/disk-image-validator/README.md` (churn=155)
  - `util/disk-image-validator/helper.py` (churn=135)
  - `util/disk-image-validator/gem5-bridge-driver-validate.py` (churn=131)
  - `util/disk-image-validator/test_gem5_bridge.sh` (churn=49)
- 复现: `git show d516a6397f572bf35f0817c2cb7955519f0876a8`

### #2796 tests: Add Mac OS .opt & .fast Compilations to CI Workflow

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2796
- 代表 commit: `a9ccfef01331` (2025-12-02)
- 变更规模: commits=1, files=2, +83/-0 (churn=83)
- 影响范围: topdirs=.github; subsys=.github; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.github/workflows/daily-tests.yaml` (churn=43)
  - `.github/workflows/ci-tests.yaml` (churn=40)
- 复现: `git show a9ccfef01331c9c6072ae59873719a0bc13bb591`

### #2790 misc: bump pre-commit from 4.3.0 to 4.5.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2790
- 代表 commit: `8755c087857f` (2025-12-02)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=requirements.txt; subsys=requirements.txt; arch=-
- 主要改动文件（Top 8 by churn）:
  - `requirements.txt` (churn=2)
- 复现: `git show 8755c087857f44c5be9e2ff374f1bb6c4592cd10`

### #2795 cpu: Fix Branch Pred Simple BTB sets check

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2795
- 代表 commit: `2cfcaf532aa1` (2025-12-03)
- 变更规模: commits=1, files=1, +2/-2 (churn=4)
- 影响范围: topdirs=src; subsys=cpu/pred; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/pred/simple_btb.cc` (churn=4)
- 复现: `git show 2cfcaf532aa14a835568954e7a20ac9f6f2db16d`

### #2797 arch-riscv: Stop leaking snoop state into A/D writes

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2797
- 代表 commit: `afa0dc942bd5` (2025-12-03)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show afa0dc942bd5c6a06ff2790c53a3def8e4da52d3`

### #2789 base: Add destructors to stats objects

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2789
- 代表 commit: `948ef0bda630` (2025-12-09)
- 变更规模: commits=1, files=1, +11/-0 (churn=11)
- 影响范围: topdirs=src; subsys=base; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/base/statistics.hh` (churn=11)
- 复现: `git show 948ef0bda63051f6c42ae59b341f7e14df1ea9f1`

### #2456 cpu,sim,switch: Fix assertion failure in switchable CPUs due to doClone

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2456
- 代表 commit: `54ef7b4c8c6b` (2025-12-09)
- 变更规模: commits=1, files=2, +68/-0 (churn=68)
- 影响范围: topdirs=src; subsys=cpu, sim; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/cpu/base.hh` (churn=35)
  - `src/sim/syscall_emul.hh` (churn=33)
- 复现: `git show 54ef7b4c8c6bcab76091bad6d669d3378071597c`

### #2812 util: Bump GPU build docker to ROCm 7.0

- 动作（heuristic）: 更新/依赖
- PR 链接: https://github.com/gem5/gem5/pull/2812
- 代表 commit: `5517796a0df8` (2025-12-09)
- 变更规模: commits=1, files=2, +3/-3 (churn=6)
- 影响范围: topdirs=util; subsys=util; arch=-
- 主要改动文件（Top 8 by churn）:
  - `util/dockerfiles/gpu-fs/Dockerfile` (churn=4)
  - `util/dockerfiles/gpu-fs/README.md` (churn=2)
- 复现: `git show 5517796a0df8b6b7ec90c4504f3e26aa3de66922`

### #2813 misc: clean up devcontainer extensions and features

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2813
- 代表 commit: `67ff3e275a9a` (2025-12-09)
- 变更规模: commits=1, files=1, +3/-6 (churn=9)
- 影响范围: topdirs=.devcontainer; subsys=.devcontainer; arch=-
- 主要改动文件（Top 8 by churn）:
  - `.devcontainer/devcontainer.json` (churn=9)
- 复现: `git show 67ff3e275a9a455e28b4466492e18da5a91f3e89`

### #2802 gpu-compute: Add missing MFMA timings

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2802
- 代表 commit: `fb83f56f2f22` (2025-12-09)
- 变更规模: commits=1, files=1, +191/-124 (churn=315)
- 影响范围: topdirs=src; subsys=gpu-compute; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/gpu-compute/compute_unit.cc` (churn=315)
- 复现: `git show fb83f56f2f229edf4d334c006f1bfe57dc7ef9f8`

### #2772 arch-x86: Fix assertion due to clflush in DataTranslation::finish

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2772
- 代表 commit: `cf949085ff85` (2025-12-09)
- 变更规模: commits=1, files=1, +2/-1 (churn=3)
- 影响范围: topdirs=src; subsys=arch; arch=x86
- 主要改动文件（Top 8 by churn）:
  - `src/arch/x86/tlb.cc` (churn=3)
- 复现: `git show cf949085ff85c4d3080f50a03514bffb619b6343`

### #2827 misc: Add v25.1.0.0 details to RELEASE-NOTES.md

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2827
- 代表 commit: `2b357d595605` (2025-12-15)
- 变更规模: commits=1, files=1, +120/-0 (churn=120)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=120)
- 复现: `git show 2b357d59560534d639e4604cbe16731a1aec712d`

### #2833 misc: Split IQ from FDP in the RELEASE-STAGING.md

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2833
- 代表 commit: `7319cd594e4d` (2025-12-15)
- 变更规模: commits=1, files=1, +9/-8 (churn=17)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=17)
- 复现: `git show 7319cd594e4d7f41862608995d473536a59960fa`

### #2834 misc: Rework Arm improvements

- 动作（heuristic）: 重构/整理
- PR 链接: https://github.com/gem5/gem5/pull/2834
- 代表 commit: `c0e236575e4a` (2025-12-16)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show c0e236575e4a9f7089184f64fbc3bd2c0ea27480`

### #2840 misc: Remove CPU/MMU repetition

- 动作（heuristic）: 移除/弃用
- PR 链接: https://github.com/gem5/gem5/pull/2840
- 代表 commit: `9036542bf23d` (2025-12-17)
- 变更规模: commits=1, files=1, +0/-12 (churn=12)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=12)
- 复现: `git show 9036542bf23d112cf75c368be66e9096ccf7af1f`

### #2853 misc: Add non-serializing MiscReg contribution to RELEASE-NOTES.md

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2853
- 代表 commit: `d29370b1f4bc` (2025-12-23)
- 变更规模: commits=1, files=1, +5/-0 (churn=5)
- 影响范围: topdirs=RELEASE-NOTES.md; subsys=RELEASE-NOTES.md; arch=-
- 主要改动文件（Top 8 by churn）:
  - `RELEASE-NOTES.md` (churn=5)
- 复现: `git show d29370b1f4bcae6ced5542893ddc644842f096ae`

### #2839 arch-arm: Define Armv90 and Armv94 release objects

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2839
- 代表 commit: `d51ce2791ebf` (2025-12-23)
- 变更规模: commits=1, files=1, +20/-2 (churn=22)
- 影响范围: topdirs=src; subsys=arch; arch=arm
- 主要改动文件（Top 8 by churn）:
  - `src/arch/arm/ArmSystem.py` (churn=22)
- 复现: `git show d51ce2791ebf46e67be10240341ff7c0129c1c67`

### #2829 mem: Fix asan error in backdoor test

- 动作（heuristic）: 测试
- PR 链接: https://github.com/gem5/gem5/pull/2829
- 代表 commit: `4a44b41d56e8` (2025-12-30)
- 变更规模: commits=1, files=1, +8/-8 (churn=16)
- 影响范围: topdirs=src; subsys=mem; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/backdoor_manager.test.cc` (churn=16)
- 复现: `git show 4a44b41d56e838dae1cfdea3efad04eff9182141`

### #2852 misc: Add a PyPort to write to physmem from python

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2852
- 代表 commit: `186a6ed39809` (2025-12-30)
- 变更规模: commits=1, files=7, +256/-3 (churn=259)
- 影响范围: topdirs=src, tests; subsys=python, sim, tests; arch=-
- 主要改动文件（Top 8 by churn）:
  - `tests/gem5/py_port/read-write.py` (churn=102)
  - `src/python/pybind11/port.cc` (churn=83)
  - `tests/gem5/py_port/test.py` (churn=50)
  - `src/python/SConscript` (churn=13)
  - `src/python/pybind11/pybind.hh` (churn=4)
  - `src/sim/init.cc` (churn=4)
  - `src/sim/System.py` (churn=3)
- 复现: `git show 186a6ed39809212a7bd36aeed215cd8afc2b07e4`

### #2826 stdlib, resources: Fix `list_resources` by calling a new endpoint

- 动作（heuristic）: 新增/支持
- PR 链接: https://github.com/gem5/gem5/pull/2826
- 代表 commit: `40bb5e5b6be0` (2025-12-30)
- 变更规模: commits=1, files=4, +69/-1 (churn=70)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/resources/client_api/jsonclient.py` (churn=38)
  - `src/python/gem5/resources/client_api/azure_functions_client.py` (churn=17)
  - `src/python/gem5/resources/client_api/abstract_client.py` (churn=11)
  - `src/python/gem5/resources/client.py` (churn=4)
- 复现: `git show 40bb5e5b6be05accfdd50c99f734dd12e6e1e2c1`

### #2846 mem-cache: fix memory leakage in fetch directed prefetcher

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2846
- 代表 commit: `2c4b047f5ef1` (2025-12-30)
- 变更规模: commits=1, files=1, +4/-0 (churn=4)
- 影响范围: topdirs=src; subsys=mem/cache/prefetch; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/prefetch/fdp.cc` (churn=4)
- 复现: `git show 2c4b047f5ef1e1e7532e719ddb884a26b6fa5e12`

### #2824 mem-cache: fix segmentation fault caused by MSHR debug flag

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2824
- 代表 commit: `bfdea367e668` (2025-12-30)
- 变更规模: commits=1, files=2, +5/-2 (churn=7)
- 影响范围: topdirs=src; subsys=mem/cache; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/mem/cache/mshr.hh` (churn=6)
  - `src/mem/cache/cache.cc` (churn=1)
- 复现: `git show bfdea367e66816e77cd8c6b23430e25fcd77f54f`

### #2800 configs: Fix tuple unpacking error in Simulation.py with fast-forward

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2800
- 代表 commit: `3435fee529ca` (2025-12-30)
- 变更规模: commits=1, files=1, +1/-1 (churn=2)
- 影响范围: topdirs=configs; subsys=configs; arch=-
- 主要改动文件（Top 8 by churn）:
  - `configs/common/Simulation.py` (churn=2)
- 复现: `git show 3435fee529cae54e6a5df19e172bf06bbc4bfd45`

### #2836 stdlib: Fix multiprocessing stat file name

- 动作（heuristic）: 修复/纠错
- PR 链接: https://github.com/gem5/gem5/pull/2836
- 代表 commit: `4b05cf8086d2` (2025-12-30)
- 变更规模: commits=1, files=1, +3/-2 (churn=5)
- 影响范围: topdirs=src; subsys=python; arch=-
- 主要改动文件（Top 8 by churn）:
  - `src/python/gem5/utils/multiprocessing/_command_line.py` (churn=5)
- 复现: `git show 4b05cf8086d2fc821592130998468eb2b4e8c752`

### #2803 misc: Release v25.1.0.0

- 动作（heuristic）: 其他
- PR 链接: https://github.com/gem5/gem5/pull/2803
- 代表 commit: `7a2b0e413d06` (2025-12-31)
- 变更规模: commits=1, files=0, +0/-0 (churn=0)
- 影响范围: topdirs=-; subsys=-; arch=-
- 复现: `git show 7a2b0e413d06c5ce7097104abef3b1d9eaabca91`

