# Matrix ISA / O3 / CUTE Backend

## 目标

本 PR 在 gem5 RISC-V O3 路径中接入第一版 matrix 执行链路，目标是让 matrix ISA decode、O3 发射/提交、`MatrixAmuBuffer` 转发、CUTE backend、matrix register file、matrix load/store timing 能在 SE workload 中形成闭环。

当前重点不是完整 RTL 等价模型，而是建立可运行、可观测、可继续校准的 matrix 主路径，方便 RTL / gem5 reviewer 明确本 PR 已覆盖的抽象层次和后续需要补齐的 timing 细节：

- ISA 能解析当前 matrix GEMM / firmware 使用的 matrix 指令编码。
- O3 能识别 matrix 指令类别并按 matrix FU / IQ 路径调度。
- matrix 指令能通过 O3 的 `MatrixAmuBuffer` 进入 CUTE backend。
- backend 能执行 matrix load/store、mzero、MMA timing、release completion。
- LocalMMU / backend register-bank 仲裁能表达第一阶段带宽、bank conflict 和 load-store timing。

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
- gem5 侧对应口径：它是 O3 commit 后的 matrix backend，不参与 ISA decode，也不替代 cache / TLB / memory hierarchy 的真实 timing。
- 数据口径：load/store 的数据内容通过 functional access 保证 SE workload 能闭环。
- timing 口径：LocalMMU、backend register-bank 仲裁、MatrixTE 表达当前阶段的固定延迟、bank conflict、读写拍数和 completion 边界。

backend 的基本工作方式是：

1. O3 将已经允许进入 AMU 的指令封装成 `CuteRequest`。
2. `DetailedCuteBackend` 把 request 放入 decoded FIFO，并用 scoreboard 检查 matrix A/B/C register 依赖。
3. request 被分配到对应路径：
   - matrix load/store 进入 AML / BML / CML 风格的 LSU path。
   - MMA 进入 compute path，模拟 ADC / BDC / MTE / CDC 的粗粒度执行阶段。
   - `mzero` 进入 arith path，直接更新 matrix register file。
   - release 等 backend work 和 pending store drain 后再完成。
4. backend 每个 `step()` 推进 LocalMMU、register-bank 仲裁、compute unit 和 completion queue。
5. completion 返回 O3，O3 再释放 token 或清理 matrix owner 状态。

### Matrix Load / Store 原理

matrix load/store 目前是“timing model + functional data movement”的混合模型。LocalMMU 只表达 load/store beat 的 fixed-latency timing；真实数据读写由 `Gem5MatrixMemoryAdapter` 经由 `readMatrixElem<T>()` / `writeMatrixElem<T>()` 完成。这两个 helper 使用 `AmuLsuDesc::tc` 创建 `SETranslatingPortProxy` 或 `TranslatingPortProxy`，对 guest VA 做 functional access。也就是说，当前模型能让 SE GEMM 读写正确数据，但还没有把 matrix LSU 接到真实 L2 / TL / cache miss timing。

load 路径大致是：

1. `MatrixAmuBuffer` 将 committed load request 送入 backend。
2. backend 根据 tile shape 计算 payload size，并拆成 64B LocalMMU beats。
3. `LocalMMUModel` 每 cycle 最多 issue 一个 beat，并在固定延迟后返回 read response。
4. read response 表示数据 beat 在 timing model 中已回来；所有 read response 到齐后，functional memory adapter 才通过 `loadTile()` / `readMatrixElem<T>()` 按元素读出完整 tile payload。
5. 每个 64B response 转成两个 32B MatrixReg write chunk，仅用于 loader write timing/resource 仲裁计数。
6. backend register-bank 仲裁处理 loader write 和 compute read/write 的 bank conflict。
7. 所有 response 和 MatrixReg write chunk 完成后，load completion。

