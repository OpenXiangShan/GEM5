# lifetime trace 与 inter-gap

只有 DB 可读、schema 可识别、含 PC/instruction 与 cycle/order 字段，并能证明覆盖后 20M 时，才做严格指令级比较。RTL 重跑产物还必须确认含 `lifetime` 表：它只能来自以 `WITH_CHISELDB=1` 构建、并显式运行 `--dump-db --dump-select-db lifetime --db-path <trace.db>` 的 emu（通过 `perf_trigger` 时需先固定其默认 lifetime 选择）。先用 SQLite 检查两侧 DB：

```bash
sqlite3 "<GEM5_TRACE_DB>" '.tables'
sqlite3 "<GEM5_TRACE_DB>" '.schema'
sqlite3 "<RTL_TRACE_DB>" '.tables'
sqlite3 "<RTL_TRACE_DB>" '.schema'
sqlite3 "<RTL_TRACE_DB>" "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'lifetime';"
```

RTL 的查询未返回 `lifetime` 时，DB 不合格：报告 trace 缺失，不得继续对该 DB 运行 ClockAnalysis 或给出指令级结论。不要假定表名、字段名或 trace 已自动排除 warmup；保存原始/保留行数、过滤条件和 window evidence。缺少 window、身份或等价事件时，结论只能是 `Unresolved`。

## 两侧 inter-gap

确认 schema 兼容且两侧均有 `lifetime` 表后，分别运行：

```bash
python3 "<GEM5_WORKTREE>/util/ClockAnalysis.py" \
  "<GEM5_TRACE_DB>" -p 333 -P gem5 --tool bbl --inter-gap \
  > "<ARTIFACT_ROOT>/<CASE_ID>/inter-gap/gem5/inter_gap.txt"
```

```bash
python3 "<GEM5_WORKTREE>/util/ClockAnalysis.py" \
  "<RTL_TRACE_DB>" -P rtl --tool bbl --inter-gap \
  > "<ARTIFACT_ROOT>/<CASE_ID>/inter-gap/rtl/inter_gap.txt"
```

`-P rtl` 使用 RTL 的 cycle 定义（period=1）。从两侧输出中按 `Count * Total commit time` 排序，记录 top basic block 的 occurrence、平均 commit cycles、估算累计影响、first PC 和 jumped-from；再关联 opcode、函数和指令类别。`Total commit time` 是 block 平均值，累计值只是估算。必须把造成主要 gap 的热点写入 `reports/inter_gap_hotspots.md` 并在最终报告中告知用户，不能只保留原始 TXT。

## RTL 与事件归因

RTL 不直接套用 GEM5 参数；`ClockAnalysis.py -P rtl` 用于同一 lifetime schema 的 inter-gap/BB 聚合，其他事件仍根据实际 schema 做等价 SQL/脚本分析。围绕下列边界比较：dispatch/rename、IQ enter、operand-ready、issue、execution start/complete、bypass/wakeup、writeback、commit、replay/squash。按 PC/opcode/BB/function 聚合 count、分位数、额外 gap 和累计贡献，并与定向 counter 症状交叉检查。

在报告热点后，必须阅读两侧对应源码并生成 `reports/code_behavior_diff.md`，至少写明 GEM5 文件/行号、RTL 文件/行号、当前行为、触发条件、时序/资源约束差异、inter-gap/counter 证据及预期修改方向。没有源码差异时只能报告未解决的数值差异，不得为了对齐而修改。

## 缺失 trace

`manual` 模式先报告缺失项并请求重跑确认；在 trace 合格前不运行 ClockAnalysis，也不提出指令级修改。只重跑缺失的一侧：若 RTL trace 缺失/不合格，按 [rerun-commands](rerun-commands.md) 生成一次 RTL lifetime baseline；若 GEM5 trace 缺失/不合格，使用未修改的 GEM5 baseline commit 按同一文件的 GEM5 命令生成 lifetime trace。`autonomous-iterate` 同样最多生成一次 RTL baseline，之后只重跑 GEM5。RTL DB 仍不合格、需要第二份 RTL trace、修改 RTL instrumentation 或无法证明后 20M 时，立即停止并请求用户决定。
