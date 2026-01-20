# upstream 变更总结（相对 OpenXiangShan/GEM5 分叉点）

## 摘要

- 分叉基线：`a3be84cb1b854da51716d6399ca139016714bd54`（tag: `v22.0.0.1`）
- upstream 参考：`upstream/stable` = `7a2b0e413d06c5ce7097104abef3b1d9eaabca91`
- upstream 最近 tag：`v25.1.0.0`
- upstream 相对基线新增 commit：**4495**（merge commits: 598）
- 与 `origin/xs-dev` 的对比：origin-only=1889，upstream-only=4495

## 复现步骤（可重复）

```bash
git fetch --all --prune
git merge-base origin/xs-dev upstream/stable
git rev-list --left-right --count origin/xs-dev...upstream/stable
python3 /nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/.codex/reports/upstream_since_fork_a3be84c/scripts/generate_upstream_report.py \
  --origin-ref origin/xs-dev --upstream-ref upstream/stable --base a3be84cb1b854da51716d6399ca139016714bd54 \
  --outdir /nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/.codex/reports/upstream_since_fork_a3be84c
```

## 统计概览

- 时间跨度（commit date）：`2022-06-29T12:40:28+00:00` -> `2025-12-31T13:13:38-08:00`
- 贡献者（author 去重）：229

## Release 里程碑（tag）

