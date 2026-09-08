# PairTAGE 2 Taken 预测器设计说明

PairTAGE 是一个类似于 TAGE 的分支预测器，但是其主要区别在于每个 Entry 可以存储接续的一对表项，以做到在每个周期中预测并产生两个连续的取指块。在部分切片中，可以做到提高前端供指能力的效果，进而更充分利用 KMHV3 的 8 发射结构。

本文档主要面向 GEM5 模型上实现代码的导读与审计，梳理其中的功能逻辑，发掘潜在问题。虽然也会尝试区分针对建模的、功能函数上的解读与针对微架构的、部件结构的解读，肯定还是有比较耦合的地方。不过在主要思想上和主要实现瓶颈上的表述应该是相对恰当的。

## PairTAGE 预测器结构描述
一个 PairTAGE Entry 中会存放两个取指块信息（PairBlockInfo）字段以及通用（Common）的几个字段。

_代码位置：_`pairtage.hh: 48 - 147`

| Field | Type | 含义 |
| --- | --- | --- |
| valid(?) | PairBlockInfo | 信息是否有效 |
| taken | PairBlockInfo | Branch PC 是否跳转 |
| fallThrough(?) | PairBlockInfo | 这个 block 是否仅用于标记 Fall-Through |
| branch pc | PairBlockInfo | Control-Flow PC；fall-through 时通常是 block start（？） |
| target pc | PairBlockInfo | taken target 或 fall-through target |
| isCond | PairBlockInfo | 是否条件分支 |
| isDirect | PairBlockInfo | 是否直接跳转 |
| isCall | PairBlockInfo | 是否 Call |
| isReturn | PairBlockInfo | 是否 Return |
| size(?) | PairBlockInfo | 指令流长度 |
| valid | Common | 整个 TAGE Entry 是否有效 |
| tag | Common | Entry Tag |
| counter(?) | Common | Entry 方向/置信度 Counter |
| useful | Common | 替换保护位 |
| **identifyConfidence** | Common | Branch 接续关系的可信程度/整体可信度 |


Review 提问：

1. 在 Common 中有 Valid 的情况下，PairBlockInfo 中的 valid 是否功能上重复？
2. 标记 Fall-Through 是什么特性？为什么 Branch PC 不是末尾而是开始？
3. isCond isDirect isCall isReturn 的作用是什么？
4. Size 是必要的吗？
5. Counter 在传统 TAGE Entry 中会通过增减来标注这一个 Entry 的方向性，而在 PairTAGE 表项中，每个 Block 的信息已经存放在了 PairBlockInfo 中。这个 Counter 在功能上是不是冗余了？

### PairTAGE 内部预测行为时序
对于 PairTAGE 这个 Feature 的改动很大，包括了 PairTAGE 预测器内部的实现，也包括顶层 BPU 与 Frontend 上许多部件与数据通路模型的一些改动。为了表达清晰和方便，对于 PairTAGE 外的改动会放在之后讲解。

在模型上，PairTAGE 预测时和其他预测器一样通过 `putPCHistory` 入口查表。在存放历史到 meta 并清理其中的预测信息后，会通过 `lookupEntry` 进行查表。

`lookupEntry` 会继续调用 `lookupProviders` 函数进行查表。

在结构逻辑上，会从最高表向最低表遍历，在表上基于 Start PC 和历史进行哈希或折叠等操作得出 Index 和 Tag，进而索引命中的 Entry。其中，最高表的结果为主结果（Main），次高表的结果是可选结果（Alternative）。

在这一情况下，在一个表的一个 Set 中比较 Tag 时，有一定可能会命中多路的 Entry，此时会择优选择。代码实现在 `betterProviderCandidate` 中实现并在 `lookupProviders` 相应位置中被调用。更优逻辑如下：

1. 显然，如果有一方是未被命中或不匹配的，则直接放弃（不过在模型上一般就不会执行比较逻辑了）；
2. identifyConfidence 更高者优先；
3. 相同 identifyConfidence，Useful Bit 置一者优先；
4. 上述条件没区别，Entry 中有有效 Second Block Info 者优先；
5. 上述条件没区别，比较 Counter 大小（由于 Counter 本身是否有必要存疑，可能不需要）；
6. 所有条件都没区别，则选择先命中的优先（在模型中会通过比较 Candidate 和 Current 的 Way 来做到，不过这属于模型的实现方法，和微架构无关）。

在查表结束后，PairTAGE 会将 Main 结果的 First Block 保留到 Meta 中，产生这一级预测，并单独保留 Second Block 到字段 `secondPredBlock` 中，用于顶层 BPU 单独获取。**这相当于对于 First Block 和 Second Block 有并行的数据通路。其中 First Block 正常参与多级预测流程，复用原有通路，像其他流水级预测器一样，这一预测结果可以被更高级预测器覆盖，并产生覆盖气泡。**

