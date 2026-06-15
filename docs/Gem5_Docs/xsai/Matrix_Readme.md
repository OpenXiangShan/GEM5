# Matrix ISA / O3 / CUTE Backend

## RTL 审阅文档

- [Matrix 设计文档](Matrix_Design.md)
- [Matrix 参数文档](Matrix_Params.md)

## 目标

当前 gem5 RISC-V O3 路径中已经接入 matrix 执行链路，目标是让 matrix ISA decode、O3 发射/提交、`MatrixAmuBuffer` 转发、CUTE backend、matrix register file、matrix load/store timing 能在 SE workload 中形成闭环，并给 RTL / gem5 reviewer 提供可继续校准的行为级性能模型。

当前重点不是完整 RTL 等价模型，而是建立可运行、可观测、可继续校准的 matrix 主路径，方便 RTL / gem5 reviewer 明确本 PR 已覆盖的抽象层次和后续需要补齐的 timing 细节：

- ISA 能解析当前 matrix GEMM / firmware 使用的 matrix 指令编码。
- O3 能识别 matrix 指令类别并按 matrix FU / IQ 路径调度。
- matrix 指令能通过 O3 的 `MatrixAmuBuffer` 进入 CUTE backend。
- backend 能执行 matrix load/store、mzero、streaming MMA timing、release completion。
- LocalMMU / fill table / per-bank fill FIFO / backend register-bank 仲裁能表达第一阶段带宽、bank conflict、load-store timing 和可观测反压。

## 启用方式

### Matrix

当前 matrix 路径挂在 RISC-V O3 CPU 上，默认已经开启，不需要额外的命令行参数。关键开关是 `BaseO3CPU` 的 SimObject 参数：

- `enableMatrixBackend = True`
- `enableMatrixMemPort = True`
- `enableMatrixMlsQueue = True`

也就是说，使用 `DerivO3CPU` / XiangShan O3 配置并连接 cache hierarchy 时，matrix backend、`matrix_mem_port` 和 MLS queue 默认都会参与模拟。若某个配置脚本曾经显式关闭这些参数，需要在 `m5.instantiate()` 前对目标 CPU 重新打开：

```python
for cpu in test_sys.cpu:
    cpu.enableMatrixBackend = True
    cpu.enableMatrixMemPort = True
    cpu.enableMatrixMlsQueue = True
```

这三个参数也可以用于隔离 matrix 对普通标量 / 向量路径的影响。关闭后如果 workload 真的执行 matrix 指令，当前实现会 fail-fast，而不是静默回退到不完整的 functional stub。

### RVV simple 实现

`--rvv-impl=simple` 是编译期选项，用于选择 `src/arch/riscv/isa/vector/simple/base.isa` 这一套 RVV ISA 实现；它不是运行时参数。开启 simple RVV 时需要重新构建 gem5：

```bash
scons build/RISCV/gem5.opt --gold-linker -j64 --rvv-impl=simple
```

运行 workload 时仍然要加运行时开关 `--enable-riscv-vector`，否则 CPU / System 层不会打开 RISC-V vector extension：

```bash
./build/RISCV/gem5.opt \
  configs/example/kmhv3.py \
  --raw-cpt \
  --generic-rv-cpt=<raw-bin> \
  --disable-difftest \
  --enable-riscv-vector
```

因此，matrix + simple RVV 的基本流程是：先用 `--rvv-impl=simple` 构建，再用 O3 / XiangShan 配置运行，并在运行命令里加 `--enable-riscv-vector`。matrix 默认开启；RVV 需要编译期实现选择和运行期开关同时满足。

## 总体改动范围

### ISA / Decode

