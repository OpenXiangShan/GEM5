# XiangShan Trace-Driven Simulation 使用手册

面向当前代码的 trace 模式使用说明，涵盖命令行参数、默认行为以及常见调试入口。

## 1. 快速开始

```bash
# 构建
scons -j$(nproc) --gold-linker build/RISCV/gem5.opt

# 单条 trace（FS）示例
./build/RISCV/gem5.opt configs/example/kmhv3.py \
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

## 2. Trace 专用命令行参数（configs/example/kmhv3.py）

- `--enable-trace-mode`：开启 trace 回放（同时放宽 `--generic-rv-cpt` 要求并关闭 difftest）。
- `--trace-timing-ptw`：**可选**，在 trace 模式下启用 timing TLB/PTW（默认关闭；使用静态页表模拟硬件 PTW 成本，不模拟 Linux 行为）。
- `--trace-ptw-reserved-bytes=<N>`：为页表预留的物理内存大小（默认 64MiB），并从 trace 地址映射窗口尾部扣除以避免别名。
- `--trace-ptw-page-size={4k,2m}`：静态映射页大小（默认 `4k`；`2m` 为 superpage 粗粒度映射）。
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

开启 `--trace-timing-ptw` 后：
- 配置脚本会保持 `cpu.mmu.functional=False`（走 timing 翻译路径），并在 trace 初始化阶段安装静态页表（SV39）。
- `traceAddrSize` 会减去 `--trace-ptw-reserved-bytes`，页表放在映射窗口尾部预留区间，避免 trace 地址映射别名到页表页。

### 2.1 Timing-PTW：静态页表如何构建、如何让 PTW 去访问

该功能的核心目标是：在**无 OS/无 bootloader** 的 trace 回放环境中，仍让 TLB miss 触发 gem5 的 timing page-table walker，从而把 PTW 的 cache/memory 访问成本计入统计。

#### 2.1.1 映射策略：identity-map 覆盖 trace window

- 只为 trace 地址映射窗口 `[traceAddrBase, traceAddrBase + traceAddrSize)` 构建静态映射。
- 采用 **SV39** 三层页表，并做 **VA==PA** 的 identity mapping：
  - leaf PTE 设置 `V=1, R/W/X=1, A/D=1`；
  - PPN 直接由物理页基址得到（`PPN = PA >> PGSHFT`）。
- `--trace-ptw-page-size` 控制 leaf 粒度：
  - `4k`（默认）：填充 L0 leaf（4KiB 页）。
  - `2m`：使用 2MiB superpage，在 L1 直接放 leaf（减少 PTW 流量和页表占用）。

#### 2.1.2 页表放置：预留物理区 + shrink trace window 避免别名

trace mode 的地址映射可能是 modulo/线性映射，为避免 trace 地址落入页表页导致自毁，需要把页表放在“永远不会被 trace 映射命中”的物理区：

- 开启 `--trace-timing-ptw` 时，配置脚本会从 `traceAddrSize` 尾部扣除 `--trace-ptw-reserved-bytes`。
- 页表区域固定放在扣除后的窗口尾部：`pt_region_base = traceAddrBase + traceAddrSize`，范围为 `[pt_region_base, pt_region_base + reserved_bytes)`。
- trace 初始化阶段会检查该页表区域与 trace window 不重叠，并用 `System.physProxy` 直接把页表页写入物理内存（这一步发生在仿真开始前，不属于 PTW 的“访问成本”本身）。

#### 2.1.3 触发 PTW：安装 PRV/SATP + 使用 timing 翻译路径

要让 walker 真正去“走内存层级”读 PTE，必须同时满足：

- 安装翻译上下文：trace 初始化时写 `MISCREG_PRV=PRV_S` 并设置 `MISCREG_SATP` 为 `SV39`，`satp.ppn` 指向 root page table 的物理页号。
- 让翻译走 timing 路径：配置脚本在 timing-PTW 模式下设置 `cpu.mmu.functional=False`，使 ITB/DTB miss 触发 timing TLB/PTW。

满足以上条件后，TLB miss 会触发 page-table walker 按 `SATP` 指向的 root/L1/L0 去内存读取 PTE；由于这些 PTE 页位于正常物理内存空间，walker 的读取请求会经过 cache/NoC/DRAM（含 DRAMSim3）并自然体现在统计中（例如 `*_walker_cache.*Accesses/*Misses`）。

#### 2.1.4 Prefetcher 的翻译路径一致性

本仓库的硬件 prefetcher 对虚拟地址 prefetch 也需要地址翻译；其翻译模式与 `cpu.mmu.functional` 绑定：

- default trace（functional MMU）：prefetch 翻译走 functional。
- timing-PTW：prefetch 翻译走 timing，从而与 TLB/PTW 的设定一致。

实现入口可参考：`src/cpu/o3/trace/TraceFetch.cc`、`configs/common/xiangshan.py`、`configs/common/PrefetcherConfig.py`、`src/mem/cache/prefetch/queued.cc`。

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
