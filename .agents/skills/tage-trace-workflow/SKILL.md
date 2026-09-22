---
name: tage-trace-workflow
description: 采集或分析 gem5/XiangShan TAGE trace，比较统计、热点分支和事件分叉。
---

# TAGE Trace Workflow

## 按现有产物选择入口

- 已有 `stats.txt`、`bp.db` 或 RTL sqlite：直接使用下方分析脚本，不重新编译或采集。
- 缺少所需 trace：读取 [capture-trace.md](references/capture-trace.md)，先复用版本和表支持匹配的 binary。
- 需要构建 RTL trace binary：读取 [build-trace.md](references/build-trace.md)。

比较前记录两边 commit、输入 checkpoint、配置和统计窗口；检查 restore/warmup 边界，相同 `-I` 不足以证明窗口一致。只生成结论所需的表和有界窗口。

## 分析顺序

先看计数器，再看 trace：

1. gem5 `stats.txt` 中的
   `updateAllocSuccess / updateAllocFailure / updateAllocFailureNoValidTable / updateResetU / resolveBranchHasProvider / resolveBranchUseProvider / resolveBranchHasAlt / resolveBranchUseAltTable / resolveBranchUseBaseTable / mispredictBranch*`
2. RTL `CondTrace_*` 聚出来的同口径总量
3. 再看热点 branch 的 `provider/alt/useAlt/alloc`

统计名会随 gem5 commit 变化；以上是当前 checkout 的名字，分析历史归档时要回到对应 commit 确认递增语义。

注意两个口径坑：

- `TAGEMISSTRACE.useAlt` 表示 `pred.useAlt`，不等于 stats 里的 `resolveBranchUseAltTable`
- 要和 stats 对齐时，应该优先看 `useAlt && altFound`

## 脚本

- RTL 聚合：
  [scripts/aggregate_rtl_condtrace.py](scripts/aggregate_rtl_condtrace.py)
- gem5/RTL 对拍：
  [scripts/compare_gem5_rtl_tage.py](scripts/compare_gem5_rtl_tage.py)
- 热点 branch 画像对拍：
  [scripts/compare_branch_profiles.py](scripts/compare_branch_profiles.py)
- 单个 branch 的顶层事件序列粗对拍：
  [scripts/compare_branch_event_sequences.py](scripts/compare_branch_event_sequences.py)
- 高表 alloc 生命周期分析：
  [scripts/analyze_alloc_lifecycle.py](scripts/analyze_alloc_lifecycle.py)
- prediction-time PHR contributor 归类：
  [scripts/analyze_phist_contributor.py](scripts/analyze_phist_contributor.py)
- RTL `BpuTrainTrace` 的 `self / sibling / none` block-level 贡献者归类：
  [scripts/analyze_rtl_train_contributor.py](scripts/analyze_rtl_train_contributor.py)
- gem5 `BPTRACE` 上下文窗口分析：
  [scripts/analyze_bptrace_context.py](scripts/analyze_bptrace_context.py)
- bucket 结果稳定性比较：
  [scripts/analyze_bucket_stability.py](scripts/analyze_bucket_stability.py)
- path-history bucket 聚合：
  [scripts/analyze_phistory_buckets.py](scripts/analyze_phistory_buckets.py)

常用命令：

```bash
python3 .agents/skills/tage-trace-workflow/scripts/aggregate_rtl_condtrace.py \
  --rtl-db /path/to/rtl.db --top 12

python3 .agents/skills/tage-trace-workflow/scripts/compare_gem5_rtl_tage.py \
  --gem5-stats /tmp/debug/coremark_200k_basic/stats.txt \
  --gem5-bpdb /tmp/debug/coremark_200k_basic/bp.db \
  --gem5-top-branch-csv /tmp/debug/coremark_200k_basic/topMispredictsByBranch.csv \
  --rtl-db /path/to/rtl.db \
  --top 12
```
