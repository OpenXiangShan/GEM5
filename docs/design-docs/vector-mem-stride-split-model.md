# Vector Memory Stride Split Model

## Background

O3 侧当前对向量访存的 split 建模入口位于 `src/cpu/o3/issue_queue.cc`。向量访存在操作数 ready 后不会立刻进入常规 readyQ，而是先进入 `vectorReadyQ -> vectorSplitQ -> vectorDelayedReadyQ` 这条额外时序路径。此前所有向量访存都统一使用固定 3 拍 split 延迟：

- 第 0 拍：进入 `vectorSplitQ`
- 第 3 拍：释放回后续队列

这对“一个 microop 覆盖多个元素”的 stride-family RVV 访存过于粗糙。用户要求新增一条更细的建模规则：

- 3 拍之后出第一个元素；
- 之后每拍再出一个元素；
- 若当前 microop 覆盖元素数为 `n`，则总拆分延迟为 `3 + n - 1` 拍；
- 实际 active 请求数量为 `n - inactive_element_count`。

## Modeling Contract

本次改动遵循下面这条性能因果链：

```text
vector mem uop ready
  -> enter IQ split model
  -> consume split cycles according to covered element count
  -> release to ready/replay path after modeled split latency
  -> later, at initiateAcc(), report how many active elements would really issue memory accesses
  -> expose the effect through IssueQue stats / DPRINTF
```

建模边界如下：

1. `IssueQue` 负责 split 时延建模。
2. active 请求数在 `initiateAcc()` 阶段统计，因为此时 `VL` 和 `v0` 已经被正常读取。
3. 本次不重写 `LSQRequest::_reqs` 的真实子请求结构。
4. 因此，“实际访存请求数量”在本实现中是 split-model observability，而不是把每个 active element 强制映射成一个真实 LSQ 子请求。

这样做的原因很直接：若把 stride-family 向量访存直接改成真实 `SplitDataRequest` 多子请求，会波及 `isSplit()`、replay、forwarding、RAR/RAW 检查、normal-load 快路径等现有 LSU 语义，风险明显超出本次需求。

## Scope

本次把“stride-family”定义为 RVV 的 stride-based 地址模式：

- `VectorUnitStrideLoad`
- `VectorSegUnitStrideLoad`
- `VectorUnitStrideMaskLoad`
- `VectorSegUnitStrideMaskLoad`
- `VectorStridedLoad`
- `VectorSegStridedLoad`
- `VectorUnitStrideFaultOnlyFirstLoad`
- `VectorUnitStrideStore`
- `VectorSegUnitStrideStore`
- `VectorUnitStrideMaskStore`
- `VectorStridedStore`

不包含：

- indexed load/store
- whole-register load/store

说明：

- 实现并不硬编码 `n = VLEN / eew`。
- 实际使用 `VectorMicroInst::vmi.re - vmi.rs` 作为“当前 microop 覆盖元素数”。
- 对当前目标 uop 形态，这与用户给出的 `n = VLEN / eew` 一致；同时也兼容现有模板里按单元素切分的 segment / strided 变体。

## Implementation

### 1. IssueQueue Delay Rule

修改点：

- `src/cpu/o3/issue_queue.hh`
- `src/cpu/o3/issue_queue.cc`

新增逻辑：

1. `isStrideVectorMemInst()` 判定当前指令是否属于 stride-family。
2. `getVectorMemSplitElemCount()` 从 `VectorMicroInst::vmi.rs/re` 计算当前 microop 覆盖元素数。
3. `getVectorMemSplitDelayCycles()` 对 stride-family 返回：

```text
delay = 3 + elem_count - 1
```

对其他向量访存维持原有固定 3 拍。

在 `tryStartVectorMemSplit()` 中：

- 原先固定 `clockEdge(Cycles(3))`
- 改为 `clockEdge(Cycles(delay_cycles))`

这样建模的是“split engine 从第 3 拍起每拍吐出一个元素”，而 IQ 只关心整个 split 何时结束，因此可以等价折叠成总释放时延。

### 2. Active Request Accounting Hook

修改点：