特别的，由于在 GEM5 上的多级预测代码框架比较特殊，所以在这里也解释一下将预测填入预测结果的函数 `fillStagePrediction` 中的代码逻辑：

1. 首先清除 `pred` 这一级流水的预测结果；
2. 然后检查 First PairBlockInfo 的结果，如果 Valid Bit 为假或者 Block Info 内的 fallthrough 为真则驳回预测；
3. 通过 `buildBTBEntry` 函数将这个 PairBlockInfo 转化为一个 BTB Entry；
4. 基于预测块的分支类型进行对应处理。

而对于另一条数据通路处理 Second Block 时，会在 BPU 顶层调用 PairTAGE 中的 `getSecondPredBlock` 来获取。

Review 提问：

1. 为什么 `lookupEntry` 和 `lookupProviders` 都实现了两份？这是不是没必要，还是说 MicroTAGE / TAGE 模型中有类似设计？
2. 在预测时查表结束后，直接丢弃了次选表结果，为什么？
3. 这里有一个 `builtBTBEntry` 函数，而在 BPU 顶层上也有一个 `buildPairBlockEntry` 函数，这两者是否定位上比较重复？

### PairTAGE 内部训练行为时序
PairTAGE 会利用一个来自于高级预测器的更可信的 Block Pair 进行训练，其中第一个 Block 利用在多级预测完成后最终形成的 Fetch Target Entry 取指块训练，第二个 Block 则利用 BPU 中对 MainBTB 和 TAGE 多查一次表构造的预测结果训练。

在代码实现上，它的训练入口在 `trainFromS3Pred` 上。在这个函数的内部处理逻辑中，会先取出 Meta 相关信息，并在 `lookupProviders` 使用这些信息重新查表。

BPU 在 first block 入队时调用 `buildTrainPacketFromPredForFirstBlock(finalPred)`，在 two-taken teacher 准备阶段调用 `buildTwoTakenTrainPacket(startPC, phase, btbEntries, condTakens)`；PairTAGE 内部再用 `buildTrainingBlock(packet)` 把 `TrainPacket` 转成 `PairBlockInfo`。

1. 如果取指块预测为跳转，先遍历取指块中的 BTB Entries 找到跳转的那个 BTB Entry 作为 Training Entry，如果跳转但找不到这个 BTB Entry 说明指令块结构非法；
2. 如果没有条件分支，则寻找直接跳转；
3. 也没有直接跳转，构造 Fall-Through

_注：怀疑这里的代码实现可能有问题，起码内部逻辑需要重构_

特别的，对于 First Block 和 Second Block 处理和过滤逻辑不同。First Block 由于在多级预测通路上，所以支持大部分分支类型，Second Block 则仅支持可以在 MainBTB 和 MainTAGE 查到的分支类型。

如果第一个训练块无效，则会清除表项。`trainStandaloneFallThrough` 会决定在第一个 Block 为 Fall-Through 的情况下是否可以接受第二个 Block。

如果训练块有效，则此时会将主表和次选表查到的结果和训练块进行一系列比对。其中，Blocks Match 比对完整 BlockInfo，而 Block Identify Match 仅比对 `fallthrough` `branch_pc` `isXXX` `size` 几个字段。

在特征不匹配（Identify Mismatch），且可向更高表分配时，先检查预测时差的表项的特征置信度（即 Identify Confidence），如果此时置信度为零则直接覆写。如果特征匹配，则强化 Counter、特征置信度、Useful Bit 等指标。

如果在重新查表时发现 PairTAGE 本身查表缺失，或者需要往高表分配，则进行分配行为。在模型上调用入口为 `allocateEntries` 函数，处理逻辑如下：

1. 如果提供的表已经是最高表，不再继续分配；
2. 从提供预测的表向最高表遍历，通过 Set 和 Tag 索引目标表项。如果有某路表项有相同 Tag 或相同 Identity，但是内容不一致的表项，优先被覆写，如果没有这种表项则取空路的表项分配；
3. 每 256 个无候选周期重置 Useful Bit，如果有候选则重置周期计数器减一；
4. 使用 LFSR 算法随机挑选一个表进行新项分配；
5. 利用传入的相关信息填入 Block Info。

整体来看，PairTAGE 内部的训练逻辑看下来有很多地方适合重构和修改，如果有功能问题也可以在这里重点看看。

Review 提问：

