# 昆明湖 BPU 模块 gem5 文档


## 文档说明及背景知识

在昆明湖架构的前端中，分支预测单元（BPU）的gem5模型最为详尽，最接近RTL的实现。本文档旨在帮助读者理解BPU的gem5模型，允许读者进一步得对其进行修改。

我们建议读者在阅读后续内容前先了解前端BPU的架构，关注以下内容即可：

- **Reinman G, Austin T, Calder B. A scalable front-end architecture for fast instruction delivery[J]. ACM SIGARCH Computer Architecture News, 1999, 27(2): 234-245.**
  
    本篇论文介绍了解耦式前端的基本工作原理，与昆明湖架构极为相关，阅读第1,3 section来熟悉FTB，FTQ这些组成部分的职责和他们的交互方式。

- **xiangshan-design-doc/docs/frontend/BPU/index.md**
  
    昆明湖的前端架构延续了上述论文的思路，但也存在不同之处。阅读此文档，有助于读者构建对昆明湖前端架构的全局了解。我们的后续讨论也将基于昆明湖架构，这要求读者适应它的设计文档的术语，以及这些术语在昆明湖架构下的具体意义（很可能和其他论文存在出入）。

    虽然xiangshan-design-doc为每一个BPU的子模块都提供了详细的介绍，只希望从全局的视角理解BPU的读者只阅读index.md就足够了，读者可以将子模块视为黑箱，知道各子模块的职责和接口即可。




## BPU源代码的文件结构

熟悉了昆明湖的BPU架构后，我们就可以来尝试理解它在gem5模拟器里的实现。

首先，让我们看一下BPU的文件结构：

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
BPU的全部源代码都放在src/cpu/pred/中，读者会发现这个文件夹里还有很多其它文件被省略掉了，（比如 src/cpu/pred/ras.hh），这是因为香山的gem5模型是基于官方gem5开发的。而官方gem5的代码库里本来就提供一些常见微架构的模拟代码（gem5 stdlib？）。我们在香山的gem5代码库中，保留了这些文件，但是实际使用的则是以上列举的文件。可以看到真正使用的文件大部分都在src/cpu/pred/ftb/中。

其中decoupled_bpred.cc是BPU的顶层文件，它提供接口供其他组件使用（比如前端的顶层文件（fetch.cc？）），同时它也包含了BPU的运行逻辑，并在内部调用各个子模块的接口来完成BPU的功能。

每一个子模块都在自己的文件中，例如：

1. btb.cc：BTB的实现
2. btb_tage.cc：TAGE的实现  
3. ras.cc：RAS的实现

最后，我们在stream_struct.hh中定义了一些公共的结构体，比如Fetch Block，FullPrediction等。

## BPU顶层文件：decoupled_bpred.cc/hh

下面将介绍BPU运行过程中较为关键的几个流程，或者涉及BPU的流程，希望通过这些流程，读者能更直观的理解BPU的接口的意义和用法。

更多关于BPU和Fetch交互的细节，请参见[BPU 与Fetch 交互](frontend_guide.md#interaction-with-fetch)。

### Fetch::tick()访问FTQ

Fetch::tick()是整个前端的顶层函数，而其中的fetch()函数负责模拟IFU的运行，目前我们没有模拟IFU的微架构，而是在一个tick内，根据FTQ项，获取整条Fetch Block的指令码，并人工地增加延迟。以下是Fetch::fetch()中访问FTQ的流程：

- 通过主循环遍历当前Fetch Block中的指令。
- 在每条指令的处理过程中，调用BPU::decoupledpredict()接口，访问FTQ，获取next_pc。
- BPU::decoupledpredict()可以被视作FTQ的一个access helper function，了解微架构的读者可能会发现模型和RTL在这里的差异：前者需要在fetch每一条指令时都访问该指令所在的FTQ项，而后者则是一次性的收到一个FTQ项作为流水线的输入。

下面提供一段inline过的伪代码，只对BPU感兴趣的读者可以重点关注BPU和FTQ的接口的调用：
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
> - [BPU 与 fetch的接口](frontend_guide.md#detailed-explaination-of-interface-with-fetch)


### FTQ转发redirect信号以及对BPU子预测器的更新

我们的微结构中，FTQ会将来自后端或IFU的redirect信号转发给BPU，同样的，当FTQ收到来自commit stage的信号，得知fetch block被全部提交后，也会用它存储的meta信息更新BPU的子预测器。我们的gem5模型中也做了等效的处理，只不过我们把这两种逻辑放在BPU::controlSquash()和BPU::update()中。而它们的调用是发生在Fetch::checkSignalAndUpdate()中，下面是伪代码：


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

BPU::tick()是BPU的顶层函数，它负责模拟分支预测流水线，也就是从S0 pc生成三个stage的预测结果，再将最准确的结果推到FTQ中的过程。BPU在模型上的原理和微架构类似：

```
[UBTB] ->   [BTB/TAGE/ITTAGE]
   ↓         ↓           ↓
   └──→ [FetchStream Queue] 
              ↓
     [FetchTarget Queue]
              ↓
     [Instruction Cache]
```

每个预测器都会每拍生成预测结果，其中BTB/uBTB 生成最核心的BTBEntry, 
然后其他预测器按需填入对应的方向或者别的信息，
共同生成每一级的FullBTBPrediction(predsOfEachStage), 最后3选1得到最终的FullBTBPrediction（finalPred）；

下一拍会根据finalPred 结果生成一项FSQEntry 放入FSQ 中

再下一拍会用FSQEntry生成一个FTQEntry 放入FTQ中


这里需要注意模型对微架构的一个简化：

在微架构中，对于某一个s0 pc，它的S1到S3的预测结果是在三拍中相继生成的，而我们的模型在获得s0 pc后的第一拍就生成了S1到S3的预测结果。如果我们假设没有override*发生，
那么这个简化与微架构是等效的。但是细心的读者会发现：在有override的情况下，微架构会在产生override的那拍重定向s0 pc，并覆写已经写入的FTQ项。也就是说最终的预测结果在发生
override的那拍才会写入FTQ。这样一来我们的模型就与微架构不一致了。为了弥补这个差异，我们为有override的s0 pc手动加入相应的延迟。具体做法是通过变量numOverrideBubbles。





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
|  |  |--makeNewPrediction(true);
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


### 正在修订的内容

###### 模型运行过程中，一个fetch block的生命周期

###### 其他模型的技巧：FSQ+FTQ = RTL的FTQ

###### 数据结构的关系（BTBEntry vs FullBTBPrediction）

###### 关键函数解释（参见cpu/pred/README.md）

###### 特殊功能

###### 变量解释

###### 子预测器的抽象


