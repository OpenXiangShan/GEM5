# 重跑命令

仅在 manual 模式获得用户确认，或显式开启 `MODE=autonomous-iterate` 后读取。本文件只描述输入解析和固定命令；自动迭代预算/停机条件见 [iteration-rules](iteration-rules.md)。

## 输入与 preflight

重跑需要用户提供：

```text
GEM5_COMMIT=<sha>
RTL_COMMIT=<sha>
GEM5_WORKTREE=<path>
RTL_WORKTREE=<path>
GEM5_RESULT=<existing GEM5 result>
SLICE=<benchmark_point[_weight]>
```

自动模式另外需要 `RTL_RESULT`、`ARTIFACT_ROOT` 和 `CASE_ID`。不从目录名、日志或当前 HEAD 猜 commit。profile 只从两个目标 workflow 和已有 GEM5 result 的直接 metadata/父目录证据读取：

```text
<GEM5_WORKTREE>/.github/workflows/gem5-perf-template.yml
<RTL_WORKTREE>/.github/workflows/perf-template.yml
```

当前主线的配置解析入口如下：

```text
GEM5 benchmark/checkpoint: <GEM5_WORKTREE>/util/xs_scripts/perf_benchmarks.py
GEM5 reference SO:         <GEM5_WORKTREE>/util/nemu_ref/resolve.py + lock.json
RTL checkpoint mapping:    <RTL_WORKTREE>/.github/workflows/perf-template.yml
RTL CI runner:              /nfs/home/share/ci-workloads/env-scripts/perf_trigger/main.py
```

对 GEM5，使用 `perf_benchmarks.py <benchmark_type> --github-output <file>` 解析 `checkpoint_list`、`checkpoint_root`、`config_path` 关联的 profile 和评分配置。`checkpoint_list` 是 checkpoint 列表；`cluster_config` 只用于评分/集群配置，不能作为 checkpoint source JSON。对 reference SO，使用 `util/nemu_ref/resolve.py <variant>`，并从 `lock.json` 记录 release、variant、实际路径和 hash。不要假设 SO 固定位于 `ready-to-run/`。

对 RTL，从 `perf-template.yml` 的同名 benchmark 配置取得 `CKPT_HOME` 和 `CKPT_JSON_PATH`，再按 checkpoint JSON/list 选择目标 slice。CI 的实际调度入口是共享环境中的 `perf_trigger/main.py`；workflow 通常会把 reference SO 复制到 `$SPEC_DIR/riscv64-nemu-interpreter-so`，运行时使用该副本，因此 preflight 要同时记录源 SO 和运行副本。

执行以下核验，并把选中的 profile、checkpoint、SO 和证据写入 `<ARTIFACT_ROOT>/<CASE_ID>/manifest/preflight.md`：

```bash
test "$(git -C "<GEM5_WORKTREE>" rev-parse HEAD)" = "$(git -C "<GEM5_WORKTREE>" rev-parse --verify "<GEM5_COMMIT>^{commit}")"
test "$(git -C "<RTL_WORKTREE>" rev-parse HEAD)" = "$(git -C "<RTL_WORKTREE>" rev-parse --verify "<RTL_COMMIT>^{commit}")"
readlink -f "<GEM5_CHECKPOINT>"
readlink -f "<RTL_CHECKPOINT>"
test "$(readlink -f "<GEM5_CHECKPOINT>")" = "$(readlink -f "<RTL_CHECKPOINT>")"
test -r "<GEM5_REF_SO>"
test -r "<RTL_REF_SO_SOURCE>"
test -r "<RTL_REF_SO_RUN_COPY>"
sha256sum "<GEM5_REF_SO>" "<RTL_REF_SO_SOURCE>" "<RTL_REF_SO_RUN_COPY>"
```

`GEM5_COMMIT` 和 `RTL_COMMIT` 必须由用户提供；结果 `metadata.txt` 中的 commit 只能作为交叉核验，不能在缺失时猜测。若 profile、checkpoint、reference SO variant 或 realpath/hash 不能唯一确定，停止并要求用户补充信息。只有全部核验通过才可运行。

CI-compatible 全量运行可使用 workflow 调用的 `parallel_sim.sh` 或 `distributed_sim.py`；单 slice 对齐验证可以直接调用下面的 `gem5.opt`/RTL emu 命令，但必须把解析出的参数写入 manifest。

## GEM5

```bash
cd "<GEM5_CANDIDATE_WORKTREE>"
scons build/RISCV/gem5.opt --gold-linker -j64
```

```bash
GCBV_REF_SO="<GEM5_REF_SO>" \
"<GEM5_CANDIDATE_WORKTREE>/build/RISCV/gem5.opt" \
  --outdir="<ARTIFACT_ROOT>/<CASE_ID>/trace-full/gem5/<SLICE>" \
  "<GEM5_CANDIDATE_WORKTREE>/configs/example/kmhv3.py" \
  --generic-rv-cpt="<CHECKPOINT>" \
  -I 40000000 --warmup-insts-no-switch=20000000 \
  --enable-arch-db --arch-db-dump-lifetime \
  --arch-db-file="<ARTIFACT_ROOT>/<CASE_ID>/trace-full/gem5/<SLICE>/trace.db"
```

`.zstd` 不加 `--raw-cpt`；raw `.bin` 才添加。每轮保留 command、stats、DB、退出状态和 measurement 检查。reference SO 应使用 `nemu_ref/resolve.py` 解析出的实际文件，而不是固定路径。

## RTL baseline（最多一次）

```bash
cd "<RTL_WORKTREE>"
make emu EMU_THREADS=8 WITH_DRAMSIM3=1 WITH_CONSTANTIN=1 WITH_CHISELDB=1 -j16
```

```bash
"<RTL_WORKTREE>/build/emu" \
  -i "<CHECKPOINT>" \
  --diff "<RTL_REF_SO_RUN_COPY>" \
  -W 20000000 -I 40000000 --dump-db \
  --db-path "<ARTIFACT_ROOT>/<CASE_ID>/trace-full/rtl-baseline/<SLICE>/trace.db" \
  --dump-select-db "<LIFETIME_TABLES>" \
  > "<ARTIFACT_ROOT>/<CASE_ID>/logs/rtl_<SLICE>.stdout" \
  2> "<ARTIFACT_ROOT>/<CASE_ID>/logs/rtl_<SLICE>.stderr"
```

`make emu` 是当前本地 trace 构建入口；`<LIFETIME_TABLES>` 必须由当前 emu help、源码或 schema 确认。RTL 任务可能运行数小时：记录 PID/job、命令、日志和预期 DB 后交还控制权，不持续轮询。自动模式只有在缺少合格 baseline 时允许执行这一次 RTL 运行，之后只重跑 GEM5。
