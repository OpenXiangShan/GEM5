---
name: gem5-rtl-performance-alignment
description: 对齐 GEM5 与 XiangShan RTL 同一 benchmark/slice 的性能差异。适用于后 20M 计数器、权重贡献、lifetime trace/inter-gap 和代码归因；默认逐轮确认，只有用户显式开启 autonomous-iterate 才自动修改和重跑 GEM5。
---

# GEM5 / RTL 性能对齐

## 不变量

- 默认比较总 40M committed instructions 的后 20M：GEM5 取第二个 stats block，RTL 必须证明 warmup reset 或做 40M-20M 差分。
- 先匹配共同 slice 并输出全部 slice 的带权 cycle gap，再做计数器和 trace 初筛。
- 计数器优先用 `gem5_data_proc`；lifetime DB 优先用 `ClockAnalysis.py` 做 inter-gap/BB 聚合。
- 已有结果分析不推断 commit、checkpoint 或输入；重跑时 commit 必须由用户提供。
- 所有产物写入 `<ARTIFACT_ROOT>/<CASE_ID>/`，不写 source worktree。

## 配置权威来源

workflow 只负责组合输入和调用运行器；重跑时按下面的源码入口解析实际配置，不依赖旧版 workflow 中的 `case` 分支：

```text
GEM5 benchmark/checkpoint: util/xs_scripts/perf_benchmarks.py
GEM5 reference SO:         util/nemu_ref/resolve.py + lock.json
RTL checkpoint mapping:    .github/workflows/perf-template.yml
RTL CI runner:             /nfs/home/share/ci-workloads/env-scripts/perf_trigger/main.py
RTL build:                 scripts/xiangshan.py 或用户确认的 make emu 命令
```

GEM5 的 `cluster_config` 用于评分/集群配置，不等同于 checkpoint 列表或 checkpoint source JSON。RTL workflow 可能将 reference SO 复制到 slice 结果目录后再运行，必须同时记录源文件和运行副本。

## 默认：分析已有结果

用户提供 GEM5 结果目录、RTL 结果目录、artifact 根目录和 case id。按顺序执行：

1. 读取 [analysis-rules](references/analysis-rules.md)，按精确的 benchmark/point 匹配共同 slice，忽略 RTL 多出的 slice。
2. 用 `parse_gem5_stats.py` 与 `parse_rtl_results.py` 验证每个共同 slice 的后 20M window。
3. 用 `emit_slice_gap_contribution.py` 输出完整 `reports/slice_gap_contribution.csv` 和 `slice_gap_summary.json`；按 `weight * (GEM5_cycles - RTL_cycles)` 排序，保留正贡献、负贡献和 excluded。
4. 用 `gem5_data_proc` 做 counter 初筛，并记录映射、分母和最大的正/负差异。
5. 检查 lifetime DB；双方 DB 都可读且覆盖后 20M 时，读取 [trace-and-inter-gap](references/trace-and-inter-gap.md)，对 GEM5 使用 `ClockAnalysis.py`。

先读 weighted summary；只有定位具体 slice、counter、PC 或事件时才打开完整 CSV/TXT/DB。

## 重跑与 A/B

默认 `manual` 模式：先给出单变量方案并等待用户确认。读取 [rerun-commands](references/rerun-commands.md) 前，用户必须提供两侧 commit/worktree 和目标 slice；其中定义的 preflight evidence 必须完整。

显式 `MODE=autonomous-iterate` 才开启自动模式，默认关闭。读取 [iteration-rules](references/iteration-rules.md)：冻结一份 RTL baseline（已有合格 DB 直接复用，否则最多运行一次 RTL primary-slice trace），之后只在隔离 GEM5 candidate worktree 中自动执行“单变量修改 → 编译 → 40M/20M 重跑 → lifetime/inter-gap → 与冻结 RTL 比较”。不修改或重复运行 RTL，不自动 commit/push/PR/远程 CI。

两种模式都必须比较 post-warmup IPC/cycles、相关 counters、关键 inter-gap/commit-gap、weighted cycle gap 和 correctness；结论区分 `Confirmed`、`Supported hypothesis`、`Unresolved`。

## 按需参考

- [analysis-rules](references/analysis-rules.md)：窗口、slice、权重、counter 与可比性。
- [trace-and-inter-gap](references/trace-and-inter-gap.md)：DB 检查、`ClockAnalysis.py` 和 trace 缺失处理。
- [rerun-commands](references/rerun-commands.md)：profile/checkpoint preflight、GEM5/RTL 命令。
- [iteration-rules](references/iteration-rules.md)：自动模式预算、RTL 冻结和停机条件。
