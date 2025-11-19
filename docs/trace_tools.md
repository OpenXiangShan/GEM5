**XS-GEM5 Trace 工具与脚本指南**

本文档整理了当前项目中围绕 ChampSim trace / XiangShan trace-mode 相关的辅助脚本和推荐工作流，方便后续直接复用。

---

**0. 总体目标与关键概念**

- 批量跑 XiangShan trace（ChampSim 格式），支持：
  - warmup + sample（基于 `--warmup-insts-no-switch` + `--maxinsts`）。
  - 自动并行、单个 workload 输出隔离。
- 发现并定位 trace-mode 中的 PC mismatch 等问题：
  - 快速收集错误原因。
  - 用 debug flags 重跑 aborted workload。
  - 抽取 panic 附近的 gem5 事件 + trace 指令窗口。
  - 对齐 gem5 的 sn 与 ChampSim trace 的 tracesn/PC。

---

## 1. 批量跑 ChampSim trace

### 1.1 单条 trace 封装：`util/xs_scripts/trace/run_trace_champsim.sh`

**作用**

- 封装一条 XiangShan trace 模式的 gem5 命令，支持通过环境变量控制 maxinsts 和 warmup，无需手写长命令。

**环境变量**

- `XS_MAX_INSTS`
  - 解释：正式统计阶段指令数。
  - 映射：`--maxinsts=${XS_MAX_INSTS}`。
  - 默认：`1000000`。

- `XS_WARMUP_INSTS_NO_SWITCH`
  - 解释：warmup 指令数（不换 CPU、只 reset stats）。
  - 映射：`--warmup-insts-no-switch=${XS_WARMUP_INSTS_NO_SWITCH}`。
  - 默认：`0`（不加此参数）。

- `XS_DEBUG_FLAGS` / `XS_DEBUG_START` / `XS_DEBUG_END`
  - 为 debug 重跑设计，对应 `--debug-flags/--debug-start/--debug-end`（可选）。

**示例：单条 trace 手动跑 50M + 50M**

```bash
XS_WARMUP_INSTS_NO_SWITCH=50000000 \
XS_MAX_INSTS=50000000 \
bash util/xs_scripts/trace/run_trace_champsim.sh \
  /nfs/home/share/glr/champsim_traces/ipc1_public/ipc_client_002.champsimtrace.xz
```

脚本最终会调用：

```bash
$gem5 --outdir=$PWD --stats-file=$PWD/stats.txt \
  $gem5_home/configs/example/xiangshan.py \
  --enable-trace-mode \
  --trace-file=...ipc_client_002.champsimtrace.xz \
  --trace-format=champsim \
  --warmup-insts-no-switch=50000000 \
  --maxinsts=50000000 \
  --trace-enable-decoupled-bp
```

---

### 1.2 并行控制：`util/xs_scripts/trace/parallel_trace_sim.sh`

**作用**

- 从 workload 列表读取 trace 路径 + warmup/sample 配置。
- 调用 arch_script（通常是 `util/xs_scripts/trace/run_trace_champsim.sh`）并行跑一批 workload。
- 支持 `.gz/.zstd/.xz` 自动识别，支持输出根目录重定向（`XSGEM5_WORK_ROOT`）。

**workload 列表格式**

每行：

```text
workload_name  checkpoint_path  skip  fw  dw  sample
```

字段含义：

- `skip`：skip insts（通常 0）。
- `fw`：functional_warmup insts。
- `dw`：detailed_warmup insts。
- `sample`：正式统计阶段指令数。

**warmup/sample 传递逻辑（在 `parallel_trace_sim.sh` 中完成）**

```bash
# workload 行字段：name path skip fw dw sample
IFS=' ' read -r task task_path skip fw dw sample <<< "${line}"

skip=${skip:-0}
fw=${fw:-0}
dw=${dw:-0}
sample=${sample:-0}

warmup=$((fw + dw))
total=$((warmup + sample))

XS_MAX_INSTS="${total}" XS_WARMUP_INSTS_NO_SWITCH="${warmup}" \\
    run "${checkpoint}" "${work_dir}" >"${work_dir}/${log_file}" 2>&1
```

**输出根目录指定（`XSGEM5_WORK_ROOT`）**

- 默认：结果落在调用脚本目录下的 `$tag` 中：
  - `full_work_dir=$PWD/$tag`。
- 可以通过 `XSGEM5_WORK_ROOT` 覆盖 trace 结果根目录：

```bash
export XSGEM5_WORK_ROOT=/nfs/home/goulingrui/expri_results/gem5_trace
# full_work_dir = $XSGEM5_WORK_ROOT/$tag
```

