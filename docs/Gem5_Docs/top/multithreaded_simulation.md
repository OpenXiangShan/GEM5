# 多线程仿真实现

## 目标和边界

这里讨论的是 host 侧多线程仿真，不是 guest 侧 SMT 功能本身。目标是在不改变模拟体系结构状态、不改变可复现实验结果的前提下，把 O3 pipeline 和部分 event callback 内部工作拆成可调度 task，提高宿主机并行度。

用户提出的直觉是正确的：真实流水线在时间上天然重叠，例如同一段时间里 older data 可以处在 decode，younger data 可以处在 fetch。更准确地说，多线程仿真的主要机会不是把同一个模拟周期里的五个 stage 强行同时执行，而是把 `stage × simulated-cycle` 展开成二维 task graph。沿着这个二维图看，`Commit(c)` 完成后，`IEW(c)` 可以推进；当 `IEW(c)` 产生了给下一周期 commit 使用的结果后，`Commit(c+1)` 又可以和更早周期的 `Rename(c)`、`Decode(c)` 等其他已满足依赖的 task 形成 wavefront 并行。

当前 gem5 O3 模型不是用每个真实流水段独立线程推进，而是在一个 `O3CPU tick` 事件里按固定顺序更新 stage 状态。因此实现多线程仿真时不能简单把 `fetch.tick()`、`decode.tick()` 等函数直接扔到不同 host 线程执行。正确方向是把现有确定性顺序显式化为跨周期 task graph：同一模拟周期内保持必要的强序依赖，不同模拟周期、不同 stage 之间只要依赖边已经满足，就允许 host 并行执行。

本设计文档只采用一套精确 wavefront 模型：并行化不改变模拟语义，目标是在这个约束下把可并行的 host 计算尽量展开。`task_window_cycles=1` 只是同一套模型的退化配置，用于等价验证和调试，不是另一种运行模式。主线设计不保留 `Conservative` / `WavefrontExperimental` / “快速非严格”这类模式枚举，避免为了临时过渡接口长期维护两套语义。

## 当前代码事实

O3 CPU 的 tick 事件在构造函数中注册为 `Event::CPU_Tick_Pri`，入口是 `tickEvent([this]{ tick(); }, "O3CPU tick", false, Event::CPU_Tick_Pri)`，见 `src/cpu/o3/cpu.cc:83`。CPU 构造时也把所有 stage 接到同一个 backward `TimeBuffer<TimeStruct>`，并把 fetch/decode/rename/iew 的 forward queue 接成 TimeBuffer，见 `src/cpu/o3/cpu.cc:176` 到 `src/cpu/o3/cpu.cc:190`。

当前 `CPU::tick()` 的真实执行顺序是：

```text
Commit -> IEW -> Rename -> Decode -> Fetch
then advance(fetchTimebuffer, decodeTimebuffer, renameTimebuffer, iewTimebuffer, timeBuffer)
```

这个顺序在 `src/cpu/o3/cpu.cc:569` 到 `src/cpu/o3/cpu.cc:594`。注意这是本仓库当前源码的 ground truth；如果旧文档里写成 fetch 先 tick，应以源码为准。

TimeBuffer 不是线程安全队列，而是固定大小环形时间窗口。`advance()` 只移动 base 并清空 future slot，见 `src/cpu/timebuf.hh:178` 到 `src/cpu/timebuf.hh:190`。stage 通过 `getWire(offset)` 读写相对当前 cycle 的 slot，例如：

- Fetch 读取 decode/rename/iew/commit 的 delayed backward wire，见 `src/cpu/o3/fetch.cc:363` 到 `src/cpu/o3/fetch.cc:372`。
- Decode 写 `toFetch = timeBuffer->getWire(0)`，并读 delayed fetch queue，见 `src/cpu/o3/decode.cc:176` 到 `src/cpu/o3/decode.cc:205`。
- Rename 写 `toDecode = timeBuffer->getWire(0)`，并读 IEW/Commit feedback，见 `src/cpu/o3/rename.cc:209` 到 `src/cpu/o3/rename.cc:239`。
- IEW 写 `toRename` / `toFetch`，读 commit feedback，见 `src/cpu/o3/iew.cc:397` 到 `src/cpu/o3/iew.cc:420`。
- Commit 写 `toIEW`，读 IEW 和 rename/fetch queues，见 `src/cpu/o3/commit.cc:401` 到 `src/cpu/o3/commit.cc:437`。

这意味着一个 cycle 内的 stage 顺序有真实语义。比如 Decode 会在本 tick 修改 `stallSig->blockFetch` 和 `toFetch->decodeInfo`，Fetch 随后读取这些状态；Rename 会先修改 `stallSig->blockDecode`，Decode 再运行；Commit 会先写 `commitInfo`，IEW 后运行。这个 same-cycle feedback 是直接拆 stage 并行的主要障碍。

`StallSignals` 当前也是一组全局侧带数组，定义在 `src/cpu/o3/comm.hh:334` 到 `src/cpu/o3/comm.hh:357`，包括 `blockFetch`、`blockDecode`、`blockRename`、`blockIEW` 以及对应原因。它们的写点分散在 Decode/Rename/IEW/Commit，例如 Rename 写 `blockDecode`，IEW 写 `blockRename`，Commit 写 `blockIEW`。串行倒序 tick 时，这等价于一组隐式 same-cycle latch；但进入多线程 task 后，如果不隔离，可能出现 `Decode[tid0, c]` 正在读输入时，`Rename[tid1, c]` 或 `Rename[tid1, c+1]` 提前写了全局 `blockDecode`，从而让 tid0 的 decode 被错误提前阻塞。

gem5 EventQueue 也有确定性边界。事件按 `(when, priority)` 排序，比较函数见 `src/sim/eventq.hh:530` 到 `src/sim/eventq.hh:533`；同一个 `(when, priority)` bin 内通过 `nextInBin` 栈式取出，不能把这些事件当成天然可交换。`CPU_Tick_Pri = 50`，见 `src/sim/eventq.hh:198` 到 `src/sim/eventq.hh:204`。`EventQueue::serviceOne()` 持有 event queue lock，取出 head event，设置 curTick，然后调用 `event->process()`，见 `src/sim/eventq.cc:198` 到 `src/sim/eventq.cc:223`。多 event queue 并行已有 `simQuantum` 和 global event 同步机制，但跨队列事件要么用可能非确定的 `ScopedMigration`，要么用 async queue 在 quantum 边界合并，相关约束写在 `src/sim/eventq.hh:588` 到 `src/sim/eventq.hh:620`，实现见 `src/sim/eventq.cc:424` 到 `src/sim/eventq.cc:443`。

多线程化还会放大一个已有隐患：很多系统可能把两个事件排在相同 `(when, priority)`，实际却依赖事件插入顺序或执行先后维护正确性。这种顺序在单线程 event loop 里看起来稳定，但它不是足够清晰的建模合同。并行仿真不应该靠“软约束”或调度器碰巧按旧顺序执行来维持语义；如果两个同 tick 事件存在真实先后依赖，应该给它们重新分配显式 priority，或者建成显式 dependency/barrier。优先级空间不足时，也应优先引入命名明确的 priority class，而不是继续让同优先级事件靠插入顺序传递隐含语义。

现有 SMT 结构已经按 `ThreadID` 做了不少隔离，但许多核心状态仍然共享并且顺序敏感。ROB/LSQ/IQ 有 SMT sharing policy 参数；Commit 根据 `RoundRobin` 或 `OldestReady` 选择提交线程，见 `src/cpu/o3/commit.cc:2135` 开始。另一方面，Decode/Rename/Commit 当前还会在多个 active thread 同时可运行时阻塞多线程，避免资源冲突；Fetch 的发送路径仍有 `ThreadID tid = 0; // TODO: smt support`，见 `src/cpu/o3/fetch.cc:1324`。因此 host 多线程仿真不能假设每个 guest thread 已经完全可以独立推进。

当前代码已经落地的是第一阶段运行时骨架和 System 级配置，不是完整 wavefront 调度器。`src/sim/System.py:123` 开始定义 `enable_task_parallel_sim`、`task_parallel_threads`、`task_window_cycles` 等 host-side task runtime 参数；`src/sim/system.hh:321` 的 `TaskParallelConfig` 汇总这些参数并由 `System::taskParallelConfig()` 暴露。`configs/common/Options.py` 也暴露了对应 CLI，`configs/common/Simulation.py` 的 `setTaskParallelOptions()` 负责把命令行参数写回 `System`，Xiangshan 和 SE 配置路径都会调用这个 helper。runtime 验证和推荐运行口径使用 `--task-parallel-threads=2`，实际解析后的 worker 数由 `system.cpu.taskRuntime.workerThreads` 观测；`scons -j64` 只表示编译并行度，不能等同于模拟 worker 数。

O3 CPU 内部持有 `TaskRuntime`，见 `src/cpu/o3/cpu.hh:660`，并在当前串行 `CPU::tick()` 的 stage 调用前后插入 `onSerialTickBegin()` / `onSerialTickEnd()`，见 `src/cpu/o3/cpu.cc:584` 和 `src/cpu/o3/cpu.cc:600`。`src/cpu/o3/task_runtime.hh` / `src/cpu/o3/task_runtime.cc` 已经提供 `runStrong()`、`submitWeak()`、`waitForOrder()`、`waitForPreAdvance()`、`waitForAll()` 和 `drain()`：`runStrong()` 在 `enable_task_parallel_sim=false` 时直接执行旧函数；开启后把 owner event thread 上的状态更新记录为 strong task。当前 barrier 已经从“进入任意 strong task 前等待全部 weak task”收紧为按 `TaskOrderKey` 的 selective barrier：只等待和归并 order 不晚于当前 strong task 的 weak task，order 更晚的 future weak task 可以继续留在 worker 侧运行，直到后续 stage-local barrier、drain 或最终 `waitForAll()`。各 stage 的真实 `run*Prepare()` 也不再在提交本 stage prepare 后无条件 `waitForAll()`，而是调用 `waitForOrder(stage_prepare_order)`，只等待本 prepare 及更早依赖；这避免 `Decode[c]` / `Fetch[c]` 的 prepare 等待把 `Commit[c+1]` 这类更晚 future probe 过早收回。`submitWeak()` 可以 lazy 启动 worker pool，worker 只执行 weak task 的 `run` 部分，owner 线程在同步点按 `TaskOrderKey` 做确定性 merge；未开启或任务粒度低于 `task_min_work` 时 inline 执行。weak task 还带有生命周期标签：默认 `PreAdvanceDrain` 表示必须在 TimeBuffer circular slot advance 前完成；只有输入已经压成不含 TimeBuffer slot 指针和 `DynInstPtr` 所有权的轻量 snapshot 时，才能显式标成 `CrossTimeBufferAdvance`。这类 task 现在可以越过 TimeBuffer advance；如果当前 CPU tick 结束时 EventQueue 队头就是下一次 CPU tick，还可以留到下一拍，并由后续 stage-local future prepare barrier 在对应 consumer stage 前 merge。若队头是 exit/drain/memory/device 等任何其他事件，则 `onSerialTickEnd()` 仍会收回全部 task，避免跨越可见 event 边界或 stats dump。`task_runtime_self_test` 可以显式触发一个 worker/merge 自检：逆序提交 weak task，要求 owner 按 `TaskOrderKey` 顺序 merge。

`onSerialTickBegin()` 现在只刷新 worker host-time stats 和采样 in-flight cycle，不再全量 `waitForAll()`。上一拍延迟到下一拍的 `CrossTimeBufferAdvance` future prepare 结果由 `CPU::tick()` 中的局部 barrier 收回：Commit 前等待 `Commit/phase=2`，IEW/Rename/Decode 前分别等待本 stage 的 `phase=2`，Fetch 前等待 `Fetch/phase=4`。因此 `Commit[c+1]` 的 future prepare 可以继续和当前拍更早 stage 重叠，直到真正要被 `Commit[c+1]` 消费前才 merge；这扩大了跨 callback 并行窗口，同时不允许 worker 提前发布 stall edge、TimeBuffer output 或架构状态。

当前 `CPU::tick()` 已经把 `Commit/IEW/Rename/Decode/Fetch` 包装为 `TaskRuntime::runStrong()`，但仍按旧顺序执行。这是 `task_window_cycles=1` 的强序 task scaffold：它让 stage 边界进入 task runtime 的观测和 barrier 体系，为后续 `C/I/R/D/F × cycle` wavefront DAG 提供接口；它本身不改变 stage 执行顺序，也不引入跨周期并行。

当前还增加了一个只读的 static wavefront plan 审计层。CPU 把 `fetchToDecodeDelay`、`decodeToRenameDelay`、`renameToIEWDelay`、`renameToROBDelay` 和 `iewToCommitDelay` 传给 `TaskRuntime`，runtime 按 `effectiveWindow = min(task_window_cycles, max_in_flight_cycles)` 构造缓存化的 `C/I/R/D/F × cycle` 粗粒度 DAG，统计 task 数、same-cycle 和 forward-delay 依赖边、ASAP 关键路径、最大 ready set 和 ready slack。这里显式包含 `Rename[c] -> Commit[c + renameToROBDelay]`，因为当前 Commit tick 会消费 Rename->ROB queue 并把指令搬入 ROB；少掉这条边会高估 ready set。这个 planner 只读参数并累加 host-only stats，不调度未来 cycle，不推进 TimeBuffer，也不写 ROB/LSQ/BPU/架构状态；它的作用是先证明窗口内是否存在可并行 ready set，并给后续真正 wavefront scheduler 提供可检查的边界。

event horizon 也已经显式化为 host-only 观测。`CPU::tick()` 在当前 event 被 EventQueue 取出后读取队头 event，并按 `(next_event_tick, next_event_priority)` 与未来 CPU tick 的 `(clockEdge(offset), CPU_Tick_Pri)` 比较，计算当前候选窗口里有多少 CPU cycle 可以在不跨越外部 event 的前提下提交。当前实现记录 `candidateCycles`、`committableCycles`、被 horizon 截断的 cycle 数、第一次阻塞发生在第几个 future-cycle offset，以及阻塞来自更早 tick 的 event 还是同一个 future CPU tick 上 priority 不晚于 `CPU_Tick` 的 event。开启 `--event-priority-audit` 时，还会把 blocker event 的名字按固定 subsystem bucket 分类为 `MemoryController`、`L1Cache`、`L2Slice`、`L2Wrapper`、`L2Other`、`L3MemSidePort`、`L3Cache`、`Prefetcher`、`Interconnect`、`CPU`、`Device`、`Other`，并分别统计总量、更早 tick blocker 和同 future CPU tick priority blocker；默认 runtime 不构造 event name，避免把诊断字符串处理放进热路径。它不实际合并未来 cycle，也不改变 `schedule()`/`deschedule()` 行为；后续真正 wavefront merge 必须使用同一类边界判断，保证未来 CPU state 不会早于同 tick 更高优先级 event 或更早 tick event 发布。

当前观测到的 memory/cache blocker 需要按依赖边细分，不能简单整体忽略。`MemCtrl::processRespondEvent()` 会调用 `accessAndRespond()` 发送 ready response，XS L2 slice 的 response/main-pipe/retry 事件也显式放在 `Minimum_Pri` 系列 priority，都会早于 `CPU_Tick_Pri`。因此后续若要放宽 event horizon，只能证明某个 future prepare 的输入不依赖这些事件的可见结果；不能把所有 default-priority memory/cache event 统一移到 CPU tick 之后。

TimeBuffer slot 显式化已经开始落地。`TimeBuffer` 现在暴露 `pastCycles()` / `futureCycles()` 只读边界；`src/cpu/o3/pipeline_snapshot.hh` / `src/cpu/o3/pipeline_snapshot.cc` 增加了 `PipelineTimeBufferSnapshots`，可以按 `[-past, future]` 捕获 backward `TimeStruct` 和四条 forward queue 的完整 slot frame，并提供 `Slots::get(offset)` 这种不会触发 assert 的边界检查读取接口。含 `PCStateBase` 的字段通过 clone 做深拷贝，含 `DynInstPtr` 的字段保持引用计数语义；相关深拷贝定义放在 `src/cpu/o3/comm.cc`，避免在只看到 `DynInst` 前向声明的编译单元里实例化 refcount 操作。当前 `CPU::tick()` 在 runtime 开启时捕获 stage 执行前 input frame 和 stage 执行后、TimeBuffer advance 前 output frame，并把这些 frame 保存到固定容量 cycle ring 中，容量由 `max(task_window_cycles, max_in_flight_cycles)` 配置。`PipelineTimeBufferSnapshots::inputFrame(cycle)` / `outputFrame(cycle)` 可以按 cycle 查询冻结 frame；查询 miss 表示对应 cycle 不在当前窗口内。现阶段仍只提交当前 cycle 的真实 stage，但这个 ring 是后续多个 in-flight cycle 共享 TimeBuffer snapshot 的边界。

所有 current-cycle 的真实 stage TimeBuffer 输入 helper 现在都把 snapshot 命中作为 runtime 契约：`task_parallel` 开启且读取的是 `curCycle()` 时，缺少 input frame 或对应 slot 会直接报错，而不是回退到 live wire。非 runtime 路径和只读 future probe 仍可以用旧 fallback/skip 逻辑维持过渡兼容。这保证当前已经迁移到 input frame 的 stage 不会在并行准备路径里悄悄观察可变 wire。

第一条真正消费 snapshot 的 weak prepare task 也已经接入，但现在只作为 `--task-trace` 诊断路径启用。开启诊断时，每个 runtime-enabled tick 会提交一个 `TaskStage::Runtime` / `phase=1` 的 weak task，worker 只读 `PipelineTimeBufferSnapshots::inputFrame()`，统计 forward queue 中的 instruction reference、fetch group、squash/redirect 类控制信号和 resolved CFI 数量；owner merge 只保存 `lastInputPrepareSummary()` 并更新 `taskRuntime.timeBufferPrepareMerges`、`timeBufferPreparedInstRefs`、`timeBufferPreparedControlSignals` 等统计。这个 prepare summary 本身目前还不驱动 stage 行为，因此默认 runtime 不再为它提交 worker task；真实 stage prepare 仍直接读取每周期 input snapshot ring。

当前还增加了第一条 horizon-gated future-cycle prepare 骨架，同样只在 `--task-trace` 诊断路径启用：在当前 CPU tick 的五个 strong stage 都完成后、TimeBuffer `advance()` 之前，CPU 用 `source_offset_shift=1` 捕获一个只存在于 task 私有 `Frame` 中的 `cycle+1` TimeBuffer input snapshot。这个 shifted frame 的 offset 语义等价于所有 TimeBuffer advance 一拍后的视图，超出旧 `future` 边界的新 slot 按 TimeBuffer advance 后的空 slot 处理。随后 CPU 提交 `TaskStage::Runtime` / `phase=2` weak task 做只读 summary，让 summary 计算可以和本 tick 尾部的 TimeBuffer advance、activity 更新、调度和 drain 检查重叠。这个 future snapshot 不写入主 `PipelineTimeBufferSnapshots` ring，不被 stage 行为消费；为了避免 `--maxinsts` / drain / checkpoint 等退出路径在 stats dump 前留下 pending task，owner 线程仍会在同一个 event callback 结束前等待它完成。是否把结果发布为 `pendingFutureTimeBufferPrepare` 仍放在 `tryDrain()` 之后判断：只有 event horizon 允许、并且 CPU 最终确实调度了下一拍 tick 时，才更新 `futureTimeBuffer*` host-only stats 和 pending summary；如果提前计算出来但 drain/idle 取消了下一拍 tick，则计入 `specDiscarded` 并按 skipped 处理。future summary 会暂存在 CPU 内部；下一次 runtime-enabled tick 捕获真实 input snapshot 后，如果 cycle 命中，就先复用上一拍 summary 作为当前 host-only input prepare summary，再提交一个 `Runtime/phase=3` verification weak task 重新读取真实 input snapshot 并对比 cycle 和 summary 内容。它的意义是诊断“当前 stage 输出完成后可以准备下一周期只读输入，并且下一周期能够验证/复用这个结果”的边界；默认 runtime 不为这个 host-only summary 消耗 worker。

