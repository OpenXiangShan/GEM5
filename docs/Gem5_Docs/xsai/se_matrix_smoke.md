# SE 模式 Matrix Smoke 实现说明

## 目标

这份实现的目标不是完整支持 AME，也不是直接推进到 FS/Linux。

当前目标很明确：

- 在 gem5 中补齐当前软件栈真正需要的最小 matrix 子集
- 让 gem5 **初步支持在 SE 模式下运行用户态 AME 程序**
- 先把功能跑通，作为后续 FS 工作前的 smoke 基线
- 把下面这些程序作为后续回归基线
  - `libc_mmap_smoke_xsai`
  - `precomp_rand_repro`
  - `gemm_precomp`
  - `hello_xsai` 作为更长链路的补充验证

## 当前范围

当前分支只做 gem5 侧的 SE smoke 支持。

包含：

- 当前 smoke 路径需要的最小 matrix 指令 decode
- `MatrixController` 中的最小 matrix 架构状态，`RiscvISA::ISA` 保留适配层
- matrix tile 配置
- matrix load/store helper
- `int8 x int8 -> int32 accumulate` 的最小功能模型
- `msyncreset` / `mrelease` / `macquire` 的最小 token 语义
- SE 模式下为了 syscall/trap 正确性而关闭 rename folding 的配置修复
- CUTE-aligned `MatrixController` 行为级控制骨架
- 基于 fixed/analytic ready tick 的 matrix issue/scoreboard/token timing，可通过
  `mrelease/macquire` 观察 acquire stall
- 行为级 CUTE-to-L2 request pipeline：LocalMMU 仲裁、source id 占用、
  TL-A 请求带宽、读数据/写 ack 响应端口
- matrix 指令映射到 O3 op class，能参与 O3 issue/execute 资源模型
- `system.cpu.isa.*` 下的 matrix task、memory、timing、acquire stall 统计
- RiscvISA 参数化的 matrix analytic timing 常量

不包含：

- 完整 AME 指令覆盖
- FS/raw Linux bring-up
- 周期准确的 matrix 执行时序模型或 RTL cycle-accurate CUTE 模型
- matrix 单元和 cache hierarchy 的真实 packet 级时序交互
- 真实 cache miss、DRAM backpressure、TL retry 的逐级 RTL 时序展开
- 比当前 smoke 程序更广的 matrix 指令族

## 当前实现思路

### 1. matrix 状态放在 `MatrixController`

当前 SE smoke 仍保留简单功能模型，但状态已经从 `RiscvISA::ISA`
迁移到 `src/matrix/MatrixController` 中，ISA 只保留 decoder 调用适配层：

- tile 尺寸
  - `tileM`
  - `tileK`
  - `tileN`
- matrix 数据缓冲
  - AB matrix register storage
  - C accumulator register storage
- token 状态
  - 32 个 matrix token

这对当前 smoke 用例已经足够，因为当前首先保证功能正确性，同时把控制面
拆出来作为后续性能模型的挂点。
可以把它理解成：

- 当前先把 matrix 单元建成一个 gem5 内部的功能模型
- 先保证用户态程序能“算对、跑通”
- 控制面开始维护 CUTE-aligned FIFO、scoreboard、LocalMMU 统计和行为级
  ready tick

### 2. matrix 内存访问走 translating proxy

`mlae8` / `mlbe8` / `mlce32` / `msce32` 当前使用：

- SE 下：`SETranslatingPortProxy`
- 兼容路径下：`TranslatingPortProxy`

这样实现简单，而且和当前 smoke 目标匹配。

这里的含义是：

- 当前 matrix load/store 主要是按“功能正确”的方式访问内存
- 目标是保证用户态程序看到正确结果
- 功能数据仍在指令执行时同步产生或写回
- matrix 数据计算不做 cycle 级数据通路建模，也不按 `M/N/K` 展开计算流水；
  只保留固定/粗粒度 ready tick
  以维持依赖和 token 语义

因此当前实现里：

