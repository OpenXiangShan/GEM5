# 昆明湖 BPU 模块 gem5 文档


## 文档说明及背景知识

在昆明湖架构的前端中，分支预测单元（BPU）的 gem5 模型最为详尽，最接近 RTL 的实现。本文档旨在帮助读者理解 BPU 的 gem5 模型，允许读者进一步得对其进行修改。

我们建议读者在阅读后续内容前先了解前端 BPU 的架构，关注以下内容即可：

- **Reinman G, Austin T, Calder B. A scalable front-end architecture for fast instruction delivery[J]. ACM SIGARCH Computer Architecture News, 1999, 27(2): 234-245.**
  
    本篇论文介绍了解耦式前端的基本工作原理，与昆明湖架构极为相关，阅读第 1,3 section 来熟悉 FTB，FTQ 这些组成部分的职责和他们的交互方式。

- **xiangshan-design-doc/docs/frontend/BPU/index.md**
  
    昆明湖的前端架构延续了上述论文的思路，但也存在不同之处。阅读此文档，有助于读者构建对昆明湖前端架构的全局了解。我们的后续讨论也将基于昆明湖架构，这要求读者适应它的设计文档的术语，以及这些术语在昆明湖架构下的具体意义（很可能和其他论文存在出入）。

    虽然xiangshan-design-doc为每一个BPU的子模块都提供了详细的介绍，只希望从全局的视角理解BPU的读者只阅读index.md就足够了，读者可以将子模块视为黑箱，知道各子模块的职责和接口即可。




## BPU 源代码的文件结构

熟悉了昆明湖的 BPU 架构后，我们就可以来尝试理解它在 gem5 模拟器里的实现。

首先，让我们看一下 BPU 的文件结构：

```
src/cpu/pred/
├── btb/
│   ├── btb.cc
│   ├── btb.hh
│   ├── btb_ittage.cc
│   ├── btb_ittage.hh
│   ├── btb_sc.cc
│   ├── btb_sc.hh
│   ├── btb_tage.cc
│   ├── btb_tage.hh
│   ├── btb_ubtb.cc
│   ├── btb_ubtb.hh
│   ├── decoupled_bpred.cc
│   ├── decoupled_bpred.hh
│   ├── fetch_target_queue.cc
│   ├── fetch_target_queue.hh
│   ├── folded_hist.cc
│   ├── folded_hist.hh
│   ├── history_manager.hh
│   ├── jump_ahead_predictor.hh
│   ├── loop_buffer.hh
│   ├── loop_predictor.hh
│   ├── ras.cc
│   ├── ras.hh
│   ├── stream_struct.hh
│   ├── test/
│   ├── timed_base_pred.cc
│   ├── timed_base_pred.hh
│   ├── uras.cc
│   └── uras.hh
├── ftb/
├── stream/
├── BranchPredictor.py
├── README.md
├── SConscript
...
```
BPU 的全部源代码都放在 src/cpu/pred/中，读者会发现这个文件夹里还有很多其它文件被省略掉了，（比如 src/cpu/pred/ras.hh），这是因为香山的gem5模型是基于官方gem5开发的。而官方gem5的代码库里本来就提供一些常见微架构的模拟代码（gem5 stdlib？）。我们在香山的 gem5 代码库中，保留了这些文件，但是实际使用的则是以上列举的文件。可以看到真正使用的文件大部分都在 src/cpu/pred/ftb/中。

其中 decoupled_bpred.cc 是 BPU 的顶层文件，它提供接口供其他组件使用（比如前端的顶层文件（fetch.cc？）），同时它也包含了 BPU 的运行逻辑，并在内部调用各个子模块的接口来完成 BPU 的功能。

每一个子模块都在自己的文件中，例如：

1. btb.cc：BTB 的实现
2. btb_tage.cc：TAGE 的实现  
3. ras.cc：RAS 的实现

最后，我们在 stream_struct.hh 中定义了一些公共的结构体，比如 Fetch Block，FullPrediction 等。

## BPU 顶层文件：decoupled_bpred.cc/hh