store 路径相反：backend 先从 C register 做 snapshot，再拆成 64B LocalMMU store beats；所有 store ack 到齐后，functional memory adapter 再通过 `storeTile()` / `writeMatrixElem<T>()` 写完整 tile payload。这里的 64B beat、store ack 和 32B chunk 主要驱动 timing/resource 状态，不表示 architectural MRegFile 已经按每个 beat 做真实数据搬运，也不表示 store 数据逐 beat 写入 architectural memory。release 会等待 pending store 清空，避免 store 还没 ack 就释放 token。

### MTE / MMA 原理

MTE / MMA path 是 CUTE compute 的分段抽象模型。ADC / BDC / CDC 被显式建模。当前实现会在 compute read 阶段把本次 MMA 需要的 A/B/C matrix register 内容读入 backend task state，作为后续逐 beat timing/functional 更新的快照；这不是说硬件一次把大矩阵整体吞进 MTE，而是 gem5 backend 用 task-local snapshot 保存 architectural matrix register 数据，方便按 8x8 tile 和 K-group 推进。

大致执行方式是：

1. backend issue MMA 前，scoreboard 确认 A/B/C source 和 C dest 的依赖关系。
2. compute read frontend 通过 register-bank 仲裁同时申请 A/B/C register read；读回后形成 `bufferedTensorA` / `bufferedTensorB` / `bufferedTensorC` snapshot。
3. ADC / BDC / CDC read 完成后才进入 MTE execute 阶段。
4. `MatrixTE` 根据 `mtilem` / `mtilen` / `mtilek` 和 element type 计算 `acceptedInputBeats`、FReduce tail、CDC writeback beats。当前抽象按 8x8 tile、32B reduce width 建模；在 supported `mtilen == 128` 形状下，full 128x128x64 int8 GEMM 对应 16 个 M tile、16 个 N tile、2 个 K group，因此是 512 个 MTE accepted input beats。CDC writeback 也按同样的 tile/K-group 粒度推进 512 个 writeback beats。
5. 当前 supported timing 只覆盖 `mtilen == 128` 的形状；int8 tile-level writeback 还要求 M/N/K 分别按 8/8/32 对齐。对满足条件的 int8 tile writeback，MTE 阶段主要完成 datatype/shape 检查和 timing 推进；CDC writeback 再按 8x8 tile 与 K 维 32-element group 更新 full C tensor 中的一个 8x8 tile 区域，并用 `cdcTileWriteTensor` 暂存该 beat 更新后的 whole-tensor 结果。这里不是把一个物理 C tile entry 逐 beat 写入 RTL CRegFile，而是在 gem5 logical C tensor 上更新对应 8x8 区域。
6. CDC writeback 仍走 register-bank 仲裁，和 load fill / compute read/write 共享 C bank conflict 约束；全部 writeback beat 完成后才产生 completion。

因此，MMA 不是单纯加一个固定 latency；模型保留了“ADC/BDC/CDC 读、MTE 输入 beat、CDC 分 beat 写回、C bank 仲裁、task completion 边界”的中间状态。这里的 512 不是一次性读取 512 份完整矩阵，而是 16x16 个 8x8 C tile 乘以 2 个 K group 的 beat 数。限制也仍然存在：它还不是所有 datatype / CDC reorder / RTL ready-valid 细节的完整逐拍等价模型。

### 文件职责

- `CUTETOP.hh` / `CUTETOP.cc`
  - backend 主状态机。
  - 接收 `CuteRequest`，调度 LSU / MMA / Arith / Release task。
  - 维护 completion queue、pending store、release gating 和 backend drain 状态。
  - 将 matrix LSU payload 拆成 64B LocalMMU beats，并把 load response 转成 MatrixReg loader write chunk。
- `CUTEParameters.hh`
  - 定义 ISA / O3 / backend 共享的 matrix request、completion、LSU/MMA 描述和 tensor payload。
- `TaskController.hh` / `TaskController.cc`
  - 定义 backend 抽象接口、decoded FIFO entry、task accept、dependency check、backend task life-cycle 和 trace。