- 新增 `src/arch/riscv/insts/matrix_static_info.hh`，在 static inst 层保存 matrix 指令分类、route、commit boundary、源/目的寄存器摘要和 load/store 形状信息。
- 扩展 RISC-V matrix decode：
  - `mlae` / `mlbe` 合并 e8/e16 路径，元素宽度由 `RD[4:3]` 提供，matrix register index 使用 `RD[2:0]`。
  - `mlce32` / `msce32` 使用 C register e32 路径，并检查 `md >= 4`。
  - `mzero`、`mfmacc_s_h`、`mmacc_w_b` 生成 `MatrixExecPayload`，向 backend 透传 `mtilem` / `mtilen` / `mtilek` 和 element type。
- 保留 renamed tile carrier：`mtilem`、`mtilen`、`mtilek`。

### O3 / Scheduler

- 新增 matrix op class / FU class，并在 `configs/common/FUScheduler.py` 中加入 matrix issue queues：
  - `matrixIntIQ`
  - `matrixCsrIQ`
  - `matrixExecIQ`
  - `matrixMemIQ`
- 增加 matrix wakeup channel 和 matrix misc register read/write helper，使 matrix carrier 能走独立调度资源。
- 扩展 O3 dyn inst metadata，记录 matrix static info、payload、MLS queue slot、AMU buffer 状态。
- 新增 `MlsUnit` / `MlsVirtualQueue` / `MlsReplayQueue`，为 matrix memory instruction 建立独立的早期地址/权限检查、TLB replay 和虚拟队列路径。
- 新增 `MatrixAmuBuffer`，在 writeback/commit 后把需要进入 CUTE backend 的 matrix request 送入 backend。
- O3 CPU 持有 `matrix::DetailedCuteBackend`，通过 `serviceMatrixBackend()` 推进 backend step，并在 completion 后释放 token 或清理 owner。

### Matrix Backend

新增 `src/matrix` 下的第一版 CUTE backend。它的定位不是完整 RTL 逐拍模型，而是在 O3 commit 之后承接 `MatrixAmuBuffer` 送来的 matrix request，把 matrix load/store、MTE compute、mzero 和 release 跑成一个可观测的 backend 闭环。

backend 的边界如下：

- RTL 侧对应口径：抽象建模 AML/BML/CML、ADC/BDC/MTE/CDC、matrix regfile、LocalMMU 和 register-bank resource。
- gem5 侧对应口径：它是 O3 commit 后的 matrix backend，不参与 ISA decode。matrix memory 通过 O3 暴露的 `matrix_mem_port` 作为普通 gem5 `RequestPort` 接入当前 cache hierarchy；这不是 RTL 专用 matrix L2 / TL 通道。
- 数据口径：timing-connected path 中，load 数据来自 cache `ReadResp` payload，store 数据来自 C register snapshot 打包后的 64B cache-line payload 和 byte enable；不再依赖 cache ack 之后的 `loadTile()` / `storeTile()` functional 搬运来完成 O3 连接路径。
- timing 口径：LocalMMU 仍表达 AML/BML/CML queue、source ID、beat issue 和 backend-visible completion 边界；`matrix_mem_port` 负责把 beat 变成真实 gem5 timing `Packet` 并处理 response / retry；fill table/per-bank FIFO、backend register-bank 仲裁和 streaming MatrixTE 继续表达 bank conflict、读写拍数、result FIFO 和 completion 边界。

backend 的基本工作方式是：

1. O3 将已经允许进入 AMU 的指令封装成 `CuteRequest`。
2. `DetailedCuteBackend` 把 request 放入 decoded FIFO，并用 scoreboard 检查 matrix A/B/C register 依赖。
3. request 被分配到对应路径：
   - matrix load/store 进入 AML / BML / CML 风格的 LSU path。
   - MMA 进入 compute path，模拟 ADC / BDC / MTE / CDC 的粗粒度执行阶段。
   - `mzero` 进入 arith path，直接更新 matrix register file。
   - release 等 pending store drain 后再完成；当前实现不等待所有非 store backend work drain。
4. backend 每个 `step()` 推进 LocalMMU、fill drain、register-bank 仲裁、compute unit 和 completion queue。
5. completion 返回 O3，O3 再释放 token 或清理 matrix owner 状态。

### Matrix Load / Store 原理