同一条 horizon-gated path 现在还增加了 Commit future prepare reuse。在 `Rename[c]` strong task 完成后、`Decode[c]` 开始前，CPU 用 pre-advance shifted offset 读取那些在 TimeBuffer advance 后会成为 `Commit[c+1]` 输入的 backward slot、Rename->Commit slot 和 IEW->Commit slot，并在 owner 侧复制成 `CommitPrepareInput`。随后提交一组 `TaskStage::Commit` / `phase=2` 的 `CommitThreadPrepare[tid,c+1]` weak task，分别计算每线程 ROB state/capacity block、active 状态和 `CommitToIEW` stall 候选，再由同 phase、`localSeq=numThreads` 的 owner merge task 组合成 `CommitPrepareResult` 并写入 pending 结果。selective barrier 会允许这些 future weak task 跨过当前周期的 `Decode` / `Fetch` strong barrier，以及这些 stage 内部的 current-cycle prepare barrier，留在 worker 侧运行。因为 worker 捕获的是 owner 已经构造好的 `CommitPrepareInput` 和私有 result，不持有 TimeBuffer slot 指针或 `DynInstPtr` 所有权，这组 task 已经标成 `CrossTimeBufferAdvance`，可以越过本 tick 的 TimeBuffer advance，并在下一次 owner `waitForAll()` 前按 `TaskOrderKey` merge。下一拍真实 `runCommitPrepare()` 如果 cycle 命中，就由 owner 直接 `mergeCommitPrepareResult()` 复用 pending result，不再提交本周期 Commit prepare worker；`--task-runtime-self-test` 开启时，会用当前冻结输入重算一次只读 expected 并更新 `commit.futurePrepareChecks/Matches/Mismatches`。这个 reuse 不发布未来 `CommitToIEW` stall edge，不写 ROB/fixedbuffer，也不把 Commit 提交、squash、trap、difftest 或 BPU commit training 提前；它只把已经证明只读稳定的 prepare 控制结果提前计算并在下一拍 owner merge。遇到下一拍 Commit prepare 之前还会改变输入的控制路径，例如 future IEW squash、trap/TC/SquashAfter、当前 ROBSquashing，或者缺少必要 shifted slot 时，future prepare 会保守跳过并计入 `commit.futurePrepareSkipped`。这条 gate 的含义是：当前只提前准备“下一拍 prepare 输入已经完全只读可预测”的子集，把 squash/status 更新这类强序前置控制留给后续单独拆分。

在 Commit future prepare 之后，又增加了第一条成组 future wavefront prepare reuse：`Commit[c+1] -> IEW[c+1]`。CPU 仍在 `Rename[c]` 完成后发起这段 wavefront，但现在会先在 owner 侧用 shifted TimeBuffer slot 构造 `CommitToIEW` latch，并把 IEW 会消费的 `Rename->IEW` / `Commit->IEW` 输入降成不含 TimeBuffer slot 指针和 `DynInstPtr` 所有权的 `IEWPrepareInput`。随后提交 `IEW/phase=2` weak task，worker 只消费这份轻量 input，只计算下一拍的 `IEWPrepareResult`；这组 task 因此已经标成 `CrossTimeBufferAdvance`，可以越过本 tick 的 TimeBuffer advance。owner merge 会把只读 prepare 结果写入 IEW 的 pending future prepare；下一拍真实 `runIEWPrepare()` 如果 cycle 命中，就由 owner 直接复用该结果，不再提交本周期 IEW prepare worker。`--task-runtime-self-test` 开启时，IEW 会用当前冻结输入重算 expected 并更新 `iew.futurePrepareChecks/Matches/Mismatches`；下一拍真实 stage 全部执行后，`taskRuntime.futureWavefrontPrepare*` 只对比这条 wavefront 实际复用的 `CommitToIEW` 输入。这个 prepare-only reuse 不预测也不发布 future `IEWToRename` latch，不写 LSQ/IQ/ROB/fixedbuffer，不提前 dispatch，也不改变体系结构状态；因此 active dispatch 不再阻止这第一段 IEW prepare reuse。`IEWToRename` 的可观察影响由后续 Rename/Decode/Fetch consumer wavefront 和各 stage prepare self-test 验证。consumer wavefront 仍需要预测 post-dispatch `IEWToRename` latch；当前实现会先把 future active dispatch 输入降成 owner-side 轻量 `FutureDispatchPreviewInput`，再用 lookahead resource-token dry-run 判断 direct-dispatch 下一拍是 drain 还是 blocked。future token 模型现在复刻真实 `Scheduler::lookahead()` 的“每次 selector counter 清零、共享 dispatch table 的 OpClass 共用同一 counter”语义，因此 direct-dispatch `SchedulerNotReady` blocked latch 也可以发布；first-inst `SerializeBlocked` blocked latch 仍可发布。dispatch queue、LQ/SQ full、多个 active thread、混合输入或缺少快照仍继续跳过。

在 `Commit[c+1] -> IEW[c+1]` 之后，又接入了第二段成组 future wavefront latch/prepare reuse：`Commit[c+1] -> IEW[c+1] -> Rename[c+1]`。这段 reuse 只能在 `Decode[c]` strong task 完成后提交，因为 `Rename[c+1]` 会消费 advance 后的 `Decode->Rename` forward slot，也就是当前 `Decode[c]` 写出的结果。CPU 会在 owner 侧复用前一段逻辑得到 `CommitToIEW` 和 `IEWToRename`，然后用 predicted `IEWToRename`、shifted `DecodeStruct`、shifted IEW/Commit backward slot 构造不含 TimeBuffer slot 指针和 `DynInstPtr` 所有权的 `RenamePrepareInput`。worker 只消费这份轻量 input，预测下一拍 `RenameToDecode` latch，并同时保存对应的 `RenamePrepareResult`；这组 task 因此已经标成 `CrossTimeBufferAdvance`，可以越过本 tick 的 TimeBuffer advance。owner merge 保存 pending latch，也会把只读 prepare 结果写入 Rename 的 pending future prepare；下一拍真实 `runRenamePrepare()` 如果 cycle 命中，就由 owner 直接复用该结果，不再提交本周期 Rename prepare worker。`--task-runtime-self-test` 开启时，Rename 会用当前冻结输入重算 expected 并更新 `rename.futurePrepareChecks/Matches/Mismatches`；下一拍真实 `StallSignalBank` 捕获后仍用 `taskRuntime.futureRenameWavefrontPrepare*` 对拍 predicted latch。Rename preview 只覆盖不会做 commit squash、active rename 或 free-list/history/map 修改的子集：如果 future commit input 带 squash/robSquashing/doneSeqNum，或者 prepare 会选中 active thread 进入真实 rename，则跳过。对已经存在的 `releaseSeq < finalCommitSeq` backlog，future input 不再直接跳过，而是在 owner 侧用只读 token 投影模拟下一拍 `releaseWidth` 范围内会释放的物理寄存器数量：它扫描 historyBuffer 中将被 `removeFromHistory()` 覆盖的旧映射，用当前 phys-reg refcount 计算每个 reg class 的可用 token 增量，只把 token 数加进 `RenamePrepareInput::freePhyRegs`，不修改 refcount、free list、historyBuffer 或 release 序列。这个 reuse 不发布未来 stall edge，不提前释放物理寄存器，不写 rename map/free list/history/fixedbuffer，也不改变体系结构状态。

在第二段之后，又接入了第三段成组 future wavefront latch/prepare reuse：`Commit[c+1] -> IEW[c+1] -> Rename[c+1] -> Decode[c+1]`。这段 reuse 在 `Fetch[c]` strong task 完成后提交，使用 shifted `Fetch->Decode` slot 和 shifted Commit backward slot。CPU owner 先预测 `CommitToIEW`、`IEWToRename` 和 `RenameToDecode` 三条 latch，再把 predicted `RenameToDecode`、shifted Fetch 输入、shifted Commit backward 输入和 Decode 内部只读状态压成 `DecodePrepareInput`；worker 只消费这份轻量 input，预测下一拍 `DecodeToFetch` latch，并同时保存对应的 `DecodePrepareResult`。因为 weak task 不再持有 TimeBuffer slot 指针，也不在 worker 上复制或销毁 `DynInstPtr`，这段 task 已标成 `CrossTimeBufferAdvance`，可以越过本 tick 的 TimeBuffer advance。owner merge 保存 pending latch，也会把只读 prepare 结果写入 Decode 的 pending future prepare；下一拍真实 `runDecodePrepare()` 如果 cycle 命中，就由 owner 直接复用该结果，不再提交本周期 Decode prepare worker。`--task-runtime-self-test` 开启时，Decode 会用当前冻结输入重算 expected 并更新 `decode.futurePrepareChecks/Matches/Mismatches`；下一拍真实 `StallSignalBank` 捕获后仍用 `taskRuntime.futureDecodeWavefrontPrepare*` 对拍 predicted latch。Decode preview 只根据冻结的 `fixedbuffer` 空/非空状态、stallbuffer 队头、shifted Fetch 输入是否会让某线程变为 non-empty，以及 predicted `RenameToDecode` latch 计算 prepare 结果；它不移动 stallbuffer，不写 `toRename`，不改 `DynInst`，不做 branch/self-squash。只要 future commit 会 squash，或者 prepare 会选中 active thread 进入真实 `decodeInsts()`，就跳过。这个 reuse 不发布 future `DecodeToFetch`，不移动 Decode 队列，也不改变 Fetch 可见输入。

在第三段之后，又接入了第四段成组 future wavefront output/prepare reuse：`Commit[c+1] -> IEW[c+1] -> Rename[c+1] -> Decode[c+1] -> Fetch[c+1]`。CPU owner 先预测前三条 stall latch 和 predicted `DecodeToFetch` latch，再把 `DecodeToFetch`、shifted Decode/Commit backward 输入、当前 fetch queue 队头 seqNum 和 Fetch 只读控制状态压成 `Fetch::FutureDecodeQueueInput`；worker 只消费这份轻量 input，预测下一拍 `FetchToDecode` output summary：`toDecode.size`、`fetchStallReason` 和即将送往 Decode 的指令 seqNum，并同时保存对应的 `FetchToDecodePrepareResult`。因为 worker 不再持有 TimeBuffer slot 指针，也不读取或复制 `fetchQueue` 里的 `DynInstPtr`，这段 task 已标成 `CrossTimeBufferAdvance`，可以越过本 tick 的 TimeBuffer advance。Fetch preview 不调用 `dbpbtb->tick()`、不更新 FTQ/BPU、不中断/发起 cache request、不调用 decoder、不分配 `DynInst`，也不弹 `fetchQueue`。owner merge 会把只读 prepare 结果写入 Fetch 的 pending future prepare；下一拍真实 `runFetchToDecodePrepare()` 如果 cycle 命中，就由 owner 直接复用该 result，不再提交本周期 Fetch-to-Decode prepare worker。真实 pop `fetchQueue`、写 `toDecode`、activity 记录和 stats 应用仍由 owner 的 `applyFetchToDecodePrepareResult()` 完成。当前只覆盖两个严格稳定子集：所有线程都被 predicted `DecodeToFetch` 阻塞，因此真实 Fetch 即使内部取到新指令也不会送 Decode；或者 tid 0 当前 `fetchQueue` 已经至少有 `decodeWidth` 条，下一拍即使 Fetch 追加新指令，送往 Decode 的队头 `decodeWidth` 条及 stall reason 仍稳定。尝试把空队列/cache pending 也视为稳定会在 1M coremark 中出现“预期空输出、真实下一拍送 8 条”的 mismatch，原因是同 tick/前置 event 或 BPU tick 可能在 Fetch 发送前打开输入；因此这些情况必须保守跳过。这个 reuse 不发布 future `FetchToDecode`，不提前 BPU/FTQ/cache/decoder，也不改变前端体系结构状态。

其它 stage 的 future prepare 不能简单照搬 Commit probe。以 Decode 为例，`Decode[c+1]` 的 prepare 输入包含 same-cycle `Rename[c+1] -> Decode[c+1]` stall edge；这个 edge 不是 `Rename[c]` 的历史 latch，而是必须等 `Rename[c+1]` merge 后才稳定。曾经做过一个保守实验：只在 future Fetch 输入为空、Decode stallbuffer 为空、future Commit 不 squash 时提交 standalone `Decode[c+1]` probe，1M coremark 仍出现 3 次 future prepare mismatch。结论是 Decode future prepare 必须放进成组 wavefront：先完成 `Rename[c+1]` 的 owner merge，再由 `Decode[c+1]` 消费这个 merge 后的 latch/result；不能让 Decode 独立使用上一拍 current edge 预测下一拍输入。

Commit current prepare 和 future prepare reuse 的空线程处理都保持同一条语义：`fixedbuffer` 为空的线程不会直接填默认 no-stall，而是在 owner 侧调用同一个 `prepareCommitThreadControl()`。这样 `ROBSquashing` / `TrapPending` 或 ROB capacity 这类即使没有新 rename 指令也必须传播到 IEW 的 backpressure 不会丢失，同时避免为确定很小的 per-thread 控制计算提交 worker task。current prepare 计入 `commit.prepareInlineEmptyThreads`，future prepare 计入 `commit.futurePrepareInlineEmptyThreads`。

真实 stage prepare 拆分已经覆盖 Commit、IEW、Rename、Decode 和 Fetch。Commit 在 owner 侧先把 Rename queue 搬入 `fixedbuffer`，然后构造 `CommitPrepareInput`，冻结每线程 `fixedbuffer` 大小、ROB 剩余 entry、ROBSquashing/TrapPending 状态，并优先通过 `CPU::pipelineInputBackward(cycle, -iewToCommitDelay)` 从 input frame 的 backward TimeBuffer slot 读取来自 IEW 的 ROB head stall reason；在 runtime current-cycle 路径里缺少 matching input frame 或 backward slot 会直接报错，非 runtime 或未来 probe 路径才保留旧 fallback。runtime 开启时会把每个当前周期 `CommitThreadPrepare[tid,c]` 拆成独立 weak task，只计算该线程的 ROB state/capacity block、active 状态和 `CommitToIEW` stall 候选，随后由 `CommitMerge[c]` 在 owner 侧按 tid 顺序统一选择 selected thread、处理 multiple-active 仲裁、发布 IEW stall edge，并由原 strong task 继续真正插入 ROB。Commit 的提交、squash、trap、difftest、misc reg、BPU commit training、PCEventQueue 和 stats/debug 顺序仍全部强序；Commit future prepare 也复用同一套 per-thread 计算和 owner merge 组合逻辑，cycle 命中时下一拍直接复用该 prepare 控制结果，不发布未来 stall edge，也不提前执行提交相关副作用。IEW 的 `IEWPrepareInput` 冻结 `fixedbuffer` 是否为空、`CommitToIEW` stall latch、LSQ 是否可接收新 dispatch，以及 owner 侧预先计算好的 dispatch/LSQ stall reason；LSQ 可接收判断仍在 owner 侧完成，因为当前 helper 会消费上一拍 pop 计数。runtime 开启时，`fixedbuffer` 为空的线程在 owner 侧直接调用同一个 `prepareIEWThreadControl()` 并记录 `iew.prepareInlineEmptyThreads`，从而保留 commit/LDST block 对 Rename 的 backpressure 但避免提交空线程 worker task；其余线程才提交 `IEWThreadPrepare[tid,c]` weak task，只计算该线程的 commit block、LD/ST queue block、active 状态和 `IEWToRename` stall 候选，随后由 `IEWMerge[c]` 在 owner 侧按 tid 顺序统一选择 selected thread、处理 multiple-active 仲裁并发布 Rename stall edge。IEW dispatch 真正执行以后，还会构造 `DispatchStatusPrepareInput`，冻结 selected thread 的 ROB head、LQ head、SQ head 状态以及 LQ/SQ empty/full 位；worker 只用这些 snapshot 计算写回 Rename 的 `rob/lq/sqHeadStallReason`，owner merge 时用旧 `checkDispatchStall()` / `checkLSQStall()` 对拍，不一致则报错。scheduler ready/lookahead、IQ/LSQ insert、HTM 状态、scoreboard 和 dispatch stats 仍全部留在 owner dispatch 路径。IEW writeback 也增加了 `WritebackPrepareInput`：owner 冻结当前 `toCommit` 前 `wbWidth` 个连续 entry 的 tid、load saved-request 标志和 wakeup eligibility；只有存在 valid entry 时才提交 worker task，worker 只计算 valid entry 数、每线程 `instsToCommit` 统计增量和每 slot 的 wakeup 条件，空 entry 周期在 owner 侧 inline 返回同等 zero result，避免无效 task；owner merge 对拍后仍按旧顺序执行 probe、`scheduler->writebackWakeup()`、`instQueue.wakeDependents()`、scoreboard setReg 和 writeback stats。Rename 采用同样形状：`RenamePrepareInput` 冻结 `fixedbuffer` 需求、free physical register 数量、`IEWToRename` stall latch，并优先通过 `CPU::pipelineInputBackward(cycle, -iewToRenameDelay)` 从 input frame 的 backward TimeBuffer slot 读取 IEW 的 ROB/LQ/SQ head stall reason；runtime 开启时，`fixedbuffer` 为空的 inactive 线程在 owner 侧直接调用同一个 `prepareRenameThreadControl()` 并记录 `rename.prepareInactiveThreads`，从而保留 can-rename、RegFull 统计和 debug 字段但避免提交无效 worker task；其余线程才提交 `RenameThreadPrepare[tid,c]` weak task，只计算该线程的 can-rename、IEW block、RegFull 候选和 `RenameToDecode` stall 候选，随后由 `RenameMerge[c]` 在 owner 侧按 tid 顺序统一选择 selected/blocked thread、处理 multiple-active 仲裁、更新 RegFull stats 和 Decode stall edge。Decode 先构造 `DecodePrepareInput`，只包含本周期需要的私有输入：`fixedbuffer` 是否为空以及 `RenameToDecode` stall latch；runtime 开启时，inactive 线程也就是 `RenameToDecode` 未阻塞且 `fixedbuffer` 为空的线程，会在 owner 侧直接填入默认 no-stall result 并记录 `decode.prepareInactiveThreads`，避免提交确定无工作的 worker task；active 或 blocked 线程才提交 `DecodeThreadPrepare[tid,c]` weak task，只计算该线程的 active/block 和 decode/fetch stall 候选，随后由 `DecodeMerge[c]` 在 owner 侧按 tid 顺序统一选择 selected/blocked thread 并处理 multiple-active 仲裁。Fetch 先构造 `ResolvePrepareInput`，冻结当前 resolve queue 的 FTQ id 列表和本周期 IEW incoming resolved CFI；有 incoming CFI 时 worker 只计算 incoming CFI 应 append 到哪个已有 entry、是否要创建新 entry、queue full 统计和 enqueue/occupancy 统计，owner merge 后才修改 `resolveQueue`；没有 incoming CFI 时在 owner 侧 inline 执行同一个只读控制计算，保留旧的 occupancy/queue-full 采样但避免提交空 worker task。如果本周期入队前已经存在待处理队头，Fetch 随后构造 `ResolveDequeuePrepareInput`，冻结旧队头的 stream id 和 resolved PC 列表；worker 只决定是否处理这个旧队头并携带待标记的 PC 列表，owner merge 后仍按旧顺序调用 `dbpbtb->prepareResolveUpdateEntries()` / `markCFIResolved()` / `resolveUpdate()`，通知 FTQ/BPU resolve 结果并弹出 `resolveQueue`。随后 Fetch 构造 `FetchTargetPrepareInput`，冻结 FTQ round-robin 起点以及每个线程当前是否有可 fetch target 和对应 target tid；有 target 时 worker 用这个 snapshot 只读计算 selected tid，不推进 FTQ round-robin 指针；没有 target 时 owner inline 返回 InvalidThreadID 并记录 no-target 统计，避免提交确定无工作的 target-select task。owner merge 后若没有 target 则直接返回；若有 target，再构造 `FetchPrepareInput`，冻结 trace mode、FTQ/FSQ head 可用性、`canFetchInstructions()` 结果、macroop/fetch buffer 状态、interrupt/delayedCommit、fetch status、cache request status 和 fetch PC；worker 只计算 frontend 是否 ready、I-cache access 是否完成、是否允许继续取指、是否被 interrupt 阻塞以及 idle 状态。Fetch strong task 随后调用原来的 `getTargetTid()` 推进 round-robin 指针并校验它和 target-select prepare snapshot 中的 selected tid 一致，取指、BPU、cache、decoder、PC 和 `fetchQueue` 更新仍由 owner 执行。Fetch 末尾还会构造 `FetchToDecodePrepareInput`，冻结 `DecodeToFetch` block latch、每线程 `fetchQueue` size、当前 fetch stall reason 和 commit `robSquashing` 输入；如果所有线程都被 Decode 阻塞，owner inline 执行同一个控制计算并只更新 stall/bubble 统计，不提交确定不会弹 `fetchQueue` 或写 `toDecode` 的 worker task；否则 worker 只计算本周期送往 Decode 的条数、最终 stall reason 以及 frontend bubble/decode stall 统计增量，owner merge 后再弹出 `fetchQueue`、写 `toDecode` 并记录 activity。这个阶段仍然在对应 stage `tick()` 内等待 prepare 完成，因此它是 `task_window_cycles=1` 下的等价迁移，不是完整 wavefront；但它已经把“读输入快照、私有计算、确定性 merge、owner 写共享状态”的真实 stage 形状接入到了 task runtime。

Fetch 的 resolved-CFI 侧带队列现在也显式携带 `tid`，append 匹配 key 从单独的 FTQ id 扩展为 `(tid, ftqId)`。这样 SMT 下不同线程可复用相同 FTQ id 而不会在 worker prepare 中合并到同一个 resolve entry；old-front dequeue 也会把 entry 的 tid 交回 owner，用对应线程调用 `prepareResolveUpdateEntries()`、`markCFIResolved()`、`resolveUpdate()`，resolve 失败后的 prediction block 也只作用于对应线程。

LSQ store-buffer offload quota 也已经拆出只读 prepare。`LSQ::processWriteback()` 仍由 owner 先执行 `writebackBlockedStore()` 和 `storeBufferWriteback()`，因此 AMO、cache packet、store-buffer flushing 和 block 状态保持强序；只有在 store buffer 不阻塞后，owner 冻结 active thread 列表、每线程 `countStoreBufferOffloadableEntries()` demand、每拍最大 offload entry 数和 round-robin 起点。有非零 offload demand 时，worker 只计算 per-thread quota、granted entry 数和下一次 round-robin tid；全线程 demand 为 0 时在 owner 侧 inline 返回同等 zero quota，避免无效 worker task，但仍按旧顺序调用 `offloadToStoreBuffer(0)`，保留零大小 store 和 data prefetch 推进这类 owner 副作用。owner merge 对拍后更新 `nextStoreBufferOffloadTid`，再按旧顺序调用 `offloadToStoreBuffer()`，因此 SQ entry 移动、direct-to-cache、flush/complete 以及 packet 发送都不离开 owner。