1. 第二个 Block 真的有必要将 MBTB 和 TAGE 的结果组合成一个 FullBTBPrediction 传进来吗？是不是太重了？
2. 训练块构建的相关函数逻辑命名混淆，是否可以优化？
3. `buildTrainingBlockResult` 内部代码可读性太差，实现复杂，是否可以重构？
4. 感觉 `buildTrainingBlock` 没必要实现，First Block 和 Second Block 用同一个 `buildTrainingBlockResult` 但是过滤需求不同，是不是复杂性的根源？
5. 第一个预测块无效就清除表项，没有检查置信度，是不是不应该这样做？
6. `skipStandaloneFallThrough` 条件处理太复杂，是否可以优化？
7. 重新查的结果和主表/次选表结果比较逻辑太复杂了，是否可以简化？

## BPU 顶层数据通路变更描述
在当前设计中，PairTAGE 的训练没有走解析通路或提交通路，利用后端传回的信息进行训练。它利用高级预测器的第三级预测信息以及额外为 Second Block 多查一次 MainBTB 表和 TAGE 表得到的信息来训练。也就是说，模型上主要的预测和训练的功能接口都在 `tick` 函数内，不会分离。除此之外，从 RTL 落地角度看，对 MainBTB 和 TAGE 多查一次表的操作可能是一个主要的设计障碍。

```latex
   BPU tick
    ├─ reset per-thread PairTAGE TrainPacket / twoTaken temporary state
    │
    ├─ requestNewPrediction(curTid)
    │   ├─ init stage preds
    │   ├─ pairtage->setPredictionPhase(s0PairPhase)
    │   ├─ components[i]->putPCHistory()
    │   │    └─ PairTAGE 输出 first block 到 stagePreds，暂存 secondPredBlock/meta
    │   └─ generateFinalPredAndCreateBubbles()
    │
    ├─ processNewPrediction(tid)
    │   ├─ createFetchTargetEntry(tid)
    │   ├─ finalTrainPacket = pairtage->buildTrainPacketFromPredForFirstBlock(finalPred)
    │   ├─ s0PC = finalPred.getTarget()
    │   ├─ updateHistoryForPrediction(entry, finalPred)
    │   ├─ fillAheadPipeline(entry)
    │   ├─ ftq.insert(firstEntry)
    │   └─ advancePairPhase()
    │
    ├─ prepareTwoTakenTraining(tid)
    │   ├─ MBTB getPredictedEntriesNoSideEffect(s0PC)
    │   ├─ TAGE lookupNoSideEffect() or BTB counter fallback
    │   └─ twoTakenTrainPacket = pairtage->buildTwoTakenTrainPacket(...)
    │
    ├─ processTwoTakenBlock(tid)
    │   ├─ phase / first-block-match / FTQ-space / second-valid / teacher-match checks
    │   ├─ build FullBTBPrediction secondPred from PairTAGE second block
    │   ├─ merge teacher direct-cond entries before selected branch
    │   ├─ refreshTwoTakenPredictionMetas(tid, secondPred)
    │   ├─ createFetchTargetEntry(tid, s0PC, secondPred)
    │   ├─ updateHistoryForPrediction(entry, secondPred)
    │   ├─ fillAheadPipeline(entry)
    │   ├─ ftq.insert(secondEntry)
    │   ├─ pairtage->recordTwoTakenBlockEnqueued()
    │   └─ advancePairPhase()
    │
    └─ pairtage->trainFromS3Pred(finalTrainPacket, optional twoTakenTrainPacket)
```

如上是顶层 `tick` 内的调用关系。原有的 `requestNewPrediction` 和 `processNewPrediction` 和主线结构没有太大变化，基本相当于把 PairTAGE 的第一预测块结果插入到多级预测结构的数据通路中。在原本的多级预测结束以及历史推进完成后，BPU 会利用新产生的 S0 PC 对 MainBTB 和 TAGE 进行一次无副作用（No Side Effects）的查表，接着利用查表的结果判断 PairTAGE 产生的第二预测块是否预测准确，从而确定是否要将第二个预测块转换为额外的一个取指块并入队 FTQ。最后，调用上一章节讲解过的 `trainFromS3Pred` 函数，以多级预测最终产生的取指块和额外查表的结果作为依据对 PairTAGE 进行更新。

### 预测、取指块入队与历史推进时序描述
参考 `tick` 内顺序介绍模型实现。在进入 `tick` 时，在完成初始化的一些逻辑后，首先会进入 `requestNewPrediction` 函数。这一函数会对各个预测器通过 `putPCHistory` 进行查表产生预测。在这里会调用到 PairTAGE 的预测查表接口，将查出的第一个预测块信息转化为正常预测结果，填入多级预测结果中，并暂存查表结果中的第二预测块信息。