- 计算本身是功能模型
- 和 cache 的交互也主要是功能正确优先
- AME 计算指令只有固定抽象 timing，用于控制 token/acquire 的可观测 stall
- matrix load/store/mmacc 同时映射到 O3 `MemReadOp` / `MemWriteOp` /
  `IntMultOp`，配置和同步类指令映射到 `IntAluOp`
- 默认 CUTE 访存不向共享 L2 发逐请求 timing probe packet；只保留
  controller 内部的高性能抽象 request/source/response timing

### 3. CUTE-to-L2 请求管理是行为级管线

matrix load/store 的性能侧不再只用“请求数乘固定延迟”的总量公式，而是在
`MatrixController` 中按请求推进一个抽象 LocalMMU/L2 管线：

- LocalMMU 仲裁延迟：`matrix_local_mmu_arb_cycles`
- CUTE-to-L2 请求管线延迟：`matrix_l2_request_pipeline_cycles`
- LocalMMU source id 数量限制，source 在读响应或写 ack 返回前保持占用
- source id 分配按 RTL `Cute2TL` 的空闲 source 选择方式抽象：有空闲 source
  时取最高编号，source 全满时等待最早释放的 source；选择点是抽象 TL-A
  issue slot 的 fire tick，而不是更早的 request-ready tick
- TL-A 请求带宽：`matrix_local_mmu_issue_per_cycle`
- C store request 数按 MatrixMN rounding、stride 和 64B 外部传输粒度切分，
  避免低估非对齐 store 的抽象访存压力
- 读响应和写 ack 共用一个抽象响应端口，服务间隔由
  `matrix_l2_response_pipeline_cycles` 控制
- response 完成后在 controller 内更新 completion/store barrier/token timing；
  不再建模端口侧 response queue 的 1-2 拍可见边界

这和 RTL 的 LocalMMU/Cute2TL 控制语义只做到必要的行为级近似，不追逐
1-2 拍的 requester-port 细节。默认生产路径不再把 CUTE 接成一个真实
`matrix_l2_port` timing requester，也不会向 shared L2 发送逐 64B probe
packet。controller 内部仍保留 request count、LocalMMU issue throughput、
source outstanding、读/写响应延迟、统一 response slot 和 token/acquire
可见 timing，用这些粗粒度约束表达对性能有明显影响的长延迟访存行为。

需要注意的是，RTL 里 CUTE 没有自己的 L2 cache，架构上仍是共享 CPU 的
L2 fabric；但 gem5 默认模型现在只在 controller 内部抽象这条路径，不连接
独立的 CUTE requester port。旧的 `MatrixL2RequestPort`、逐请求 timing
callback、端口侧 response/helper 和 `memoryL2Port*` 统计已经删除；后续若要
重做 shared-L2 探针，应该作为独立实验路径重新接入，不能混入默认模型。

### 4. token 语义接入行为级 timing

当前 token 模型仍是同步 smoke 可用的简化版，但 `mrelease` 不再总是立即
增加 token，而是生成一个 ready tick：

- `msyncreset(tok)`：把 token 清零
- `mrelease(tok)`：等待前序已排 matrix task 的 analytic completion tick
  后产生 token event
- `macquire(tok, target)`：若 token 未达到 target，则按 controller 内部
  预测 ready tick quiesce；默认不再等待 CUTE L2 port in-flight 状态

这不是完整的异步完成模型，但已经能把 CUTE matrix-release-acquire
控制路径反映到 gem5 仿真时间中。当前 `mrelease` 在行为级模型中按
conservative barrier 处理，不表示 RTL 精确 release 条件。

### 5. 可调参数与统计

`RiscvISA` 暴露了当前 analytic timing 常量：

- `matrix_issue_interval_cycles`
- `matrix_load_base_cycles`
- `matrix_store_base_cycles`
- `matrix_zero_cycles`
- `matrix_compute_base_cycles`，固定 compute ready latency，不随 tile shape 展开
- `matrix_compute_read_cycles`，固定 compute source read latency
- `matrix_release_cycles`
- `matrix_local_mmu_issue_per_cycle`
- `matrix_local_mmu_arb_cycles`
- `matrix_l2_request_pipeline_cycles`
- `matrix_l2_response_pipeline_cycles`
- `matrix_local_mmu_read_latency_cycles`
- `matrix_local_mmu_write_ack_latency_cycles`

