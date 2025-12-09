# XiangShan Trace-Driven Simulation 使用手册

面向当前代码的 trace 模式使用说明，涵盖命令行参数、默认行为以及常见调试入口。

## 1. 快速开始

```bash
# 构建
scons -j$(nproc) --gold-linker build/RISCV/gem5.opt

# 单条 trace（FS）示例
./build/RISCV/gem5.opt configs/example/xiangshan.py \
  --enable-trace-mode \
  --trace-file=/path/to/trace.champsimtrace.xz \
  --trace-format=champsim \
  --warmup-insts-no-switch=50000000 \
  --maxinsts=1000000 \
  --trace-enable-decoupled-bp \
  --trace-mispredict-penalty=8 \
  --stats-file=m5out/stats.txt
```

批量/并行脚本见 `docs/trace_tools.md`（`run_trace_champsim.sh`, `parallel_trace_sim.sh`, 分布式调度等）。

## 2. Trace 专用命令行参数（configs/example/xiangshan.py）

- `--enable-trace-mode`：开启 trace 回放（同时放宽 `--generic-rv-cpt` 要求并关闭 difftest）。
- `--trace-file=<path>`（必需） / `--trace-format={champsim,cbp2025}`（默认 `champsim`）。
- `--warmup-insts-no-switch=<N>`：仅重置统计的 warmup（不换 CPU）。
- `--maxinsts=<N>`：统计阶段指令数，复用通用参数。
- `--trace-enable-decoupled-bp`：在 trace 模式下使用解耦 BP 前端。
- `--trace-checkpoint-interval=<N>`：reader 检查点间隔，默认 64。
- `--trace-disable-bp-validation`：跳过 BP 预测与 trace 真值的比较。
- `--trace-mispredict-penalty=<cycles>`：mispredict 后 Fetch stall 周期（默认 8）。
- `--trace-disable-wrongpath` / `--trace-wrongpath-use-traceinst`：控制 wrong-path 注入策略。

地址映射（CPU 参数，可在配置脚本中设置）：
`traceAddrMapMode`（`linear|hash`，默认 `linear`）、`traceAddrBase=0x80000000`、`traceAddrSize=0x40000000`、`traceAddrPageAlign=true`。

## 3. Reader 行为概览

- 缓冲/历史：最多 1024 条 buffer + 4096 条历史窗口；`softSeekToInstruction()` 优先命中历史窗口，否则降级为硬 seek 重读。
- 检查点：Fetch 每 64 个 seqNum 调用 `createCheckpoint()`；压缩输入回滚时会通过重读快进。
- 分支目标推断：缺失 target 时，用“pending 指令 + 下一条 PC”推断，必要时修正 taken target mismatch。
- 压缩支持：ChampSim 通过 `gzip -dcq` / `xz -dc` 管道，CBP2025 支持 raw/gzip。
- 访存值：ChampSim trace 无数据时生成确定性伪值；未写入 mem size（`memSizes` 为空）。
- 统计：`TraceReaderStats` 挂在 CPU/fetch 名下（例如 `system.cpu.traceReader.stats.instrRead`）。

## 4. Trace 格式

- **ChampSim**：PC、branch flag/结果、2 dst + 4 src regs、2 dst + 4 src 地址；`.bin/.gz/.xz`。
- **CBP2025**：分支类型+nextPC+taken、有效地址+size、寄存器依赖与值；`.gz`/raw。
离线解析：`util/trace/dump_champsim_trace.py`（支持 `--format cbp2025`）、`util/trace/count_champsim_trace_insts.py`。

## 5. 监控与调试

- 调试标志：`--debug-flags=Fetch,TraceReader,O3CPU`（BP 相关加 `BPred,DecoupleBP`，访存加 `LSQ,MemDepUnit`）。
- 关键统计：`system.cpu.ipc`、`system.cpu.committedInsts`、`system.cpu.fetch.*`、`system.cpu.branchPred.*`、`system.cpu.icache/dcache.*miss_rate`.
- Trace 源验证：`util/trace/dump_champsim_trace.py --start-index <N> --count <K>`。
- 常见问题：
  - 路径错误：确保 `--trace-file` 存在（`file` 命令检查格式）。
  - 无统计：检查 `--maxinsts` 与 `--warmup-insts-no-switch` 是否过小；确认格式选择正确。
  - 回滚异常：超过历史窗口时会重读 trace，压缩输入可能较慢；必要时缩小 `--trace-mispredict-penalty` 或调小并行度。
