# 向量访存指令完成后延迟 3 拍实现方案

## 背景与目标

目标是让 RVV 向量访存相关指令在访存执行完成后，再延迟 3 拍进入写回/完成路径。

这里的“向量访存相关指令”包括 vector load 和 vector store 两类 OpClass：

- `VectorUnitStrideLoad` 到 `VectorWholeRegisterLoad`
- `VectorUnitStrideStore` 到 `VectorWholeRegisterStore`

建模后的时序建议为：

```text
issue -> load/store pipe -> LSQ/cache 完成访存
      -> vector mem completion delay queue 等待 3 cycles
      -> IEW writeback/finish -> wakeup/scoreboard/commit 可见
```

对于 vector load，延迟 3 拍后才让 load 结果对依赖指令可见；如果需要严格建模“物理向量寄存器写入时间”，还要延迟真实 regfile 写入。

对于 vector store，通常没有向量目的寄存器，因此不伪造向量寄存器写回；延迟的是该 store 在后端完成路径上的可见性，例如进入 IEW writeback/commit-ready 的时间。

不建议简单修改所有 vector OpClass 的 `opLat += 3`。那会把 vector ALU、执行单元占用、issue 选择、load/store pipe 发起访存等行为全部后移，和“访存完成后额外延迟”不一致。

## 当前关键路径

当前 O3 里，相关路径大致如下：

- vector load/store 通过 issue queue 选择后进入 load/store pipe 和 LSQ。
- vector load 的 memory response 到达后，LSQ 调用 `inst->completeAcc(pkt)` 生成结果，然后调用 `IEW::readyToFinish(inst)`。
- vector store 在 store 侧完成后也会调用 `IEW::readyToFinish(inst)`。
- `IEW::readyToFinish()` 当前会把指令放入 IEW 到 commit 的 writeback queue，并可能触发 scheduler bypass/writeback 相关状态。
- `IEW::writebackInsts()` 中会调用 scheduler wakeup、IQ wakeup、scoreboard ready、送 commit。
- `InstructionQueue::wakeDependents()` 当前同时包含寄存器依赖 wakeup 和 memory dependency 完成。
- `DynInst::setRegOperand()` 当前会直接调用 `cpu->setReg()` 写物理寄存器。因此 vector load 的真实结果写入可能已经发生在 `completeAcc()` 阶段，而不是 IEW writeback 阶段。

已有代码里可以复用或抽取类似 `isVectorMemInst()` 的判断逻辑，避免重新散落判断 OpClass。

## 建模合同

推荐采用以下行为合同：

- 只有 vector memory OpClass 进入 3 拍延迟；普通 vector ALU 不受影响。
- unit-stride load 即使已经绕过 split 流程，也仍然在访存完成后走这个 3 拍 completion delay。
- 延迟点放在 LSQ/cache 完成之后、IEW writeback/finish 之前。
- 不改变 issue queue 选择、split 策略、load/store pipe 发起请求、cache miss/hit 行为。
- vector load 的寄存器依赖 wakeup、scoreboard ready、commit 可见性延迟 3 拍。
- vector store 的完成/commit 可见性延迟 3 拍，但不产生不存在的向量寄存器写回。
- squash/fault 指令不能在延迟队列里产生任何新副作用。
- 默认参数为 0，保证现有配置行为不变；需要建模时在 `kmhv3.py` 中设为 3。

默认建议把 memory dependency 完成也随 vector mem instruction finish 一起延迟，因为用户需求现在是“向量访存相关指令需要延迟三拍”，不是只延迟向量寄存器结果可见性。如果后续确认 RTL 中 memory-order 完成早于寄存器/commit 可见性，可以再拆出独立开关。

## 参数设计

在 `src/cpu/o3/BaseO3CPU.py` 增加参数：

```python
vectorMemCompletionDelay = Param.Cycles(
    0, "Extra delay from vector memory completion to IEW writeback"
)
```

在 `configs/example/kmhv3.py` 中按目标配置打开：

```python
cpu.vectorMemCompletionDelay = 3
```

参数默认值必须保持 0，以保证所有未显式打开该模型的配置行为不变。

## 指令范围判断

建议新增一个共享 helper，而不是在多个文件里写 OpClass 范围：

```cpp
bool isVectorMemCompletionDelayedInst(const DynInstPtr &inst)
{
    const auto op = inst->opClass();
    return (op >= enums::VectorUnitStrideLoad &&
            op <= enums::VectorWholeRegisterLoad) ||
           (op >= enums::VectorUnitStrideStore &&
            op <= enums::VectorWholeRegisterStore);
}
```

如果工程里已有 `IssueQue::isVectorMemInst()`，可以把它提升成更通用的 helper，或者在 IEW 中增加同语义的私有 helper。推荐命名围绕模型行为，例如 `shouldDelayVectorMemCompletion()`，而不是 RTL 信号名。

## 推荐实现

### 1. 在 IEW 增加 vector memory completion delay queue

在 `src/cpu/o3/iew.hh` 中增加队列项：