- `src/cpu/exec_context.hh`
- `src/cpu/o3/dyn_inst.hh`
- `src/cpu/o3/dyn_inst.cc`

新增接口：

- `ExecContext::notifyVectorMemSplitAccess(element_count, active_req_count)`

默认实现为空，避免影响 simple/minor 等其他 CPU 模型。

O3 `DynInst` 重载后，将数据回传给所属 `IssueQue`：

```text
RVV initiateAcc()
  -> ExecContext hook
  -> DynInst::notifyVectorMemSplitAccess()
  -> IssueQue::recordVectorMemSplitAccess()
```

这样做的关键好处是：

- 不需要在 issue 阶段重新读寄存器；
- 不会污染 regfile read stats；
- active 请求数来自真实 `VL/v0`，而不是静态估计。

### 3. RVV ISA Template Changes

修改点：

- `src/arch/riscv/isa/vector/base/vector_mem.isa`
- `src/arch/riscv/isa/vector/base/vector_mem.temp.isa`
- `src/arch/riscv/isa/vector/simple/vector_mem.isa`
- `src/arch/riscv/isa/vector/simple/vector_mem.temp.isa`

新增 helper：

- `count_active_vector_mem_elements()`
- `record_vector_mem_split_access()`

放在 `vector_mem.temp.isa` 的 header helper 区域中。

active 元素数规则：

```text
vend = min(rVl, re)
if rs >= vend: active = 0
else if vm == 1: active = vend - rs
else: active = sum(elem_mask(v0, ei), ei in [rs, vend))
```

load/store 的 `initiateAcc()` 在真正发起访存前上报：

- `element_count = vmi.re - vmi.rs`
- `active_req_count = active_element_count`

需要注意的一个实现细节：

- 不能简单在 `temp.isa` 里写 `#if is_vecWhole ... #else ... rVl ...`
- 因为 ISA 生成器会在模板展开前做 operand 分析，未被编译器执行的分支仍可能暴露非法 operand
- 所以最终采用 `vector_mem.isa` 中“按实例注入字符串”的方式，为 whole / non-whole 变体分别生成合法代码

## Observability

新增 `IssueQue` stats：

- `vectorStrideSplitInsts`
  - stride-family split admission 次数
- `vectorStrideSplitElems`
  - 所有 stride-family split 覆盖的元素总数
- `vectorStrideSplitDelayCycles`
  - 总 modeled split 拍数
- `vectorStrideSplitActiveReqs`
  - 由 `initiateAcc()` 上报的 active 请求总数

同时保留 `DPRINTF(Schedule)`：

- split 入队时打印 `elem_count` 和 `delay`
- `initiateAcc()` 回传时打印 `active_req_count`

## Accuracy and Limitations

本实现保留了这些关键性能因果：

- stride-family uop 覆盖元素越多，split 完成越慢；
- inactive element 不贡献 active request 计数；
- active request 计数来自真实 `VL/v0`，不是静态猜测。

本实现刻意没有建模这些更细的行为：

- 每个 active element 成为一个真实 LSQ 子请求；
- split 过程中元素间与 LSU/MemPipe 的逐拍交织；
- inactive element 是否仍消耗部分下游 LSU 仲裁资源。

这是一个有意的粒度选择。当前需求要解决的是 IQ 侧 split 时延规则，而不是对 LSU 功能请求路径做大重构。

## Validation

本次已执行：

```bash
scons build/RISCV/gem5.opt --gold-linker -j64
```

结果：

- RVV ISA 模板重新生成成功
- `src/cpu/o3/issue_queue.cc`、`src/cpu/o3/dyn_inst.cc`、生成的 `inst-constrs.cc` / `generic_cpu_exec.cc` 等关键文件均编译通过
- 最终 `RISCV/gem5.opt` 链接成功

## Future Work

如果后续需要把 active 请求数进一步下沉成真实 LSQ 子请求，建议单独立项，重点评估：

- `SplitDataRequest` 对 normal-load / replay path 的副作用
- store-to-load forwarding 的 fragment 组合复杂度
- `isSplit()` 扩散到 RAR/RAW/MDP 逻辑后的性能和语义影响