matrix load/store 现在有 timing-connected 路径。backend 仍先用 `LocalMMUModel` 表达 AML/BML/CML queue、source ID、beat issue 和 backend completion 边界；当 `matrix_mem_port` 已连接时，LocalMMU issue 出来的 beat 会被转换成 `MatrixTimingMemoryAdapter::Request`，再由 O3 的 `MatrixMemPort` 发送普通 gem5 timing `Packet`。因此这条路径会经过当前配置下的 cache hierarchy、request retry 和 `recvTimingResp`，但它不是 RTL 专用 matrix L2 / TL path，也没有在 matrix backend 内部实现 cache tag、MSHR、replacement 或 coherence 策略。

地址和数据规划由 `MemoryLoader.hh` 完成：

1. `TimingLoadPlan` 根据 tile shape、stride、transpose 和 element size，把 matrix load 覆盖的 guest address 拆成 64B cache-line beats；有 `ThreadContext` 时先做 functional translation，beat 内记录 line offset 到 tensor byte offset 的映射。
2. `TimingStorePlan` 先读取 C register snapshot，再把需要写回的元素打包到 64B `lineData`，同时生成 `byteMask` 和 `byteEnable`，用于 partial-line store。

load 路径大致是：

1. `MatrixAmuBuffer` 将 committed load request 送入 backend。
2. backend 建立 `TimingLoadPlan`，把每个 cache-line beat enqueue 到 `LocalMMUModel`。
3. `LocalMMUModel` 选出 source ID 和 beat 后，backend 附上 physical line address / packet size，经 `matrix_mem_port` 发送 `ReadReq`。
4. cache `ReadResp` 回来后，backend 按 source ID 完成 LocalMMU response；response 只有在 bounded matrix L2 fill table、target-bank FIFO 或 BML bypass path 能接收时才被 backend service。service 完成后 source ID 释放，response payload 同时 scatter 到 task-local tensor byte buffer。
5. 如果 `ReadResp` 数据大小、beat index 或 tensor byte mapping 不匹配，当前 task 进入 `Unsupported`；timing-connected path 不再在 response 到齐后回退到 `loadTile()` functional 读取。
6. fill table entry 用独立 handle 跟踪后续 MatrixReg loader write chunk，避免 source ID 复用和 fill drain 生命周期混在一起；当前普通 64B response 按 `MatrixRegResource::EntryBytes=32` 拆成 2 个 32B chunks，A/B/C 都走同一个 chunk 计算口径。
7. fill table 还有 per-bank FIFO depth=2；每个 backend step 可按 target bank drain candidate，A/B 写 chunk 仍走 register-bank 仲裁，C fill candidate 当前直接 retire。所有 cache response 和 MatrixReg write chunk 完成后才产生 load completion。
8. BML 有可选 fill-table bypass：显式开启或 `matrixReduceWidthBytes >= matrixOutsideDataWidthBytes` 时，BML response 可不占 fill table，直接尝试 MatrixReg write chunk。默认 32B reduce width < 64B outside width，因此旁路关闭。

store 路径大致是：

1. backend 先从 C register 读取 snapshot，建立 `TimingStorePlan`，每个 64B beat 携带写入数据、byte mask 和 byte enable。
2. `LocalMMUModel` issue store beat 后，`MatrixMemPort` 按 cache line 做 clean/invalidate 去重：首次遇到 line 时发送带 `PHYSICAL | CLEAN | INVALIDATE | DST_POU` flag 的维护请求，让当前 cache hierarchy 处理目标 line 的 dirty / ownership 状态。
3. 维护请求响应返回后，`MatrixMemPort` 再发送真正带数据和 byte enable 的 write packet；同 line 已经完成维护的后续 beat 直接发 write，同 line 正在维护的 beat 会排队等待。
4. write 响应返回后，backend 才把对应 source ID 记为 store ack 并释放；store completion 和 release 都会等待 pending store ack 清空。
5. store 数据不再通过 ack 之后的 `storeTile()` functional 写内存。最终写入何时命中、miss、进入 MSHR 或写回下层，由当前 gem5 cache hierarchy 决定；matrix backend 只维护 backend-visible 的 beat/source/completion 状态。