- `MemoryLoader.hh` / `MemoryLoader.cc`
  - 定义 backend memory adapter。
  - `Gem5MatrixMemoryAdapter` 当前通过 `AmuLsuDesc::tc` 创建 SE / FS translating proxy 做 functional VA read/write，用于 SE 路径闭环。
- `Scoreboard.hh` / `Scoreboard.cc`
  - 跟踪 matrix A/B/C register hazard 和 task 依赖。
- `MatrixTE.hh` / `MatrixTE.cc`
  - 第一版 MatrixTE timing 估算。
  - 建模 A/B/C read response、accepted input beats、CDC writeback beats。
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
  - fixed latency read response / store ack。

本 PR 的 `src/matrix/SConscript` 只注册当前 backend 生产文件；register-bank 仲裁逻辑保留在 `CUTETOP.hh` / `CUTETOP.cc`，matrix architectural state / whole-tensor functional storage 放在 `MRegFile.hh` / `MRegFile.cc`，不注册 `GTest` 或 `*.test.cc` target。当前 register-bank 仲裁是独立 timing/resource helper，不表示 `MRegFile` 已经实现 RTL 的物理 8-bank / 32B-entry SRAM 存储。

## 已建模的 timing / resource

- LocalMMU：
  - 64B beat。
  - 每 cycle issue 一个 beat。
  - 64 outstanding source ID 上限。
  - AML/BML/CML per-client queue。
  - client round-robin issue。
  - fixed configurable latency。
  - read response / store ack。
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
  - CDC D beat timing window。
  - CDC tile-level writeback 的第一版 timing/state。
- Trace：
  - 新增 `MatrixCuteTrace` debug flag。
  - 记录 backend submit、dependency、LocalMMU enqueue/issue/response、MatrixReg loader write grant/stall、CDC read/writeback 等关键事件。

## 当前未对齐点 / 已知限制

这些不是本 PR 已解决内容，需要在 review 时明确边界：

- LocalMMU 还没有接真实 L2 / TL / LLC：
  - 没有 `RequestPort` / `Packet` / `recvTimingResp` / retry。
  - 没有 cache tag、hit/miss、MSHR、coherence、replacement。
  - 当前通过 fixed latency 表达“选中且无 miss”的理想路径。
- LocalMMU 没有完整 ready/backpressure：
  - response 被取走时就释放 source ID。
  - 没有建模 response ready 为 false 时 source ID 被占住。
  - 没有建模 TL/LLC 反压导致的 Decoupled bubble。
- Source ID 复用目前是空闲 ID 分配，不是严格 RTL source ID round-robin reuse。
- LSU fill table 未建模：
  - 当前 read response 直接转成 pending MatrixReg write chunk。
  - 没有 bounded fill table，也没有 fill table full 后对 LocalMMU response 施加 backpressure。
- Register-bank 仲裁是抽象 bank/resource 模型：
  - 建模 8 bank、32B entry、A/B priority、C odd/even conflict。
  - 该仲裁模型不等于 `MRegFile` 的物理存储布局；`MRegFile` 仍是 logical A/B/C register 的 whole-tensor functional storage。
  - 还不是 RTL 中所有 per-bank FIFO、pipeline register、ready/valid 组合的逐拍等价实现。
- MatrixTE / CDC：
  - 当前只做第一版 timing/state。
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
scons build/RISCV/gem5.opt --linker=gold -j64 --rvv-impl=simple
```

结果：PASS。

已通过 SE GEMM smoke。该命令显式打开 `--matrix-kmhv3-scheduler`，用于覆盖 SE 模式下的 matrix IQ/FU 调度；不带该开关时 `configs/example/se.py` 仍使用默认 scheduler。

```bash
build/RISCV/gem5.opt \
  --outdir=/tmp/gem5-se-gemm-precomp-matrix-kmhv3-scheduler \
  configs/example/se.py \
  -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp \
  --enable-riscv-vector \
  --no-pf \
  --matrix-kmhv3-scheduler
