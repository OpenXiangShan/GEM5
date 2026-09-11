# 分析规则

## Window

正式口径是总 40M、warmup 前 20M、measurement 后 20M。

- GEM5 必须恰有两个 stats block，只取第二段：`IPC = committedInsts / numCycles`。
- RTL 的 `clock_cycle`/`commitInstr` 可能是 reset 后值或累计值。没有日志、源码或 20M/40M 边界证据时，不参与精确比较。

```bash
python3 "<SKILL_DIR>/scripts/parse_gem5_stats.py" "<GEM5_RESULT>/stats.txt" \
  --output "<ARTIFACT_ROOT>/<CASE_ID>/counters/gem5/measurement.json"
python3 "<SKILL_DIR>/scripts/parse_rtl_results.py" "<RTL_RESULT>" \
  --window-semantics reset-after-warmup \
  --output "<ARTIFACT_ROOT>/<CASE_ID>/counters/rtl/measurement.json"
```

累计 RTL counter 只能使用显式的 `cumulative-at-boundaries` 差分；不要把两个 GEM5 stats block 相加或平均。

## Slice 与权重

从 GEM5 的 `stats.txt` 和 RTL 的结果/日志目录列出 slice。以 `<benchmark>_<point>` 精确匹配；RTL 目录名最后一段是数值时，它是 weight。RTL 多出的 slice 忽略；任何一侧重复、名字无法解析或缺失 weight 的 slice 记录为 excluded，不猜测匹配关系。

为每个共同 slice 建立一条最小 JSON record：`identity`、`benchmark`、`slice`、`weight`、`weight_source`、GEM5/RTL measurement JSON，以及两侧 window-valid flag。对合格 slice：

```text
extra_cycles = GEM5_cycles - RTL_cycles
weighted_extra_cycles = weight * extra_cycles
```

将 records 交给 `emit_slice_gap_contribution.py`。按 `weighted_extra_cycles` 降序输出全部共同 slice，保留正/负贡献和 excluded；summary 给出净 gap、absolute gap、coverage 与 excluded 原因。不要平均 per-slice IPC。

## Counter 初筛

复用用户指定或环境中明确找到的 `gem5_data_proc`，优先检查 `basic`、`intel_topdown`、`branch` 及适用的 local targets。它用于发现 frontend、speculation、backend、memory、IQ/FU、replay、cache 等方向的症状，不能替代 RTL window 证明，也不能单独证明硬件结构有问题。

保留完整 normalized 表；按相同分母的 `RTL - GEM5` 和绝对差异排序，只报告 top 正差、top 负差、最大绝对差和缺失项。每个条目记录原始名称、公式/分母、window status 和 mapping quality：`exact`、`derived`、`approximate` 或 `unavailable`。

初筛之后必须做一次热点驱动的定向分析：根据 `ClockAnalysis.py` 找到的 opcode、PC、basic block 或指令类别选择相关 counter，重新记录该类指令的 count、stall/replay/issue/FU/queue/memory 事件及其分母，并与对应 inter-gap 和 slice contribution 对齐。定向 counter 只能解释已经定位的热点，不能替代源码检查。

完成一次分析后的报告至少包含：

- `reports/slice_gap_contribution.csv`：所有共同 slice 的 weight、GEM5/RTL cycles、绝对 gap、weighted gap、正负贡献和 excluded 原因；
- `reports/counter_triage.md`：通用初筛和定向 counter 的名称、公式/分母、mapping quality、window 状态和初步定位；
- `reports/inter_gap_hotspots.md`：两侧 top basic block/PC/opcode、occurrence、平均 inter-gap 和累计影响；
- `reports/code_behavior_diff.md`：对应 GEM5/RTL 源码位置、实现差异和性能影响判断。

没有源码行为差异证据的热点只能标记为 `Numerical mismatch without code evidence`，不得据此提出 GEM5 修改。

## 收敛判据

子项总差距使用共同 slice 的 weighted cycles 计算：

```text
RTL_weighted_cycles  = Σ(weight_i × RTL_cycles_i)
GEM5_weighted_cycles = Σ(weight_i × GEM5_cycles_i)
weighted_cycle_gap_pct =
  abs(GEM5_weighted_cycles - RTL_weighted_cycles)
  / RTL_weighted_cycles × 100%
```

当 `weighted_cycle_gap_pct < 5%`、correctness 正常、且关键热点 inter-gap/counter 没有反向恶化时，整个子项才可标记为 `converged`。单个 slice 小于 5% 只能标记该 slice 局部收敛，不能替代子项整体判据。

## 结论等级

- `Confirmed`：counter、trace 和代码语义在同一事件边界上相互支持。
- `Supported hypothesis`：证据方向一致，但事件映射或 window 仍有近似部分。
- `Unresolved`：缺少等价事件、窗口或运行证据。
