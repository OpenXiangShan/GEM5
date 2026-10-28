# 本地窗口口径纠正审计

**此前将若干结果称为“20M 预热后 2M ROI”是错误的。** 它们实际在预热阈值之前就达到总指令退出上限，因而测量的是 checkpoint 恢复后的前约 2M 指令。可作为冷启动条件下的局部机制探针，不能当正式预热窗口，更不能外推 SPEC 分数。逐目录 config、完整块、simInsts 和日志记录见 [local-window-correction-audit.json](local-window-correction-audit.json)。本审计未修改 repo，也未重跑模拟。

## 原因与实证

`configs/common/Simulation.py:784-786,818-820` 分别把 `--maxinsts` 和 `--warmup-insts-no-switch` 写入 CPU 参数；`src/cpu/base.cc:369-372` 按 max 指令数安排退出；`src/cpu/o3/cpu.cc:1485-1493` 只有提交达到 warmup 时才打印并安排 dump/reset。单核本流程的 max 是从恢复起的总提交上限，不是 reset 后 ROI 长度。

| 目录 | 检查组数 | warmup / max 参数 | 完整块 | warmup 日志 | 实际含义 |
|---|---:|---|---|---|---|
| `tea-roi` | 4 | 20M / 2M | 每组 1 | 均无 | 恢复后前约 2M；不是正式 ROI |
| `formal-frontend-ablations` | 12 | 20M / 2M | 每组 1 | 均无 | 恢复后前约 2M；不是正式 ROI |
| `override-separation` | 4 | 20M / 2M | 每组 1 | 均无 | 恢复后前约 2M；不是正式 ROI |
| `final-twelve-tea` | 5 已有 config | 20M / 20M | 1 组一个完整块，4 组无完整块 | 完成组有 | 完成组只到预热边界，尚无预热后的测量窗口 |

上述前三类全部自然达到 max 指令退出，但正常退出和 difftest 通过不证明已完成预热。每块 `simInsts` 与 `system.cpu.committedInsts` 约 2M，实际值因批提交边界多至数条。`final-twelve-tea/perlbench_checkspam_5886/baseline/tea` 为 20,000,011 条、4,429,945 cycles，精确等于 full CI run1667 该点**第一个**块；CI 第二块 19,999,994 条、4,102,519 cycles 才是预热后正式 ROI。到达 warmup 阈值时打印日志并不足以证明已经采集第二个块。

## 受影响的结果

以下是已检查出的直接受影响入口；不声称穷尽所有引用：

- repo `docs/experiments/tea-upperbound-20261028/round-03-tea.md` 的 gcc/mcf “正式预热”TEA 及其 `data/tea-gcc_expr_1687*.{json,csv}`、`data/tea-mcf_12886*.{json,csv}`；`round-02.md:38`、`plan.md:52` 的转述；`/tmp/tea-upperbound-20261028/tea-report/{gcc_expr_1687,mcf_12886}.md`。TEA 守恒与同冷窗口比较仍可作为局部观测，不支持正式 ROI 的表述。
- repo `round-06-frontend-roi.md`、`data/formal-frontend-ablations.json`、`data/formal-front-ablation-audit.json` 及其图；`plan.md:53`；临时 `formal-front-ablation-audit.md/.json`。原条件 IPC、分支和开关激活数据只代表冷启动窗口；不可称为“预热 20M 后的效果”。此前我完成的独立审计误将 `warmupInstCount=20M` 当作已经执行预热，此结论作废。
- repo `round-07-override.md` 中本地 2M 三档比较、`data/override-separation-local.json`、`plan.md:53`：A/B/C 同为冷窗口，不是正式预热窗口；逐项参数与动态启用检查仍是局部证据。R7 完整 benchmark 的正规 CI 结果需与该局部结果分开。
- `/tmp/tea-upperbound-20261028/tea-residual-plan.md` 的命令模板错误：`--warmup-insts-no-switch=20000000 --maxinsts=2000000` 必须纠正后使用；此前建议的指标/守恒方法不因窗口错误自动失效。
- `final-twelve-tea` 的预热段单块以及引用它作为 ROI 的任何中间分析不得作为正式热点结果；尚未产生完整块的运行不形成结果。

已有 full CI 145 点、20M 预热＋20M ROI 的正式统计不受本次局部脚本错误影响；run1667 示例的第二块已独立核对。不能把冷窗口百分比与完整 CI 加权结果相加或等同。

## 后续验收门槛

1. 显式验证命令与 INI：单核非切换流程设 `warmupInstCount=W`、`max_insts_any_thread=W+ROI`；若 `W=20M, ROI=2M` 则 max=22M；若与 20M CI 正式窗口对齐则 max=40M。提交批次边界的少量超出需保留实际计数。
2. 运行正常退出、difftest 开启且无 mismatch；日志确实出现 warmup dump/reset。采集至少两个**完整** stats blocks，第一个为预热、最后一个为 reset 后 ROI；仅一个块即拒绝，无论预热日志是否出现。
3. 两块分别检查 `simInsts`、`system.cpu.committedInsts`，最后块接近所需 ROI，记录 `numCycles`；TEA 每周期采样时同时校验 samples、四状态、CSV 权重/事件与特殊 PSV 守恒。
4. 若与同配置同 checkpoint 完整 CI 对照，使用相同 binary/REF/参数及同长度窗口，比较首块和末块的 cycles/insts 及关键计数；2M ROI 不能直接与 20M ROI 周期逐值比较。任何不一致先确认 phase、配置和 observer equivalence，不以 normal exit 自动认定有效。