其中对行为级性能趋势影响最大的旋钮是 LocalMMU request issue throughput、
CUTE-to-L2 request/response pipeline latency、读响应/写 ack latency、load/store
base latency 和 release barrier latency。它们在通用配置脚本中也有对应的命令
行入口：

- `--matrix-local-mmu-issue-per-cycle`
- `--matrix-l2-request-pipeline-cycles`
- `--matrix-l2-response-pipeline-cycles`
- `--matrix-local-mmu-read-latency-cycles`
- `--matrix-local-mmu-write-ack-latency-cycles`
- `--matrix-load-base-cycles`
- `--matrix-store-base-cycles`
- `--matrix-zero-cycles`
- `--matrix-release-cycles`
- `--matrix-compute-base-cycles`
- `--matrix-compute-read-cycles`

这些选项用于粗粒度敏感性分析；不要把它们理解成 RTL 每一级 FIFO 或每一级
流水 stage 的精确控制。

运行后可在 `stats.txt` 的 `system.cpu.isa.*` 下观察：

- task 计数：`tasksAccepted`、`mmaTasks`、`cStoreTasks`
- LocalMMU 行为级统计：`memoryRequests`、`memoryBusBytes`
- CUTE-to-L2 管线统计：`memoryPipelineRequests`、
  `memoryPipelineReadResponses`、`memoryPipelineWriteAcks`、
  `memoryPipelineSourceStallTicks`、`memoryPipelineRequestQueueTicks`、
  `memoryPipelineResponseQueueTicks`、`memoryPipelineMaxOutstanding`
- 默认模型不会驱动 shared-L2 requester 端口，也不再注册 `memoryL2Port*`
  旧探针计数器
- timing 统计：`timingTasks`、`timingQueueTicks`、`timingBusyTicks`
- 同步统计：`acquireStallEvents`、`acquireStallTicks`、`tokenReleaseEvents`

### 6. SE 模式关闭 rename folding

`configs/example/se.py` 里会关闭：

- `enableMoveElimination`
- `enableConstantFolding`

原因是：

SE 下 trap/syscall 对寄存器的写回绕过了正常的 renamed writeback 路径。
如果保留 rename-time folding/elimination，O3 里用户态可见的 architectural
寄存器更新可能会丢失。

这件事虽然不是 matrix 指令本身，但真实 smoke 程序依赖 libc/syscall，
所以它是 SE matrix smoke 能稳定跑起来的前提。

## 当前支持的指令子集

当前只实现了现有 smoke 程序真正用到的子集：

- 配置 / 状态
  - `msettilem`
  - `msettilek`
  - `msettilen`
  - `mzero1r`
- 内存
  - `mlae8`
  - `mlbe8`
  - `mlce32`
  - `msce32`
- 计算
  - `mmacc.w.b`
- 同步 / token
  - `msyncreset`
  - `mrelease`
  - `macquire`

## 和主线的关系

当前实现默认依赖最新 `xs-dev` 已经包含的两块修复：

- aligned-L2 foreign snoop 过滤
- classic-L2 的 DRRIP slice 参数初始化

这两块已经单独拆成 PR 并进入主线，所以当前 SE matrix smoke 分支不再重复携带。

## 当前验证结论

已经验证通过：

- `libc_mmap_smoke_xsai`
  - 通过
- `precomp_rand_repro`
  - 通过
  - `errors_shown=0`
- `gemm_precomp`
  - 通过
  - 8 个 precomputed case 全过

补充验证：

- `hello_xsai`
  - 能正常进入 userland
  - allocator 测试全部通过
  - 能进入 randomized matrix fuzz
  - 在扩大的 timeout 窗口内没有完整跑完，但在 timeout 前没有观察到 correctness failure