### MTE / MMA 原理

MTE / MMA path 是 CUTE compute 的分段抽象模型。ADC / BDC / CDC 被显式建模。当前实现会在 compute read 阶段把本次 MMA 需要的 A/B/C matrix register 内容读入 backend task state，作为后续逐 beat timing/functional 更新的快照；这不是说硬件一次把大矩阵整体吞进 MTE，而是 gem5 backend 用 task-local snapshot 保存 architectural matrix register 数据，方便按 8x8 tile 和 K-group 推进。

大致执行方式是：

1. backend issue MMA 前，scoreboard 确认 A/B/C source 和 C dest 的依赖关系。
2. compute read frontend 通过 register-bank 仲裁同时申请 A/B/C register read；读回后形成 `bufferedTensorA` / `bufferedTensorB` / `bufferedTensorC` snapshot。
3. ADC / BDC / CDC read 完成后才进入 MTE execute 阶段。
4. `MatrixTE` 根据 `mtilem` / `mtilen` / `mtilek` 和 element type 计算 `acceptedInputBeats`、FReduce tail、CDC writeback beats。当前抽象按 8x8 tile、32B reduce width 建模；在 supported `mtilen == 128` 形状下，full 128x128x64 int8 GEMM 对应 16 个 M tile、16 个 N tile、2 个 K group，因此是 512 个 MTE accepted input beats。
5. 当前 supported timing path 是 streaming 模型：MTE 每 step 在 result pipeline / FIFO 有容量时接受 input beat；每个 input beat 经过 6-cycle FReduce tail 后变成 result beat；result FIFO 默认深度 2，满时会记录 stall 并阻塞后续 result production/input acceptance；CDC 侧每 step 消费 result beat 并推进 writeback。
6. 当前 supported timing 只覆盖 `mtilen == 128` 的形状；int8 tile-level writeback 还要求 M/N/K 分别按 8/8/32 对齐。对满足条件的 int8 tile writeback，CDC writeback 再按 8x8 tile 与 K 维 32-element group 更新 full C tensor 中的一个 8x8 tile 区域，并用 `cdcTileWriteTensor` 暂存该 beat 更新后的 whole-tensor 结果。这里不是把一个物理 C tile entry 逐 beat 写入 RTL CRegFile，而是在 gem5 logical C tensor 上更新对应 8x8 区域。
7. 这里的 CDC beat grant 是行为级资源进度；当前 streaming path 没有完整复刻 RTL CDC reorder、after-ops 和所有 datatype 的逐拍行为。

因此，MMA 不是单纯加一个固定 latency；模型保留了“ADC/BDC/CDC 读、MTE 输入 beat、FReduce tail、result FIFO、CDC 分 beat 写回、task completion 边界”的中间状态。这里的 512 不是一次性读取 512 份完整矩阵，而是 16x16 个 8x8 C tile 乘以 2 个 K group 的 beat 数。限制也仍然存在：它还不是所有 datatype / CDC reorder / RTL ready-valid 细节的完整逐拍等价模型。

### 文件职责

- `CUTETOP.hh` / `CUTETOP.cc`
  - backend 主状态机。
  - 接收 `CuteRequest`，调度 LSU / MMA / Arith / Release task。
  - 维护 completion queue、pending store、release gating 和 backend active 状态。
  - 将 matrix LSU payload 拆成 64B LocalMMU beats，并把 load response 转成 fill table/per-bank FIFO 或 BML bypass 下的 MatrixReg loader write chunk。
- `CUTEParameters.hh`
  - 定义 ISA / O3 / backend 共享的 matrix request、completion、LSU/MMA 描述和 tensor payload。
- `TaskController.hh` / `TaskController.cc`
  - 定义 backend 抽象接口、decoded FIFO entry、task accept、dependency check、backend task life-cycle 和 trace。