Decode 的 Fetch->Decode forward queue 输入也开始从 live wire 收敛到 input frame。`Decode::tick()` 入口通过 `CPU::pipelineInputFetchToDecode(cycle, -fetchToDecodeDelay)` 取得本周期 FetchStruct；命中 snapshot 时，后续搬入 decode stallbuffer、传播 `fetchStallReason` 和 idle stall 归因都使用同一个冻结 FetchStruct，缺失时才回退原 `fromFetch` live wire。这个改动仍由 owner thread 在 Decode strong task 内搬运 `DynInstPtr` 和更新 fixedbuffer，不让 worker 写 Decode 内部队列；它只是把 Fetch->Decode 输入边界显式化，为后续 wavefront 中多个 cycle 同时读取 fetch queue slot 提供统一入口。

Decode 的 Commit->Decode backward TimeBuffer 输入也开始从 live wire 收敛到 input frame。`Decode::tick()` 入口通过 `CPU::pipelineInputBackward(cycle, -commitToDecodeDelay)` 取得本周期 TimeStruct；命中 snapshot 时，commit squash 判断、Decode squash 执行和 `squashVersion` 更新都使用同一个冻结 commit 输入，缺失时才回退原 `fromCommit` live wire。这个改动仍由 owner thread 在 Decode strong task 内执行 stallbuffer/fixedbuffer 清理和 squash 状态更新；它只是把 Commit->Decode 的 backward 输入边界显式化，避免后续 wavefront 中 Decode 读到未来 Commit 写入。

Rename 的 Decode->Rename forward queue 输入同样开始从 live wire 收敛到 input frame。`Rename::tick()` 入口通过 `CPU::pipelineInputDecodeToRename(cycle, -decodeToRenameDelay)` 取得本周期 DecodeStruct；命中 snapshot 时，后续搬入 rename fixedbuffer、向 IEW 传播 `fetchStallReason` / `decodeStallReason`，以及 rename stall 归因都使用同一个冻结 DecodeStruct，缺失时才回退原 `fromDecode` live wire。这个改动仍由 owner thread 在 Rename strong task 内搬运 `DynInstPtr` 和更新 rename map/free list 相关状态，不让 worker 写 Rename 内部队列；它只是把 Decode->Rename 输入边界显式化，为后续 wavefront 中多个 cycle 同时读取 decode queue slot 提供统一入口。

Rename 的 backward TimeBuffer 输入也开始从 live wire 收敛到 input frame。`Rename::tick()` 入口分别通过 `CPU::pipelineInputBackward(cycle, -iewToRenameDelay)` 和 `CPU::pipelineInputBackward(cycle, -commitToRenameDelay)` 取得本周期 IEW/Commit feedback；命中 snapshot 时，IEW head stall fallback、commit squash、doneSeqNum 对物理寄存器释放的推进以及 squashVersion 更新都使用同一组冻结 TimeStruct，缺失时才回退原 `fromIEW` / `fromCommit` live wire。这个改动仍由 owner thread 在 Rename strong task 内执行 rename map/free list/historyBuffer 副作用；它只是把 Rename 的 backward 输入边界显式化，为后续 wavefront 中多个 cycle 同时读取 IEW/Commit feedback 提供统一入口。

Commit 的 Rename->Commit/ROB forward queue 输入也开始从 live wire 收敛到 input frame。`Commit::tick()` 入口通过 `CPU::pipelineInputRenameToCommit(cycle, -renameToROBDelay)` 取得本周期 RenameStruct；命中 snapshot 时，trap/TC/SquashAfter/IEW redirect 对 rename-to-ROB in-flight 指令的 squash、把 renamed 指令搬入 commit fixedbuffer、ROB 插入和对应 branch stats 都使用同一个冻结 RenameStruct，缺失时才回退原 `fromRename` live wire。这个改动仍由 owner thread 在 Commit strong task 内执行 ROB 插入、提交、trap、difftest、PCEventQueue 和 BPU commit training；它只是把 Rename->ROB 输入边界显式化，为后续 wavefront 中多个 cycle 同时读取 rename queue slot 提供统一入口。

Commit 的 IEW->Commit forward queue 输入也开始从 live wire 收敛到 input frame。`Commit::tick()` 入口通过 `CPU::pipelineInputIEWToCommit(cycle, -iewToCommitDelay)` 取得本周期 IEWStruct；命中 snapshot 时，IEW redirect/squash 仲裁、squash target 元数据转发、branch/value/order violation 归因，以及 completed inst 的 `setCanCommit()` 标记都使用同一个冻结 IEWStruct，缺失时才回退原 `fromIEW` live wire。这个改动仍由 owner thread 在 Commit strong task 内执行 ROB squash、ready-to-commit 标记、提交、trap、difftest、PCEventQueue 和 BPU commit training；它只是把 IEW->Commit 输入边界显式化，为后续 wavefront 中多个 cycle 同时读取 IEW completion/squash queue slot 提供统一入口。

IEW 的 Rename->IEW forward queue 输入也开始从 live wire 收敛到 input frame。`IEW::tick()` 入口通过 `CPU::pipelineInputRenameToIEW(cycle, -renameToIEWDelay)` 取得本周期 RenameStruct；命中 snapshot 时，后续搬入 IEW fixedbuffer、统计 fetch/decode/rename stall reason，以及 dispatch 未完全成功时透传 rename stall reason 都使用同一个冻结 RenameStruct，缺失时才回退原 `fromRename` live wire。这个改动仍由 owner thread 在 IEW strong task 内执行 IQ/LSQ dispatch、scheduler、scoreboard、LSQ tick 和写回相关状态，不让 worker 写 backend 队列；它只是把 Rename->IEW 输入边界显式化，为后续 wavefront 中多个 cycle 同时读取 rename queue slot 提供统一入口。

IEW 的 Commit->IEW backward TimeBuffer 输入也开始从 live wire 收敛到 input frame。`IEW::tick()` 入口通过 `CPU::pipelineInputBackward(cycle, -commitToIEWDelay)` 取得本周期 TimeStruct；命中 snapshot 时，后续 commit squash/robSquashing 处理、doneSeqNum/doneMemSeqNum 应用、nonSpecSeqNum/strictlyOrdered load replay、dispatch serialize 判断以及 LSQ/IQ commit feedback 都使用同一个冻结 commit 输入，缺失时才回退原 `fromCommit` live wire。这个改动仍由 owner thread 在 IEW strong task 内执行 scheduler/LSQ/IQ/scoreboard 副作用；它只是把 Commit->IEW 的 backward 输入边界显式化，为后续 wavefront 中多个 cycle 同时读取 commit feedback 提供统一入口。

Fetch 的 backward TimeBuffer 输入也开始从 live wire 收敛到 input frame。`Fetch::tick()` 入口构造 `FetchBackwardInput`，通过 `CPU::pipelineInputFetchBackward()` 分别冻结 `Decode->Fetch`、`IEW->Fetch` 和 `Commit->Fetch` 三条实际被 Fetch 消费的 backward slot；命中 snapshot 时，decode squash、IEW resolved CFI 入队、commit squash/redirect/interrupt/emptyROB/robSquashing 以及 frontend bubble 统计都使用同一组冻结 TimeStruct slot，缺失时才回退原 `fromDecode` / `fromIEW` / `fromCommit` live wire。这个改动仍由 owner thread 在 Fetch strong task 内执行 FTQ/FSQ、BPU 更新、cache request、decoder、PC 和 fetch queue/toDecode 写入；它只是把 Fetch 的 backward 输入边界显式化，为后续 wavefront 中多个 cycle 同时读取 redirect/resolve/commit feedback 提供统一入口。

`StallSignals` 也已经开始迁移到显式 latch bank。当前 `src/cpu/o3/comm.hh` 中增加了 `StallSignalBank`、`StallSignalEdge` 和 `StallSignalLatch`：bank 内部按 `CommitToIEW`、`IEWToRename`、`RenameToDecode`、`DecodeToFetch` 四条边保存 block/reason latch，同时暴露 `legacyView()` 给尚未重构的 stage 使用。`CPU::tick()` 在每周期 stage 执行前调用 `stallSignalBank.beginCycle()` 把 edge latch publish 到 legacy view，在五个 strong stage 执行后调用 `stallSignalBank.endCycle()` 把 legacy view capture 回 edge latch。

`StallSignalBank` 现在还有一个按 cycle 编址的固定窗口 ring，容量由 `max(task_window_cycles, max_in_flight_cycles)` 配置。当前 cycle 的 `beginCycle()`、producer `set()` / `setBlock()` 和 `endCycle()` 都会把四条 edge latch 同步到 `CycleSlot[cycle]`；同时保留不带 cycle 的旧 API，确保当前串行 `Commit -> IEW -> Rename -> Decode -> Fetch` 的 same-cycle 可见性不变。新的 `snapshot(cycle, edge)` 返回冻结 cycle slot 指针，给后续 wavefront prepare 使用；如果窗口中没有对应 cycle，调用方必须视为不能读取未来/过期 latch，而不能回退到实时全局 `stallSig`。

当前真实 stage prepare 已经开始消费按 cycle 编址的 stall latch：IEW、Rename、Decode 和 Fetch 的 stall 输入读取改为通过 `CPU::stallSignalSnapshotOrCurrent(cycle, edge)` 访问。该入口先查 `StallSignalBank::snapshot(cycle, edge)` 并记录命中情况，命中时消费 `CycleSlot[currentCycle]` 中冻结的 edge latch；`task_parallel` runtime 开启且读取 `curCycle()` 时，miss 会直接报错，不再回退到 current edge latch。非 runtime 或后续只读 future/probe 路径仍可以使用 fallback 维持过渡兼容，但 fallback 必须经过 CPU helper，以便统计和 strict check 保持一致；`StallSignalBank` 不再提供裸 `snapshotOrCurrent()`。真正驱动 stage 行为的 current-cycle prepare 必须命中 `snapshot(cycle, edge)`。

四条 stall edge 都已经从纯 legacy 读写推进到 typed snapshot/delta API：Commit 写 `CommitToIEW`，IEW 读 cycle-indexed `CommitToIEW` 并写 `IEWToRename`，Rename 读 cycle-indexed `IEWToRename` 并写 `RenameToDecode`，Decode 读 cycle-indexed `RenameToDecode` 并写 `DecodeToFetch`，Fetch 读 cycle-indexed `DecodeToFetch`。每条 producer 仍通过 `StallSignalBank::set()` / `setBlock()` 同步更新 legacy view，保留当前串行 `Commit -> IEW -> Rename -> Decode -> Fetch` 的 same-cycle 可见性，也为后续冻结 per-cycle snapshot 提供明确边界。

代码里不应再引入 `TaskWindowPolicy` 或 `task_parallel_mode`。window 不是策略对象，而是单一 wavefront 模型里的资源/反压上限：`task_window_cycles` 控制最多准备多少个 CPU cycle，`max_in_flight_cycles` 控制最多有多少 prepared-but-unpublished cycle，`max_ready_tasks` 控制 ready queue 积压。它们改变的是同一套调度器的窗口大小和资源边界，不改变模拟语义。

## 数据流 wavefront

把当前 `CPU::tick()` 展开成二维 task graph 后，可以用 `C[c]`、`I[c]`、`R[c]`、`D[c]`、`F[c]` 表示第 `c` 个模拟周期上的 Commit、IEW、Rename、Decode、Fetch task。当前源码中的单周期逆序执行对应这些 same-cycle 强序边：

```text
C[c] -> I[c] -> R[c] -> D[c] -> F[c]
```

用数据流时序图看，可并行性更直观。每一行是一组沿流水线推进的数据/周期槽，每一列是 host 调度时刻：

```text
host time:    T1  T2  T3  T4  T5  T6
slot 0:       C   I   R   D   F
slot 1:           C   I   R   D   F
slot 2:               C   I   R   D   F
```

在 `T2` 这一列，`slot 0` 的 `I` 和 `slot 1` 的 `C` 同时 ready，因此从数据流上允许并行执行；到 `T3`，则可以形成 `R(slot 0) || I(slot 1) || C(slot 2)`。这就是跨模拟周期 wavefront 并行的来源：不是同一个数据项同时处在多个 stage，而是多个数据项/周期槽在同一 host 时间列上处在不同 stage。

这些边保留了当前 backpressure、squash、stall signal 和共享结构的可见顺序。与此同时，forward pipeline queue / TimeBuffer 又给出跨周期数据边：

```text
I[c] -> C[c + iewToCommitDelay]
R[c] -> I[c + renameToIEWDelay]
R[c] -> C[c + renameToROBDelay]
D[c] -> R[c + decodeToRenameDelay]
F[c] -> D[c + fetchToDecodeDelay]
```

在当前常见配置里，`iewToCommitDelay`、`renameToIEWDelay`、`renameToROBDelay`、`decodeToRenameDelay` 多数是 1，而 `fetchToDecodeDelay` 可能是 3 或 5。这样形成的不是单链表，而是一个有向无环图。注意完整 Commit stage 既消费 IEW 完成/squash，也消费 Rename->ROB queue；因此完整 `Commit[c+1]` 不能只等 `IEW[c]`，还要等 `Rename[c]`。如果后续把 Commit 继续拆成“只提交 ROB head 的 commit subphase”和“接收 Rename queue 的 ROB insert subphase”，前者可以释放更早的 `I[c] || C[c+1]` 并行度；当前 full-stage prepare 的第一条安全 overlap 是在 `Rename[c]` 后提前准备 `Commit[c+1]`，并与 `Decode[c]` / `Fetch[c]` 并行。host 调度器每次可以运行所有依赖已经满足的 task。例如忽略外部事件和更复杂的 split task，仅看相邻 delay 为 1 且包含 Rename->Commit 边的后端，启动后会出现类似这样的 ready set：

```text
host step 0: C[0]
host step 1: I[0]
host step 2: R[0]
host step 3: D[0] || C[1]
host step 4: F[0] || I[1]
```

这个例子说明了用户最初描述的重点：流水线数据流本身允许不同模拟周期上的不同 stage 同时在 host 上执行。具体到当前代码，`C[c+1]` 中处理 IEW 完成/squash 的 merge 部分需要等待 `I[c]`，但 `C[c+1]` 的 prepare 部分可以先与 `I[c]` 并行。因此实现上应把 stage task 进一步拆成 `prepare` 和 `merge`：`prepare` 尽量沿上面的时序图形成 wavefront，`merge` 再按依赖边发布可见状态。

这个 wavefront 模型还有一个额外边界：不能绕过 EventQueue 提前消费未来模拟时间的外部事件。`C[c+1]` 这类未来周期 task 只有在模拟调度器确认 `cycle c+1` 的 CPU tick 之前所有更高优先级 event 都已经产生可见状态时，才允许进入强序 merge。worker 可以提前做只读 prepare，但最终写共享状态必须受 event horizon 约束。

## Stall signal 隔离

`stallSig` 不能继续作为并行 task 共享读写的全局变量。它应该被重构为按 `cycle × edge × tid` 编址的 latch bank，并区分输入快照、局部输出和确定性 merge 结果。

建议把当前四类 block 信号拆成显式边：

```text
Commit[c] -> IEW[c]:    blockIEW[tid],    iewBlockReason[tid]
IEW[c]    -> Rename[c]: blockRename[tid], renameBlockReason[tid]
Rename[c] -> Decode[c]: blockDecode[tid], decodeBlockReason[tid]
Decode[c] -> Fetch[c]:  blockFetch[tid],  fetchBlockReason[tid]
```

每条边都对应一个 cycle-local latch。producer stage task 不能直接写全局 `stallSig`，只能产出 `StallSignalDelta`：

```cpp
struct StallSignalDelta {
    StageEdge edge;
    Cycles cycle;
    ThreadID tid;
    bool block;
    StallReason reason;
};
```

然后由对应 stage 的 merge task 按固定顺序写入 `StallSignalBank[cycle][edge][tid]`。consumer stage task 只能读取自己的 `StallSignalSnapshot`，这个 snapshot 在 task 开始前由调度器从对应 bank 固化下来。这样可以保证：

- `Decode[tid0, c]` 只看 `Rename[c] -> Decode[c]` 已经 merge 的信号，不会看到 `Rename[tid1, c+1]` 的未来输出。
- per-thread task 不能越权阻塞其他线程；如果需要“多个 active thread 时阻塞所有线程”这种全局仲裁，必须在 stage-level merge task 中统一决定，而不是由某个 thread-local task 顺手写全局数组。
- 同一个 cycle 内的 same-cycle 依赖仍然保留：`Decode[c]` 依赖 `Rename[c] -> Decode[c]` 的 merge 完成，`Fetch[c]` 依赖 `Decode[c] -> Fetch[c]` 的 merge 完成。

具体执行上，每个 stage 可以拆成两层：

```text
RenameThreadPrepare[tid, c]  // 只读输入快照，计算本线程是否可 rename
    -> RenameMerge[c]        // 统一仲裁资源冲突，写 blockDecode bank
        -> DecodeThreadPrepare[tid, c]
```

这样就能避免用户指出的问题：当 host 上线程 0 在跑 `Decode[0, c]`、线程 1 在跑 `Rename[1, c]` 或 `Rename[1, c+1]` 时，Rename 不会通过共享 `stallSig` 提前改变 Decode 的输入。Decode 的输入来自已经冻结的 snapshot，而不是实时全局数组。

长期看，`StallSignals` 最好不要作为 CPU-wide mutable state 暴露给所有 stage。当前 `StallSignalBank::legacyView()` 就是过渡适配层：`task_window_cycles=1` 时它仍映射回旧 `stallSig` 接口，方便未拆分的 stage 逻辑继续等价运行；四条 stall edge 已经有 typed API 读写路径。真正进入 wavefront 后，stage prepare 应读取 bank 固化的 snapshot，stage merge 只提交对应 edge 的 delta。

## 建模合同

多线程仿真模型的合同是：

1. 默认精确模式下，提交指令序列、架构寄存器、memory side effect、difftest 步进、异常/中断、squash/redirect、TimeBuffer 可见时机必须与单线程模拟一致。
2. EventQueue 的 `(tick, priority, same-bin order)` 是全局入口顺序。task scheduler 不能绕开 EventQueue 直接并行执行两个原本有序的 event callback。
3. 如果两个同 tick event 存在真实先后依赖，必须显式化为不同 priority 或 dependency，不能依赖同 priority bin 内的插入顺序作为软约束。
4. CPU tick 可以展开为多个带模拟周期编号的 stage task；task 只要所有数据依赖和 event horizon 依赖满足，就可以在 host 上并行运行。
5. 只有 owner event thread 或 CPU wavefront merge phase 能最终写 SimObject 共享状态、schedule/deschedule gem5 Event、推进 TimeBuffer、更新可见架构状态。
6. `stallSig` 这类跨 stage/cycle 侧带信号必须通过 edge-local latch bank 传递，禁止 worker task 或 thread-local task 直接写 CPU-wide 全局数组。
7. worker task 只允许读 snapshot 或处理私有副本，并产出 `TaskResult`。所有共享状态写入经过确定性 merge phase，按固定 `orderKey` 应用。
8. 对 stats/debug/difftest 默认要求确定性。若某类 stats 允许并行累加，必须使用 thread-local counter 并按固定顺序归并；debug log 要么只在 merge phase 输出，要么按 task order buffer 化后输出。

可以把因果链写成：

```text
gem5 event -> CPU/event task graph -> read snapshot/local compute
    -> deterministic merge -> shared state update
    -> TimeBuffer/EventQueue observable result
```

这个合同优先保证“同一个输入得到同一个模拟结果”。加速来自把耗时的候选计算、分类、扫描、统计、局部 pipeline 内部工作并行化，而不是改变 cycle 级架构顺序。

## Task 抽象

建议增加一个 CPU 内部 task runtime，而不是直接复用 EventQueue 作为细粒度 task 队列。EventQueue 仍然负责模拟时间；task runtime 只负责一个 event callback 内的 host 并行。

核心抽象：

```cpp
enum class TaskKind {
    Strong,
    Weak,
    Merge,
    Barrier,
};

struct TaskDesc {
    Tick tick;
    Cycles cycle;
    uint64_t epoch;
    TaskKind kind;
    StageId stage;
    ThreadID tid;
    uint32_t phase;
    uint64_t orderKey;
    SmallVector<TaskId, N> deps;
    TaskResult (*run)(TaskContext&);
    void (*merge)(TaskContext&, const TaskResult&);
};
```

`Strong` task 代表必须独占 owner 状态、按依赖顺序执行的工作，例如 `Commit::commitInsts()`、squash arbitration、TimeBuffer advance、Event schedule/deschedule。`Weak` task 代表只读 snapshot 或写私有结果的工作，可以放入 worker pool。`Merge` task 在 owner event thread 或 CPU wavefront merge phase 上按 `orderKey` 应用结果。`Barrier` task 用于 same-cycle stage 边界、跨周期 forward edge 和 event horizon 边界。