- MatrixController 行为/timing 单测
  - `scons --unit-test build/RISCV/matrix/matrix_controller.test.opt -j16`
  - `build/RISCV/matrix/matrix_controller.test.opt`
  - 11/11 通过
  - 覆盖 `mmacc.w.b` 数据结果、issue/scoreboard timing、`mrelease`
    barrier、issue interval clamp、LocalMMU source id 耗尽、读响应/写 ack
    分流、source 在抽象 TL-A issue tick 选择、控制面 LocalMMU allocator
    按最高空闲 source 分配、非对齐 C store 的 MatrixMN rounding/stride
    request 切分和 pending token reset。
- 完整 RISCV 目标构建
  - `scons build/RISCV/gem5.opt -j16`
  - 通过
  - 仅有 libpng、HDF5、backtrace 相关可选依赖警告
- `coremark-2-iteration.bin` 1000 指令短仿真
  - 正常以 max instruction count 退出
  - `config.ini` 可见 matrix analytic timing 与 L2 request pipeline 参数
  - `stats.txt` 可见 `system.cpu.isa.*` 下 matrix timing/task/L2 pipeline 统计
  - 该 workload 不含 matrix 指令，因此只验证参数和统计 plumbing
- 2026-05-25 默认抽象路径短仿真
  - `/tmp/gem5-cute-abstract-cut2`
  - 正常以 max instruction count 退出
  - `config.ini` 不再出现 `system.cpu.isa.matrix_l2_port`
  - `stats.txt` 不再出现 `memoryL2Port*` 旧探针统计
- 历史验证里曾跑过 `coremark` 短仿真和 full-gem5 plumbing 检查；这些结果只能证明旧的探针路径曾经可接通，不能代表当前默认模型。

## 一条典型运行命令

```bash
cd GEM5
./build/RISCV/gem5.opt \
  --outdir=/tmp/gem5-se-gemm-precomp \
  configs/example/se.py \
  -c firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp \
  --enable-riscv-vector --no-pf
```

## 已知限制

- 当前还是 smoke-oriented 的功能模型，不是完整 AME 模型
- token 初始 ready tick 来自行为级 analytic timing；默认模型没有
  CUTE L2 port 的 in-flight request/response/source 状态
- `mrelease` 当前在行为级模型中作为前序 matrix task 的 conservative
  barrier，不是 RTL 精确 release 条件
- 当前 matrix timing 只能用于粗粒度行为级 timing 探索；compute 侧是固定
  ready tick，不用于性能评估，访存侧是 CUTE-to-shared-L2 request 管理的
  行为级性能模型
- 默认 CUTE requester 端口不接入 `L1ToL2Bus`/`L2CacheWrapper`，也不处理
  xbar retry、真实 cache response 或端口侧 response queue
- 旧的端口 reset/drain、response 回写逻辑和端口侧测试已经从代码中删除
- shared-L2 backpressure/retry/response 不作为当前默认行为级反馈输入；
  真实 cache/DRAM latency、MSHR 占用、TL retry 没有逐级展开成 RTL 级事件
- 当前计算和内存功能结果主要是功能正确优先；计算 timing 不随数据通路
  cycle 展开，访存 timing 通过
  scoreboard/token/acquire 推进仿真时间，不改变同步功能数据路径
- CStore 只保留抽象 request 数和 response timing，不发送 shared-L2
  `WriteReq` packet
- `matrixsimpletest-riscv64-xs.bin` 当前可进入 real simulation，但仍会早期触发
  既有 O3 commit-stuck，不能作为 matrix timing 正确性回归依据
- `hello_xsai` 的 randomized fuzz 明显比核心 smoke 用例更耗时
- FS/Linux 支持应该作为后续独立工作推进

## 下一步建议

这个 SE smoke 支持进入主线后，建议：

1. 把 `libc_mmap_smoke_xsai`、`precomp_rand_repro`、`gemm_precomp`
   固化为回归基线
2. 把后续主要 bring-up 重心转到 FS
3. 用可稳定运行的 matrix workload 校准 controller 内部抽象 LocalMMU/L2
   timing 参数，并和 RTL 关键吞吐/队列计数对齐
4. 只有在真实 workload 需要时，再继续扩展 matrix 指令覆盖和时序模型
