**XS-GEM5 Trace 工具与脚本指南**

本文档整理了当前项目中围绕 ChampSim trace / XiangShan trace-mode 相关的辅助脚本和推荐工作流，方便后续直接复用。

---

**常用流程（ChampSim/CBP2025，一条龙）**

1) 统计指令数（可选）：`TRACE_FORMAT=cbp2025 bash util/xs_scripts/trace/count_traces_from_list_parallel.sh <lst> <trace_root> <out.tsv>`.
2) 生成 workload 列表（默认 warmup=sample=50M）：`bash util/xs_scripts/gen_champsim_workloads.sh <trace_root> <out.lst>`（已支持 champsim/cbp）。
3) 并行跑 trace：`TRACE_FORMAT=cbp2025 bash util/xs_scripts/trace/parallel_trace_sim.sh util/xs_scripts/trace/run_trace_champsim.sh <workload.lst> <trace_root> <tag>`.
4) 查看运行状态：`bash util/xs_scripts/check_parallel_runs.sh <work_root>/<tag> [--details]`.
5) Debug：panic/PC mismatch 时用 `util/xs_scripts/trace/report_pc_mismatch_bind.sh`（必要时先 `extract_panic_events.sh`/`extract_debug_events.sh`）。
   - `TRACE_FORMAT` 贯穿计数/跑批/片段抽取，默认 `champsim`，CBP 时显式设置 `TRACE_FORMAT=cbp2025`。

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

## 1. 批量跑 trace（ChampSim / CBP2025）

### 1.1 单条 trace 封装：`util/xs_scripts/trace/run_trace_champsim.sh`（支持 champsim/cbp2025）

**作用**

- 封装一条 XiangShan trace 模式的 gem5 命令，支持 champsim/cbp2025，支持通过环境变量控制 maxinsts 和 warmup，无需手写长命令。

**环境变量**

- `XS_MAX_INSTS`
  - 解释：正式统计阶段指令数。
  - 映射：`--maxinsts=${XS_MAX_INSTS}`。
  - 默认：`1000000`。

- `TRACE_FORMAT`
  - 解释：trace 格式，`champsim` 或 `cbp2025`。
  - 映射：`--trace-format=${TRACE_FORMAT}`。
  - 默认：`champsim`。

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
TRACE_FORMAT=champsim \
bash util/xs_scripts/trace/run_trace_champsim.sh \
  /nfs/home/share/glr/champsim_traces/ipc1_public/ipc_client_002.champsimtrace.xz
```

CBP 示例（保持命令一致，仅切换 TRACE_FORMAT+trace 文件）：

```bash
XS_WARMUP_INSTS_NO_SWITCH=50000000 \
XS_MAX_INSTS=50000000 \
TRACE_FORMAT=cbp2025 \
bash util/xs_scripts/trace/run_trace_champsim.sh \
  /nfs/home/share/glr/cbp_traces/compress/compress_0_trace.gz
```

脚本最终会调用：

```bash
$gem5 --outdir=$PWD --stats-file=$PWD/stats.txt \
$gem5_home/configs/example/kmhv3.py \
  --enable-trace-mode \
  --trace-file=...ipc_client_002.champsimtrace.xz \
  --trace-format=champsim \
  --warmup-insts-no-switch=50000000 \
  --maxinsts=50000000 \
  --trace-enable-decoupled-bp