TaskResult 应该是小对象或 arena 分配对象，避免在热路径频繁堆分配。所有 task 数量都有上界：每个 CPU tick 的 stage task 数量由 stage 数、`numThreads`、pipeline width、IQ/LSQ 小固定队列深度决定；Event callback 派生 task 必须设置最大 fanout。

## Pipeline task graph

这里要说清楚“不能做什么”和“实际采用什么”，但这不是保留两套运行模式：

- 同周期直接 stage 并行不是实现目标：不能把 `C[c]`、`I[c]`、`R[c]`、`D[c]`、`F[c]` 无依赖地同时跑，因为 same-cycle 共享写入真实存在。
- 唯一实现目标是跨周期 wavefront 并行：把 `C[c+1]`、`R[c]`、`F[c-1]` 等不同周期、不同 stage task 放进同一个 ready set，只要数据依赖已满足就并行。

same-cycle 强序边仍然是：

```text
Commit writes commitInfo / ROB state
    -> IEW reads commitInfo, updates LSQ/IQ
        -> Rename reads IEW stall/free-entry feedback
            -> Decode reads Rename feedback
                -> Fetch reads Decode/Rename/IEW/Commit feedback
```

跨周期 wavefront 调度器维护每个 stage task 的未满足依赖计数。task 完成后释放后继边；ready queue 用固定 `orderKey = (cycle, stageOrder, tid, localSeq)` 保证可复现。一个 CPU window 的抽象图是：

```text
for cycle in [base, base + task_window_cycles):
  C[cycle] -> I[cycle] -> R[cycle] -> D[cycle] -> F[cycle]
  I[cycle] -> C[cycle + iewToCommitDelay]
  R[cycle] -> I[cycle + renameToIEWDelay]
  R[cycle] -> C[cycle + renameToROBDelay]
  D[cycle] -> R[cycle + decodeToRenameDelay]
  F[cycle] -> D[cycle + fetchToDecodeDelay]

eventHorizon[cycle] -> C[cycle], I[cycle], R[cycle], D[cycle], F[cycle]
```

对 same-cycle backpressure/stall edge，还需要额外把 producer merge 作为 consumer 的依赖：

```text
CommitMerge[c].stallOut -> IEWPrepare[c].stallIn
IEWMerge[c].stallOut    -> RenamePrepare[c].stallIn
RenameMerge[c].stallOut -> DecodePrepare[c].stallIn
DecodeMerge[c].stallOut -> FetchPrepare[c].stallIn
```

每个 stage task 内部还可以继续拆 prepare/merge 子任务：

| 模块 | 必须强序 | 可尝试弱序 |
| --- | --- | --- |
| Commit | ROB head 选择、提交、异常/中断、BPU commit training、`updateMiscRegs()`、difftest、PCEventQueue、squashAfter、`toIEW->commitInfo` 写入、ROB 插入 | 已落地 ROB 接收侧只读 backpressure 计算；后续可扩展到提交前 ready/head 状态采样、部分统计预聚合、commit trace 字符串生成 |
| IEW | `scheduler->tick()`、`ldstQueue.tick()`、dispatch 入 IQ/LSQ、ready select、scoreboard/readyQ 更新、load/store 请求发出、commit feedback 应用 | per-IQ 候选扫描、per-thread LSQ 局部 ready 检查、执行结果计算、统计聚合；最终端口仲裁和队列更新仍 merge 强序 |
| Rename | free list、rename map、historyBuffer、serialize barrier、`stallSig->blockDecode` 写入 | 每条指令需求寄存器数量预扫描、只读 stall reason 分类；实际分配和 map 更新强序 |
| Decode | stallBuffer/fixedbuffer 移动、branch self-squash、vector config serialize barrier、`stallSig->blockFetch` 写入 | 静态属性分类、stall reason 预计算；能否输出到 rename 仍强序 |
| Fetch | target tid 选择、squash/redirect 消费、`dbpbtb->tick()`、FTQ/FSQ 状态、I-cache request、decoder/PC 更新、fetchQueue/toDecode 写入 | 已落地 fetch eligibility 只读评估；后续可扩展到指令字节预解码或 trace 元数据准备，最终更新 predictor/cache queue 仍强序 |

这里的重点是：跨周期 wavefront 不要求改变 `fetchToDecodeDelay`、`commitToFetchDelay` 或 squash latency。它只是把原来单线程串行执行的拓扑顺序改成同一个 DAG 上的并行拓扑执行。真正需要重构的是状态保存方式：当前 TimeBuffer 的 `advance()` 是全局 base 指针移动，不适合多个 cycle 同时在 flight。wavefront 版本需要把 TimeBuffer slot 显式索引化，或在 task window 内维护 per-cycle latch bank，等 merge 到全局 committed frontier 时再推进原 TimeBuffer 视图。

## EventQueue 集成

EventQueue 不应该被细粒度 task runtime 替代。推荐规则：

1. `EventQueue::serviceOne()` 仍串行选择 event，保持原有 `(when, priority)` 顺序。
2. CPU tick event 可以把若干未来 CPU cycle 纳入内部 task window，但只能在 event horizon 允许时提交这些 cycle 的可见状态。
3. event callback 进入后，如果对象支持 task 化，可以提交内部 weak task；callback 返回前必须完成所有会影响本 event 可见结果的 task merge。
4. worker task 禁止直接调用 `schedule()`、`deschedule()`、`reschedule()`。需要调度事件时，把请求写入 `TaskResult`，由 owner event thread 在 merge phase 调用。
5. 对同 tick 同 priority 的事件做审计：凡是存在真实先后依赖的事件，必须调整为不同 priority class，或者引入显式 dependency/barrier。不要用“同一个 worker queue 中保持提交顺序”这类软约束来掩盖模型依赖。
6. 现有 multi-eventq/`simQuantum` 并行保持原语义，不作为 CPU 内部 task 并行的依赖。跨 eventq 的确定性同步继续使用 global event/async queue。

这样做的好处是把两个时间尺度分开：EventQueue 负责模拟时间和全局 event 顺序；task runtime 负责同一个 event 内的 host 计算并行。

推荐增加一个 event priority 审计阶段：

```text
Event trace / debug log
  -> 按 (when, priority) 分组
  -> 找出同组内访问同一 SimObject 或同一资源的事件
  -> 判断是否存在 write-after-read / write-after-write / side-effect ordering
  -> 为真实依赖分配显式 priority 或 dependency
```

这一步应该在扩大 task 并行窗口前完成。否则并行化会把旧模型中隐藏的 same-bin 顺序假设暴露成偶发 bug。

## 运行时同步与负载均衡

多线程仿真不仅是把 stage 拆成 task，还需要一个明确的运行时策略。这里至少有四类问题必须进入设计合同：事件同步、负载均衡、任务粒度和运行时反压。

事件同步的核心是 event horizon。CPU wavefront 可以在内部形成多个 in-flight cycle，但不能越过 EventQueue 已确认的可见边界。调度器应维护一个 `committableCycle` 和一个 `preparedCycle`：

```text
preparedCycle:    worker 已经可以基于 snapshot 提前计算到的最远 CPU cycle
committableCycle: 已确认没有更高优先级外部事件会改变状态、可以发布可见结果的最远 CPU cycle
```

worker 可以提前执行 `prepare`，但 `merge/commit` 只能推进到 `committableCycle`。如果下一个 event 可能在 `cycle c` 前改变 CPU 可见输入，所有 `cycle >= c` 的 merge 都必须等待。这可以避免未来 CPU cycle 提前消费尚未执行的 cache/memory/device/interrupt event。

同步原语建议只保留少数几类：

- `StageBarrier(c, edge)`：保证 same-cycle stage 依赖，例如 `RenameMerge[c] -> DecodePrepare[c]`。
- `ForwardBarrier(c, edge)`：保证 forward latency，例如 `IEW[c] -> Commit[c + iewToCommitDelay]`。
- `EventHorizonBarrier(c)`：保证外部 event 已经执行到允许 CPU cycle `c` merge。
- `DrainBarrier` / `QuiesceBarrier`：drain、checkpoint、切换 CPU、退出线程时强制收敛所有 in-flight task。

负载均衡不能破坏确定性。推荐采用“确定性 ready queue + work stealing 只影响执行者、不影响 merge 顺序”的模式：

- ready task 按固定 `orderKey` 入队，`orderKey` 至少包含 `(cycle, stage, phase, tid, localSeq)`。
- worker 可以偷取任务提高利用率，但 task 输出只进入 per-task result slot。
- merge 永远按 `orderKey` 或显式 dependency 拓扑顺序执行，与哪个 worker 实际跑了 task 无关。
- 对长尾 stage，例如 IEW/LSQ、BPU、cache miss 相关处理，应拆成 bounded 子任务，避免一个 worker 长时间占住关键路径。

任务粒度需要有阈值。过细会让调度开销超过收益，过粗会导致并行度不足。建议用 `task_min_work` 和 stage-specific batching 控制：

- 小于阈值的 per-thread/per-instruction 工作直接 inline。
- IEW/LSQ 这类热点按 IQ、LSQ thread、ready queue、load/store pipe lane 拆成 bounded task。
- Commit 的架构 side effect 不拆细；只把只读预处理或 trace 字符串构造放到 weak task。
- Fetch/BPU 如果内部存在多级预测器流水，可以按 predictor substage 或 fetch block 拆 task，但最终 FTQ/FSQ 更新强序 merge。

运行时反压也需要显式建模。`task_window_cycles` 不能无限扩大，否则 snapshot、latch bank、TaskResult 和未提交 side effect 会占用大量内存，并可能让 squash 后需要丢弃大量已准备结果。建议设置：

- `max_in_flight_cycles`：CPU wavefront 最多提前多少 cycle。
- `max_ready_tasks`：ready queue 上限，超过后 owner thread 等待当前 ready queue 清空，并 inline 执行当前 task；对应 backpressure 次数必须可由 stats 观测。
- `max_spec_task_waste`：被 squash/event horizon 丢弃的 prepare 工作比例上限，用于限制后续 speculative/future prepare 提交；默认 100 表示完全放行以保持旧 runtime 行为。

负载均衡目标不是让所有 worker 永远忙，而是在不增加关键路径等待的前提下减少 wall-time。应通过 stats 区分“可并行工作不足”和“同步/负载不均造成等待”，否则很难判断多线程仿真是否真的有效。

## 数据所有权和 race 规则

必须采用单写者规则：

- SimObject 字段、stage 成员、TimeBuffer、ROB/IQ/LSQ、BPU、regfile、scoreboard、difftest state 只能由 owner event thread 写。
- `StallSignals`、TimeBuffer wires、stage-to-stage sideband payload 都视为 latch bank，由 task runtime 按 cycle/edge 管理；worker 只读 snapshot，只写 delta。
- worker 可以读取只读 snapshot。snapshot 可以是指针加版本号，也可以是紧凑副本；不能在 worker 执行期间被 owner 修改。
- `DynInstPtr` 是可变对象，默认视为强序状态。worker 不应直接修改 `DynInst` 标志位、寄存器映射、squash/completed/commit 状态。若确实需要并行处理，worker 只返回待应用操作列表。
- `DynInstPtr` 的引用计数本身也不是 worker-safe 资源。weak task 不能复制、移动或销毁带 `DynInstPtr` 的对象，也不能把包含 `DynInstPtr` 的 TimeBuffer frame 交给 worker 持有；需要只读观察时使用 owner 已冻结的轻量输入、`const DynInstPtr &`、raw seqNum/PC 摘要，且任何会增减引用计数的副本都必须在 owner event thread 上创建和销毁。
- future wavefront probe 可以在同一个 CPU tick 内跨过更晚 stage barrier 执行。读取 current TimeBuffer slot 地址或间接持有 `DynInstPtr` 的 future weak task 仍必须在 `TimeBuffer::advance()` 前完成并 merge；输入已经在 owner 侧降成独立轻量 snapshot 的 task 可以标成 `CrossTimeBufferAdvance`，允许跨过 circular buffer advance，但仍只能在 owner barrier 上按固定 `TaskOrderKey` merge。
- stats 使用 thread-local shard，merge phase 固定顺序累加。对需要精确事件顺序的统计，直接保持强序。
- DPRINTF/trace/archDB/difftest 在精确模式下保持强序。需要并行生成文本时，用 `(tick, stage, tid, seqNum, orderKey)` 排序后输出。

最小实现里可以先不做复杂 read/write set 检查，而是按 API 约束区分 `snapshot()`、`runWeak()`、`merge()` 三个阶段。后续再为 debug build 增加 owner-thread assert 和 task race checker。

## 参数和观测

这些参数属于 host-side simulation runtime，放在 `System` 层级，而不是任何单个 CPU 模型层级。当前代码已经按这个方向实现：Python 参数在 `System.py`，C++ 配置在 `System::TaskParallelConfig`，CPU 只读取所属 system 的 runtime 配置。

不需要 `task_parallel_mode`：主线只维护一套精确 wavefront 调度模型。也不需要 `TaskWindowPolicy`：窗口相关参数是数值上限和反压阈值，不是可切换策略。

| 参数 | 默认值 | 含义 |
|---|---:|---|
| `enable_task_parallel_sim` | `false` | 总开关，默认保持旧行为 |
| `task_parallel_threads` | `2` | worker 数；2 是当前推荐默认值，避免单线程模拟器本身已经很快时过量占用 host 线程。显式设为 0 时按 host 自动选择，但最多仍按默认 cap 使用 2 个 worker |
| `task_window_cycles` | `1` | CPU wavefront 准备窗口深度；1 是同一套模型的串行等价验证配置 |
| `task_deterministic` | `true` | 强制固定 merge 顺序和 deterministic debug/stat policy |
| `task_min_work` | `0` | 小于阈值的工作直接 inline，避免 task overhead |
| `task_trace` | `false` | 打开当前 `TaskSched` 运行时 trace；后续 wavefront 调度可扩展到 task graph、critical path 和 worker utilization |
| `task_runtime_self_test` | `false` | 显式运行 worker 执行和确定性 merge 自检 |
| `event_priority_audit` | `false` | 记录同 tick 同 priority 的事件组，辅助发现隐式顺序依赖 |
| `max_in_flight_cycles` | `1` | CPU wavefront 允许的最大未提交 cycle 数 |
| `max_ready_tasks` | `0` | ready queue 上限，0 表示不设显式上限；非 0 时触发 backpressure 后 owner 会等待并 inline 当前 task |
| `max_spec_task_waste` | `100` | squash/event horizon 导致的 prepare 浪费比例阈值；100 表示不节流，低于 100 时超过阈值的 tick 不再提交新的 speculative future prepare |

当前已落地的 stats/debug：