| tag | date | commit | subject |
|---|---|---|---|
| `v22.0.0.1` | 2022-06-18 | `39f85b7a3be1` | misc: Update version info to v22.0.0.1 |
| `v22.0.0.2` | 2022-07-27 | `1d03f6de9415` | misc: Update RELEASE-NOTES.md for v22.0.0.2 |
| `v22.1` | 2022-12-30 | `5fa484e2e026` | misc: Merge the v22.1 release staging into stable |
| `v22.1.0.0` | 2022-12-30 | `5fa484e2e026` | misc: Merge the v22.1 release staging into stable |
| `v23.0` | 2023-08-11 | `6835f0665744` | misc: Minor release v23.0.1.0 (#174) |
| `v23.0.0.0` | 2023-07-07 | `1db206b9d371` | misc: Merge branch 'release-staging-v23-0' into stable |
| `v23.0.0.1` | 2023-07-10 | `af72b9ba5805` | misc: Update RELEASE-NOTES.md for v23.0.0.1 hotfix |
| `v23.0.1.0` | 2023-08-11 | `6835f0665744` | misc: Minor release v23.0.1.0 (#174) |
| `v23.1` | 2023-12-28 | `bae34876780d` | misc: Merge `release-staging-v23-1` into `stable` (#711) |
| `v23.1.0.0` | 2023-12-28 | `bae34876780d` | misc: Merge `release-staging-v23-1` into `stable` (#711) |
| `v24.0` | 2024-08-08 | `b1a44b89c7ba` | misc: v24.0.0.1 Hotfix release (#1425) |
| `v24.0.0.0` | 2024-06-27 | `43769abaf051` | misc: Merge v24.0 release staging branch to stable (#1274) |
| `v24.0.0.1` | 2024-08-08 | `b1a44b89c7ba` | misc: v24.0.0.1 Hotfix release (#1425) |
| `v24.1` | 2025-04-11 | `b9da2bfe1e21` | misc: Hotfix v24.1.0.3 (#2177) |
| `v24.1.0.0` | 2024-12-07 | `63d25922a2db` | tests: Update pyunit tests references to include 24.1 (#1843) |
| `v24.1.0.1` | 2024-12-19 | `c9625ce9cc5b` | v24.1.0.1 Hotfix Release  (#1875) |
| `v24.1.0.2` | 2025-02-12 | `186a913a48f1` | misc: Hotfix v24.1.0.2 (#1964) |
| `v24.1.0.3` | 2025-04-11 | `b9da2bfe1e21` | misc: Hotfix v24.1.0.3 (#2177) |
| `v25.0` | 2025-08-11 | `ddd4ae35adb0` | misc: Hotfix 25.0.0.1 (#2496) |
| `v25.0.0.0` | 2025-06-18 | `d22064c1c05f` | misc: Release v25.0.0.0 (#2322) |
| `v25.0.0.1` | 2025-08-11 | `ddd4ae35adb0` | misc: Hotfix 25.0.0.1 (#2496) |
| `v25.1` | 2025-12-31 | `7a2b0e413d06` | misc: Release v25.1.0.0 (#2803) |
| `v25.1.0.0` | 2025-12-31 | `7a2b0e413d06` | misc: Release v25.1.0.0 (#2803) |

## 可视化

![每月 commit 数](figures/commits_per_month.png)

![每月 PR 数](figures/prs_per_month.png)

![Top topic 前缀](figures/top_topics.png)

![Arch 活跃度](figures/arch_activity.png)

![Top-level 目录活跃度](figures/topdirs_activity.png)

## 主要改动主题（commit message 前缀 Top 15）

| topic | commits |
|---|---:|
| `misc` | 480 |
| `arch-arm` | 401 |
| `arch-riscv` | 363 |
| `stdlib` | 269 |
| `tests` | 253 |
| `mem-ruby` | 196 |
| `cpu` | 173 |
| `configs` | 147 |
| `arch-vega` | 146 |
| `no-prefix` | 117 |
| `util` | 111 |
| `scons` | 106 |
| `base` | 102 |
| `dev-amdgpu` | 93 |
| `mem-cache` | 91 |

## 主要改动子系统（unique commits Top 15）

| subsystem | unique commits |
|---|---:|
| `arch` | 1072 |
| `python` | 508 |
| `configs` | 351 |
| `mem` | 337 |
| `tests` | 325 |
| `util` | 261 |
| `.github` | 247 |
| `dev` | 230 |
| `cpu` | 199 |
| `base` | 174 |
| `src` | 153 |
| `sim` | 147 |
| `cpu/o3` | 131 |
| `gpu-compute` | 89 |
| `cpu/simple` | 75 |

## 主要改动架构（src/arch/<arch> unique commits Top 15）

| arch | unique commits |
|---|---:|
| `arm` | 480 |
| `riscv` | 375 |
| `amdgpu` | 151 |
| `x86` | 124 |
| `sparc` | 49 |
| `mips` | 45 |
| `power` | 44 |
| `generic` | 35 |
| `isa_parser` | 22 |
| `SConscript` | 8 |
| `micro_asm.py` | 7 |
| `null` | 4 |
| `micro_asm_test.py` | 4 |
| `Kconfig` | 2 |
| `SConsopts` | 1 |

## 上游 release highlights（摘自 RELEASE-NOTES.md）

### Version 25.1

- Neoverse V2 core model.
- New branch predictor.
- Towards Armv9 support with a full FEAT_SVE2 implementation.
- Decoupled front end and fetch-directed prefetcher (FDP).
- Distributed instruction/issue queue.
- Non-serializing behavior for O3CPU MiscRegClass registers.
- Improved Arm table-walk machinery.
- Multiple GPUs and configurable GPU memory size.
- Improved statistics infrastructure.

### Version 25.0

- Hypercalls and New Exit Event Handlers
- Improved RISC-V and Arm ISA Support
- Python Utilities
- OptionalParam and DictParam

## 逐 commit 细节

- 每个 commit 一行说明：`commits.md`
- 每个 commit 详细摘要：`commits_detailed_zh.md`
- PR 汇总（按 release tag）：`prs_by_release_zh.md`
- PR 一句话摘要（按 release tag）：`prs_one_liner_by_release_zh.md`
- PR 逐条摘要：`prs_detailed_zh.md`
- release 版本摘要（按 tag/RELEASE-NOTES）：`releases_zh.md`
- 重点目录聚合：`focus_subsystems_zh.md`
- 人类友好入口：`overview_zh.md`
- 原始数据：`data/commits.csv`（commit 元信息 + 聚合 numstat）
- 原始数据：`data/files.csv`（逐文件 numstat，体积较大）
- 原始数据：`data/prs.csv`（PR 聚合数据）
