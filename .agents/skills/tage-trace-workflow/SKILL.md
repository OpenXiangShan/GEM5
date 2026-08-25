---
name: tage-trace-workflow
description: 用于编译 XiangShan RTL trace 版 emu、运行 RTL/gem5 的 TAGE trace、并对 gem5 stats/bp.db 与 RTL CondTrace 做共口径聚合比较。适用于 coremark、SPEC checkpoint slice、sjeng_22213 等单切片对拍与首轮分叉定位。
---

# TAGE Trace Workflow

这个 skill 只做三件事：

- 编译能落 `CondTrace_*` / `microTageTrace` 的 RTL `emu`
- 运行 gem5 / RTL 的 trace，并固定到相同 instruction window
- 用脚本把 gem5 `stats.txt` / `bp.db` 和 RTL sqlite db 聚成同口径摘要

## 1. RTL 编译

先固定 RTL checkout；显式路径优先，下面的个人 home 只作本机 fallback：

```bash
export XIANGSHAN_HOME="${XIANGSHAN_HOME:-/nfs/home/yanyue/workspace/xs-env/XiangShan}"
git -C "$XIANGSHAN_HOME" rev-parse HEAD
cd "$XIANGSHAN_HOME"
```

然后使用当前 trace 配置：

```bash
make clean
make emu \
  EMU_THREADS=8 \
  EMU_TRACE=fst \
  WITH_DRAMSIM3=1 \
  WITH_CONSTANTIN=1 \
  WITH_CHISELDB=1 \
  WITH_ROLLINGDB=1 \
  CONFIG=FrontendDebugConfig \
  -j64
```

编完先做两个 sanity check：

```bash
rg -n "FrontendDebugConfig" build/time.log
rg -n "CondTrace_0_write|BpuPredictionTrace_write|microTageTrace_write" build/chisel_db.cpp
```

如果 `CondTrace_0_write()` 仍然是空桩，就不要继续跑 trace。

## 2. RTL 运行

关键点：

- `--dump-select-db` 必须是空格分隔的精确表名
- 优先只打 `CondTrace_0..7`，需要更细再加 `microTageTrace`
- 尽量先用 `-I` 把窗口限制住

示例：

```bash
./build/emu \
  --no-diff \
  -I 200000 \
  -i /nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin \
  --dump-db \
  --dump-select-db "CondTrace_0 CondTrace_1 CondTrace_2 CondTrace_3 CondTrace_4 CondTrace_5 CondTrace_6 CondTrace_7 microTageTrace" \
  > /tmp/coremark_tage.out 2>&1
```

对 `.zstd` GCPT slice，直接把路径传给 `-i` 即可。

## 3. gem5 运行

`kmhv3.py` 需要 diff 环境变量。保留调用者已有设置，否则使用 CI 常见默认值：

```bash
export GCBV_REF_SO="${GCBV_REF_SO:-/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so}"
test -f "$GCBV_REF_SO"
```

示例：

```bash
./build/RISCV/gem5.opt \
  --outdir /tmp/debug/coremark_200k_basic \
  ./configs/example/kmhv3.py \
  -I 200000 \
  --generic-rv-cpt /nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin \
  --raw-cpt \
  --enable-bp-db tage basic
```

说明：

- `tage` 会生成 `TAGEMISSTRACE`
- `basic` 会额外生成 `BPTRACE`
- 先保证 gem5 和 RTL 用同一个 `-I`

## 4. 分析顺序

先看计数器，再看 trace：

1. gem5 `stats.txt` 中的
   `updateAllocSuccess / updateAllocFailure / updateAllocFailureNoValidTable / updateResetU / resolveBranchHasProvider / resolveBranchUseProvider / resolveBranchHasAlt / resolveBranchUseAltTable / resolveBranchUseBaseTable / mispredictBranch*`
2. RTL `CondTrace_*` 聚出来的同口径总量
3. 再看热点 branch 的 `provider/alt/useAlt/alloc`

统计名会随 gem5 commit 变化；以上是当前 checkout 的名字，分析历史归档时要回到对应 commit 确认递增语义。

注意两个口径坑：

- `TAGEMISSTRACE.useAlt` 表示 `pred.useAlt`，不等于 stats 里的 `resolveBranchUseAltTable`
- 要和 stats 对齐时，应该优先看 `useAlt && altFound`

## 5. 脚本

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