- `taskRuntime.created`, `taskRuntime.inlined`, `taskRuntime.executed`, `taskRuntime.merged`
- `taskRuntime.strong`
- `taskRuntime.stageBarrierWaits`, `taskRuntime.stageBarrierDeferredTasks`, `taskRuntime.stageBarrierMaxDeferredTasks`, `taskRuntime.horizonWaits`；`stageBarrierWaits` 同时覆盖 strong task barrier 和 stage prepare 的 `waitForOrder()` barrier，deferred stats 用来观察更晚 order 的 future weak task 是否跨过这些 barrier
- `taskRuntime.readyQueueSamples`, `taskRuntime.readyQueueOccupancy`, `taskRuntime.maxReadyQueueDepth`, `taskRuntime.readyQueueBackpressureWaits`, `taskRuntime.readyQueueBackpressureInlineTasks`
- `taskRuntime.inFlightCycleSamples`, `taskRuntime.inFlightCycles`，当前串行等价 scaffold 每个 runtime-enabled tick 采样 1 个实际 in-flight CPU cycle
- `taskRuntime.wavefrontPlanSamples`, `taskRuntime.wavefrontPlanEffectiveCycles`, `taskRuntime.wavefrontPlanTasks`, `taskRuntime.wavefrontPlanEdges`
- `taskRuntime.wavefrontPlanCriticalPathLen`, `taskRuntime.wavefrontPlanMaxReadyTasks`, `taskRuntime.wavefrontPlanReadySlack`，当前只是 static coarse DAG 审计，不代表已经执行跨周期并行
- `taskRuntime.eventHorizonSamples`, `taskRuntime.eventHorizonCandidateCycles`, `taskRuntime.eventHorizonCommittableCycles`, `taskRuntime.eventHorizonLimitedCycles`
- `taskRuntime.eventHorizonBlockedSamples`, `taskRuntime.eventHorizonSameTickBlocks`, `taskRuntime.eventHorizonZeroCycleBlocks`, `taskRuntime.eventHorizonPartialWindowBlocks`, `taskRuntime.eventHorizonEarlierTickBlocks`, `taskRuntime.eventHorizonCpuPriorityBlocks`, `taskRuntime.eventHorizonMaxBlockedOffset`, `taskRuntime.eventHorizonMaxCommittableCycles`，当前只是 EventQueue 队头边界审计，不代表已经跨周期 merge；这些分类用来区分窗口被更早 tick 的外部 event 截断，还是被同一个 future CPU tick 上 priority 不晚于 `CPU_Tick` 的 event 截断
- `taskRuntime.eventHorizonBlockReasons`，按 fixed priority class 进一步归因 horizon blocker：`EarlierTick`、`EarlyPriority`、`CpuSwitch`、`DelayedWriteback`、`Default`、`DvfsSerialize`、`CpuTick`、`OtherCpuPriority`。这个 vector 只用 priority 常量分类，不在热路径保存动态 event 名字；需要具体 event 类型时再打开 `TaskGraph` trace 抽样。`EarlyPriority` 表示 priority 早于 `CPU_Switch_Pri` 的同 tick event，不等同于只包含 debug event
- `taskRuntime.eventHorizonBlockerTypes`, `taskRuntime.eventHorizonEarlierTickBlockerTypes`, `taskRuntime.eventHorizonCpuPriorityBlockerTypes`，只有开启 `--event-priority-audit` 时按固定 subsystem bucket 记录 horizon blocker event 类型：`MemoryController`、`L1Cache`、`L2Slice`、`L2Wrapper`、`L2Other`、`L3MemSidePort`、`L3Cache`、`L1Prefetcher`、`L2WrapperPrefetcher`、`L2SlicePrefetcher`、`L2OtherPrefetcher`、`L3Prefetcher`、`OtherPrefetcher`、`Interconnect`、`CPU`、`Device`、`Other`。总量桶用于和 `eventHorizonBlockedSamples` 闭合；earlier-tick 桶用于识别绝对不能跨越的外部事件；cpu-priority 桶用于识别后续可能通过显式 priority 或 dependency 继续放宽的同 tick 事件家族。默认 runtime 下三组保持全 0，并且不调用 `Event::name()`
- `event.samePriorityGroups`, `event.samePriorityInsertedEvents`, `event.samePriorityMaxDepth`，只有开启 `--event-priority-audit` 时更新，用于机器可读地统计插入到既有同 tick 同 priority bin 的次数、这些 bin 插入后的事件数累计和最大既有深度；`event.samePriorityGroupsByClass`、`event.samePriorityInsertedEventsByClass`、`event.samePriorityMaxDepthByClass` 进一步按 `Minimum`、`EarlyPriority`、`CpuSwitch`、`DelayedWriteback`、`Default`、`DvfsSerialize`、`CpuTick`、`Other` 分桶，帮助定位后续应该显式拆 priority 的事件类别。这些统计只审计隐式同优先级顺序依赖候选，不改变 EventQueue 排序。实现上 `event` 是挂到 `Root` 的直属 stats group，避免全局 `RootStats` merge 到 `Root` 时子 group 不被 stats dump 遍历
- `taskRuntime.stallSignalWindowSamples`, `taskRuntime.stallSignalWindowCapacity`, `taskRuntime.stallSignalWindowValidSlots`
- `taskRuntime.stallSignalWindowMaxValidSlots`, `taskRuntime.stallSignalWindowEdgesCaptured`，当前记录 per-cycle stall latch window 的容量、有效 slot 和捕获 edge 数
- `taskRuntime.stallSignalMerges`，按 `CommitToIEW`、`IEWToRename`、`RenameToDecode`、`DecodeToFetch` 统计 owner stage 通过 stall-edge producer wrapper 写入 stall edge 的次数，用于确认 same-cycle latch 生产侧已经显式化；它只统计 edge 写入次数，不改变 latch 内容或发布顺序，并且保持无 self-prereq 显示，避免四条 edge 全非零时被当前 vector prereq 语义隐藏。当前 wrapper 内部优先写 typed `StallSignalBank`，保留 legacy fallback 仅用于过渡
- `taskRuntime.stallSignalInputReads`, `taskRuntime.stallSignalInputReadFallbacks`, `taskRuntime.stallSignalFutureReadBlocks`，用于确认 stage/probe 是否从 per-cycle stall latch window 读取；current-cycle runtime miss 仍直接报错，non-current cycle miss 会计入 `stallSignalFutureReadBlocks`，作为后续 wavefront 不能读取过期或未来 latch 时的显式观测
- `taskRuntime.timeBufferInputSnapshots`, `taskRuntime.timeBufferOutputSnapshots`, `taskRuntime.timeBufferSlotsCaptured`
- `taskRuntime.timeBufferSnapshotWindowSamples`, `taskRuntime.timeBufferSnapshotWindowCapacity`, `taskRuntime.timeBufferInputWindowValidFrames`, `taskRuntime.timeBufferOutputWindowValidFrames`, `taskRuntime.timeBufferSnapshotWindowMaxValidFrames`，当前记录 TimeBuffer snapshot cycle ring 的容量和有效 frame 数
- `taskRuntime.timeBufferStageInputReads`, `taskRuntime.timeBufferStageInputReadMisses`，当前用于确认 stage 是否命中本周期 input frame；在 `task_parallel` runtime 的 current-cycle 路径里 miss 会直接报错，非 runtime 或未来 probe 路径才保留旧 fallback
- `taskRuntime.timeBufferBackwardSlotReads`, `taskRuntime.timeBufferBackwardSlotReadMisses`，当前用于确认 Commit/Rename prepare、Rename tick 的 IEW/Commit backward 输入、IEW 的 Commit->IEW tick 输入和 Decode 的 Commit->Decode squash 输入是否命中 input frame 中指定 backward slot；在 runtime current-cycle 路径里 miss 会直接报错
- `taskRuntime.timeBufferFetchBackwardSlotReads`, `taskRuntime.timeBufferFetchBackwardSlotReadMisses`，当前用于确认 Fetch 是否命中 input frame 中指定 backward slot；在 runtime current-cycle 路径里 miss 会直接报错
- `taskRuntime.timeBufferFetchToDecodeSlotReads`, `taskRuntime.timeBufferFetchToDecodeSlotReadMisses`，当前用于确认 Decode 是否命中 input frame 中指定 Fetch->Decode forward queue slot；在 runtime current-cycle 路径里 miss 会直接报错
- `taskRuntime.timeBufferDecodeToRenameSlotReads`, `taskRuntime.timeBufferDecodeToRenameSlotReadMisses`，当前用于确认 Rename 是否命中 input frame 中指定 Decode->Rename forward queue slot；在 runtime current-cycle 路径里 miss 会直接报错
- `taskRuntime.timeBufferRenameToIEWSlotReads`, `taskRuntime.timeBufferRenameToIEWSlotReadMisses`，当前用于确认 IEW 是否命中 input frame 中指定 Rename->IEW forward queue slot；在 runtime current-cycle 路径里 miss 会直接报错
- `taskRuntime.timeBufferRenameToCommitSlotReads`, `taskRuntime.timeBufferRenameToCommitSlotReadMisses`，当前用于确认 Commit 是否命中 input frame 中指定 Rename->Commit/ROB forward queue slot；在 runtime current-cycle 路径里 miss 会直接报错
- `taskRuntime.timeBufferIEWToCommitSlotReads`, `taskRuntime.timeBufferIEWToCommitSlotReadMisses`，当前用于确认 Commit 是否命中 input frame 中指定 IEW->Commit forward queue slot；在 runtime current-cycle 路径里 miss 会直接报错
- `taskRuntime.timeBufferPrepareMerges`, `taskRuntime.timeBufferPreparedInstRefs`, `taskRuntime.timeBufferPreparedControlSignals`, `taskRuntime.timeBufferPreparedResolvedCFIs`；这些 host-only summary stats 只在 `--task-trace` 诊断路径启用，默认 runtime 不提交对应 weak task。该诊断不再把包含 `DynInstPtr` 的完整 Frame 交给 worker；summary 在 owner 线程基于冻结 frame 生成，worker 只承载不含 `DynInstPtr` 的调度/merge壳
- `taskRuntime.timeBufferAdvanceWaits`, `taskRuntime.timeBufferAdvancePendingTasks`, `taskRuntime.timeBufferAdvanceMaxPendingTasks`，记录 TimeBuffer circular buffer advance 前仍有多少 `PreAdvanceDrain` weak/future task 必须归并。这个 barrier 是当前过渡实现的正确性边界：只要 worker 仍可能持有当前 TimeBuffer slot 指针或间接触碰 `DynInstPtr`，就必须在 `advance()` 前收回
- `taskRuntime.timeBufferAdvanceSafeDeferrals`, `taskRuntime.timeBufferAdvanceSafeDeferredTasks`, `taskRuntime.timeBufferAdvanceMaxSafeDeferredTasks`，记录已经标成 `CrossTimeBufferAdvance` 的 weak/future task 有多少越过了 TimeBuffer advance。它们量化后续把 future probe 输入降成 DynInst-free snapshot 后释放出的跨 tick 并行窗口
- `taskRuntime.serialTickEndSafeDeferrals`, `taskRuntime.serialTickEndSafeDeferredTasks`, `taskRuntime.serialTickEndMaxSafeDeferredTasks`，记录当前 CPU callback 尾部有多少 `CrossTimeBufferAdvance` task 被安全留到下一次 CPU tick 开始再 merge。该 deferral 只有在 EventQueue 队头就是下一次 CPU tick 时才允许；如果队头是 exit/drain 或任何会先于下一拍 CPU tick 执行的事件，仍然同步 `waitForAll()`
- `taskRuntime.futureTimeBufferInputSnapshots`, `taskRuntime.futureTimeBufferInputSnapshotSlots`，当前记录 event horizon 和最终 CPU schedule 都允许时发布的下一拍私有 TimeBuffer input snapshot；实际 frame 在 TimeBuffer advance 前用 shifted offset 捕获，且只在 `--task-trace` 诊断路径启用
- `taskRuntime.futureTimeBufferPrepareMerges`, `taskRuntime.futureTimeBufferPrepareSkipped`, `taskRuntime.futureTimeBufferPreparedInstRefs`, `taskRuntime.futureTimeBufferPreparedControlSignals`, `taskRuntime.futureTimeBufferPreparedResolvedCFIs`，当前只观测下一拍只读 prepare 是否被 horizon/CPU schedule 允许以及其 summary，不参与 stage 行为；该诊断默认关闭，且不允许 worker 持有带 `DynInstPtr` 的 future Frame。若提前计算完成但 tick 尾部 drain/idle 取消下一拍，会计入 skipped
- `taskRuntime.futureTimeBufferPrepareReuses`, `taskRuntime.futureTimeBufferPrepareChecks`, `taskRuntime.futureTimeBufferPrepareMatches`, `taskRuntime.futureTimeBufferPrepareMismatches`, `taskRuntime.futureTimeBufferPrepareStale`，当前用于确认上一拍准备的 future summary 是否在下一拍被复用、是否和真实 input snapshot 一致，以及是否出现过期 cycle；这些诊断默认关闭
- `taskRuntime.specPrepared`, `taskRuntime.specDiscarded`, `taskRuntime.specThrottled`；`specDiscarded` 当前用于记录已经提前计算、但最终没有发布的 speculative/future prepare，默认 runtime 中只由仍启用的真实 future prepare 类任务贡献；`specThrottled` 记录 `max_spec_task_waste < 100` 且 `specDiscarded / (specPrepared + specDiscarded)` 超过阈值时，被反压门控禁止提交新 future prepare 的 tick 数
- `taskRuntime.futureWavefrontPrepareProbes/Merges/Skipped/Checks/Matches/Mismatches/Stale`，当前用于确认 horizon-gated `Commit[c+1] -> IEW[c+1]` prepare-only probe 是否只读可预测；对应 IEW prepare 结果可以在下一拍 owner 侧复用，但 probe 不预测或发布 future `IEWToRename` latch，也不驱动 active dispatch
- `taskRuntime.futureRenameWavefrontPrepareProbes/Merges/Skipped/Checks/Matches/Mismatches/Stale`，当前用于确认 horizon-gated `Commit[c+1] -> IEW[c+1] -> Rename[c+1]` latch probe 是否只读可预测；对应 Rename prepare 结果可以在下一拍 owner 侧复用，但 probe 不发布 future latch，也不驱动 active rename
- `taskRuntime.futureDecodeWavefrontPrepareProbes/Merges/Skipped/Checks/Matches/Mismatches/Stale`，当前用于确认 horizon-gated `Commit[c+1] -> IEW[c+1] -> Rename[c+1] -> Decode[c+1]` latch probe 是否只读可预测；对应 Decode prepare 结果可以在下一拍 owner 侧复用，但 probe 不发布 future latch，也不驱动 active decode
- `taskRuntime.futureFetchWavefrontPrepareProbes/Merges/Skipped/Checks/Matches/Mismatches/Stale`，当前用于确认 horizon-gated `Commit[c+1] -> IEW[c+1] -> Rename[c+1] -> Decode[c+1] -> Fetch[c+1]` output summary probe 是否只读可预测；对应 Fetch-to-Decode prepare 结果可以在下一拍 owner 侧复用，但 probe 不发布 future output，也不提前前端状态机
- `taskRuntime.futureWavefrontSkipReasons`，按 `CommitPreview`、`IEWInput`、`IEWPreview`、`RenameInput`、`RenamePreview`、`DecodeInput`、`DecodePreview`、`FetchInput`、`FetchPreview` 统计四段 future wavefront probe 是在哪个只读边界保守跳过。这个 vector 不参与调度，只用于判断下一步应该扩展哪个稳定子集；其 total 应与四段 `future*WavefrontPrepareSkipped` 之和闭合
- `taskRuntime.workerBusyHostNs`, `taskRuntime.workerIdleHostNs`, `taskRuntime.steals`；busy/idle 当前记录 worker 执行或等待 task 的 host-side nanosecond 累计值，属于运行时负载观测，不参与模拟语义
- `taskRuntime.stageWeakTasks[stage]`, `taskRuntime.stageWeakWork[stage]`, `taskRuntime.stageWeakMerges[stage]`, `taskRuntime.stageInlineTasks[stage]`
- `taskRuntime.stageTaskRunHostNs[stage]`, `taskRuntime.stageTaskMergeHostNs[stage]`，当前用于把 weak task 的 host run/merge 时间按 stage 归因；run 时间包含 worker 和 inline 执行，merge 时间只统计 owner thread 应用结果的时间，不参与模拟语义
- `commit.prepareTasks`, `commit.prepareMerges`, `commit.prepareActiveThreads`, `commit.prepareBlockedThreads`, `commit.prepareInlineEmptyThreads`, `commit.prepareMultipleActive`；其中 `fixedbuffer` 为空的线程在 owner 侧调用同一个 per-thread helper，保留 ROB state/capacity backpressure 但避免提交空线程 worker task
- `commit.futurePrepareProbes`, `commit.futurePrepareSkipped`, `commit.futurePrepareMerges`, `commit.futurePrepareReuses`, `commit.futurePrepareInlineEmptyThreads`, `commit.futurePrepareChecks`, `commit.futurePrepareMatches`, `commit.futurePrepareMismatches`, `commit.futurePrepareStale`，当前用于确认 horizon-gated 下一拍 Commit prepare 是否只读可预测、是否被下一拍 owner 复用，以及 self-test 下是否与当前冻结输入重算结果一致；reuse 只替代 Commit prepare 控制计算，不提前发布提交、squash、trap、difftest 或 BPU commit training
- `iew.prepareTasks`, `iew.prepareMerges`, `iew.prepareActiveThreads`, `iew.prepareBlockedThreads`, `iew.prepareInlineEmptyThreads`；其中 `fixedbuffer` 为空的线程在 owner 侧调用同一个 per-thread helper，保留 commit/LDST backpressure 但避免提交空线程 worker task
- `iew.futurePrepareProbes`, `iew.futurePrepareSkipped`, `iew.futurePrepareMerges`, `iew.futurePrepareReuses`, `iew.futurePrepareChecks`, `iew.futurePrepareMatches`, `iew.futurePrepareMismatches`, `iew.futurePrepareStale`，当前用于确认 `Commit[c+1] -> IEW[c+1]` wavefront 中的 IEW prepare 结果是否被下一拍 owner 复用，以及 self-test 下是否与当前冻结输入重算结果一致；reuse 只替代 IEW prepare 控制计算，不提前 dispatch 或写 IQ/LSQ/scoreboard
- `iew.futureInputSkipReasons`，按 `MissingSnapshot`、`NoActiveThreads`、`CommitControl`、`CommitProgressWithLDSTBlock` 细分 future IEW prepare input 构造为什么不安全。这个 vector 只对 `taskRuntime.futureWavefrontSkipReasons::IEWInput` 继续归因，不改变 owner merge 或体系结构状态；其 total 应与 runtime 的 `IEWInput` 闭合
- `iew.futureInputCommitControlReasons`，按 `Squash`、`RobSquashing`、`DoneSeqNum`、`DoneMemSeqNum`、`NonSpecSeqNum`、`StrictlyOrdered` 继续细分 `CommitControl`。只记录第一个阻塞字段，使 total 与 `iew.futureInputSkipReasons::CommitControl` 闭合；当前 `DoneSeqNum/DoneMemSeqNum` 在没有 LDST block 时不再阻塞，而是计入 accepted progress 统计
- `iew.futureInputAllowedCommitProgress`，按 `DoneSeqNum`、`DoneMemSeqNum` 记录已经被 future IEW input 接受的 commit progress 字段出现次数。`doneSeqNum/doneMemSeqNum` 的真实 IQ/LSQ commit 更新发生在 IEW prepare 之后，因此只有当同一线程已经处于 `!ldstCanInsert`、且 stall reason 可能在下一拍 self-test 前变化时，才通过 `CommitProgressWithLDSTBlock` 保守跳过
- `iew.futurePreviewSkipReasons`，按 `ActiveDispatch`、`MultipleActive` 细分 `taskRuntime.futureWavefrontSkipReasons::IEWPreview`。这个 vector 只在 owner-side skip merge 或 owner-side probe 失败路径更新，不允许 worker 直接改 stats；其 total 应与 runtime 的 `IEWPreview` 闭合
- `iew.futureActiveDispatchSources`，按 `ExistingFixedBuffer`、`RenameInput`、`Mixed`、`Unknown` 细分阻塞 IEW preview 的 active dispatch 输入来源；`iew.futureActiveDispatchModes` 按 `DirectIssue`、`DispatchQueue` 记录 active dispatch 采用的后端路径。两者只在 `ActiveDispatch` preview skip 时更新，total 应与 `iew.futurePreviewSkipReasons::ActiveDispatch` 闭合
- `iew.futureActiveDispatchPreviewOutcomes`，记录 active dispatch 在 IEW latch preview 边界上的结果：`Skipped` 表示仍需 owner-side dispatch 后才能知道最终 `IEWToRename`；`DrainedNoResource` 表示命中了“direct dispatch 只弹出已有 squashed fixedbuffer、无 Rename 新输入”的零资源安全子集；`DrainedWithResources` 表示 owner-side resource-token dry-run 证明 direct-dispatch 会 drain，可以复用 predicted latch；`BlockedWithResources` 表示 resource-token dry-run 证明 direct-dispatch 会 blocked，可以发布 `IEWToRename.block=true`，不发布任何 IQ/LSQ/scoreboard/fixedbuffer side effect。当前接受 direct-dispatch `SchedulerNotReady` blocked 和 first-inst `SerializeBlocked` blocked；LQ/SQ full、不支持路径或输入不完整仍计入 `Skipped`。`iew.futureActiveDispatchInsts` 记录这些 active dispatch preview 的可见指令数分布
- `iew.futureActiveDispatchPreviewBlockReasons`，按 `BuildInputFailed`、`InvalidPreview`、`UnsupportedTokens`、`SerializeBlocked`、`LQFull`、`SQFull`、`SchedulerNotReady` 细分 `Skipped` active dispatch preview 的原因。这个 vector 只记录未发布的 blocked/invalid 子集，不改变 IEW latch preview 行为；其 total 应与 `futureActiveDispatchPreviewOutcomes::Skipped` 闭合
- `iew.futureActiveDispatchSchedulerBlockReasons`，继续把被跳过的 `SchedulerNotReady` 细分为 `InvalidState`、`InvalidOp`、`InvalidDispSeq`、`InvalidSelector`、`ReplayBlocked`、`IQFull`、`InportFull` 等 scheduler token 原因。direct-dispatch `SchedulerNotReady` 当前已由修正后的 lookahead token 模型接受，因此这个 vector 主要用于未来不支持/跳过路径
- `iew.futureDispatchPreviewChecks`, `iew.futureDispatchPreviewMatches`, `iew.futureDispatchPreviewDifferences`, `iew.futureDispatchPreviewDifferenceReasons`, `iew.futureDispatchPreviewStale`，用于把上一拍保存的 future direct-dispatch dry-run 与下一拍真实 IEW dispatch 前的 current-cycle dry-run 对拍。这个自检只比较 resource/control 形状，不比较尚未建模的 ROB head stall reason；difference reason 细分 `ActualMissing`、`Tid`、`VisibleInsts`、`DispatchedBeforeBlock`、`Drained`、`BlockReason`、`SchedulerBlockReason` 等字段，用来判断 future IQ/inport token snapshot 是否足以发布 output latch
- `iew.futureDispatchPreviewDispatchedBeforeBlockDiffDirections` 和 `iew.futureDispatchPreviewDispatchedBeforeBlockDelta`，继续细分 `DispatchedBeforeBlock` 差异的方向和绝对幅度。它们只在对拍自检发现阻塞前 dispatch 数量不同且下一拍真实 preview 存在时更新，不改变 future latch 发布策略
- `iew.futureDispatchPreviewDrainedDiffDirections`，独立细分 future/current dispatch preview 的 drained/block 方向差异。这个 vector 不改变 `futureDispatchPreviewDifferenceReasons` 的第一个差异字段语义，只补足被 `DispatchedBeforeBlock` 优先命中的 case：`FutureBlockedActualDrained` 表示 future 预测阻塞但下一拍真实 dry-run 已经 drain，`FutureDrainedActualBlocked` 表示相反方向
- `iew.dispatchOutputSnapshotChecks/Matches/Mismatches/MismatchFields`，用于验证 current-cycle direct-dispatch dry-run 是否准确预测真实 owner dispatch 的 output side effect 数量：`FixedBufferPops`、`SquashedPops`、`IQInserts`、`LQInserts`、`SQInserts`、`NonSpecInserts`、`BarrierInserts`、`ProducerAdds`。这个 snapshot 仍只做自检，真实 fixedbuffer/IQ/LSQ/scheduler 写入只发生在 owner dispatch 路径
- `iew.futureDispatchOutputSnapshotChecks/Matches/Differences/DifferenceFields`，用于把上一拍 future output snapshot 与下一拍 current-cycle dry-run output snapshot 对拍。它不改变 `futureDispatchPreviewMatches` 的 resource/control 统计，也不放宽 latch 发布策略；其字段分布用于决定 dispatch output snapshot 下一步要精确建模哪些 side effect
- `iew.futureDispatchOutputPublishability`、`iew.futureDispatchOutputStableBlockedReasons` 和 `iew.futureDispatchOutputStableBlockedPops`，继续把 future output snapshot 对拍结果按可发布候选分类：actual 缺失、resource/control 不同、output 不同、稳定 drained、稳定 blocked 无 side effect、稳定 blocked 有 side effect。这个分类只在下一拍自检时更新，不改变当前 conservative 发布策略；它用于量化后续真正发布带 side effect blocked latch 的收益面和 block reason 组成
- `iew.futureDispatchBlockTokenChecks/Matches/Differences/DifferenceFields` 以及按 publishability 交叉的 match/difference vector，继续把 `SchedulerNotReady` 阻塞点展开到目标 IssueQue token：valid、scheduler reason、目标 IQ index、selector、opClass、dispSeq、freeEntries、freeInports、replayBlocked。IEW 侧只保存 primitive snapshot，不保存 `IssueQue*` 或 `DynInstPtr`；该统计只做下一拍自检和归因，不发布 future latch，不提前修改 IQ/inport token
- `iew.futureRenameLatchPreviewChecks/Matches/Differences/DifferenceReasons/Stale`，用于把上一拍构造的 candidate `IEWToRename` latch 与下一拍真实 IEW dispatch 后的 stall latch 对拍。candidate latch 即使在 `SchedulerNotReady` 等当前仍跳过的 future preview 中也会被保存，但只用于自检，不参与 wavefront 发布；difference reason 区分 block bit 和 reason 字段，帮助判断是 latch 本身不稳定，还是只剩后端 output side effect 没有建模
- `iew.dispatchDrainPreviewProbes`, `iew.dispatchDrainPreviewSkipped`, `iew.dispatchDrainPreviewSkipReasons`, `iew.dispatchDrainPreviewMatches`, `iew.dispatchDrainPreviewMismatches`，当前观测 current-cycle direct-dispatch 在真实 dispatch 之后是否会 drain `fixedbuffer`。direct path 在 `Scheduler::lookahead()` 之后冻结 IQ entry/inport/replay token、`disp_seq` 映射、每线程 LQ/SQ 余量和 serialize 状态，并用 owner-side dry-run 私有消费 token；真实 dispatch、IQ/LSQ insert、scoreboard 和 fixedbuffer pop 仍由旧 owner 路径执行。future consumer wavefront 复用同一套 token 语义，但在 `buildLookaheadDispatchTokenState()` 中复刻真实 lookahead 的 selector counter reset 和共享 counter 映射，使用未来拍重置后的 inport token，以及 owner 侧构造的不持有 `DynInstPtr` 的 `FutureDispatchPreviewInput`。dispatch queue 或不支持的 scheduler path 会跳过；`SplitStore` skip reason 保留为旧统计槽位，当前 split-store 已通过双 IQ token dry-run 覆盖，期望为 0
- `iew.dispatchDrainPreviewStallReasonMatches`, `iew.dispatchDrainPreviewStallReasonMismatches`, `iew.dispatchDrainPreviewStallReasonSideEffectSkips`，当前观测 current-cycle direct-dispatch 在预测 blocked 且 drain 结论正确时，是否还能预测 dispatch 后发布给 Rename 的 `blockReason`。只有 dry-run 在遇到阻塞前没有任何真实 dispatch side effect 时才比较 stall reason；如果阻塞前已经会弹出/dispatch 指令，则真实路径可能已经改变 IQ readyTick、LSQ/scoreboard 或 ROB head 归因，计入 side-effect skip，不把它误当作可发布的 future blocked latch
- `iew.dispatchStatusPrepareTasks`, `iew.dispatchStatusPrepareMerges`, `iew.dispatchStatusPrepareMismatches`，当前用于确认 IEW dispatch 后写回 Rename 的 ROB/LQ/SQ head stall reason 已经能从冻结 head snapshot 中计算，并和旧 helper 结果一致
- `iew.writebackPrepareTasks`, `iew.writebackPrepareMerges`, `iew.writebackPrepareNoWork`, `iew.writebackPrepareMismatches`，当前用于确认 IEW writeback 的 bounded slot 控制分类已经拆成只读 prepare；真实 wakeup、scoreboard 和 probe 仍在 owner stage
- `lsq.sbufferOffloadPrepareTasks`, `lsq.sbufferOffloadPrepareMerges`, `lsq.sbufferOffloadPrepareNoDemand`, `lsq.sbufferOffloadPrepareMismatches`, `lsq.sbufferOffloadPrepareGranted`，当前用于确认 LSQ store-buffer offload round-robin quota 已经拆成只读 prepare；真实 SQ entry offload、store buffer 修改和 cache packet 发送仍在 owner stage
- `rename.prepareTasks`, `rename.prepareMerges`, `rename.prepareActiveThreads`, `rename.prepareBlockedThreads`, `rename.prepareInactiveThreads`；其中 `fixedbuffer` 为空的 inactive 线程在 owner 侧调用同一个 per-thread helper，保留 RegFull 统计但避免提交无效 worker task
- `rename.futurePrepareProbes`, `rename.futurePrepareSkipped`, `rename.futurePrepareMerges`, `rename.futurePrepareReuses`, `rename.futurePrepareChecks`, `rename.futurePrepareMatches`, `rename.futurePrepareMismatches`, `rename.futurePrepareStale`，当前用于确认 `Commit[c+1] -> IEW[c+1] -> Rename[c+1]` wavefront 中的 Rename prepare 结果是否被下一拍 owner 复用，以及 self-test 下是否与当前冻结输入重算结果一致；reuse 只替代 Rename prepare 控制计算，不提前 active rename、释放物理寄存器或写 rename map/free list/history
- `rename.futurePrepareMismatchReasons`，按 `Cycle`、`SelectedTid`、`BlockedTid`、`ActiveThreads`、`BlockedThreads`、`RegFullEvents`、`MultipleActive` 和 per-thread input/output 字段细分 future Rename prepare self-test mismatch。这个 vector 只在 mismatch 时归因，不改变 owner merge 行为；最近一次 broad blocked-latch 诊断显示 4362 次 mismatch 主要来自 `SelectedTid=4360`，用于证明 `SchedulerNotReady` blocked latch 仍需要完整 dispatch-output snapshot
- `rename.futureCandidatePrepareChecks/Matches/Mismatches/MismatchReasons/Stale`，用于诊断被 IEWPreview 保守跳过的 `SchedulerNotReady` candidate latch 如果继续驱动 Rename prepare，会不会改变下一拍真实 Rename prepare。candidate result 不进入 `pendingFuturePrepare`，不复用、不发布 `RenameToDecode` latch，也不写 rename map/free list/history/fixedbuffer；它只在下一拍 `runRenamePrepare()` 用当前真实输入重算 expected 并按既有 mismatch reason 对拍。`rename.futureCandidatePrepareMatchesBySchedulerReason/MismatchesBySchedulerReason` 和 `rename.futureCandidatePrepareMatchesByExpectedPops/MismatchesByExpectedPops` 继续按 IEW future-side 可见 profile 拆分 candidate 结果，判断粗 scheduler reason 或 expected pop 数是否足以定义可发布谓词
- `rename.futureCandidateInputChecks/Matches/Differences/DifferenceFields` 以及 `futureCandidateInputMatchDifferenceFields` / `futureCandidateInputMismatchDifferenceFields`，用于把上述 candidate prepare 继续拆到 Rename prepare 的真实输入字段。它比较 candidate `RenamePrepareInput` 和下一拍 current-cycle `RenamePrepareInput`，字段包括线程数、Rename fixedbuffer empty/size、物理寄存器需求/可用数、`IEWToRename` block/reason，以及 IEW 送来的 ROB/LQ/SQ head stall reason。该统计只做归因，不参与 pending reuse，不发布任何 latch
- `rename.futureCandidatePrepareInputStability`，把 candidate prepare 结果的 match/mismatch 与 candidate/current `RenamePrepareInput` 的 match/diff 交叉成四个桶。它用于验证 Rename prepare 是否已经被 `RenamePrepareInput` 完整封装：如果出现 `PrepareMismatchInputMatch`，说明 prepare 还读了未显式化状态；如果该桶为 0，则后续可把可发布谓词集中到 input 等价性证明上
- `rename.futureCandidateIEWBlockDiffDirections`，把 candidate/current `IEWToRename.block` 输入差异按方向和 prepare match/mismatch 交叉。它用于区分 future preview 是漏阻塞还是过度阻塞：`CandidateFalseActualTrue` 表示 candidate 少阻塞，`CandidateTrueActualFalse` 表示 candidate 多阻塞。这个 vector 只在 input 已经不同的诊断路径更新，不改变任何 latch 发布策略
- `rename.futureInputSkipReasons`，按 `MissingSnapshot`、`NoActiveThreads`、`CommitControl`、`ReleaseSeqNotReady` 细分 future Rename input 构造为什么不安全。`ReleaseSeqNotReady` 现在保留为历史/回归观测桶；当前实现会用只读 release-token projection 覆盖 pending release backlog，而不是直接因为 `releaseSeq != finalCommitSeq` 跳过
- `rename.futureInputCommitControlReasons`，按 `Squash`、`RobSquashing`、`DoneSeqNum` 继续细分 `CommitControl`。当前 `doneSeqNum` 仍是 Rename future input 的强序边界，因为它会改变 `finalCommitSeq/releaseSeq` 语义，不和 backlog release-token projection 混在一起放宽
- `rename.futurePreviewSkipReasons`，按 `ActiveRename`、`MultipleActive` 细分 future `RenameToDecode` latch preview 为什么不能安全发布。这个 vector 只在 owner-side merge/skip 路径更新，不让 worker 改 stats；其 total 应与 runtime `RenamePreview` 闭合，用于区分“active rename side effect 尚未建模”和“多个 active thread 仲裁不稳定”这两类不同边界
- `rename.futureInputVirtualReleaseSteps` 和 `rename.futureInputVirtualReleaseRegs` 记录 owner 侧只读 release-token projection 的触发次数和虚拟增加的 phys-reg token 数；这些 stats 只解释 future input 为什么可以继续构造，不代表真实 free list 或 historyBuffer 被 worker 修改
- `decode.prepareTasks`, `decode.prepareMerges`, `decode.prepareActiveThreads`, `decode.prepareBlockedThreads`, `decode.prepareInactiveThreads`；其中 inactive 线程在 owner 侧 inline 填入默认 no-stall result，避免提交无效 worker task
- `decode.futurePrepareProbes`, `decode.futurePrepareSkipped`, `decode.futurePrepareMerges`, `decode.futurePrepareReuses`, `decode.futurePrepareChecks`, `decode.futurePrepareMatches`, `decode.futurePrepareMismatches`, `decode.futurePrepareStale`，当前用于确认 `Commit[c+1] -> IEW[c+1] -> Rename[c+1] -> Decode[c+1]` wavefront 中的 Decode prepare 结果是否被下一拍 owner 复用，以及 self-test 下是否与当前冻结输入重算结果一致；reuse 只替代 Decode prepare 控制计算，不提前 active decode、不移动 stallbuffer/fixedbuffer、不写 `toRename` 或修改 `DynInst`
- `decode.futurePreviewSkipReasons`，按 `ActiveDecode`、`MultipleActive` 细分 future `DecodeToFetch` latch preview 为什么不能安全发布。这个 vector 只在 owner-side merge/skip 路径更新，不让 worker 改 stats；其 total 应与 runtime `DecodePreview` 闭合，用于区分“active decode/stallbuffer side effect 尚未建模”和“多个 active thread 仲裁不稳定”这两类不同边界
- `fetch.resolvePrepareTasks`, `fetch.resolvePrepareMerges`, `fetch.resolvePrepareNoIncoming`, `fetch.resolvePrepareQueueFull`，当前用于确认 Fetch resolved-CFI incoming 合并已经拆成只读 prepare，并且空 incoming 周期会在 owner 侧 inline 避免无效 worker task；真实 `resolveQueue` 入队仍在 owner stage，SMT key 为 `(tid, ftqId)`
- `fetch.resolveDequeuePrepareTasks`, `fetch.resolveDequeuePrepareMerges`, `fetch.resolveDequeuePrepareNoWork`, `fetch.resolveDequeuePrepareMismatches`, `fetch.resolveDequeuePrepareCFIs`，当前用于确认 Fetch old-front resolved-CFI dequeue 控制已经拆成只读 prepare；真实 BPU/FTQ resolve update 和 `resolveQueue` pop 仍在 owner stage，并使用 queue entry 携带的 tid
- `fetch.targetPrepareTasks`, `fetch.targetPrepareMerges`, `fetch.targetPrepareNoTarget`, `fetch.targetPrepareMismatches`，当前用于确认 Fetch target-select prepare 是否只读选择了和真实 `getTargetTid()` 一致的 tid，并且 no-target 周期会在 owner 侧 inline 避免无效 worker task；真实 FTQ round-robin 推进仍在 owner stage
- `fetch.prepareTasks`, `fetch.prepareMerges`, `fetch.prepareFrontendReady`, `fetch.prepareReadyToFetch`, `fetch.prepareInterruptBlocked`
- `fetch.toDecodePrepareTasks`, `fetch.toDecodePrepareMerges`, `fetch.toDecodePrepareAllBlocked`，当前用于确认 Fetch 末尾送 Decode 的控制计算已从 owner 写队列逻辑中拆成只读 prepare，并且 all-blocked 周期会在 owner 侧 inline 避免无效 worker task；真实 `DynInstPtr` pop、`toDecode` 写入和 activity 更新仍在 owner stage
- `fetch.futureToDecodePrepareProbes`, `fetch.futureToDecodePrepareSkipped`, `fetch.futureToDecodePrepareMerges`, `fetch.futureToDecodePrepareReuses`, `fetch.futureToDecodePrepareChecks`, `fetch.futureToDecodePrepareMatches`, `fetch.futureToDecodePrepareMismatches`, `fetch.futureToDecodePrepareStale`，当前用于确认 `Commit[c+1] -> IEW[c+1] -> Rename[c+1] -> Decode[c+1] -> Fetch[c+1]` wavefront 中的 Fetch-to-Decode prepare result 是否被下一拍 owner 复用，以及 self-test 下是否与当前冻结输入重算结果一致；reuse 只替代送 Decode 的控制计算，不提前 BPU/FTQ/cache/decoder，也不让 worker pop `fetchQueue` 或写 `toDecode`
- `fetch.futureInputSkipReasons`，按 `MissingSnapshot`、`NoActiveThreads`、`CommitControl`、`DecodeControl`、`AllBlockedNoTid`、`FetchQueueNotReady`、`MissingInst` 细分 future Fetch-to-Decode input 构造为什么不安全。这个 vector 只在 owner-side input 构造失败路径更新，不让 worker 改 stats；其 total 应与 runtime `FetchInput` 闭合。`FetchQueueNotReady` 表示当前 owner 侧 `fetchQueue` 中不足一拍 `decodeWidth` 的可见指令，不能直接按部分宽度发布，因为下一拍真实 Fetch 在送 Decode 前还会先跑前端并可能追加新指令
- `fetch.futureInputQueueNotReadyOutcomes`，把上述 `FetchQueueNotReady` candidate 延迟到下一拍真实 owner `runFetchToDecodePrepare()` 时归因：`NoSupplyStillNotReady` 表示下一拍前端仍没有补足一拍 `decodeWidth`，`PartialSupply` 表示补了一部分但仍不满，`FilledToWidth` 表示已经补满，`Blocked`/`QueueShrank`/`Stale` 分别覆盖阻塞、队列被 squash/清空或 candidate 过期。`futureInputQueueNotReadyCandidateInsts` 和 `futureInputQueueNotReadyActualInsts` 分别累加 candidate 时和下一拍真实 prepare 时的队列可见条数。这些统计只做 next-cycle hindsight 归因，不驱动 future publish；它们用于寻找后续可提前证明 no-supply 的谓词
- debug flag：`TaskGraph`, `TaskSched`, `TaskRace`