下面将介绍 BPU 运行过程中较为关键的几个流程，或者涉及 BPU 的流程，希望通过这些流程，读者能更直观的理解 BPU 的接口的意义和用法。

更多关于 BPU 和 Fetch 交互的细节，请参见[BPU 与 Fetch 交互](frontend_guide.md#interaction-with-fetch)。

### Fetch::tick() 访问 FTQ

Fetch::tick() 是整个前端的顶层函数，而其中的 fetch() 函数负责模拟 IFU 的运行，目前我们没有模拟 IFU 的微架构，而是在一个 tick 内，根据 FTQ 项，获取整条 Fetch Block 的指令码，并人工地增加延迟。以下是 Fetch::fetch() 中访问 FTQ 的流程：

- 通过主循环遍历当前 Fetch Block 中的指令。
- 在每条指令的处理过程中，调用 BPU::decoupledpredict() 接口，访问 FTQ，获取 next_pc。
- BPU::decoupledpredict() 可以被视作 FTQ 的一个 access helper function，了解微架构的读者可能会发现模型和 RTL 在这里的差异：前者需要在 fetch 每一条指令时都访问该指令所在的 FTQ 项，而后者则是一次性的收到一个 FTQ 项作为流水线的输入。

下面提供一段 inline 过的伪代码，只对 BPU 感兴趣的读者可以重点关注 BPU 和 FTQ 的接口的调用：
```
// IFU的顶层函数
Fetch::tick()
|  // helper function
|--Fetch::fetch()
|  |  // 主循环，每个迭代对应一条指令的fetch
|  |--while( numInst < fetchWidth && ! predictBranch)
|  |  |
|  |  |  // helper function
|  |  |--predictBranch |= Fetch::lookupAndUpdateNextPC( next_pc)
|  |  |  |  // 调用BPU的接口，获取next_pc和跳转与否
|  |  |  |--BPU::decoupledpredict(&pc)
|  |  |     |  // 调用FTQ的接口，获取当前正在读取的FTQ项（对应RTL的ifuPtr）
|  |  |     |--FTQ::getTarget()
|  |  |     |--set(pc, target)
|  |  |     |--if(run_out_of_this_entry)
|  |  |     |  FTQ::processFetchTargetCompletion()
|  |  |--set(this_pc, *next_pc)
|--BPU::trySupplyFetchWithTarget()
```

>相关内容链接：
> - [BPU 与 fetch 的接口](frontend_guide.md#detailed-explaination-of-interface-with-fetch)


### FTQ 转发 redirect 信号以及对 BPU 子预测器的更新

我们的微结构中，FTQ 会将来自后端或 IFU 的 redirect 信号转发给 BPU，同样的，当 FTQ 收到来自 commit stage 的信号，得知 fetch block 被全部提交后，也会用它存储的 meta 信息更新 BPU 的子预测器。我们的 gem5 模型中也做了等效的处理，只不过我们把这两种逻辑放在 BPU::controlSquash() 和 BPU::update() 中。而它们的调用是发生在 Fetch::checkSignalAndUpdate() 中，下面是伪代码：


```
Fetch::tick()
|
|--Fetch::checkSignalAndUpdate()
   |
   |--if(fromCommit->commitInfo[tid].squash)
   |  BPU::controlSquash()
   |--BPU::update()

```


### 分支预测流水线

BPU::tick() 是 BPU 的顶层函数，它负责模拟分支预测流水线，也就是从 S0 pc 生成三个 stage 的预测结果，再将最准确的结果推到 FTQ 中的过程。BPU 在模型上的原理和微架构类似：

```
[UBTB] ->   [BTB/TAGE/ITTAGE]
   ↓         ↓           ↓
   └──→ [FetchStream Queue] 
              ↓
     [FetchTarget Queue]
              ↓
     [Instruction Cache]
```

每个预测器都会每拍生成预测结果，其中 BTB/uBTB 生成最核心的 BTBEntry, 
然后其他预测器按需填入对应的方向或者别的信息，
共同生成每一级的 FullBTBPrediction(predsOfEachStage), 最后 3 选 1 得到最终的 FullBTBPrediction（finalPred）；

下一拍会根据 finalPred 结果生成一项 FSQEntry 放入 FSQ 中

再下一拍会用 FSQEntry 生成一个 FTQEntry 放入 FTQ 中


这里需要注意模型对微架构的一个简化：

在微架构中，对于某一个 s0 pc，它的 S1 到 S3 的预测结果是在三拍中相继生成的，而我们的模型在获得 s0 pc 后的第一拍就生成了 S1 到 S3 的预测结果。如果我们假设没有 override*发生，
那么这个简化与微架构是等效的。但是细心的读者会发现：在有 override 的情况下，微架构会在产生 override 的那拍重定向 s0 pc，并覆写已经写入的 FTQ 项。也就是说最终的预测结果在发生
override 的那拍才会写入 FTQ。这样一来我们的模型就与微架构不一致了。为了弥补这个差异，我们为有 override 的 s0 pc 手动加入相应的延迟。具体做法是通过变量 numOverrideBubbles。





*：override：一旦高级预测器在后续流水级的预测结果与已有结果不一致，就将会使用高级预测器结果作为新的输出更新后续 FTQ 中存储预测块结果 并重定向 s0 级 PC，清空新结果流水级之前的流水级的错误路径结果。
```
BPU::tick()
|
|--if(!receivedPred && numOverrideBubbles == 0)
|  // 生成最终预测结果，并创建override bubbles
|  BPU::generateFinalPredAndCreateBubbles()
|  |
|  |--finalPred = predsOfEachStage[numStages - 1]
|  |--numOverrideBubbles = firstHitStage
|  |--receivedPred = true
|
|--processEnqueueAndBubbles()
|  |
|  |--tryEnqFetchTarget();
|  |  |
|  |  |--if (!validateFTQEnqueue())
|  |  |  return;
|  |  |--ftq_entry = createFtqEntryFromStream(ftq_enq_state.streamId)
|  |  |--fetchTargetQueue.enqueue(ftq_entry);
|  |
|  |--tryEnqFetchStream();
|  |  |
|  |  |--if (!validateFSQEnqueue())
|  |  |  return;
|  |  |--processNewPrediction(true);
|  |  |  |
|  |  |  |--entry = createFetchStreamEntry();
|  |  |  |--s0PC = finalPred.getTarget(predictWidth);
|  |  |  |--updateHistoryForPrediction(entry);
|  |  |  |--fetchStreamQueue.emplace(fsqId, entry);
|  |  |  |--fsqId++;
|  |  |
|  |  |--receivedPred = false;
|  |  
|  |--if (numOverrideBubbles > 0)
|     numOverrideBubbles--;
|
|--requestNewPrediction()
   |
   |--if (!receivedPred)
      |--for (int i = 0; i < numComponents; i++)
         components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);
```

### `FetchTarget` / `BranchInfo` 中的 PC 视图

最近几轮 decoupled BTB 改动后，前端里和“控制指令 PC”相关的语义不再只有一个值，而是拆成了几种职责不同的视图。理解这些视图的区别，是理解跨块 4B RVI 控制指令行为的关键。

#### 1. `startPC`

- 表示该控制指令的**架构起始地址**，也就是指令前半部分所在的位置。
- 它仍然是统计、trace、fall-through 计算、RAS/uRAS call fall-through 推导时最稳定的语义来源。
- 对 2B 指令和不跨块的 4B 指令来说，`startPC` 通常也就是读者最直观理解的“这条 branch 的 PC”。

#### 2. `controlPC`

- 表示 predictor-visible 的控制指令视图。
- 在当前实现里，对跨块的 4B RVI 控制指令，`controlPC` 可以是**尾半字（tail halfword）**所在的 PC，而不再强制等于前半字的 `startPC`。
- 这样做的目的是让 decoupled BTB 在“当前块真正能触发预测的那个位置”上与 RTL 的边界更一致。
- 因此 `controlPC` 主要服务于：
  - BTB/TAGE/ITTAGE/uBTB 等预测器的 key / lookup / 覆盖判断
  - `predEndPC` 等 coverage 计算
  - fetch 侧“当前是否命中 taken 控制点”的判断

#### 3. `ownerStartPC`

- 这是 owner-migration 引入后的 fetch 侧语义，表示“当前 `FetchTarget` 真正拥有的最早 inst-start PC”。
- 默认情况下它等于 `startPC`；只有在 split-control owner migration 发生时，它才会小于当前 target 的 `startPC`。
- 典型场景是：
  - 一条 4B 控制指令前半部分落在当前块末尾
  - 尾半部分落在下一块开头
  - predictor 仍以“下一块”为 fetch target
  - 但真正应该在下一块消费的那条控制指令，其架构 `startPC` 仍位于上一块
- 这时 following target 会携带 `ownerStartPC < startPC`，表示它在 fetch/buildInst 阶段对这条跨块控制指令拥有所有权。

#### 4. `predEndPC`

- 表示当前 `FetchTarget` 的预测覆盖上界（右开区间）。
- fetch request、decode/buildInst 侧的“当前 PC 是否还属于本 target”判断，统一依赖 `[ownerStartPC, predEndPC)` 或 `[startPC, predEndPC)` 这样的 coverage 区间，而不是旧的固定大窗口。

### split-control owner migration 的 fetch 语义

在 tail-halfword control-PC 视图下，跨块 4B 控制指令的“触发预测位置”与“架构起始位置”可能已经不在同一个 fetch target 中。为了解决这个问题，fetch 侧引入了 owner migration：

- 当前正在 fetch 的 target 记为 `current`
- 顺序上的下一项 target 记为 `following`
- 若 `following` 声明 `ownerStartPC < following.startPC`，并且当前 inst-start PC 已进入这个 owner 区间，则 fetch 会先把 `current` 消费掉，再把这条跨块控制指令交给 `following`

这套语义只允许发生在**顺序相邻的 split-control handoff** 中，不允许把普通 taken-target target 误判成 owner handoff。当前代码里已经把这部分条件收敛成 `FetchTarget` helper，而不是继续散落在 `fetch.cc` 和测试里各写一套。

### trace / FS 模式下的边界

- FS/trace 模式都需要保留 `startPC` 作为架构语义来源，尤其是恢复、trace wrong-path NOP sizing、call fall-through 等路径。
- trace mode 不再在 fetch 阶段执行 FTQ owner migration；这样做的目的是避免 trace-only 的顺序消费路径受 FTQ owner handoff 状态机影响。
- 也因此当前可以把这几层语义理解为：
  - predictor key / trigger / coverage：优先看 `controlPC`
  - 架构归属 / stats / trace / fall-through：优先看 `startPC`
  - fetch owner handoff：看 `ownerStartPC`

### 这轮收敛后的读码顺序

如果要快速验证 owner-migration 语义是否仍然成立，建议按下面顺序读：

1. `src/cpu/pred/btb/common.hh`
   - 先确认 `startPC / controlPC / ownerStartPC` 的职责划分
   - 再确认 `FetchTarget` helper 是否仍然完整表达 owner contract
2. `src/cpu/o3/fetch.cc`
   - 看 `maybeMigrateSplitControlOwner()` 是否只消费 helper，而没有重新拼一套手写判定
   - 看 normal fetch 里的 in-owner-range / taken match 是否仍然走 helper
3. `src/cpu/o3/trace/TraceFetch.cc`
   - 看 trace mode 是否仍然保持“跳过 fetch-time owner migration、优先顺序消费”的边界
4. `src/cpu/pred/btb/test/btb.test.cc`
   - 看 helper 级测试是否还覆盖默认 owner、split-control handoff、taken match 等回归点

这样读的好处是把问题分成两类：

- 如果 helper 本身就错了，问题属于 `FetchTarget` 契约
- 如果 helper 正确、但 fetch 行为不对，问题才属于 fetch 消费流程


### 正在修订的内容

###### 模型运行过程中，一个 fetch block 的生命周期

###### 其他模型的技巧：FSQ+FTQ = RTL 的 FTQ

###### 数据结构的关系（BTBEntry vs FullBTBPrediction）

###### 关键函数解释（参见 cpu/pred/README.md）

###### 特殊功能

###### 变量解释

###### 子预测器的抽象