- `MemoryLoader.hh` / `MemoryLoader.cc`
  - 定义 backend memory adapter。
  - 定义 `TimingLoadPlan` / `TimingStorePlan`，把 matrix tile memory footprint 拆成 cache-line timing requests。
  - `Gem5MatrixMemoryAdapter` 仍保留 functional adapter 接口，主要用于非 timing-connected 的隔离路径；O3 连接路径的数据闭环应通过 `matrix_mem_port` timing response / write ack 完成。
- `Scoreboard.hh` / `Scoreboard.cc`
  - 跟踪 matrix A/B/C register hazard 和 task 依赖。
- `MatrixTE.hh` / `MatrixTE.cc`
  - 第一版 MatrixTE timing 估算。
  - 建模 A/B/C read response、accepted input beats、FReduce tail、CDC writeback beats。
- `MRegFile.hh` / `MRegFile.cc`
  - matrix architectural state / whole-tensor functional storage。
  - 不是 RTL 物理 8-bank / 32B-entry SRAM 存储模型。
- `CUTETOP.hh` / `CUTETOP.cc` 中的 backend register-bank 仲裁
  - timing/resource helper，用于抽象 8 bank、32B entry、full-bank access。
  - A/B loader write priority。
  - C bank odd/even read/write conflict。
  - 1-cycle read response。
- `LocalMMUModel.hh` / `LocalMMUModel.cc`
  - 64B beat 级 LocalMMU timing model。
  - 每 cycle 最多 issue 一个 beat。
  - AML/BML/CML per-client request queue。
  - client round-robin issue。
  - 64 个 source ID / max outstanding。
  - standalone mode 可以用 fixed latency 产生 read response / store ack。
  - timing-memory mode 下 source ID 会交给外部 `matrix_mem_port`，等待真实 timing response；load response 被 fill table 或 BML bypass 接收后释放 source ID，fill drain 再由 fill handle / bypass progress 跟踪。

`src/matrix/SConscript` 当前注册 backend 生产文件，并注册 focused matrix gtest：`local_mmu_model.test`、`timing_load_plan.test`、`timing_store_plan.test`、`timing_store_backend.test`、`matrix_reg_resource.test` 和 `scoreboard.test`。当前 register-bank 仲裁是独立 timing/resource helper，不表示 `MRegFile` 已经实现 RTL 的物理 8-bank / 32B-entry SRAM 存储。

## 已建模的 timing / resource

- LocalMMU：
  - 64B beat。
  - 每 cycle issue 一个 beat。
  - 64 outstanding source ID 上限。
  - AML/BML/CML per-client queue。
  - client round-robin issue。
  - standalone fixed configurable latency。
  - timing-memory mode 的 external issue / response completion。
- Matrix memory port：
  - `matrix_mem_port` 是普通 gem5 cached `RequestPort`。
  - load beat 发 `ReadReq`，从 `ReadResp` payload 拼回 matrix tensor。
  - store beat 按 line 先做 PoU clean+invalidate 维护，再发携带 data / byte enable 的 write；同 line clean 完成后后续 beat 可直接 write。
  - request 被下游拒绝时进入 blocked queue，并通过 `recvReqRetry()` 重发。
- Matrix L2 fill table：
  - load response accept 时分配 bounded fill table entry，而不是 request issue 时提前占用。
  - fill table entry 使用独立 handle 管理后续 MatrixReg loader write chunk，source ID 在 response accept 或 BML bypass 完成后释放。
  - 全局 fill table 默认 4 entries，per-bank FIFO 默认 depth 2。
  - chunk 数由 response byte size 和 32B MatrixReg entry size 计算，普通 64B response 为 2 chunks。
  - fill table full 或 target-bank FIFO full 时 response 会先停在 backend pending response queue，source ID 继续占用，直到后端可以接收。
  - BML 可选绕过 fill table 直接写 MatrixReg chunk；默认配置下该旁路关闭。