---

### 1.3 生成 workload 列表：`util/xs_scripts/gen_champsim_workloads.sh`

**作用**

- 扫描一个目录下的所有 `*.champsimtrace.xz`，生成 workload 列表，默认 warmup=50M、sample=50M。

**用法示例**

```bash
bash util/xs_scripts/gen_champsim_workloads.sh \
  /nfs/home/share/glr/champsim_traces/ipc1_public \
  ./ipc1_public_traces.lst
```

生成示例行：

```text
ipc_client_002  ipc1_public/ipc_client_002.champsimtrace  0 50000000 0 50000000
```

---

### 1.4 批跑示例（ipc1_public，全量 50M + 50M）

```bash
# 1) 生成列表
bash util/xs_scripts/gen_champsim_workloads.sh \
  /nfs/home/share/glr/champsim_traces/ipc1_public \
  ./ipc1_public_traces.lst

# 2) 配置并行度与结果目录
export xsgem5_para_jobs=32
export XSGEM5_WORK_ROOT=/nfs/home/goulingrui/expri_results/gem5_trace

# 3) 并行跑（trace 专用 parallel_trace_sim.sh）
bash util/xs_scripts/trace/parallel_trace_sim.sh \
  util/xs_scripts/trace/run_trace_champsim.sh \
  ./ipc1_public_traces.lst \
  /nfs/home/share/glr/champsim_traces \
  trace_ipc1_50M_50M
```

所有输出将位于：

```text
/nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M/<workload_name>/
```

---

## 2. 监控与统计

### 2.1 批跑状态汇总：`util/xs_scripts/check_parallel_runs.sh`

**作用**

- 对某个 tag 目录统计：
  - 工作总数。
  - `COMPLETED / RUNNING / ABORTED / PENDING`。
  - `stats.txt` 是否存在。

**用法**

```bash
WORK_ROOT=/nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M

# 概览
bash util/xs_scripts/check_parallel_runs.sh "$WORK_ROOT"

# 详细
bash util/xs_scripts/check_parallel_runs.sh "$WORK_ROOT" --details
```

---

### 2.2 trace 指令总数统计：`util/trace/count_champsim_trace_insts.py`

**作用**

- 统计 ChampSim trace 内记录条数（指令数）。

**单个 trace**

```bash
python3 util/trace/count_champsim_trace_insts.py \
  --trace /nfs/home/share/glr/champsim_traces/ipc1_public/ipc_client_002.champsimtrace.xz
```

输出：一个整数，例如 `100003840`。

---

### 2.3 trace 内容窗口查看：`util/trace/dump_champsim_trace.py`

**新增参数**

- `--start-index <N>`：从第 N 条记录开始输出（0-based，全局指令序号）。
- `--count <K>`：输出 K 条记录（窗口长度）。

**示例：查看 idx=83375045 附近 100 条指令**

```bash
start=$((83375045 - 99))
python3 util/trace/dump_champsim_trace.py \
  --trace /nfs/home/share/glr/champsim_traces/ipc1_public/ipc_client_002.champsimtrace.xz \
  --start-index "$start" \
  --count 100
```

---

## 3. 错误收集与 debug 重跑

### 3.1 收集 ABORTED 任务错误原因：`util/xs_scripts/collect_parallel_errors.sh`

**作用**

- 针对某个批跑结果目录，收集所有 ABORTED workload 的错误摘要。

**用法**

```bash
WORK_ROOT=/nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M

bash util/xs_scripts/collect_parallel_errors.sh "$WORK_ROOT" \
  "$WORK_ROOT/errors_summary.tsv"
```

输出 TSV：

```text
workload_name\tabort_tick\treason_line
```

---

### 3.2 对 ABORTED workload 带 debug 重跑：`util/xs_scripts/rerun_aborted_with_debug.sh`

**作用**

- 对所有 ABORTED workload：
  - 从顶层 `log.txt` 解析 abort_tick 和 trace_file。
  - 设置 `XS_DEBUG_FLAGS/XS_DEBUG_START/XS_DEBUG_END`。
  - 调用 `run_trace_champsim.sh` 在 `debug/` 子目录下重跑。

**参数**

- 第 1 个参数：`WORK_ROOT`。
- 第 2 个参数（可选）：`DEBUG_FLAGS`，默认：

  ```text
  IEW,Fetch,Commit,CommitTrace,DecoupleBP,TraceReader,Decode
  ```

- 并行度：`xsgem5_para_jobs`（默认 8）。

