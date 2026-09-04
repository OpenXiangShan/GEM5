---
name: mgsc-table-probe
description: 分析香山 MGSC/SC 在前端微测试上的效果。适用于以下场景：(1) 用 off/l_only/g_only/i_only/full 等 A/B profile 批量运行 mgsc_test；(2) 比较不同 profile 下的 topMispredictsByBranch.csv 和 stats.txt；(3) 使用 bp.db 里的 MGSCTRACE 将每个分支的收益/损失归因到具体 SC 表；(4) 决定如何为 Global 或 IMLI 表设计新的测试。
---

# MGSC 表探测

## 概览
运行标准化的 SC 表 A/B 实验，并产出分支级别的归因结果：
- `summary.csv`：case 级别的 `off` 与各 profile 的 delta 汇总。
- `branch_delta.csv`：分支级别的误预测 delta，以及 SC 修复/伤害与表贡献比例。
- `report.md`：便于人工快速阅读的排序报告。

这个 skill 的目标是快速迭代 SC 测试质量，而不是做完整的性能调优。

## 快速开始

1) 对所有 `mgsc_test` 二进制执行 probe：
```bash
python3 .agents/skills/mgsc-table-probe/scripts/mgsc_table_probe.py \
  --outdir debug/sc_table_probe \
  --profiles off,l_only,g_only,i_only,full \
  --max-workers 4
```

2) 快速检查单个 case：
```bash
python3 .agents/skills/mgsc-table-probe/scripts/mgsc_table_probe.py \
  --outdir debug/sc_table_probe_smoke \
  --tests fp_sc_alias_pair \
  --profiles off,g_only,i_only \
  --max-workers 1
```

3) 仅重建报告（不重新运行 gem5）：
```bash
python3 .agents/skills/mgsc-table-probe/scripts/mgsc_table_probe.py \
  --outdir debug/sc_table_probe \
  --profiles off,l_only,g_only,i_only,full \
  --skip-run
```

## 路径解析

- GEM5 仓库：优先使用 `GEM5_HOME`，其次从当前目录和脚本位置向上查找 `SConstruct` 与 `configs/example/kmhv3.py`。
- MGSC 测试：优先使用 `AM_HOME`；未设置时才采用本机默认 `/nfs/home/yanyue/tools/nexus-am`。
- 所有路径都可用 `--gem5-bin`、`--config`、`--cpt-dir` 和 `--src-dir` 显式覆盖。

个人 home 路径只是方便本机使用的 fallback，不是跨机器前提。脚本会在创建运行任务前检查 checkpoint 目录、gem5 binary 和配置文件。

## 工作流

1) **基线 + 单表隔离 profile**
- 始终包含 `off`。
- 在检查 `full` 之前，先加入单表 profile（如 `g_only`、`i_only`）。

2) **快速筛选有价值的测试**
- 在 `summary.csv` 中，优先关注满足 `condMiss_delta < 0` 且 `mgsc_net_use > 0` 的 case。

3) **筛选有价值的分支**
- 在 `branch_delta.csv` 中，优先关注满足以下条件的行：
  - `delta_misp < 0`
  - `focus_decisive_ratio` 较高
  - `focus_agree_fix_ratio` 较高

4) **决定下一步微测试方向**
- 如果 `g_only` 很少带来改善，且 `focus_decisive_ratio(g)` 很低，说明 global-history 模式较弱。
- 如果 `i_only` 从未带来帮助，说明循环/迭代相位信号暴露得还不够。
- 可以参考 `references/test-patterns.md` 里的模式来编写下一个测试。

## 输出

- `debug/sc_table_probe/summary.csv`
- `debug/sc_table_probe/branch_delta.csv`
- `debug/sc_table_probe/report.md`
- `debug/sc_table_probe/report.json`

## 关键参数

- `--profiles`：从 `off,l_only,g_only,i_only,full` 中选择。
- `--tests`：逗号分隔的测试名（不带后缀），例如 `fp_sc_alias_pair,imli_iter`。
- `--extra-param`：透传额外的 gem5 `--param`。
- `--copy-cpt-to-tmp`：避免路径访问问题。
- `--skip-run`：仅生成报告，不执行运行。

## 注意事项

- 除非你明确要评估交互效应，否则在做 SC 子表归因时应保持 `microtage` 关闭。
- 不同 profile 之间必须使用同一组 checkpoint，否则 delta 无效。
- 做 branch PC 映射时，使用 mgsc_test 构建目录下的 `*-riscv64-xs.txt` 反汇编文件。

## 参考资料

- 关于面向 G/IMLI 的微测试模式，参见 `references/test-patterns.md`。
- `scripts/mgsc_trace_report.py` 可用于对已有 `bp.db` 做更细的 branch/run 级门限与表贡献报告；它不负责重跑 gem5。
