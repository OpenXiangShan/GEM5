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
- RTL lifetime 重跑必须以 `WITH_CHISELDB=1` 构建，并在 emu 命令中显式传入 `--dump-db --dump-select-db lifetime --db-path <trace.db>`；普通 `--dump-db` 生成的 DB 不能直接当作 lifetime trace。

## 标准分析闭环

分析依据从用户给出的 `GEM5_RESULT` 和 `RTL_RESULT` 开始；`ARTIFACT_ROOT`/`CASE_ID` 只决定报告存放位置，已有结果分析阶段不要求 commit/worktree。按以下顺序执行：

1. 验证两侧后 20M window，精确匹配共同 slice，按权重计算并报告所有 slice 对子项总 cycle gap 的贡献（从高到低）。
2. 使用 `gem5_data_proc` 做通用 counter 初筛，给出 frontend、backend、memory、IQ/FU、replay 等方向的初步定位；此阶段只形成假设，不宣称已证明原因。
3. 检查两侧 lifetime DB。两侧均合格时，用 `ClockAnalysis.py` 分别处理 GEM5 和 RTL trace，按 inter-gap/累计影响找出主要 basic block、PC、opcode 和指令类别，并把热点写入报告告知用户。任一侧缺失或不合格时，标记 trace 缺失，不运行指令级结论，并只重跑缺失的一侧：RTL baseline 最多一次；GEM5 则先用未修改的 baseline commit 生成一次 trace，之后所有 candidate 也必须生成 trace。
4. 根据热点指令/指令类别做定向 counter 分析，把热点的 inter-gap、计数器差异和 slice 总 gap 关联起来。
5. 在提出任何 GEM5 修改前，必须阅读 GEM5 和 RTL 对应源码，报告文件/行号、实际行为、时序/资源约束差异、已有 counter/trace 证据和预期影响。仅为了让 inter-gap 数值接近、但没有源码行为差异证据时，不得修改。
6. 用户确认后（或显式 `MODE=autonomous-iterate`）只修改 GEM5，在隔离 worktree 中编译并重跑目标 slice；每次 candidate 都必须生成后 20M stats、counter、lifetime DB 和 GEM5 `ClockAnalysis.py` inter-gap 报告。RTL baseline 不修改、不重复运行。
7. 对比修改前后热点 inter-gap、定向 counter、slice contribution 和子项总 gap。若子项加权 cycle gap 小于 5% 且 correctness 正常，则标记 `converged`；用户没有要求继续时停止，否则继续分析剩余差异。

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

用户提供 GEM5 结果目录、RTL 结果目录、artifact 根目录和 case id。按 [标准分析闭环](#标准分析闭环) 执行；commit/worktree/checkpoint/SO 仅在需要重跑时再收集。

1. 读取 [analysis-rules](references/analysis-rules.md)，执行 window、slice、权重和 counter 规则。
2. 用 `parse_gem5_stats.py`、`parse_rtl_results.py` 和 `emit_slice_gap_contribution.py` 生成初始结果报告。
3. 读取 [trace-and-inter-gap](references/trace-and-inter-gap.md)；若 trace 合格则处理两侧 DB，若缺失则只重跑缺失一侧的 lifetime trace。
4. 找到热点后执行定向 counter 分析和源码行为差异归因，再决定是否提出 GEM5 修改。

先读 weighted summary；只有定位具体 slice、counter、PC 或事件时才打开完整 CSV/TXT/DB。

## 重跑与 A/B

默认 `manual` 模式：先给出单变量方案并等待用户确认。读取 [rerun-commands](references/rerun-commands.md) 前，用户必须提供两侧 commit/worktree 和目标 slice；其中定义的 preflight evidence 必须完整。

显式 `MODE=autonomous-iterate` 才开启自动模式，默认关闭。读取 [iteration-rules](references/iteration-rules.md)：冻结一份 RTL baseline（已有合格 DB 直接复用，否则最多运行一次 RTL primary-slice trace），之后只在隔离 GEM5 candidate worktree 中自动执行“源码差异确认 → 单变量修改 → 编译 → 40M/20M 重跑 → lifetime/inter-gap → 与冻结 RTL 比较”。不修改或重复运行 RTL，不自动 commit/push/PR/远程 CI。

两种模式都必须比较 post-warmup IPC/cycles、相关 counters、关键 inter-gap/commit-gap、weighted cycle gap 和 correctness；报告必须包含 slice 贡献、热点指令、源码实现差异和修改前后变化，结论区分 `Confirmed`、`Supported hypothesis`、`Unresolved`、`converged`。

## 按需参考

- [analysis-rules](references/analysis-rules.md)：窗口、slice、权重、counter 与可比性。
- [trace-and-inter-gap](references/trace-and-inter-gap.md)：DB 检查、`ClockAnalysis.py` 和 trace 缺失处理。
- [rerun-commands](references/rerun-commands.md)：profile/checkpoint preflight、GEM5/RTL 命令。
- [iteration-rules](references/iteration-rules.md)：自动模式预算、RTL 冻结和停机条件。