**用法**

```bash
export xsgem5_para_jobs=8

bash util/xs_scripts/rerun_aborted_with_debug.sh \
  /nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M
```

每个 ABORTED workload 目录下会生成：

- `debug/log.txt`
- `debug/stats.txt`

---

## 4. BUILD/BIND 与 trace 对齐工具链

### 4.1 `util/extract_gem5_events.py`（增强版）

**作用**

- 从 gem5 debug 日志中按 tick 抽取 key events，并打上标签：
  - `BUILD`：`Instruction PC (...) created [sn:...]`
  - `SQUASH`：`Squashing, setting PC to`。
  - `COMMIT`：`Committing instruction with PC`。
  - `WP_ENTER` / `WP_EXIT`：wrong-path 进入/退出。
  - `BIND`：`Bind trace metadata ... [sn:]->[tracesn:] pc=... taken=...`。

**新增参数**

```bash
--bind           Include BIND events (default on)
--no-bind        Exclude them
--bind-pattern   Regex for BIND events (default 'Bind trace metadata')
```

---

### 4.2 为所有 debug 目录生成 events：`util/xs_scripts/extract_debug_events.sh`

**作用**

- 对 `ROOT_DIR` 下所有 `*/debug/log.txt`：
  - 找到最后一条 tick（`Exiting @ tick` 或 `Program aborted at tick`）。
  - 取 `from_tick = last_tick - 10000`。
  - 调用 `extract_gem5_events.py` 生成 `debug/events.txt`。

**用法**

```bash
bash util/xs_scripts/extract_debug_events.sh \
  /nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M
```

---

### 4.3 BUILD/BIND 对齐：`util/trace/align_trace_bind_events.py`

**作用**

- 对 `events.txt` 中的 BUILD/BIND 行按 sn 对齐。

**用法**

```bash
python3 util/trace/align_trace_bind_events.py \
  /path/to/debug/events.txt \
  > /path/to/debug/bind_align.tsv
```

输出列：

```text
sn  build_tick  build_pc  bind_tick  trace_pc  taken  tracesn
```

---

### 4.4 针对 panic 的大窗口抽取：`util/xs_scripts/extract_panic_events.sh`

**作用**

- 只处理含 `Trace stream PC mismatch` panic 的 debug 日志：
  - 从顶层 `log.txt` 提取 `abort_tick`。
  - 使用 `from_tick = abort_tick - WINDOW`（默认 1e6）。
  - 调用 `extract_gem5_events.py` 生成 `events_panic.txt`。
  - 调用 `align_trace_bind_events.py` 生成 `bind_align_panic.tsv`。

**用法**

```bash
bash util/xs_scripts/extract_panic_events.sh \
  /nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M \
  1000000  # WINDOW in ticks
```

---

### 4.5 PC mismatch 汇总 + BUILD/BIND 对齐 + trace 片段：`util/xs_scripts/report_pc_mismatch_bind.sh`

**作用**

- 针对整个 `WORK_ROOT`：
  1. 从顶层 `log.txt` 中抽取 PC mismatch panic 信息：
     - `built_pc` / `expect_pc` / `sn` / `abort_tick`。
  2. 从 `debug/bind_align_panic.tsv`（优先）或 `debug/bind_align.tsv` 对齐：
     - `build_tick` / `build_pc`。
     - `bind_tick` / `trace_pc` / `taken` / `tracesn`。
  3. 生成 ChampSim trace 片段：
     - index = `tracesn`（若无，则用 `sn`）。
     - `start-index = max(0, index - 99)`，`count = 100`。
     - 调用 `dump_champsim_trace.py --start-index --count`。
     - 写入 `<workload>/panic_trace_snippet.txt`。
  4. 汇总输出 TSV：

     ```text
     workload  sn  abort_tick  built_pc  expect_pc  build_tick  build_pc  bind_tick  trace_pc  taken  tracesn
     ```

**用法**

```bash
WORK_ROOT=/nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M

bash util/xs_scripts/report_pc_mismatch_bind.sh \
  "$WORK_ROOT" \
  "$WORK_ROOT/pc_mismatch_bind.tsv"
```

生成内容包括：

- 全局汇总：`$WORK_ROOT/pc_mismatch_bind.tsv`。
- 每个触发 PC mismatch 的 workload 下的片段文件：

  ```text
  $WORK_ROOT/<workload>/panic_trace_snippet.txt
  ```

---

如果在未来继续扩展 trace 调试工具，推荐同步更新本文件，以保持工具链文档和实现一致。
