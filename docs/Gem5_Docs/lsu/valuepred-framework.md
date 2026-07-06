# GEM5 值预测框架与新增 Predictor 示例

本文总结当前 `src/cpu/valuepred/` 的值预测框架，重点说明：

1. 当前值预测器和组合值预测器是如何接到 O3 流水中的。
2. 如果要新增一个自己的值预测算法，最小需要改哪些地方。
3. 新增的 `ExampleValuePredictor` 如何作为模板直接复制使用。

## 1. 当前框架的核心对象

### 1.1 `VPUnit`

所有值预测器都继承 `VPUnit`，统一实现四个接口：

- `predict(const VPPredictRequest &request)`
- `update(const VPUpdateInfo &updateInfo, const VPPredictionRecord *record, const VPFeedback &feedback)`
- `specUpdate(const VPSpecUpdateInfo &specUpdateInfo)`
- `squash(ThreadID tid, uint64_t seq_no)`

其中：

- `predict()` 在预测阶段调用，返回 `VPPredictionCandidate`
- `update()` 在 commit 阶段调用，拿到真实值和反馈信息
- `specUpdate()` 给需要投机更新的预测器预留
- `squash()` 给需要清理 in-flight predictor state 的实现预留

### 1.2 公共输入与私有状态

当前框架把“公共输入”和“预测器私有状态”分开处理。

公共输入：

- `VPPredictRequest`
  - 预测阶段公共字段：`pc / seqNo / tid`
- `VPUpdateInfo`
  - 更新阶段公共字段：`pc / seqNo / tid / actualValue / isMisprediction`

可选扩展：

- `VPPredictRequestExtension`
- `VPUpdateInfoExtension`

预测器如果需要额外信息，不要把字段硬塞进公共 struct，而是定义自己的扩展类，然后通过：

- `request.emplaceExt<MyPredictExt>(...)`
- `updateInfo.emplaceExt<MyUpdateExt>(...)`

在 predictor 内部再通过：

- `request.getExt<MyPredictExt>()`
- `updateInfo.getExt<MyUpdateExt>()`

把这些信息取出来。

预测器私有状态：

- `VPPredictionRecord`

如果某个 predictor 需要把 predict 阶段的私有上下文保留到 commit，再定义一个派生类挂到
`candidate.record` 上，commit 时再从 `record` 里恢复。

## 2. 当前 O3 流水怎样调用值预测

### 2.1 Fetch 侧

`src/cpu/o3/fetch.cc` 在发起值预测前构造 `VPPredictRequest`。

当前公共字段来自：

- `instruction->getPC()`
- `instruction->seqNum`
- `tid`

示例 predictor 额外演示了如何在 fetch 侧挂 predictor-specific 信息：

- `curTick()`
- `instruction->opClass()`

### 2.2 Commit 侧

`src/cpu/o3/commit.cc` 在训练前构造 `VPUpdateInfo`。

当前公共字段来自：

- `head_inst->getPC()`
- `head_inst->seqNum`
- `tid`
- `head_inst->actualValue`
- `head_inst->vpMisprediction`

此外还会构造 `VPFeedback`，用于告诉 predictor：

- 本次预测是否被选中
- 是否真的被流水线应用
- 当时是否提供了预测
- 预测值是什么
- 这次预测最终是否正确

示例 predictor 额外演示了如何在 commit 侧挂 predictor-specific 信息：

- `head_inst->effAddr`
- `head_inst->physEffAddr`
- `head_inst->effAddrValid()`

## 3. 组合值预测器 `CompositeValuePredictor`

`CompositeValuePredictor` 不是一个具体算法，而是一个组合框架。

它的职责是：

1. 对每个 child predictor 调用 `predict()`
2. 收集每个 child 的 `VPPredictionCandidate`
3. 交给 arbiter 选择最终采用哪个 child 的结果
4. 在 commit 时把统一的 `VPUpdateInfo + VPFeedback` fan-out 给各 child

当前 arbiter 是独立 SimObject，可配置为：

- 固定优先级
- 随机
- round-robin
- confidence-based

这意味着新增一个 direct-value predictor 时，通常不需要修改
`CompositeValuePredictor` 本身；只要它实现了 `VPUnit`，就可以直接作为 child 接进去。

说明：

- 当前 `MemoryRenaming` 仍然是比较特殊的一类实现，它的 `VPResult.value`
  现在更像 sideband 信息而不是直接注入的值，因此没有纳入当前 direct-value
  composite 主路径。

## 4. `ExampleValuePredictor` 模板说明

本次新增的示例文件有：

- `src/cpu/valuepred/example_value_predictor_metadata.hh`
- `src/cpu/valuepred/example_value_predictor.hh`
- `src/cpu/valuepred/example_value_predictor.cc`

它们展示了三件事。

### 4.1 如何定义 predict request 扩展

`ExamplePredictRequestExt` 继承 `VPPredictRequestExtension`，示例字段是：

- `predictTick`
- `opClass`

这类字段通常来自 fetch 调用点，而不是所有 predictor 都共享的稳定核心字段。

### 4.2 如何定义 update info 扩展

