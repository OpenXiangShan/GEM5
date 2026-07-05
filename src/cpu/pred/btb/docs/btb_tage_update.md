# BTB/TAGE update path 说明

这份文档记录当前 BTB frontend 的训练协议。核心边界是：预测器训练应该由
actual branch facts 加 prediction-time meta/history 驱动；当真实控制流结果已经
存在时，不应该再从预测期的 `BTBEntry` 里反推 actual outcome。

## Update Contract

commit update 和 resolve update 应该收敛到同一种输入形态：

```text
ftqId/startPC + prediction meta/history snapshot + actual branch set
```

二者差异只应该是“什么时候训练”，而不是“训练数据从哪里重建”。

`actual branch set` 是同一个 FTQ entry 内按 PC 排序的真实控制流结果。resolve
update 和 commit update 都从 FTQ 的 `resolvedBranches` 得到它。
`makeUpdateBranchPrefix()` 只保留到第一个 taken 或 mispredicted branch
为止的 prefix；这个点之后的 younger branch 不属于本次训练 prefix。

同一个 FTQ 内的多个 branch 可能在不同周期 resolve。Fetch 会按 `tid + ftqId`
把它们 combine 到同一个 pending resolve queue entry，FTQ/`FetchTarget` 里也
会让每个 branch fact 只累积一次。这和 RTL resolve queue combine 的语义一致：
一个真实 branch fact 不能被重复训练。

## Update Context

`makeBaseBranchUpdateContext(target)` 从 prediction-time `FetchTarget`
snapshot 构造 update context，只保留 `tid/startPC/asidHash/predTick` 这类
预测期上下文。actual branch prefix 不写回 context，而是直接进入
direction/target entry builders、branch-context consumers 和 update-boundary
helper。这条路径是 predictor update 的普通入口；它不需要先把真实结果写回
`FetchTarget`，因此训练边界更接近：

```text
prediction snapshot + actual branch set -> BranchUpdateContext + update entries
```

direction/target entry builder 在存在 per-entry resolved fact 时不会依赖
`FetchTarget` 中的单一 executed branch summary。它们会先按 entry PC 查
`actual branch set`：

- direction update 使用该 entry 自己的 branch identity/attrs、base direction 和
  actual taken，不再把完整 predicted BTB entry 作为方向训练输入；
- target update 使用该 entry 自己的 actual target 和 branch attributes；
- RAS/uRAS 这类 branch-context consumer 直接消费 actual branch prefix，不再从
  `BranchUpdateContext` 里读取 stream-level actual summary；
- 没有 resolved branch set 时，commit update 只保留 base context，
  不再从单一的 stream-level legacy summary 伪造 actual result。

这正是当前协议区别于旧模型的关键：旧模型经常用 predicted BTB entries 加一个
executed branch summary 去猜训练 entry；新模型优先消费真实 resolved facts。

## Direction Entries

`buildDirectionUpdateEntries()` 构造给 BTBTAGE、MicroTAGE、MGSC 等方向预测器消费
的训练 entry。

输入包括：

- actual update prefix 内的 predicted BTB entries；
- actual branch prefix 中预测 BTB 没有命中的 conditional branch，对它们生成
  direction-only entry；
- actual branch prefix 和 prediction-time meta/history snapshot。

对 resolved update 来说，普通方向预测器只保留 actual branch prefix 里的 entry。MGSC
有自己的 filter，因为它对 conditional entry 集合的需求略有不同。

TAGE update 仍然需要 prediction-time meta/history。missing conditional branch
只有在它的位置能用预测时 block context 表达时才能训练；如果 branch 已经超出预测
block 可表达范围，就跳过这次训练，而不是用错误历史硬造 provider。

## Target Entries

`buildTargetUpdateEntries()` 构造给 MBTB、ABTB 等 target predictor 消费的训练
entry。

输入包括：

- actual update prefix 内的 predicted BTB entries；
- actual branch prefix；其中实际 taken、但预测 entry 未覆盖的 branch 会直接构造
  new target entry。

每个 `TargetUpdateEntry` 都携带自己的 `actualBranch`，`taken`、`mispred` 和
target 都直接从这条 resolved branch 读取。entry 级 actual outcome 只来自
actual branch prefix；如果某个 predicted entry 没有对应 actual branch，就不会
构造 target update entry。因此 indirect target 修正会用该 entry PC 对应的真实
target，而不是从整个 `FetchTarget` 的单一 summary 里猜。

target update 不会为了每个 missing not-taken conditional branch 分配 target
entry。这类 branch 是 direction-training fact，不是 target-allocation fact。

## Resolve Queue Squash Rule

当更老的 FTQ entry redirect 时，fetch 会删除更年轻的 FTQ entries。resolveQueue
里同线程、更年轻 FTQ 的 pending entry 也必须同步删除。否则 wrong-path branch 可能
在 recovery 后继续训练，并且用到重建路径的 history。

清理规则和 `FetchTargetQueue::squashAfter()` 保持一致：

```text
drop if resolvedTid == tid && resolvedFTQId > squashFtqId
```

same-FTQ entry 不删除。它内部仍由 actual branch prefix 决定哪些 branch 能训练。

## What This Is Not

当前路径不是 metadata-free 的 TAGE reread 方案。模拟器仍然保存并使用 prediction-time
meta/history snapshot，因为 provider lookup、allocation 和 folded-history context
必须对应当时的预测。

它也不是逐 RTL signal 复刻。这里保留的是影响性能的因果链：

```text
actual branch set + prediction-time context
    -> explicit direction/target update entries
    -> predictor table update
    -> prediction quality and resolve queue pressure
```