后续进入真实 wavefront 调度后，还需要继续补充 `event.samePriorityOrderedDeps` 等更细粒度人工归因观测。

最近一次验证中，`--event-priority-audit` 的 1000 inst 2-worker coremark raw-cpt 短跑输出 `event.samePriorityGroups=2165`、`event.samePriorityInsertedEvents=10601`、`event.samePriorityMaxDepth=20`；priority-class 分桶中 `EarlyPriority/DelayedWriteback/Default/Other` 的 groups 分别为 `179/576/743/667`，inserted-events 分别为 `364/1166/1630/7441`，class max depth 分别为 `2/2/3/20`。与早期结果相比，L2 wrapper/L3 worker prefetcher 的每拍空转事件已经被改成 event-driven 调度，Default-priority same-bin 压力从 `28520` 组降到 `743` 组。细化后的 `eventHorizonBlockerTypes` 在同一短跑中与 `eventHorizonBlockedSamples=27739` 闭合：`MemoryController=26469`、`L2Slice=665`、`Interconnect=372`、`Other=73`、`Device=62`、`L3MemSidePort=54`、`CPU=27`、`L3Cache=16`、`L2WrapperPrefetcher=1`、`L3Prefetcher=0`。拆分后 `eventHorizonEarlierTickBlockerTypes::total=26207` 与 `eventHorizonEarlierTickBlocks` 闭合，其中 `MemoryController=26097`；`eventHorizonCpuPriorityBlockerTypes::total=1532` 与 `eventHorizonCpuPriorityBlocks` 闭合，其中 `L2Slice=665`、`MemoryController=372`、`Interconnect=372`、`L3MemSidePort=54`、`CPU=27`、`Other=25`、`L3Cache=16`、`L2WrapperPrefetcher=1`。关闭 audit 的同口径短跑这三组 blocker type 全 0，且两边 `simTicks=9236754`、`simInsts=1013` 和关键 CPU stats 保持一致。Commit->IEW->Rename->Decode->Fetch wavefront 输入私有化并标成 `CrossTimeBufferAdvance` 后，1000 inst 2-worker self-test 在 `simTicks=9236754`、`simInsts=1013` 正常退出，必须 pre-advance drain 的 `timeBufferAdvanceWaits/PendingTasks/MaxPendingTasks` 均为 0，同时 `timeBufferAdvanceSafeDeferrals=11635`、`timeBufferAdvanceSafeDeferredTasks=51720`、`timeBufferAdvanceMaxSafeDeferredTasks=6`，`serialTickEndSafeDeferrals=11455`、`serialTickEndSafeDeferredTasks=51019`、`serialTickEndMaxSafeDeferredTasks=6`，`futureFetchWavefrontPrepareMatches=2987`。1M no-audit 2-worker self-test 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，必须 pre-advance drain 的 `timeBufferAdvanceWaits/PendingTasks/MaxPendingTasks` 均为 0，同时 `timeBufferAdvanceSafeDeferrals=195069`、`timeBufferAdvanceSafeDeferredTasks=453495`、`timeBufferAdvanceMaxSafeDeferredTasks=6`，`serialTickEndSafeDeferrals=75607`、`serialTickEndSafeDeferredTasks=191852`、`serialTickEndMaxSafeDeferredTasks=6`，`futureFetchWavefrontPrepareMatches=4742`。1M 关键 CPU stats 与默认串行基线一致，Decode future prepare `24653/24653` checks matched，Fetch future-to-decode prepare `4742/4742` checks matched，dispatch/writeback/LSQ 自检 mismatch 均为 0。

去掉 tick begin 的全量收回、改成 stage-local future prepare barrier 后，1000 inst 2-worker self-test `/tmp/stage_selective_begin_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`stageBarrierDeferredTasks=364611`、`stageBarrierMaxDeferredTasks=4`，`serialTickEndSafeDeferrals=11455`，future fetch wavefront `2987/2987` matched。1M no-audit 2-worker self-test `/tmp/stage_selective_begin_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`stageBarrierDeferredTasks=3520267`、`stageBarrierMaxDeferredTasks=4`，`serialTickEndSafeDeferrals=75607`，future fetch wavefront `4742/4742` matched；`commit.branches=230870`、`decode.decodedInsts=1325610`、`decode.squashedInsts=179712`、`fetch.fetchBubbles=920616`、`fetch.fetchBubbles_max=69540`、`fetch.resolveDequeueCount=192931`、`iew.dispatchedInsts=1248952`、`iew.instsToCommit=996158`、`iew.writebackCount=996158`、`rename.renamedInsts=1249784`、`rename.stallEvents::RegFull=2399` 与默认串行基线一致，future/self-test mismatch 和 stale 只出现已知 0 值。

新增 future wavefront skip reason vector 后，1000 inst 2-worker self-test `/tmp/future_skip_reason_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`futureWavefrontSkipReasons::total=10488`，与四段 future wavefront skipped 总和 `396+592+669+8831=10488` 闭合。1M 2-worker self-test `/tmp/future_skip_reason_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`futureWavefrontSkipReasons::total=748277`，与四段 skipped 总和 `164621+180937+191404+211315=748277` 闭合；主要跳过点为 `IEWInput=406360`、`IEWPreview=168172`、`CommitPreview=83952`、`RenameInput=25488`、`RenamePreview=23460`、`DecodePreview=20934`、`FetchInput=19911`。同一 1M runtime 与默认串行基线在上述关键 CPU stats 上保持一致，future/self-test mismatch 和 stale 均为 0。

继续给 IEW future input 增加内部归因后，1000 inst 2-worker self-test `/tmp/iew_input_reason_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`iew.futureInputSkipReasons::total=320`，与 `taskRuntime.futureWavefrontSkipReasons::IEWInput=320` 闭合，且全部来自 `CommitControl`。1M 2-worker self-test `/tmp/iew_input_reason_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`iew.futureInputSkipReasons::total=406360`，与 runtime `IEWInput=406360` 闭合，`MissingSnapshot=0`、`NoActiveThreads=0`、`CommitControl=406360`。进一步细分 commit control 后，1000 inst 2-worker self-test `/tmp/iew_commit_control_reason_1k_t2` 中 `iew.futureInputCommitControlReasons::total=320`，分布为 `DoneSeqNum=164`、`DoneMemSeqNum=96`、`NonSpecSeqNum=60`；1M 2-worker self-test `/tmp/iew_commit_control_reason_1m_t2` 中 `iew.futureInputCommitControlReasons::total=406360`，分布为 `DoneSeqNum=284440`、`DoneMemSeqNum=121848`、`NonSpecSeqNum=72`，`Squash/RobSquashing/StrictlyOrdered` 均为 0。同一 1M runtime 与默认串行基线在上述关键 CPU stats 上保持一致，future/self-test mismatch 和 stale 均为 0。

在此基础上放宽 `doneSeqNum/doneMemSeqNum` 后，直接接受所有 commit progress 的 1M 诊断跑曾出现 2 次 `iew.futurePrepareMismatches`，差异只发生在 `!ldstCanInsert` 时的 `renameBlockReason`，说明 LDST blocked reason 仍不是稳定输入。最终实现只在没有 LDST block 时接受 done progress，并新增 `CommitProgressWithLDSTBlock` 保守跳过。1000 inst 2-worker self-test `/tmp/iew_allow_done_ldstgate_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`IEWInput=60`，`futureInputAllowedCommitProgress::total=424`，mismatch/stale 均为 0。1M 2-worker self-test `/tmp/iew_allow_done_ldstgate_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`IEWInput=1000`，其中 `CommitControl=72`、`CommitProgressWithLDSTBlock=928`，`futureInputAllowedCommitProgress::DoneSeqNum=284420`、`DoneMemSeqNum=405360`；同一 1M runtime 与默认串行基线在上述关键 CPU stats 上保持一致，IEW future/self-test mismatch 和 stale 均为 0。

继续给 IEW preview 增加 owner-side skip 归因后，1000 inst 2-worker self-test `/tmp/iew_preview_reason_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`iew.futurePreviewSkipReasons::total=580`，与 runtime `IEWPreview=580` 闭合，全部来自 `ActiveDispatch`。1M 2-worker self-test `/tmp/iew_preview_reason_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`iew.futurePreviewSkipReasons::total=527540`，与 runtime `IEWPreview=527540` 闭合，`ActiveDispatch=527540`、`MultipleActive=0`；同一 1M runtime 与默认串行基线在上述关键 CPU stats 上保持一致，IEW future/self-test mismatch 和 stale 均为 0。这说明当前最大的 IEWPreview 边界不是多线程仲裁，而是 active dispatch 会写 IQ/LSQ/scoreboard/fixedbuffer，需要后续单独设计 dispatch output snapshot 或更细粒度 IEW subphase。