- Register-bank 仲裁：
  - 8 bank。
  - 32B entry。
  - full-bank grant。
  - 1-cycle read response。
  - A/B loader write priority。
  - C bank odd/even read/write conflict。
  - 这是 timing/resource helper，不是 `MRegFile` 的物理存储布局。
- MatrixTE：
  - ADC / BDC / CDC read latency。
  - int8 accepted beats。
  - FReduce tail。
  - MTE result FIFO depth 和 FIFO full stall。
  - CDC beat grant / tile-level writeback 的第一版 timing/state。
- Trace：
  - 新增 `MatrixCuteTrace` debug flag。
  - 记录 backend submit、dependency、LocalMMU enqueue/issue/response、MatrixReg loader write grant/stall、CDC read/writeback 等关键事件。

## 当前未对齐点 / 已知限制

这些不是本 PR 已解决内容，需要在 review 时明确边界：

- Matrix memory path 已接入 gem5 timing port，但还不是 RTL 专用 matrix L2 / TL：
  - `matrix_mem_port` 通过普通 gem5 `RequestPort` 进入当前 cache hierarchy，能覆盖 `Packet`、`recvTimingResp` 和 request retry。
  - cache tag、hit/miss、MSHR、coherence、replacement 由通用 gem5 cache 配置决定，不是 matrix backend 内部的 RTL 对齐资源模型。
  - 尚未建模 CUTE / matrix 专用 TL source、L2 bank、专用 MSHR、source/channel 仲裁和 RTL matrix L2 refill protocol。
- Store 维护请求是当前 gem5 cache 集成策略：
  - store line 首次遇到时先发 `CLEAN | INVALIDATE | DST_POU` 维护请求，再发 data-carrying write，用于处理当前 cache model 下 dirty line / partial-line write 的正确性边界。
  - 已完成 clean 的 line 后续 beat 直接写；clean in-flight 的同 line beat 会排队等待。
  - 这不是已经校准过的 RTL matrix store protocol。
- LocalMMU / fill table backpressure 仍是行为级近似：
  - `matrix_mem_port` 已覆盖 request-side retry；fill table full 或 bank FIFO full 时 backend 会暂存 response 并继续持有 source ID。
  - 由于普通 gem5 `recvTimingResp()` 已经把 response 交给 CPU port，当前没有把 fill table full 真实反压回 cache/L2 response channel。
  - 还没有建模 TL/LLC 反压导致的所有 Decoupled bubble。
- Source ID 复用目前是空闲 ID 分配，不是严格 RTL source ID round-robin reuse。
- LSU fill table 还不是 RTL 等价：
  - 当前已建模 bounded fill table、per-bank FIFO、response accept 后 source release、fill handle drain、BML bypass 和 32B chunk。
  - 还没有完整建模 RTL 的 Matrix_MN 物理 sub-bank 级 fill FIFO、C loader repeat fill 的全部控制状态和 response.ready 对 L2 channel 的逐拍反压。
- Register-bank 仲裁是抽象 bank/resource 模型：
  - 建模 8 bank、32B entry、A/B priority、C odd/even conflict。
  - 该仲裁模型不等于 `MRegFile` 的物理存储布局；`MRegFile` 仍是 logical A/B/C register 的 whole-tensor functional storage。
  - 还不是 RTL 中所有 per-bank FIFO、pipeline register、ready/valid 组合的逐拍等价实现。
- MatrixTE / CDC：
  - 当前只做第一版 streaming timing/state。
  - MTE result FIFO 默认深度 2，和 RTL `ResultFIFODepth=8` 需要核对。
  - 还没有完整 RTL 等价的 tile-level functional writeback、CDC reorder、所有 data type 和 corner case。
- Matrix tile state rename：
  - `mtilem` / `mtilen` / `mtilek` 当前仍沿用 renameable misc register 路径承载，还没有拆成独立 matrix tile state rename / storage 资源。