```

结果：`All 8 precomp tests PASSED.`

可选补充 trace run：

```bash
./build/RISCV/gem5.opt \
  --debug-flags=MatrixCuteTrace \
  --debug-file=matrix_cute.trace \
  --outdir=/tmp/gem5-se-gemm-matrix-trace \
  configs/example/se.py \
  -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp \
  --enable-riscv-vector \
  --no-pf \
  --matrix-kmhv3-scheduler
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
  - 当前 `Gem5MatrixMemoryAdapter` 通过 `readMatrixElem<T>()` / `writeMatrixElem<T>()` 做 functional memory access，只保证数据闭环；不要把它当作真实 L2 / TL timing。
- Backend timing/resource abstraction：
  - `LocalMMUModel` 的 64B beat、fixed latency、source ID 和 AML/BML/CML round-robin 是否适合作为当前阶段抽象。
  - backend register-bank 仲裁的 A/B loader write priority、C odd/even read/write conflict 是否符合 RTL 抽象口径。
  - MTE / CDC 的 accepted input beats、CDC writeback beats、C bank arbitration 是否覆盖当前 GEMM 主路径需要观察的 timing 边界。
- Release / drain：
  - release 是否等待 pending backend work 和 store ack，避免 store 未完成时释放 token。
  - checkpoint / drain panic 边界是否足够清楚，避免保存不完整的 matrix architectural/backend state。

## 后续建议

1. 接真实 L2/TL timing port，替换 fixed-latency LocalMMU response。
2. 增加 LocalMMU response ready/backpressure，source ID 在 response 未被消费前不释放。
3. 增加 LSU fill table / per-bank fill FIFO 的容量和反压模型。
4. 用 trace / stats 校准各级 queue / buffer 容量是否合理：
   - 重点看 `MatrixAmuBuffer`、backend decoded FIFO、LocalMMU AML/BML/CML queue、source ID outstanding、LSU fill table / per-bank fill FIFO、CDC writeback pending state。
   - 对照 GEMM 主路径的 occupancy、block reason 和 backpressure 事件，判断当前容量是接近 RTL 资源约束，还是只是不产生 stall 的功能性默认值。
5. 评估 two-bank scatter fill 的 RTL / software co-design：
   - 当前 GEM5 timing model 中，一个 64B LocalMMU read response 会生成两个 32B MatrixReg loader write chunk，两个 chunk 使用同一个目标 bank，并写入连续 entry。
   - 可选方向是让 compiler / runtime 通过软件数据重排，把 memory payload 预先按 MatrixReg bank layout 打包，使一个 64B response 携带两个不同 bank 的 32B chunk。这样 load fill 可以一次写入两个 bank，理论上把 load 写入 MRegFile 的周期缩短一半。
   - 这个方向需要 fill table / fill buffer 记录每个 32B chunk 各自的 `{bank, addr}`，fill drain 支持 per-chunk scatter write，并在某个目标 bank 忙时保留未完成 chunk。
   - 这不是当前 RTL/GEM5 contract；当前文档仍按 `64B -> same-bank two entries` 描述现状。
6. 评估 MMA macro-op 拆分：
   - 当前模型以一个 MMA request 为 backend task，并用 accepted input beats / CDC writeback beats 表达中间 timing。
   - 后续可以在 A/B/C load 到 MRegFile 后，把 MMA macro request 进一步拆成 `8x32x8` MAC 子任务：每个子任务读取 A 的 `8x32` slice、B 的 `32x8` slice，并累加到一个 `8x8` C tile。
   - 这样可以更直接表达 ADC / BDC / MTE / CDC 的 tile-level 调度、bank conflict、queue occupancy 和 partial-sum/writeback 边界，但需要同步定义子任务队列、依赖释放和 cancel / squash 语义。
7. 将 source ID 分配改为与 RTL 一致的 round-robin reuse。
8. 给 `LocalMMUModel`、backend register-bank 仲裁、`CUTETOP` 补独立 gtest，减少仅靠 SE GEMM smoke 的风险。
9. 继续校准 MatrixTE / CDC tile writeback 的逐拍 RTL 等价行为。
