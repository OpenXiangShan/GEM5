# Vector Split Units

## Scope

本文档描述 O3 后端里向量访存的 split 建模，不描述 RVV 宏指令拆 micro-op 的 ISA 语义。

当前实现位置在 `src/cpu/o3/issue_queue.{hh,cc}`，作用对象是：

- `isVector() && isMemRef()` 的向量访存指令
- 指令在操作数 ready 之后，进入 issue 前的 split 延迟模型

## Current Model

向量访存 ready 后，会经过下面这条路径：

1. 进入 `vectorReadyQ`
2. 按访问方向被分配到 `VLSplit` 或 `VSSplit` split unit
3. 在 split unit 中等待固定 `3` 拍
4. 释放到 `vectorDelayedReadyQ`
5. 再回到常规 `readyQ`，参与后续 schedule / issue

本次实现后，每个 `IssueQue` 默认有两类拆分资源：

- `VLSplit[0]`、`VLSplit[1]`：向量 load 拆分单元
- `VSSplit[0]`、`VSSplit[1]`：向量 store 拆分单元

`vectorSplitUnits` 参数控制每个方向的实例数，默认值为 `2`。

## Two-Unit Semantics

同一方向的 split unit 之间没有阻塞关系；`VLSplit` 和 `VSSplit` 两个资源池之间也没有阻塞关系。

每个 split unit 内部保留原有阻塞语义：

- 如果该 unit 里已经有会阻塞拆分的向量访存正在 split，这个 unit 就不再接收新的同方向待拆分指令
- 同方向的另一个 unit 如果没有被这种指令占住，仍然可以继续接收新的待拆分指令

现有规则中，`VectorUnitStrideLoad` 仍然视为非阻塞型：

- load 只会送入 `VLSplit`，store 只会送入 `VSSplit`
- 每个方向的指令会被送入某个具体编号的 split unit
- `VectorUnitStrideLoad` 不会把所在 unit 标记成 blocked；其他向量访存沿用原有 blocker 语义

因此，效果上等价于：

- 旧模型：1 套“split 通道 + 全局 blocker”
- 新模型：4 个带方向的 split 通道（2 个 `VLSplit` + 2 个 `VSSplit`），每通道各自维护 blocker

## Unit Selection

待拆分指令先按方向进入 `vectorLoadReadyQ` 或 `vectorStoreReadyQ`。

当 `IssueQue` 尝试启动 split 时：

1. 按对应方向 FIFO 顺序取最老指令
2. 在对应方向所有未 blocked 的 split unit 中做轮转选择
3. 将该指令送入选中的 unit
4. 若该指令是非 `unit-stride`，则只阻塞它所在的那个 unit

如果某个方向的 2 个 unit 都被阻塞，则该方向的 ready queue 停止继续向前推进，直到至少一个同方向 unit 释放；另一方向仍可独立推进。

## What Stays Unchanged

这次修改没有改变下面这些语义：

- split 延迟仍然是固定 `3` 拍
- 释放后仍然先进入 `vectorDelayedReadyQ`
- 后续仍然走原有 `readyQ -> select -> schedule -> toFu` 路径
- 没有把一个 vector memory micro-op 进一步拆成多个 LSQ 子请求
- 没有改 RVV ISA 模板里的 macro-op / micro-op 生成方式

## Config Surface

`src/cpu/o3/FuncScheduler.py` 中给 `IssueQue` 新增了：

```python
vectorSplitUnits = Param.Unsigned(
    2, "Number of independent vector load/store split units per direction")
```

默认值是 `2`。如果需要回退到旧行为，可以显式把它设成 `1`。