- O3 matrix path：
  - 当前重点是让 matrix 指令通过 O3 / `MatrixAmuBuffer` / CUTE backend 闭环。
  - `MatrixAmuBuffer` 是 RTL `AMUCtrlBuffer` 的阶段性近似；当前已建立 commit 后 backend-visible 边界，但还不是固定槽位、完整 ready/valid、`canEnqueue`、SMT/global-oldest 语义的逐拍等价实现。
  - `MatrixAmuBuffer`、MLS replay、commit gating 还没有逐项做 RTL cycle equivalence 校准。
- MLS replay / cancel：
  - `mls_unit.cc` 当前已覆盖 matrix load/store 的早期检查、TLB not-ready replay 和基础 fault path，但还没有完整实现 replace cancel、load cancel、pipe nuke 等与标量 LSQ replay/cancel 对齐的语义。
- Checkpoint / drain：
  - O3 当前在有 live matrix state 或 pending backend state 时会 panic，避免 checkpoint 丢失 matrix architectural/backend 状态。

## 验证记录

已通过 build：

```bash
scons -Q build/RISCV/gem5.opt -j8
```

结果：PASS。

已通过 SE GEMM smoke。该命令覆盖 timing-connected load/store、`matrix_mem_port`、store clean/invalidate + write 路径和 backend release/store drain 边界。

```bash
./build/RISCV/gem5.opt \
  --outdir=/tmp/gem5-matrix-true-store-gemm \
  --debug-flags=MatrixCuteTrace \
  --debug-file=matrix_cute_trace.log \
  configs/example/se.py \
  --cpu-type=DerivO3CPU \
  --caches \
  --l2cache \
  --no-pf \
  -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp \
  --enable-riscv-vector
```

结果：`All 8 precomp tests PASSED. Exiting @ tick 426210030 because m5_exit instruction encountered`

当前源码注册了 focused matrix unit tests，可用 `scons build/RISCV/unittests.opt --unit-test` 或对应 gtest target 验证；这些测试覆盖 LocalMMU、load/store plan、store backend、MatrixRegResource 和 scoreboard。是否纳入某次提交范围应按 PR scope 单独决定。

可选补充 trace run：

```bash
./build/RISCV/gem5.opt \
  --debug-flags=MatrixCuteTrace \
  --debug-file=matrix_cute.trace \
  --outdir=/tmp/gem5-se-gemm-matrix-trace \
  configs/example/se.py \
  --cpu-type=DerivO3CPU \
  --caches \
  --l2cache \
  --no-pf \
  -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp \
  --enable-riscv-vector
```

## PR review 重点

建议 reviewer 按层次检查，避免把 ISA / O3 / backend timing / functional memory access 混在一起看：

- ISA decode：
  - matrix 指令编码是否覆盖当前 matrix GEMM / firmware 使用的 `mlae`、`mlbe`、`mlce32`、`msce32`、`mzero`、`mfmacc_s_h`、`mmacc_w_b`。
  - `RD[4:3]` width、`RD[2:0]` matrix register index、`mtilem` / `mtilen` / `mtilek` carrier 是否与 RTL / firmware 约定一致。
- O3 integration：
  - matrix op class、FU/IQ、wakeup channel 是否符合现有 O3 scheduler 组织。
  - `MatrixAmuBuffer` 在 writeback/commit 后送 request、backend completion 后释放 token / owner 的顺序是否保持 commit、squash、drain 语义。
- Matrix memory path：
  - `MlsUnit` 的虚拟队列和 replay 状态是否覆盖 TLB not-ready、fault、retry 这类 matrix load/store 前端条件。
  - `matrix_mem_port` 的普通 gem5 `RequestPort` 连接、`ReadReq` / `WriteReq` packet、request retry 和 `recvTimingResp` source ID completion 是否符合 O3/backend 边界。
  - store 的 PoU clean+invalidate 维护请求加 data-carrying write、以及按 line 去重的 clean 状态，是否是当前 gem5 cache 集成下可接受的 partial-line store 策略。
  - 不要把普通 `matrix_mem_port` 描述成 RTL 专用 matrix L2 / TL path；matrix 专用 source/channel/MSHR/fill protocol 仍是后续工作。