继续细分 active dispatch 形态后，1000 inst 2-worker self-test `/tmp/iew_active_dispatch_shape_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`futureActiveDispatchSources::total=580` 与 `ActiveDispatch=580` 闭合，其中 `RenameInput=472`、`ExistingFixedBuffer=108`、`Mixed/Unknown=0`，`futureActiveDispatchModes::DirectIssue=580`、`DispatchQueue=0`。1M 2-worker self-test `/tmp/iew_active_dispatch_shape_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`futureActiveDispatchSources::total=527540` 与 `ActiveDispatch=527540` 闭合，其中 `RenameInput=314172`、`ExistingFixedBuffer=213368`、`Mixed/Unknown=0`，`futureActiveDispatchModes::DirectIssue=527540`、`DispatchQueue=0`；同一 1M runtime 与默认串行基线在上述关键 CPU stats 上保持一致，IEW future/self-test mismatch 和 stale 均为 0。因此后续应优先设计 direct-dispatch output snapshot，尤其是从 future `Rename->IEW` 输入直接进入 dispatch 的子集，而不是先处理 dispatch queue 模式或多 active thread 仲裁。

继续把 `Commit[c+1] -> IEW[c+1]` 第一段从 latch preview 拆成 prepare-only reuse 后，active dispatch 不再阻止 IEW prepare 自身提前计算，只有后续 Rename/Decode/Fetch consumer wavefront 仍要求 `IEWToRename` latch 可预测。1000 inst 2-worker self-test `/tmp/iew_prepare_active_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`iew.futurePrepareReuses=11620`，相比拆分前 `/tmp/iew_active_squash_1k_t2` 的 `11475` 增加 145；`iew.futurePreviewSkipReasons::ActiveDispatch=435`，相比 580 只剩 consumer preview 的跳过。1M 2-worker self-test `/tmp/iew_prepare_active_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`iew.futurePrepareReuses=194819`，相比拆分前的 `62934` 明显增加；`iew.futurePreviewSkipReasons::ActiveDispatch=395655`，相比 527540 只统计 consumer latch preview 边界。新增 `futureActiveDispatchPreviewOutcomes::DrainedNoResource=0`，说明这条 coremark profile 没有命中 squashed-only latch preview 子集；当前收益主要来自 prepare-only 拆分，而不是 active dispatch output 建模。1M runtime 与默认串行 `/tmp/workerpf_cmp_default_1m/stats.txt` 在 `commit.branches=230870`、`decode.decodedInsts=1325610`、`decode.squashedInsts=179712`、`fetch.fetchBubbles=920616`、`fetch.fetchBubbles_max=69540`、`fetch.resolveDequeueCount=192931`、`iew.dispatchedInsts=1248952`、`iew.instsToCommit=996158`、`iew.writebackCount=996158`、`rename.renamedInsts=1249784`、`rename.stallEvents::RegFull=2399` 上保持一致，future/self-test mismatch 和 stale 均为 0。

继续把 active dispatch output 向 consumer wavefront 放宽时，current-cycle direct-dispatch drain preview 已从“只接受全 squashed fixedbuffer”推进到显式 resource-token dry-run。preview 在 direct path 的 `Scheduler::lookahead()` 之后、真实 dispatch 副作用之前运行，冻结每个目标 IssueQue 的 entry/inport 余量、replay 阈值、`disp_seq -> issue queue` 映射、每线程 LQ/SQ 余量和 serialize/commit input 条件；随后按真实 dispatch 顺序私有消费 token。split-store address 使用事务性双 token 检查，同时扣 StoreDataOp 和 store-address IQ token；nop/eliminated、barrier、atomic/non-spec 指令仍按真实路径先检查 IQ ready，但不消费 IQ entry。1000 inst 2-worker self-test `/tmp/dispatch_token_dryrun_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`dispatchDrainPreviewProbes=473`、`dispatchDrainPreviewSkipped=0`、`dispatchDrainPreviewMatches=473`、`dispatchDrainPreviewMismatches=0`。1M 2-worker self-test `/tmp/dispatch_token_dryrun_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`dispatchDrainPreviewProbes=308727`、`dispatchDrainPreviewSkipped=0`、`dispatchDrainPreviewMatches=308727`、`dispatchDrainPreviewMismatches=0`，`iew.futurePrepareMismatches=0`、`dispatchStatusPrepareMismatches=0`、`writebackPrepareMismatches=0`、`sbufferOffloadPrepareMismatches=0`。

同一版 current-cycle 代码还跑了串行 1M `/tmp/dispatch_token_dryrun_1m_serial`，与 2-worker self-test 在关键统计上完全一致：`simTicks=164676159`、`simInsts=1000009`、`system.cpu.numCycles=494524`、`system.cpu.cpi=0.494520`、`system.cpu.ipc=2.022165`、`iew.dispatchedInsts=1248952`、`iew.dispLoadInsts=204797`、`iew.dispStoreInsts=60138`、`rename.renamedInsts=1249784`。这证明 current-cycle token dry-run 只是自检观测，不改变架构状态或时序统计。

随后把 token dry-run 接到 future consumer latch preview，但只接受“预测会 drain”的 active direct-dispatch；预测 blocked 的结果继续 skip，因为一次诊断跑 `/tmp/future_dispatch_token_1k_t2` 出现 `rename.futurePrepareMismatches=4` 且 `simTicks` 变化，说明 blocked latch 还不能作为稳定输出发布。最终 1000 inst 2-worker self-test `/tmp/future_dispatch_token_drain_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`futureActiveDispatchPreviewOutcomes::DrainedWithResources=342`、`Skipped=93`、future/self-test mismatch 为 0。1M 2-worker self-test `/tmp/future_dispatch_token_drain_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`DrainedWithResources=209409`、`Skipped=186246`、`iew.futurePreviewSkipReasons::ActiveDispatch=186246`，相比 prepare-only 阶段的 `395655` 明显减少；`iew.futurePrepareMismatches=0`、`dispatchDrainPreviewMismatches=0`、`dispatchStatusPrepareMismatches=0`、`writebackPrepareMismatches=0`、`sbufferOffloadPrepareMismatches=0`。同一版串行 1M `/tmp/future_dispatch_token_serial_1m` 与 2-worker run 在 `simTicks=164676159`、`simInsts=1000009`、`system.cpu.numCycles=494524`、`system.cpu.ipc=2.022165`、`iew.dispatchedInsts=1248952`、`iew.dispLoadInsts=204797`、`iew.dispStoreInsts=60138`、`rename.renamedInsts=1249784` 上完全一致。

继续给 Rename future input 增加内部归因和只读 release-token projection 后，1000 inst 2-worker self-test `/tmp/rename_future_release_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`rename.futureInputSkipReasons::ReleaseSeqNotReady=0`，`futureInputVirtualReleaseSteps=99`、`futureInputVirtualReleaseRegs=189`，future/self-test mismatch 均为 0。1M 2-worker self-test `/tmp/rename_future_release_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，和上一轮 `/tmp/rename_future_input_reason_1m_t2` 的体系结构进度一致；`ReleaseSeqNotReady` 从 `141477` 降到 `0`，`futureInputVirtualReleaseSteps=37314`、`futureInputVirtualReleaseRegs=132672`，`taskRuntime.futureWavefrontSkipReasons::RenameInput` 从 `186453` 降到 `130014`，`rename.futurePrepareReuses/Matches` 从 `40190` 提升到 `48910`；剩余 `RenameInput` 全部来自 `CommitControl::DoneSeqNum=130014`。同一 1M run 中 `iew.futurePrepareMismatches=0`、`dispatchDrainPreviewMismatches=0`、`dispatchStatusPrepareMismatches=0`、`writebackPrepareMismatches=0`、`sbufferOffloadPrepareMismatches=0`，说明 release-token projection 只扩大只读 prepare 复用范围，没有改变真实 Rename side effect。

继续给 IEW active-dispatch preview 的 skipped 子集增加 block reason 归因后，1000 inst 2-worker self-test `/tmp/iew_active_block_reason_final_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`futureActiveDispatchPreviewBlockReasons::total=93` 与 `Skipped=93` 闭合，其中 `SchedulerNotReady=66`、`SerializeBlocked=27`。1M 2-worker self-test `/tmp/iew_active_block_reason_final_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`futureActiveDispatchPreviewBlockReasons::total=186246` 与 `Skipped=186246` 闭合，其中 `SchedulerNotReady=186213`、`SerializeBlocked=33`，其它原因均为 0；所有 future/self-test mismatch 仍为 0。一次诊断性放宽只接受 `SchedulerNotReady` predicted-blocked latch 的 1000 inst run `/tmp/iew_scheduler_block_accept_1k_t2` 出现 `simTicks=9475182` 和 `rename.futurePrepareMismatches=4`，因此当时判断该子集不能直接发布；后续 lookahead counter 修正后，这个阶段性结论已被更新。

继续给 current-cycle direct-dispatch dry-run 增加 blocked stall reason 诊断后，先前直接比较所有 blocked case 的 1000 inst run 暴露 6 次原因不一致：dry-run 在 dispatch 副作用前看到 ROB head 仍是 `InstNotReady`，真实 owner dispatch 弹出/插入前序指令后已经把同一阻塞点的可见原因推进到 `ScalarReadyButNotIssued`。最终实现把这类“阻塞前已有 dispatch side effect”的 case 计入 `dispatchDrainPreviewStallReasonSideEffectSkips`，只在无前置 side effect 的 blocked case 上验证 stall reason。1000 inst 2-worker self-test `/tmp/iew_dispatch_stall_reason_sidefx_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`dispatchDrainPreviewMismatches=0`、`dispatchDrainPreviewStallReasonMatches=23`、`dispatchDrainPreviewStallReasonMismatches=0`、`dispatchDrainPreviewStallReasonSideEffectSkips=48`。1M 2-worker self-test `/tmp/iew_dispatch_stall_reason_sidefx_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`dispatchDrainPreviewMismatches=0`、`dispatchDrainPreviewStallReasonMatches=57932`、`dispatchDrainPreviewStallReasonMismatches=0`、`dispatchDrainPreviewStallReasonSideEffectSkips=57579`，且 `iew.futurePrepareMismatches=0`、`dispatchStatusPrepareMismatches=0`、`writebackPrepareMismatches=0`、`sbufferOffloadPrepareMismatches=0`。结论是：无前置 side effect 的 blocked reason 可以 owner-side 预测；带前置 dispatch side effect 的 blocked latch 仍必须保守跳过，除非后续建模完整 dispatch output snapshot。

随后尝试把无前置 dispatch side effect 的 future blocked latch 全部发布，1000 inst `/tmp/iew_blocked_latch_safe_1k_t2` 仍在 `simTicks=9236754`、`simInsts=1013` 正常退出，`BlockedWithResources=27`，但 1M `/tmp/iew_blocked_latch_safe_1m_t2` 改变为 `simTicks=164918250`、`simInsts=1000005`，并产生 `rename.futurePrepareMismatches=4362`。新增 mismatch reason 归因后复跑 broad 1M `/tmp/rename_mismatch_reason_broad_1m_t2`，仍为 `simTicks=164918250`、`simInsts=1000005`、`BlockedWithResources=122346`、`rename.futurePrepareMismatches=4362`，其中 `SelectedTid=4360`、`ThreadDecodeBlockReason=2`。因此最终实现只接受 first-inst `SerializeBlocked` 且 `dispatchedBeforeBlock=0` 的 blocked latch，`SchedulerNotReady` 继续跳过。收紧后 1000 inst `/tmp/iew_blocked_serialize_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`DrainedWithResources=342`、`BlockedWithResources=27`、`Skipped=66`。最新 1M `/tmp/rename_mismatch_reason_safe_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，`DrainedWithResources=209409`、`BlockedWithResources=33`、`Skipped=186213`，且 `rename.futurePrepareMismatchReasons::total=0`、`iew.futurePrepareMismatches=0`、`dispatchDrainPreviewMismatches=0`、`dispatchStatusPrepareMismatches=0`、`writebackPrepareMismatches=0`、`sbufferOffloadPrepareMismatches=0`。这个子集只发布 `IEWToRename.block=true`，不提前任何后端状态修改；后续若要接受 `SchedulerNotReady`，必须建模完整 dispatch-output snapshot 和 Rename prepare 对其的消费关系。

继续把 `SchedulerNotReady` 拆成 scheduler token 原因后，1M 2-worker self-test `/tmp/future_sched_block_reason_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/rename_mismatch_reason_safe_1m_t2` 保持一致，所有 future/self-test mismatch 仍为 0。新 vector 与 `SchedulerNotReady=186213` 闭合：`IQFull=171852`、`InportFull=14361`，`ReplayBlocked/Invalid*` 均为 0。这说明当前主要剩余边界不是 dispatch selector 或 replay queue，而是 future IQ entry token 与 inport token 的可见性；后续要继续减少 `IEWPreview`，应优先建模下一拍 dispatch 前 IQ entry/inport snapshot 以及它们和 Rename prepare 的输出关系。

随后新增 future direct-dispatch preview 对拍：上一拍 future preview 保存 `DispatchDrainPreviewResult`，下一拍真实 IEW dispatch 前用 current-cycle dry-run 对比，仍不改变 latch 发布策略。1M 2-worker self-test `/tmp/future_dispatch_preview_reason_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与安全基线一致，future/self-test mismatch 仍为 0。`futureDispatchPreviewChecks=131885`，其中 `Matches=101786`、`Differences=30099`、`Stale=0`；差异几乎全部来自 `DispatchedBeforeBlock=30094`，只有 `SchedulerBlockReason=5`，`Drained/BlockReason/Tid/VisibleInsts/ActualMissing` 均为 0。这在当时说明 future token 模型不能稳定预测阻塞前已经 dispatch 的指令数；后续验证证明根因是 future lookahead selector counter 与真实 `Scheduler::lookahead()` 不等价。

进一步增加 `DispatchedBeforeBlock` 方向和幅度统计后，1M 2-worker self-test `/tmp/future_dispatch_preview_delta_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与安全基线一致，`rename.futurePrepareMismatchReasons::total=0`，`iew.futurePrepareMismatches=0`，dispatch/writeback/LSQ 相关 mismatch 均为 0。对拍总量保持 `futureDispatchPreviewChecks=131885`、`Matches=101786`、`Differences=30099`、`Stale=0`；`DispatchedBeforeBlock=30094` 中 `FutureLess=26299`、`FutureGreater=3795`，绝对差值 `mean=2.208048`、`max=8`，其中 delta 1/2 占 `36.56%/36.48%`。结论是剩余不稳定主要是 future preview 少估阻塞前真实 dispatch side effect 的数量；在没有完整 output snapshot 前，继续保守跳过带 side effect 的 blocked latch 是正确边界。

继续把 future/current dispatch preview 的 drained 方向独立拆开后，1M 2-worker self-test `/tmp/iew_dispatch_drained_dir_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致，已发布 future/self-test mismatch 仍为 0。新增 `futureDispatchPreviewDrainedDiffDirections` 与 `futureRenameLatchPreviewDifferences=14354` 闭合：`FutureBlockedActualDrained=11515`、`FutureDrainedActualBlocked=2839`。这说明 IEW candidate latch 的 block-bit 不稳定全部来自 dispatch preview 的 drained/block 状态翻转，其中更大的方向是 future 预测 blocked、下一拍真实已经 drain；Rename candidate mismatch 中的 7006 个过阻塞 case 是这个方向的 consumer-visible 子集。

继续把 direct-dispatch output side effect 做成只观测 snapshot 后，1M 2-worker self-test `/tmp/dispatch_output_snapshot_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与安全基线一致，`rename.futurePrepareMismatchReasons::total=0`，`iew.futurePrepareMismatches=0`，dispatch/writeback/LSQ 相关 mismatch 均为 0。current-cycle dry-run 与真实 owner dispatch 的 output snapshot 完全一致：`dispatchOutputSnapshotChecks=308727`、`Matches=308727`、`Mismatches=0`。future output snapshot 与下一拍 current dry-run 的对拍为 `futureDispatchOutputSnapshotChecks=131885`、`Matches=101791`、`Differences=30094`；差异字段为 `FixedBufferPops=30094`、`IQInserts=29938`、`LQInserts=21849`、`SQInserts=5855`、`ProducerAdds=29938`，`SquashedPops/NonSpecInserts/BarrierInserts=0`。这说明 future blocked latch 剩余风险已被定位到普通 dispatch side effect 数量，而不是 squashed pop、barrier 或 non-spec queue；后续若要发布带 side effect 的 blocked latch，必须先把 fixedbuffer pop 与 IQ/LQ/SQ insert 数量纳入可发布 output snapshot，并验证 Rename/Decode consumer 对这些字段的消费关系。

随后增加 future output snapshot 的 publishability 分类后，1M 2-worker self-test `/tmp/dispatch_publishability_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与安全基线一致，所有 future/self-test mismatch 仍为 0。`futureDispatchOutputPublishability::total=131885` 与 checked preview 闭合，其中 `PreviewDifferent=30099`、`OutputDifferent=0`、`StableDrained=66964`、`StableBlockedNoSideEffect=26986`、`StableBlockedSideEffect=7836`、`ActualMissing=0`。稳定 blocked 的 reason 几乎全部是 `SchedulerNotReady=34811`，只有 `SerializeBlocked=11`；稳定 blocked pop 分布中 `0=26986`，有 side effect 的 pop 数为 1..7 共 7836。这个结果说明，后续如果要扩大 IEWPreview reuse，真正的收益候选是“稳定的 SchedulerNotReady blocked latch + 可发布 dispatch output snapshot”，而不是 LQ/SQ full、serialize 或 barrier/non-spec 特例；下一步必须把 Rename consumer 需要的 `IEWToRename` block/reason 与 dispatch 后 head stall reason 一起建模，而不能只提前发布 block bit。

继续增加 candidate `IEWToRename` latch 对拍后，1M 2-worker self-test `/tmp/rename_latch_preview_fix_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与安全基线一致，所有 future/self-test mismatch 仍为 0。`futureRenameLatchPreviewChecks=131885` 与 checked dispatch preview 闭合，其中 `Matches=117531`、`Differences=14354`、`Stale=0`；difference reason 全部是 `Block=14354`，`Reason=0`。这说明 candidate latch 的 stall reason 已经稳定，但 block bit 仍有一部分不能仅由上一拍 future preview 推出；在真正发布稳定 `SchedulerNotReady` blocked latch 之前，还需要把 latch match/diff 与 output publishability 做交叉归因，并找出 block bit 差异是否来自同周期多个 future wavefront 复用、active-thread selection 变化，还是 dispatch side effect 对 fixedbuffer empty/blocked 状态的反馈。

把 latch 对拍结果和 dispatch output publishability 交叉统计后，1M 2-worker self-test `/tmp/rename_latch_publish_x_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与安全基线一致，所有 future/self-test mismatch 仍为 0。`futureRenameLatchPreviewMatchesByPublishability` 中 `StableDrained=66964`、`StableBlockedNoSideEffect=26986`、`StableBlockedSideEffect=7836` 全部匹配；`futureRenameLatchPreviewDifferencesByPublishability::PreviewDifferent=14354`，其余 publishability class 的 difference 均为 0。结论是：只要 future dispatch preview 和 output snapshot 都稳定，candidate `IEWToRename` latch 在这条 1M workload 上也稳定；不稳定 block bit 全部来自 future preview 本身已经不同的子集。因此下一步可以把 stable class 作为发布候选继续建模，但仍不能发布 `PreviewDifferent` 子集，也不能绕过 fixedbuffer pop / IQ-LSQ insert snapshot 的验证。

继续把 publishability 按 future-time 可见的 block reason、scheduler token reason 和 expected pop 数拆开后，1M 2-worker self-test `/tmp/dispatch_publish_reason_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与安全基线一致，所有 future/self-test mismatch 仍为 0。stable blocked 的 `SchedulerNotReady=34811` 中，`IQFull=33738`、`InportFull=1073`；`PreviewDifferent=30099` 中，future 预测为 blocked 的有 `SchedulerNotReady=27260`，其 scheduler reason 为 `IQFull=23546`、`InportFull=3714`，另外 2839 个 `PreviewDifferent` 是 future 预测 drained、但下一拍 side effect 数量不同。`futureDispatchOutputPreviewDifferentPops` 显示 expected pop 为 0 的不稳定 case 仍有 15840 个，1..8 pop 共 14259 个。这个结果说明，`SchedulerNotReady`、`IQFull/InportFull`、甚至 expected pop=0 都不足以单独定义安全发布谓词；下一步必须把目标 IssueQue 的 free-entry/free-inport token identity 和数量作为 snapshot 建模，区分“同一个 IQ token 状态稳定”和“同样叫 IQFull/InportFull 但实际 token 已被下一拍 schedule/dispatch 推进”的 case。