```

补充：`Options.py` 还提供 `--trace-checkpoint-interval`（默认 64）、`--trace-disable-bp-validation`、`--trace-mispredict-penalty`、`--trace-disable-wrongpath`、`--trace-wrongpath-use-traceinst` 等开关，默认地址映射为 `linear @ 0x80000000`（1 GiB，page-align）；（大多数暂未实现）。

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

> 如需 CBP trace，将 `TRACE_FORMAT=cbp2025` 置于并行命令前，arch_script 会继承该环境变量。

**输出根目录指定（`XSGEM5_WORK_ROOT`）**

- 默认：结果落在调用脚本目录下的 `$tag` 中：
  - `full_work_dir=$PWD/$tag`。
- 可以通过 `XSGEM5_WORK_ROOT` 覆盖 trace 结果根目录：

```bash
export XSGEM5_WORK_ROOT=/nfs/home/goulingrui/expri_results/gem5_trace
# full_work_dir = $XSGEM5_WORK_ROOT/$tag
```

---

### 1.3 生成 workload 列表：`util/xs_scripts/gen_champsim_workloads.sh`（champsim/cbp 均可）

**作用**

- 扫描一个目录下的所有压缩/未压缩 trace（`.xz/.gz/.champsimtrace`），生成 workload 列表，默认 warmup=50M、sample=50M。用于 champsim/cbp 均可。

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

### 1.5 分布式调度（多机，NFS 共享）：`util/xs_scripts/trace/distributed_trace_scheduler.py`

**作用**

- 读取 workload 列表与机器列表，按机器实时 load 与逻辑核数（取 cores/2）分配任务。
- 全局并行度上限 + 每机并行上限，可通过 `TRACE_FORMAT` 支持 champsim/cbp。
- 在共享 NFS 下调度，输出 dispatch 日志，任务状态文件复用已有监控脚本。

**前提**

- 机器间共享 NFS（工作目录、trace、脚本可直接访问）。
- 可免密 SSH 到机器列表内的 host（端口 22）。
- 假设超线程开启，容量=logical/2；当下发任务 n 时要求 n+load <= logical/2。

**服务器列表示例（servers.txt）**

```
node-a
node-b
node-c
```

**运行示例（CBP）**

```bash
python3 util/xs_scripts/trace/distributed_trace_scheduler.py \
  --arch-script util/xs_scripts/trace/run_trace_champsim.sh \
  --workload-list cbp_traces.lst \
  --trace-root /nfs/home/share/glr/cbp_traces \
  --server-list servers.txt \
  --tag cbp_run \
  --max-global 64 \
  --max-threads-per-host 8 \
  --poll-seconds 30 \
  --trace-format cbp2025   # 或设置环境变量 TRACE_FORMAT
```

- 工作根目录：`$XSGEM5_WORK_ROOT/<tag>`（默认当前目录/<tag>）。
- 日志：`dispatch.tsv`（task, host, trace_path, warmup, sample, status, start_ts, end_ts）。
- 任务目录：`<work_root>/<task>/` 下的 `running/completed/abort/log.txt`，可用现有 `check_parallel_runs.sh` 监控。

**调度规则**

- 每轮获取 host(t) 的 load1、逻辑核数，容量= min(max_threads_per_host, floor(cores/2 - load1))。
- 仅当容量>running 时才补充任务；全局并行度不超过 `--max-global`。
- 任务启动命令：SSH 到目标 host，`OUTDIR=$work_dir TRACE_FORMAT=$TRACE_FORMAT XS_MAX_INSTS/XS_WARMUP_INSTS_NO_SWITCH` 调用 `run_trace_champsim.sh`，完成后在 `work_dir` 写 `completed` 或 `abort`。
- 周期性轮询（默认 30s）task 状态与负载，持续补位，直至全部完成。
- 启动是在远端后台执行（nohup + &），SSH 会快速返回；若节点暂时不可用会在调度日志中看到 launch 警告。
- 控制台输出：每次获取机器容量都会打印 `[CAP] host cores/load/cap` 行；每次派发打印 `[DISPATCH] task -> host warmup/sample total ...`；任务完成/失败打印 `[DONE] task on host status=... elapsed=...s ...`。
- 启动时会输出总任务数 `[INFO] total tasks=N`，派发行包含 `progress=done/total` 方便观测整体完成度。
- 若工作目录下某任务已存在 `completed` 标记，调度器会跳过该任务并在控制台输出 `[SKIP]`；取消后重启时不会重复跑已完成的任务。
- Ctrl-C 中断时会尝试向已派发任务的远端 PID 发送 `kill -TERM`，并在任务目录标记 `abort`。

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

- 统计 trace 内记录条数（指令数），支持 champsim/cbp2025（`--format` 或环境变量 `TRACE_FORMAT`）。

**单个 trace**

```bash
python3 util/trace/count_champsim_trace_insts.py \
  --trace /nfs/home/share/glr/champsim_traces/ipc1_public/ipc_client_002.champsimtrace.xz \
  --format champsim

TRACE_FORMAT=cbp2025 python3 util/trace/count_champsim_trace_insts.py \
  --trace /nfs/home/share/glr/cbp_traces/compress/compress_0_trace.gz
```

批量计数：

```bash
TRACE_FORMAT=cbp2025 bash util/xs_scripts/trace/count_traces_from_list_parallel.sh \
  cbp_traces.lst /nfs/home/share/glr/cbp_traces cbp_trace_inst_counts.tsv 16
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

# CBP 请额外指定格式
python3 util/trace/dump_champsim_trace.py \
  --format cbp2025 \
  --trace /nfs/home/share/glr/cbp_traces/compress/compress_0_trace.gz \
  --limit 10
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

### 3.2 对 ABORTED workload 带 debug 重跑：`util/xs_scripts/trace/distributed_rerun_aborted_with_debug.py`

**作用**