接着执行 `processNewPrediction`。当前它负责把 `finalPred` 转成 first-block `FetchTarget` 并尝试入队，主要步骤是：

1. 若没有有效预测、override bubble 尚未消耗完、FTQ 满或 PC 无效，则返回；
2. 调用 `createFetchTargetEntry(tid)` 创建 first-block entry；
3. 若 PairTAGE 启用，调用 `buildTrainPacketFromPredForFirstBlock(finalPred)`，把最终 first-block 预测转成 `finalTrainPacket`；
4. 把 `s0PC` 推进到 `finalPred.getTarget(predictWidth)`；
5. 调用 `updateHistoryForPrediction(entry, finalPred)` 推进全局历史、路径历史、各组件 folded history、RAS/MGSC 状态；
6. 调用 `fillAheadPipeline(entry)`；
7. 将 first-block entry 插入 FTQ；
8. 推进 `s0PairPhase`，并设置 `firstBlockProcessedThisTick=true`。

### PairTAGE 第二预测块处理与训练数据通路
这一部分代码主要在 BPU 的 `tick` 函数中最后调用，设计比较大的改动。

`prepareSecondBlockTrainingPrediction` 内部会先创建一个空的 `FullBTBPrediction` 结构体，然后通过调用 MainBTB 新增的 `getPredictedEntriesNoSideEffect` 和 TAGE 的 `lookupNoSideEffect` 获得预测信息并填入，形成第二个预测结果。

这个来自高级预测器的信息第一次被使用是在 `processSecondBlock` 函数内。这个函数首先会创建一个空的取指块到 `pendingSecond` 中，接着调用 `pairtageFirstBlockStatusForSecondBlock` 函数对 PairTAGE 第一个预测块进行校验：

1. 首先调用 `buildFirstTrainingPairBlockFromPrediction` 重新从 Final Pred 构建 PairBlockInfo，然后和从 PairTAGE Meta 拿到的第一个块的预测信息比较，并返回比较结果；
2. 在不一致时，说明第一预测块被高级预测器覆盖，此时 `processSecondBlock` 会直接返回；
3. 调用 PairTAGE 的 `getSecondPredBlock` 接口来获得之前保存的第二个预测块信息；
4. 调用 `buildSecondTrainingPairBlockFromPrediction` 将第二个预测结果转换为预测块信息，与 PairTAGE 预测进行比较，然后调用 `pairBlocksMatch` 进行比较，如果不匹配则跳过入队；
5. 在前述校验结束后，调用 `buildPredictionFromPairBlock` 将 PairTAGE 的第二预测块转化为 `FullBTBPrediction`；
6. 调用 `mergeSecondBlockTeacherContext` 将 MainBTB + TAGE 中额外查到的表项合并到预测中，否则重定向时会导致高级预测器无法预热；
7. 调用 `refreshSecondBlockPredictionMetas` 函数，内部会遍历每个预测器并调用 `refreshPredictionMeta`，构造基于第二个块的各个预测器元信息，否则如果在 FTQ Entry 中存入了错误的 Meta，未来训练或恢复时可能会产生功能正确性的问题；
8. 调用 `createFetchTargetEntry`，构建取指块；
9. 调用 `updateHistoryForPrediction` 和 `fillAheadPipeline`，推进历史并入队第二个取指块。

Review 提问：

1. MainBTB 和 TAGE 的无副作用查表接口命名上统一。如果不是沿用内部其他接口的命名，是不是应该改一下？
2. 还需要单独先校验一遍 First Block 吗？这样很复杂，是不是用 Pred Source 之类的就足够了？
3. 校验相关函数实现应该简化或重新命名。
4. 整体上，对于 FTQ Entry，BTBPrediction，PairBlockInfo 这几个结构体间转化和比较的方式规划不够清晰，导致产生了不必要的设计。

### 各个原有子预测器接口变更描述
各个子预测器修改的动机在上述讲解之后应该比较清晰。除了 PairTAGE 预测器以外，对于其他预测器最主要的改动有两类：

1. `refreshPredictionMeta(startAddr, history, pred)`：用于 second block 额外入队前刷新各组件 meta，保证未来训练或 squash 恢复使用的是基于 second block start PC 和当前 speculative history 的元信息。
2. no-side-effect lookup：MBTB 提供 `getPredictedEntriesNoSideEffect()`，TAGE 提供 `lookupNoSideEffect()`；前者给 second block teacher 提供 BTB entries，后者只对这些 entries 中的 conditional branch 生成方向预测。

这一部分改动相对独立，所以在 Review 上和代码质量提高上暂时不是很紧急。后续可以继续优化。