继续把目标 IssueQue token identity/free-entry/free-inport 做成只读 snapshot 后，1M 2-worker self-test `/tmp/dispatch_block_token_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致，所有 future/self-test mismatch 仍为 0。current-cycle output snapshot 仍完全匹配真实 owner dispatch：`dispatchOutputSnapshotChecks=308727`、`Matches=308727`、`Mismatches=0`。新 token 对拍为 `futureDispatchBlockTokenChecks=64910`、`Matches=10468`、`Differences=54442`；差异字段主要是 `IQIndex=44456`、`Selector=45191`、`DispSeq=30094`，其次是 `OpClass=17024`、`Reason=14451`、`Valid=14354`、`FreeInports=9351`、`FreeEntries=5659`，`ReplayBlocked=0`。按 publishability 交叉后，`PreviewDifferent=30099` 全部 token 不同；即使 `StableBlockedNoSideEffect=26986` 中也只有 6062 个 token match、20913 个 token difference，`StableBlockedSideEffect=7836` 中 token match/difference 为 4406/3430。结论是：稳定的 `IEWToRename` latch/output class 并不意味着目标 IQ token 本身稳定，target-token snapshot 当前更适合作为安全发布谓词的负例归因和后续更细粒度模型输入；不能把 “same block reason + same output class” 直接推广为可提前发布 IQ/LSQ/fixedbuffer side effect。

继续把 `SchedulerNotReady` skipped latch 向 Rename consumer 做只读 candidate prepare 对拍后，1M 2-worker self-test `/tmp/rename_candidate_prepare_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致，已发布 future/self-test mismatch 仍为 0。新增 `rename.futureCandidatePrepareChecks=34304`，其中 `Matches=27298`、`Mismatches=7006`；mismatch reason 主要为 `SelectedTid=7001`，另有 `ThreadDecodeBlockReason=4`、`RegFullEvents=1`。这说明 skipped `SchedulerNotReady` candidate latch 中有相当一部分会直接改变 Rename 的线程选择，而不是只影响不可观察内部 token；即使 IEW 侧 candidate latch/output 在部分分类上稳定，继续驱动 Rename prepare 仍需要更精确的 future-time 可发布谓词。下一步如果要继续放宽，应把可发布条件绑定到 Rename prepare 真实消费的输入组合，而不是只用 IEW block reason、target IQ token 或 output snapshot 的单项稳定性。

继续给 Rename candidate prepare 增加 IEW future-side profile 归因后，1M 2-worker self-test `/tmp/rename_candidate_profile_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致，已发布 future/self-test mismatch 仍为 0。candidate 总量仍为 `Checks=34304`、`Matches=27298`、`Mismatches=7006`，并按 scheduler reason 闭合：match 中 `IQFull=26516`、`InportFull=782`，mismatch 中 `IQFull=5984`、`InportFull=1022`。按 expected fixedbuffer pop 数看，match/mismatch 在 `0` pop 上分别为 18861/4563，`1..7` pop 也同时有 match 和 mismatch，`8plus=0`。结论是：`IQFull/InportFull` 和 expected pop 数都不能直接作为 candidate prepare 的安全发布谓词；尤其 expected pop=0 仍有大量 `SelectedTid` mismatch，说明 Rename 线程选择还受下一拍真实 upstream/input 组合影响。后续需要把 candidate 谓词继续细化到 Rename prepare 真实输入的完整等价关系，例如 candidate latch、future Decode input、future IEW head-stall output 与 current-cycle input 的联合稳定性。

继续把 candidate `RenamePrepareInput` 与下一拍 current-cycle input 做字段级对拍后，1M 2-worker self-test `/tmp/rename_candidate_input_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致：`commit.branches=230870`、`decode.decodedInsts=1325610`、`decode.squashedInsts=179712`、`fetch.fetchBubbles=920616`、`fetch.fetchBubbles_max=69540`、`fetch.resolveDequeueCount=192931`、`iew.dispatchedInsts=1248952`、`iew.instsToCommit=996158`、`iew.writebackCount=996158`、`rename.renamedInsts=1249784`、`rename.fullRegistersEvents=2399`、`rename.stallEvents::RegFull=2399`。已发布 future/self-test mismatch 仍为 0。candidate prepare 分布保持 `Checks=34304`、`Matches=27298`、`Mismatches=7006`；新增 input 对拍为 `futureCandidateInputChecks=34304`、`Matches=27192`、`Differences=7112`。字段差异集中在 `IEWBlock=7029` 和 `FreePhyRegs=105`，其中所有 7006 个 candidate prepare mismatch 都伴随 `futureCandidateInputMismatchDifferenceFields::IEWBlock=7006`，另有 22 个 mismatch 同时伴随 `FreePhyRegs` 差异；fixedbuffer empty/size、demand、IEW reason、ROB/LQ/SQ head-stall reason 均为 0。结论是：当前 skipped `SchedulerNotReady` candidate prepare 的 7006 个结果差异不是由 Decode input 或 head-stall reason 引起，而是 candidate 与真实下一拍看到的 `IEWToRename.block` 不同导致 Rename 线程选择改变。后续发布谓词必须首先证明 `IEWToRename.block` 对 Rename consumer 稳定；在这个条件未满足前，不能把 stable output class、IQFull/InportFull 或 expected pop 数作为充分条件。

继续把 candidate prepare 结果和 input 等价性做四象限交叉后，1M 2-worker self-test `/tmp/rename_candidate_stability_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致，已发布 future/self-test mismatch 仍为 0。新增 `futureCandidatePrepareInputStability` 与 candidate 总量闭合：`PrepareMatchInputMatch=27192`、`PrepareMatchInputDiff=106`、`PrepareMismatchInputMatch=0`、`PrepareMismatchInputDiff=7006`。这个结果说明 `RenamePrepareInput` 已经完整覆盖 candidate prepare 的可观察控制输入；只要 candidate/current input 完全一致，prepare result 就不会 mismatch。剩余工作因此不是在 Rename prepare 内继续找隐式状态，而是继续建模和证明 future-side input 等价，尤其是 `IEWToRename.block` 以及少量 free-list release projection。

继续把 `IEWToRename.block` 的 input 差异按方向拆开后，1M 2-worker self-test `/tmp/rename_candidate_iewblock_dir_1m_t2` 仍在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致，已发布 future/self-test mismatch 仍为 0。新增方向统计中 `PrepareMismatchCandidateFalseActualTrue=0`、`PrepareMismatchCandidateTrueActualFalse=7006`，`PrepareMatchCandidateFalseActualTrue=0`、`PrepareMatchCandidateTrueActualFalse=23`。这说明当前所有会改变 Rename prepare 的 candidate block 差异都是 future candidate 预测 `IEWToRename.block=true`，但下一拍真实 input 为 false；没有漏阻塞 case。后续建模应优先解释和消除这种过度阻塞，例如继续把 future active dispatch 的 resource-token/output snapshot 与下一拍真实 dispatch side effect 对齐，而不是为漏阻塞增加保守条件。

随后把剩余过阻塞定位为 future lookahead selector token 建模错误：真实 `Scheduler::lookahead()` 每次都会把 dispatch selector counter 清零，并且共享 dispatch table 的 OpClass 共用同一个 counter；旧 future token 从当前 `dispOpdist` 值起步且按 OpClass 独立计数，导致目标 IQ selector 和 free-entry/free-inport token 偏离。修正 `buildLookaheadDispatchTokenState()` 后，1M 2-worker self-test `/tmp/lookahead_token_reset_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致；`futureDispatchPreviewChecks=131885`、`Matches=131885`、`Differences=0`，`futureDispatchOutputSnapshotMatches=131885`、`Differences=0`，`futureDispatchBlockTokenMatches=53395`、`Differences=0`，`futureRenameLatchPreviewMatches=131885`、`Differences=0`。在此基础上接受 direct-dispatch `SchedulerNotReady` blocked latch 的 `/tmp/sched_block_accept_fixed_1m_t2` 同样保持关键 CPU stats 完全一致，`iew.futurePrepareMismatches=0`、`rename.futurePrepareMismatchReasons::total=0`、`decode.futurePrepareMatches=62490`、`fetch.futurePrepareMatches=33481`；`taskRuntime.futureRenameWavefrontPrepareMerges` 从 48921 提升到 77252，`futureDecodeWavefrontPrepareMerges` 从 34195 提升到 62490，`futureFetchWavefrontPrepareMerges` 从 5196 提升到 33481。这个结果取代了前面关于 `SchedulerNotReady` 本身不可发布的阶段性结论：问题不是 blocked latch 语义不可建模，而是 future lookahead counter 语义没有和真实 owner path 对齐。

继续给 future Rename preview 增加 owner-side skip 归因后，1000 inst 2-worker self-test `/tmp/rename_preview_reason_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`rename.futurePreviewSkipReasons::total=294` 与 `taskRuntime.futureWavefrontSkipReasons::RenamePreview=294` 闭合，全部来自 `ActiveRename`，`MultipleActive=0`。1M 2-worker self-test `/tmp/rename_preview_reason_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致；`rename.futurePreviewSkipReasons::ActiveRename=139386`、`MultipleActive=0`，与 runtime `RenamePreview=139386` 闭合，`iew.futurePrepareMismatches=0`、`rename.futurePrepareMismatchReasons::total=0`，dispatch/writeback/LSQ 相关 mismatch 也均为 0。这个结果说明当前 RenamePreview 边界不是 SMT 多 active 仲裁，而是 active rename 会写 rename map、free list、history 和 fixedbuffer；因此 active Rename prepare-only 不能复用 IEW prepare-only 的拆分方式，后续若要放宽必须先把 active rename 的输出 snapshot 或更细粒度 subphase 建模。

继续给 future Decode preview 增加 owner-side skip 归因后，1000 inst 2-worker self-test `/tmp/decode_preview_reason_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`decode.futurePreviewSkipReasons::total=206` 与 `taskRuntime.futureWavefrontSkipReasons::DecodePreview=206` 闭合，全部来自 `ActiveDecode`，`MultipleActive=0`。1M 2-worker self-test `/tmp/decode_preview_reason_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致；`decode.futurePreviewSkipReasons::ActiveDecode=29524`、`MultipleActive=0`，与 runtime `DecodePreview=29524` 闭合，`iew.futurePrepareMismatches=0`、`rename.futurePrepareMismatchReasons::total=0`，dispatch/writeback/LSQ 相关 mismatch 也均为 0。这个结果说明当前 DecodePreview 边界不是 SMT 多 active 仲裁，而是 active decode 会移动 stallbuffer/fixedbuffer、写 `toRename` 并可能触发 decode-side squash；后续若要放宽必须先建模 active decode 的输出 snapshot 或更细粒度 Decode subphase。

继续给 future Fetch input 增加 owner-side skip 归因后，1000 inst 2-worker self-test `/tmp/fetch_input_reason_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`fetch.futureInputSkipReasons::total=8373` 与 `taskRuntime.futureWavefrontSkipReasons::FetchInput=8373` 闭合，其中 `FetchQueueNotReady=8370`、`DecodeControl=3`。1M 2-worker self-test `/tmp/fetch_input_reason_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致；`fetch.futureInputSkipReasons::FetchQueueNotReady=29003`、`DecodeControl=6`、其余原因为 0，total 与 runtime `FetchInput=29009` 闭合，`iew.futurePrepareMismatches=0`、`rename.futurePrepareMismatchReasons::total=0`、dispatch/writeback/LSQ 相关 mismatch 也均为 0。这个结果说明当前 FetchInput 边界几乎全部来自未来 Fetch-to-Decode 需要知道下一拍前端是否会继续向 `fetchQueue` 补充指令；后续若要减少该项，需要把 future fetch 前端 supply/no-supply 子集建模，而不是简单允许 partial-width queue preview。

继续给 `FetchQueueNotReady` 增加 next-cycle outcome 诊断后，1000 inst 2-worker self-test `/tmp/fetch_queue_outcome_1k_t2` 在 `simTicks=9236754`、`simInsts=1013` 正常退出，`FetchQueueNotReady=8370` 与 outcome total 闭合，分布为 `NoSupplyStillNotReady=8339`、`PartialSupply=12`、`FilledToWidth=19`。1M 2-worker self-test `/tmp/fetch_queue_outcome_1m_t2` 在 `simTicks=164676159`、`simInsts=1000009` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致；`FetchQueueNotReady=29003` 与 outcome total 闭合，分布为 `NoSupplyStillNotReady=16528`、`PartialSupply=7215`、`FilledToWidth=5260`，`Blocked/QueueShrank/Stale=0`；candidate 时可见队列条数累计 `12072`，下一拍真实 prepare 时为 `92502`。这说明 coremark 1M 中约 57% 的 queue-not-ready case 下一拍仍不会补足一拍，是后续可尝试发布 no-supply prepare 的候选；但另外约 43% 会 partial/full supply，不能用当前队列不足作为充分条件。

继续把 `FetchQueueNotReady` 按 future-time 可见的 fetch-side 状态拆分，并只发布第一批严格自检通过的 short-queue prepare 后，1000 inst 2-worker self-test `/tmp/fetch_shortq_final_1k_t2` 正常退出，`futureInputQueueNotReadyAcceptedStates::FrontendNotReady=1`，`futureToDecodePrepareMatches=3006`，`futureToDecodePrepareMismatchReasons::total=0`。1M 2-worker self-test `/tmp/fetch_shortq_final_1m_t2` 正常退出，关键 CPU stats 与 `/tmp/workerpf_cmp_default_1m/stats.txt` 完全一致；`futureInputQueueNotReadyStates::FrontendNotReady=11540`、`CachePending=16304`，其中只接受 `FrontendNotReady` 且当前 `queue_size > 0` 的子集，`futureInputQueueNotReadyAcceptedStates::FrontendNotReady=1159`；`futureToDecodePrepareMerges=34640`、`Reuses=34640`、`Checks=34640`、`Matches=34640`，`futureToDecodePrepareMismatchReasons::total=0`，IEW/Rename/dispatch/writeback/LSQ 相关自检 mismatch 也均为 0。结论是：`queue_size > 0 && FrontendNotReady` 可以作为首个安全 partial-width Fetch-to-Decode prepare 谓词；zero-width `FrontendNotReady` 仍跳过，因为 stall reason 依赖 same-cycle Fetch owner 副作用或当前 stall reason；`CachePending` 仍跳过，因为它在下一周期可能 partial/full supply。

这些观测用于回答两个问题：是否真的有并行工作，以及并行开销是否超过收益。

## 分阶段落地

第一阶段：运行时骨架，不改模型行为。这是同一套 wavefront runtime 的基础设施，不是单独的保守模式。

- 增加 CPU 内部 `TaskRuntime`，支持 fixed worker pool、submit weak task、wait、deterministic merge。
- 默认关闭；开启后只在少数只读统计或字符串构造上试用。
- 验证目标是 task on/off 的提交 trace、difftest、关键 stats 完全一致。
- 增加 event priority 审计工具，找出同 tick 同 priority 且存在共享资源访问的事件组，为后续显式 priority 调整提供清单。
- 增加运行时基础 stats，包括 worker busy/idle、barrier wait、ready queue occupancy、spec discard。

第二阶段：跨周期 wavefront 调度，但先保持 stage task 粗粒度。

- 把 `CPU::tick()` 展开成 `C[c]`、`I[c]`、`R[c]`、`D[c]`、`F[c]` task。
- 建立 same-cycle 和 forward-delay 依赖边，`task_window_cycles=1` 时行为必须等价于旧实现。
- 增大 `task_window_cycles`，允许不同 cycle 的 ready stage task 并行执行。
- 引入 per-cycle latch bank 或显式 TimeBuffer slot，避免多个 in-flight cycle 争用一个 `base` 指针。
- 将 `StallSignals` 迁移为 per-cycle/per-edge latch bank，先保留旧接口适配层，禁止并行 task 直接写全局 `stallSig`。
- 实现 event horizon barrier 和运行时反压，确保 worker prepare 可以提前，但 merge 不越过可见 event 边界。

第三阶段：stage 内局部并行。这个阶段继续服务同一套 wavefront 调度器，只是增加每个 stage 内部的 weak task 来源。

- IEW：优先尝试 per-IQ/per-thread 候选扫描和执行结果计算，但最终 issue/select/writeback merge 保持单线程。
- LSQ：对 per-thread load/store pipeline 的只读 ready 检查做 task 化，请求发送和 replay/violation 更新强序。
- Commit：只并行非状态改变的预处理，difftest 和提交 side effect 继续强序。
- 根据 worker busy/idle、steal 次数和 critical path stats 调整 task granularity，避免任务过细或长尾任务拖慢整体。

第四阶段：显式 prepare/commit stage API。

- 把 stage tick 拆成 `prepare(snapshot)` 和 `commit(result)`。
- `prepare` 可以并行，`commit` 按 task graph 依赖顺序 merge。
- 清理直接写 `stallSig` / TimeBuffer 的散点，使 same-cycle 可见性集中在 commit phase。

第五阶段：更激进的跨 event / 跨对象并行实验。

- 只有在前面阶段把 CPU 内部通信边界完全显式化之后才能做。
- 需要为每条边声明 latency、same-cycle 可见性、squash 优先级和 replay 行为。
- 该阶段可能改变模型结构，必须单独设计和验证，不能和前面阶段混在一起。

## 验证计划

精确模式必须先做 bit-for-bit 或 trace-for-trace 验证：

1. `enable_task_parallel_sim=false/true` 分别跑相同 checkpoint，比较 commit trace 的 `(tick, tid, seqNum, pc, inst, result)`。
2. 开 difftest 跑 `make default`、`make smt` 和已知长切片，要求无 difftest fail。
3. 对 squash 密集、AMO/LLSC、vector config、异常/中断、store-load forwarding、memory replay 的 workload 单独回归。
4. 比较 `stats.txt` 中架构相关和 timing 相关关键项；精确模式下应一致，允许的 host-only task stats 除外。
5. 打开 `--debug-flags=Event,O3CPU,Commit,IEW,Fetch` 抽样比较同 tick event 顺序和 stage 顺序。
6. 开启 `task_trace`，确认 task fanout、barrier 等待、merge 顺序符合预期。
7. 增加 stall signal 专项测试：构造两个 active thread，一个卡在 rename，一个正在 decode，确认 `Rename[tid1, c+1]` 不会提前影响 `Decode[tid0, c]` 的 `blockDecode` 输入。
8. 开启 event priority audit，列出同 tick 同 priority 事件组；对存在真实依赖的组调整 priority 后，再比较调整前后的单线程行为和关键 stats，确认只是显式化旧顺序而不是改变语义。
9. 压测 event horizon 和运行时反压：构造频繁 cache/memory/device event 或 squash 的 workload，确认 `specDiscarded` 受控、`committableCycle` 不越界、ready queue 不无限增长。
10. 做负载均衡 A/B：以 `--task-parallel-threads=2` 为默认基线，小范围调整 `task_parallel_threads`、`task_window_cycles`、`task_min_work`，检查 worker 利用率、barrier 等待和 wall-time 趋势是否合理。

如果进入跨周期 wavefront 模式，还要额外验证 `task_window_cycles=1` 与旧实现完全一致，`task_window_cycles>1` 时 `fetchToDecodeDelay`、`commitToFetchDelay`、squash recovery latency、ROB/IQ/LSQ backpressure latency 没有被无意改变。还需要用 `task_trace` 检查 DAG ready set 是否符合预期，例如完整 Commit stage 在 `Rename->Commit` 边满足后能看到 `C[c+1] || D[c]`、`I[c+1] || F[c]` 这类跨周期并行；如果后续进一步拆 Commit subphase，再单独验证更早的 `commit-head[c+1] || I[c]` overlap。

## 风险点

最大风险不是 C++ data race，而是“看似并行、实际改变了模拟时序”。需要重点防守：

- same-cycle `stallSig` 和 `TimeBuffer->getWire(0)` 可见性。
- `stallSig` 未来值污染当前周期或其他线程。必须用 `cycle × edge × tid` latch bank 和 stage-level merge 隔离，不能让 thread-local task 直接改全局 block 数组。
- 跨周期 wavefront 中的 event horizon；不能让未来周期的 CPU stage 早于同 tick 更高优先级事件提交可见状态。
- TimeBuffer 全局 `base` 指针；需要改成 task window 内可索引 latch，不能让多个 in-flight cycle 直接共享当前 TimeBuffer advance 机制。
- 事件同步和 event horizon；worker 可以提前 prepare，但 merge/commit 不能越过未处理外部事件。
- 负载均衡和任务粒度；任务过细会被调度开销吞掉收益，任务过粗会导致长尾 worker 阻塞关键路径。
- 运行时反压；`task_window_cycles`、ready queue 和 speculative prepare 结果必须有上限，避免内存膨胀和 squash 后大量无效工作。
- Commit 的架构 side effect：misc reg、PCEventQueue、difftest、BPU commit training、trace/archDB。
- EventQueue 同 tick 同 priority 的隐式顺序依赖；不能用软约束维持旧执行先后，应审计并改成显式 priority 或 dependency。
- stats/debug 的顺序敏感性，尤其是用于定位 difftest 和性能问题的 trace。
- `DynInstPtr` 生命周期和状态转换；不要让 worker 持有可能被 squash/retire 的 mutable inst 并写回。

因此推荐先做 task graph 和 `task_window_cycles=1`，证明新调度框架与旧串行 tick 完全等价；然后把 window 扩到多个 cycle，验证 wavefront 并行没有改变模拟语义；最后再扩大 stage 内 weak task 范围。