- Backend timing/resource abstraction：
  - `LocalMMUModel` 的 64B beat、source ID、AML/BML/CML round-robin 和 timing-memory external completion 是否适合作为当前阶段抽象。
  - fill table global entry、per-bank FIFO depth、BML bypass、32B chunk 和 target-bank mapping 是否接近 RTL。
  - backend register-bank 仲裁的 A/B loader write priority、C odd/even read/write conflict 是否符合 RTL 抽象口径。
  - MTE / CDC 的 accepted input beats、FReduce tail、result FIFO depth、CDC writeback beats 是否覆盖当前 GEMM 主路径需要观察的 timing 边界。
- Release / drain：
  - 当前 release 只等待 pending store ack；请核对 RTL 是否还要求等待其它 backend work。
  - checkpoint / drain panic 边界是否足够清楚，避免保存不完整的 matrix architectural/backend state。

## 后续建议

1. 将普通 `matrix_mem_port` 进一步对齐到 RTL 专用 matrix L2 / TL path，包括 dedicated source/channel、L2 bank/MSHR/fill protocol。
2. 将 backend pending response queue 进一步下沉到 cache response ready/retry 语义，使 fill table full 能真实反压 L2 response channel。
3. 将 LSU / M1 fill table 继续细化到 RTL Matrix_MN sub-bank fill FIFO、per-bank drain、response.ready 反压和 C loader repeat fill 控制。
4. 用 trace / stats 校准各级 queue / buffer 容量是否合理：
   - 重点看 `MatrixAmuBuffer`、backend decoded FIFO、LocalMMU AML/BML/CML queue、source ID outstanding、LSU fill table / per-bank fill FIFO、MTE result FIFO、CDC writeback pending state。
   - 对照 GEMM 主路径的 occupancy、block reason 和 backpressure 事件，判断当前容量是接近 RTL 资源约束，还是只是不产生 stall 的功能性默认值。
5. 评估 two-bank scatter fill 的 RTL / software co-design：
   - 当前 GEM5 timing model 中，一个 64B LocalMMU read response 会生成两个 32B MatrixReg loader write chunk，两个 chunk 进入同一个 target-bank FIFO，并写入连续 entry。
   - 可选方向是让 compiler / runtime 通过软件数据重排，把 memory payload 预先按 MatrixReg bank layout 打包，使一个 64B response 携带两个不同 bank 的 32B chunk。这样 load fill 可以一次写入两个 bank，理论上把 load 写入 MRegFile 的周期缩短一半。
   - 这个方向需要 fill table / fill buffer 记录每个 32B chunk 各自的 `{bank, addr}`，fill drain 支持 per-chunk scatter write，并在某个目标 bank 忙时保留未完成 chunk。
   - 这不是当前 RTL/GEM5 contract；当前文档仍按 `64B -> same-bank two entries` 描述现状。
6. 评估 MMA macro-op 拆分：
   - 当前模型以一个 MMA request 为 backend task，并用 accepted input beats / CDC writeback beats 表达中间 timing。
   - 后续可以在 A/B/C load 到 MRegFile 后，把 MMA macro request 进一步拆成 `8x32x8` MAC 子任务：每个子任务读取 A 的 `8x32` slice、B 的 `32x8` slice，并累加到一个 `8x8` C tile。
   - 这样可以更直接表达 ADC / BDC / MTE / CDC 的 tile-level 调度、bank conflict、queue occupancy 和 partial-sum/writeback 边界，但需要同步定义子任务队列、依赖释放和 cancel / squash 语义。
7. 将 source ID 分配改为与 RTL 一致的 round-robin reuse。
8. 继续扩展 focused gtest 覆盖 BML bypass、MTE result FIFO stall、store clean-line 去重和 release 只等 pending store 的边界。
9. 继续校准 MatrixTE / CDC tile writeback 的逐拍 RTL 等价行为。