```cpp
struct DelayedVectorMemCompletion
{
    Tick readyTick;
    DynInstPtr inst;
};

std::deque<DelayedVectorMemCompletion> delayedVectorMemCompletionQ;
```

增加 helper：

```cpp
bool shouldDelayVectorMemCompletion(const DynInstPtr &inst) const;
void enqueueVectorMemCompletionDelay(const DynInstPtr &inst);
void processDelayedVectorMemCompletions();
void enqueueWritebackNow(const DynInstPtr &inst);
```

其中 `enqueueWritebackNow()` 承载当前 `IEW::readyToFinish()` 的原有立即完成逻辑，避免延迟路径提前执行 scheduler bypass/wakeup。

### 2. 改造 IEW::readyToFinish()

把 `IEW::readyToFinish()` 改成两段：

```cpp
void
IEW::readyToFinish(const DynInstPtr &inst)
{
    if (shouldDelayVectorMemCompletion(inst)) {
        enqueueVectorMemCompletionDelay(inst);
        return;
    }

    enqueueWritebackNow(inst);
}
```

`shouldDelayVectorMemCompletion(inst)` 的条件：

- `cpu->params().vectorMemCompletionDelay != 0`
- 指令是 vector memory OpClass
- 指令没有被 squash
- 指令已经完成访存侧执行

这样 LSQ/cache 完成之前的行为保持不变，额外 3 拍只插在 memory completion 之后。

### 3. 释放延迟队列

`enqueueVectorMemCompletionDelay()` 记录 ready tick：

```cpp
auto delay = cpu->params().vectorMemCompletionDelay;
delayedVectorMemCompletionQ.push_back({
    cpu->clockEdge(delay),
    inst
});
```

`processDelayedVectorMemCompletions()` 按队头释放：

```cpp
while (!delayedVectorMemCompletionQ.empty() &&
       delayedVectorMemCompletionQ.front().readyTick <= curTick()) {
    auto inst = delayedVectorMemCompletionQ.front().inst;
    delayedVectorMemCompletionQ.pop_front();

    if (inst->isSquashed()) {
        clearDeferredVectorMemState(inst);
        continue;
    }

    enqueueWritebackNow(inst);
}
```

建议在 `IEW::tick()` 中放在 `executeInsts()` 之后、`writebackInsts()` 之前：

```cpp
executeInsts();
processDelayedVectorMemCompletions();
writebackInsts();
```

如果当前 `IEW::tick()` 已经有固定阶段顺序，应保持原有顺序，只把 delay queue release 放在 writeback 消费之前。

### 4. vector load 的真实寄存器写入处理

如果只延迟 `readyToFinish()`，vector load 的 wakeup 和 commit 可见性会延迟，但真实向量物理寄存器可能已经在 `inst->completeAcc(pkt)` 阶段通过 `DynInst::setRegOperand()` 提前写入。

如果模型只关心依赖唤醒延迟，这可以作为简化方案接受。

如果模型要求“vector load 的物理向量寄存器也晚 3 拍写入”，需要增加 deferred vector load result buffer：

```cpp
bool hasDeferredVectorMemWrites() const;
void commitDeferredVectorMemWrites();
void clearDeferredVectorMemWrites();
```

拦截条件应比旧方案更窄：

- `vectorMemCompletionDelay != 0`
- 当前指令是 vector memory load
- 目标寄存器 class 是 `VecRegClass`、`VecElemClass` 或 `VecPredRegClass`

非 vector memory 指令、普通 vector ALU、scalar load、vector store 都不进入这个 deferred regfile write 逻辑。

在 `IEW::writebackInsts()` 对该指令执行 wakeup 前提交 deferred result：

```cpp
if (inst->hasDeferredVectorMemWrites()) {
    inst->commitDeferredVectorMemWrites();
}
```

顺序应保持：

```text
vector memory delay 到期
-> commitDeferredVectorMemWrites()
-> scheduler->writebackWakeup(inst)
-> instQueue.wakeDependents(inst)
-> scoreboard ready
-> send to commit
```

这样消费者只有在真实向量结果落地后才被唤醒。

### 5. getWritableRegOperand 审计

部分 vector load 或 vector memory 语义可能通过 `getWritableRegOperand()` 拿到可写指针。如果它返回真实 regfile 指针，就会绕过 deferred result buffer。

严格实现时需要：

- 只对 vector memory load 且目标是向量寄存器的路径返回 `DynInst` scratch buffer。
- scratch buffer 初始值从当前物理寄存器拷贝，保证 read-modify-write 语义。
- 延迟到期时再把 scratch buffer 写回真实物理寄存器。

如果代码审计确认 vector memory load 不走该接口，或只在非延迟配置中使用，则可以先不改这一层，但需要在验证说明中标出剩余风险。

### 6. memory dependency 的处理策略

因为需求已经收窄为“向量访存相关指令需要延迟三拍”，默认建议让 `InstructionQueue::wakeDependents()` 中的 memory dependency complete 也随 IEW writeback 一起延迟。

这样模型表达的是：

```text
vector memory instruction 访存完成
-> 额外 3 拍 completion latency
-> 统一完成寄存器依赖、memory dependency 和 commit 可见性
```

