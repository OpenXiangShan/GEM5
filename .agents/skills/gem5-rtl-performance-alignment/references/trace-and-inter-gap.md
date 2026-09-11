# lifetime trace 与 inter-gap

只有 DB 可读、schema 可识别、含 PC/instruction 与 cycle/order 字段，并能证明覆盖后 20M 时，才做严格指令级比较。先用 SQLite 检查两侧 DB：

```bash
sqlite3 "<GEM5_TRACE_DB>" '.tables'
sqlite3 "<GEM5_TRACE_DB>" '.schema'
sqlite3 "<RTL_TRACE_DB>" '.tables'
sqlite3 "<RTL_TRACE_DB>" '.schema'
```

不要假定表名、字段名或 trace 已自动排除 warmup；保存原始/保留行数、过滤条件和 window evidence。缺少 window、身份或等价事件时，结论只能是 `Unresolved`。

## GEM5 inter-gap

确认 schema 兼容后运行：

```bash
python3 "<GEM5_WORKTREE>/util/ClockAnalysis.py" \
  "<GEM5_TRACE_DB>" -p 333 -P gem5 --tool bbl --inter-gap \
  > "<ARTIFACT_ROOT>/<CASE_ID>/inter-gap/gem5/inter_gap.txt"
```

从输出中按 `Count * Total commit time` 排序，记录 top basic block 的 occurrence、平均 commit cycles、估算累计影响、first PC 和 jumped-from；具体 opcode/依赖链再回看完整文本。`Total commit time` 是 block 平均值，累计值只是估算。

## RTL 与事件归因

RTL 不直接套用 GEM5 参数；先根据实际 schema 确认表/字段映射，再使用等价 SQL/脚本或兼容的 ClockAnalysis 入口。围绕下列边界比较：dispatch/rename、IQ enter、operand-ready、issue、execution start/complete、bypass/wakeup、writeback、commit、replay/squash。按 PC/opcode/BB/function 聚合 count、分位数、额外 gap 和累计贡献，并与 counter 症状交叉检查。

## 缺失 trace

`manual` 模式先报告缺失项并等待重跑确认。`autonomous-iterate` 仅可按 [rerun-commands](rerun-commands.md) 生成一次 RTL baseline；完成后只重跑 GEM5。RTL DB 不合格、需要第二份 RTL trace、修改 RTL instrumentation 或无法证明后 20M 时，立即停止并请求用户决定。