- 分布式（多机）重跑 ABORTED workload：
  - 扫描 `work_root/*`，只处理存在 `abort` 标记的任务。
  - 从顶层 `log.txt` 解析 abort_tick、trace_file、(可选) CommitStuck 建议窗口、maxinsts。
  - 设置 `XS_DEBUG_FLAGS/XS_DEBUG_START/XS_DEBUG_END`，调用 `run_trace_champsim.sh` 在 `debug/` 子目录下重跑。
  - 通过 SSH 下发到 `server_list` 中的机器，按照 cores/2 - load1 估算容量；支持全局/每机并发上限。
  - 支持 Ctrl-C：主控退出时会尝试对正在运行的远端任务发送 SIGTERM（基于远端记录的 pid）。
  - 支持清理旧 abort 标记：`--clear-old-abort` 会在派发前移除 `abort` 文件（保留为 `abort.prev` 以防丢信息），便于区分“新 abort”。
  - 旧的 `util/xs_scripts/rerun_aborted_with_debug.sh` 已废弃，仅保留提示信息。

**参数**

- `--work-root`：批跑结果根目录。
- `--server-list`：服务器列表（每行一个 host，可免密 SSH）。
- `--arch-script`：trace 跑批脚本，默认 `util/xs_scripts/trace/run_trace_champsim.sh`。
- `--debug-flags`：逗号分隔，默认 `IEW,Fetch,Commit,CommitTrace,DecoupleBP,TraceReader,Decode`。
- `--max-global` / `--max-threads-per-host`：全局并发与每机并发上限。
- `--poll-seconds`：轮询间隔，默认 20。

**用法**

```bash
python3 util/xs_scripts/trace/distributed_rerun_aborted_with_debug.py \
  --work-root /nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M \
  --server-list servers.txt \
  --arch-script util/xs_scripts/trace/run_trace_champsim.sh \
  --max-global 16 \
  --max-threads-per-host 4 \
  --trace-format cbp2025   # 如需 CBP，可用参数或环境变量 TRACE_FORMAT
  --clear-old-abort        # 可选：清理旧 abort 标记，重跑后新 abort 才会体现
```

每个 ABORTED workload 目录下会生成：

- `debug/log.txt`
- `debug/stats.txt`
- 状态文件 `debug/running|completed|abort`（由分布式脚本用于轮询）。

### 3.3 单个 ABORTED workload 本地重跑：`util/xs_scripts/trace/rerun_aborted_with_debug.py`

**作用**

- 针对单个 workload 目录（含 `log.txt` + `abort` 标记），解析 abort tick / 建议 debug 窗口 / trace 路径 / maxinsts，并在本机 `debug/` 子目录下重跑。
- 环境变量与分布式脚本保持一致：`XS_DEBUG_FLAGS/XS_DEBUG_START/XS_DEBUG_END/XS_MAX_INSTS/TRACE_FORMAT`，状态文件 `debug/running|completed|abort|exit_code` 与 `log.txt`。
- 默认 arch 脚本 `util/xs_scripts/trace/run_trace_champsim.sh`；默认 debug flags 同分布式脚本。

**参数**

- `--work-dir`：单个 workload 目录。
- `--arch-script`：trace 跑批脚本，默认 `util/xs_scripts/trace/run_trace_champsim.sh`。
- `--debug-flags`：默认 `IEW,Fetch,Commit,CommitTrace,DecoupleBP,TraceReader,Decode`。
- `--trace-format`：默认环境变量 `TRACE_FORMAT` 或 `champsim`。
- `--clear-old`：清理 `debug/` 下的旧标记（running/completed/abort/exit_code）。
- `--allow-missing-abort`：缺失顶层 `abort` 时也允许重跑（例如手动清理过）。

**用法**

```bash
python3 util/xs_scripts/trace/rerun_aborted_with_debug.py \
  --work-dir /nfs/home/goulingrui/expri_results/gem5_trace/trace_ipc1_50M_50M/foo_workload \
  --arch-script util/xs_scripts/trace/run_trace_champsim.sh \
  --trace-format champsim \
  --debug-flags IEW,Fetch,Commit,CommitTrace,DecoupleBP,TraceReader,Decode
```

运行产物：`<work-dir>/debug/log.txt`、状态文件 `running|completed|abort|exit_code` 与常规输出（stats 等）。

---

## 4. BUILD/BIND 与 trace 对齐工具链

### 4.1 `util/trace/extract_trace_events.py`（trace 专用；`util/extract_gem5_events.py` 为兼容入口）

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
  - 调用 `util/trace/extract_trace_events.py` 生成 `debug/events.txt`（脚本内部已指向新路径）。

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
  - 调用 `util/trace/extract_trace_events.py` 生成 `events_panic.txt`（脚本内部已指向新路径）。
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