`ExampleUpdateInfoExt` 继承 `VPUpdateInfoExtension`，示例字段是：

- `hasMemoryAddr`
- `virtualAddr`
- `physicalAddr`

这类字段通常来自 commit 调用点，是某个特定 predictor 训练时才需要的附加信息。

### 4.3 如何留出算法空壳

`ExampleValuePredictionRecord` 现在也给了一个具体示例：

- `predictTick`

它表示 predictor-private 的 predict-time 状态如何跨流水级保留到 commit。

`ExampleValuePredictor::predict()` 当前故意不做任何预测，直接返回空的
`VPPredictionCandidate`。

不过它会始终分配一个 `ExampleValuePredictionRecord`，把 predict 阶段的
`predictTick` 存进去，专门用于演示“predict 产生私有状态，update 再读回来”
这一条链路。

你需要自行补的地方是：

- 查表/查缓存/查历史的预测逻辑
- 是否设置 `candidate.result.speculative = true`
- 预测值 `candidate.result.value`
- 若要参与 composite 仲裁，给 `candidate.score`
- 若 commit 需要恢复更多 predict-time 私有状态，继续扩展
  `ExampleValuePredictionRecord`

`ExampleValuePredictor::update()` 当前也故意不做训练，只保留了：

- 如何读取 `ExampleUpdateInfoExt`
- 如何把 `record` 转回自己的 record 类型
- 如何从 `ExampleValuePredictionRecord` 里读回 `predictTick`
- 如何接收统一的 `VPFeedback`

这正是新增 predictor 时最常见的骨架。

## 5. 新增一个值预测算法的最小步骤

如果你要基于当前框架新增自己的 predictor，建议按下面顺序做。

### 第一步：定义 predictor-specific 扩展

如果算法需要额外输入，先决定这些输入属于哪一类：

- 预测阶段信息：继承 `VPPredictRequestExtension`
- commit 更新信息：继承 `VPUpdateInfoExtension`
- predict 到 update 之间必须保留的私有上下文：继承 `VPPredictionRecord`

原则是：

- 公共字段留在 `VPPredictRequest / VPUpdateInfo`
- 预测器特有字段放扩展
- 只有必须跨阶段存活的 predictor-private 状态才放 record

### 第二步：实现一个新的 `VPUnit`

新增一个 predictor 类，继承 `VPUnit`，至少实现：

- `predict()`
- `update()`
- `specUpdate()`
- `squash()`
- `getValuePredictorType()`

推荐直接复制 `ExampleValuePredictor` 再改。

### 第三步：暴露 SimObject

需要同时修改：

- `src/cpu/valuepred/ValuePredictor.py`
- `src/cpu/valuepred/SConscript`

其中：

- `ValuePredictor.py` 负责暴露 Python 配置入口和 `ValuePredType`
- `SConscript` 负责加入新的 SimObject 和 C++ source

### 第四步：在 O3 调用点挂接扩展

如果你的 predictor 需要 fetch/commit 侧的额外信息，就在：

- `src/cpu/o3/fetch.cc`
- `src/cpu/o3/commit.cc`

构造请求时 `emplaceExt<YourExt>(...)`。

这是最容易漏掉的一步。很多“predictor 代码写好了但拿不到字段”的问题，
本质上都是调用点没有把扩展挂进去。

### 第五步：决定是否接入 composite

如果你的 predictor 输出的是“可直接注入的 speculative value”，通常可以直接放进：

```python
cpu.valuePred = CompositeValuePredictor(
    predictors=[
        ExampleValuePredictor(),
        EStride(),
    ]
)
```

如果它并不是 direct-value predictor，而只是输出 sideband 信息，则要先确认它是否适合
当前 composite 语义。

## 6. 一个最小配置示例

单独挂示例 predictor：

```python
from m5.objects.ValuePredictor import ExampleValuePredictor

cpu.valuePred = ExampleValuePredictor()
```

放到组合值预测器里作为模板 child：

```python
from m5.objects.ValuePredictor import (
    CompositeValuePredictor,
    CVPFixedPriorityArb,
    ExampleValuePredictor,
    EStride,
)

cpu.valuePred = CompositeValuePredictor(
    predictors=[
        ExampleValuePredictor(),
        EStride(),
    ],
    arb=CVPFixedPriorityArb(),
)
```

注意：

- `ExampleValuePredictor` 默认不会真正做预测，因此单独挂上去时不会改变行为
- 它存在的目的就是给新算法提供一份可直接修改的模板

## 7. 推荐阅读顺序

如果你第一次接触这套框架，建议按下面顺序看代码：

1. `src/cpu/valuepred/valuepred_unit.hh`
2. `src/cpu/valuepred/valuepred_metadata.hh`
3. `src/cpu/valuepred/composite_value_predictor.hh`
4. `src/cpu/valuepred/composite_value_predictor_arb.hh`
5. `src/cpu/o3/fetch.cc`
6. `src/cpu/o3/commit.cc`
7. `src/cpu/valuepred/example_value_predictor.hh`
8. `src/cpu/valuepred/example_value_predictor.cc`

看完这一圈，通常就已经能在当前框架上加一个新的 direct-value predictor 了。