如果后续要更细分，可以扩展为两个参数：

```python
vectorMemCompletionDelay = Param.Cycles(3, ...)
vectorMemRegWakeupDelayOnly = Param.Bool(False, ...)
```

但第一版不建议引入第二个开关，除非有明确 RTL 或性能数据说明 memory-order 完成不该延迟。

### 7. squash、drain 和 fault

延迟队列必须接入生命周期：

- squash 时清掉对应 thread 的队列条目，或释放时跳过 squashed instruction。
- fault/no-execute 指令不能进入 delayed completion，或在释放时不能产生写回副作用。
- drain 判断需要包含 `delayedVectorMemCompletionQ.empty()`。
- 延迟队列中的 instruction 仍然持有 `DynInstPtr`，避免对象提前释放。

## Corner Cases

- **Unit-stride load**：不需要经过 split 流程，但访存完成后仍应进入 3 拍 completion delay。
- **Segment load/store**：以拆分后的微指令 OpClass 为准；每个完成的 vector mem microop 独立延迟。
- **Whole-register load/store**：属于 vector memory 范围，应延迟。
- **Vector store**：没有向量目的寄存器，只延迟完成路径，不做 deferred vector reg write。
- **Masked/zero-length load**：如果仍然走 vector mem completion，则延迟完成；如果完全没有 memory side effect，应按实际代码路径判断是否调用 `readyToFinish()`。
- **Fault-only-first load**：fault 路径不能提交 deferred vector result；正常部分完成时按现有语义处理。
- **Squash/replay/cancel**：延迟到期前被 squash 的指令必须跳过，不能 wakeup 或写寄存器。

## 统计与 Debug

建议增加最小 stats：

- `iew.vectorMemCompletionDelayedInsts`
- `iew.vectorMemCompletionDelayCycles`
- `iew.vectorMemCompletionDelayQOccupancy`
- 可选：按 load/store 拆分 `vectorMemCompletionDelayedLoads` / `vectorMemCompletionDelayedStores`

建议增加 debug 输出：

- 入队：seqNum、PC、opClass、isLoad/isStore、readyTick
- 出队：seqNum、当前 tick、等待 cycles
- squash/drop：seqNum、thread id、原因
- deferred load result commit：目标物理寄存器编号和 reg class

## 验证计划

最小编译验证：

```bash
scons build/RISCV/gem5.opt --gold-linker -j64
```

功能/时序验证：

- `vle -> vadd`：确认 `vle` memory response 到达后，`vadd` wakeup/issue 晚 3 拍。
- `vse` 独立 store：确认 store 发起访存时间不变，但 IEW/commit 侧完成晚 3 拍。
- `vadd -> vadd`：确认普通 vector ALU 不受 `vectorMemCompletionDelay` 影响。
- unit-stride load：确认它仍然绕过 split 流程，但完成后进入 3 拍 delay。
- 参数 A/B：`vectorMemCompletionDelay=0` 与 `3` 对比 stats，确认默认值完全保留旧行为。

建议 debug flags：

```bash
--debug-flags=IEW,Scoreboard,Schedule
```

对比重点：

- vector mem issue tick
- LSQ/cache completion tick
- delay queue enter/release tick
- load consumer wakeup/issue tick
- store commit-ready tick
- memory dependency complete tick

## 风险与替代方案

### 推荐方案风险

严格延迟 vector load 的真实物理寄存器写入，需要覆盖 `setRegOperand()` 和可能的 `getWritableRegOperand()` 直接写路径。这里改动比只延迟 wakeup 大，但模型语义最准确。

### 简化方案：只延迟 readyToFinish

第一版也可以只把 vector memory instruction 的 `readyToFinish()` 后移 3 拍，不拦截真实 regfile 写入。

优点是改动小，风险集中在 IEW 完成路径。

缺点是 vector load 数据可能已经提前写入物理寄存器，只是 scoreboard/wakeup 晚 3 拍。只要所有消费者都依赖 scoreboard，这个性能模型趋势通常仍然可用；但它不是严格的“物理寄存器写入延迟”模型。

### 不推荐方案：修改 OpClass latency

给 vector load/store OpClass 直接加 3 拍 latency 会影响 issue 后执行阶段和 pipe 占用，也可能影响 split/memory request 发起时间。该方案不能表达“访存完成后额外延迟”，不推荐。

## 推荐落地顺序

1. 增加 `vectorMemCompletionDelay` 参数，默认 0。
2. 抽出 `shouldDelayVectorMemCompletion()` 判断，只覆盖 vector memory OpClass。
3. 拆分 `IEW::readyToFinish()` 的立即写回逻辑为 `enqueueWritebackNow()`。
4. 增加 `delayedVectorMemCompletionQ` 和 release 路径。
5. 接入 squash/drain/stats/debug。
6. 先实现简化版 readyToFinish 延迟并编译验证。
7. 如果需要严格物理 regfile 写入时序，再追加 deferred vector load result buffer。
8. 在 `kmhv3.py` 中把 `vectorMemCompletionDelay` 设为 3，跑 RVV microbenchmark A/B 验证。
