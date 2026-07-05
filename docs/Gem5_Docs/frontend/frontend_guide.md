# gem5 前端代码导读

### 整体架构

参考如下内容

[总体架构 - XiangShan 官方文档](https://docs.xiangshan.cc/zh-cn/latest/frontend/overview/)

[https://github.com/OpenXiangShan/XiangShan-Design-Doc](https://github.com/OpenXiangShan/XiangShan-Design-Doc) 这个设计文档

解耦前端整体架构

![](images/1733192628347-c025d3bd-73a3-479a-9d20-00a22969e633.svg)

BPU 内部架构

![](images/1733192616906-dc9fcf8c-e7d8-4257-bff9-7a2a5c6d6b43.svg)

**GEM5 整体工作流程**

bpu -> fsq -> ftq -> Fetch(ICache) -> Decode -> exe -> commit

其中 fsq = ftq, 基本作用一样，只是 fsq 遗留代码

RTL 中描述 FTQ 职责

1. 暂存 BPU 预测取指目标，供给给 IFU 取指令
2. 暂存 BPU 预测器预测信息，当指令提交后用这些信息更新预测器训练，需要维护指令从预测到提交的完整生命周期。后端需要 pc, 从 ftq 读取。

事实上在 GEM5 中，这些职责基本由 FSQ、FTQ 共同完成，后文讲解

### 关键数据结构

#### BPU 整体结构

整个预测器叫做 DecoupledBPUWithFTB，核心是 BPU, 是和前端解耦的，是以 FTB 为关键内容的。

继承 GEM5 原本的 BPredUnit 预测器结构，但基本不使用其中的函数了

```cpp
class DecoupledBPUWithFTB : public BPredUnit {
    FetchTargetQueue fetchTargetQueue;  // FTQ
    // BP -> FSQ -> FTQ -> Fetch unit
    std::map<FetchStreamId, FetchStream> fetchStreamQueue;  
    // FSQ: 完整指令流，直到 commit 才从 FSQ 中移除
    // 5 个预测器组件
    DefaultFTB *uftb{};
    DefaultFTB *ftb{};
    FTBTAGE *tage{};
    FTBITTAGE *ittage{};
    ftb_pred::uRAS *uras{};
    std::vector<TimedBaseFTBPredictor*> components{};   // 所有预测器（5 个）组件的 vector
    std::vector<FullFTBPrediction> predsOfEachStage{};  // 每个阶段的预测结果
    bool receivedPred{false};   // 是否收到 BPU 预测结果，generateFinalPredAndCreateBubbles() 中写入 true,tryEnqFetchStream() 中写入 false

    Addr s0PC;  // 当前 PC、预测 PC
    boost::dynamic_bitset<> s0History; // 全局分支历史，970 位，推测更新
    FullFTBPrediction finalPred;  // 最终预测结果，从 predsOfEachStage 中选择最准确的
}
```

内容主要包含 FTQ、FSQ、预测结果，全局历史等

接下来会详细介绍关键组件

#### FSQ entry = FetchStream

这里 FSQ 作用和 FTQ 作用一样，只是 GEM5 第一代预测器是 stream based 的，是遗留代码，但是承担了大部分 FTQ 的工作，第二代预测器是 FTQ/FTB based，新加的一部分就放在了 FTQ entry 中

多个（kmhv2 设置 64 个）FetchStream 组成了 fetchStreamQueue，用 FetchStreamId/FsqId 来索引。

```cpp
// BP -> FSQ -> FTQ -> Fetch unit
std::map<FetchStreamId, FetchStream> fetchStreamQueue;  
// FSQ: 完整指令流，可能产生多个 FTQ 条目，直到 commit 才从 FSQ 中移除
unsigned fetchStreamQueueSize;
FetchStreamId fsqId{1};  // 索引 ID
```

单个 FSQ entry 定义如下

```cpp
struct FetchStream
{
Addr startPC;  // FetchStream 的 start pc， [startPC, predEndPC)，为 s0PC

// 以下几个内容都由 finalPred 最终预测填入
bool predTaken;  // 预测跳转
Addr predEndPC;  // 预测的流结束 pc（fall through pc）
BranchInfo predBranchInfo;  // 预测跳转的最后一条分支信息
bool isHit;  // 是有效预测，finalPred.valid 有效
bool falseHit;  // 是否假命中？
FTBEntry predFTBEntry;  // 预测的 FTB 条目

// for update/recovery, actual CFIs accumulated by resolve/commit paths
std::vector<ResolvedBranch> resolvedBranches;  // 实际控制流结果集合

// update entries are built transiently from prediction meta/history and
// resolvedBranches, not stored as persistent stream fields

bool resolved;  // squash 后变为 true，并保存 squash 信息

// 重定向相关元信息，commit 设置并传给 BPU
int squashType;  // squash 类型，ctrl(分支 redirect) or trap or other
Addr squashPC;  // squash 的 pc
int squashSource;  // squash 来源，decode or commit

unsigned predSource;  // 预测来源

// prediction metas
std::vector<std::shared_ptr<void>> predMetas;  // 预测元数据，每个组件内容不同，记录预测时候信息，向下流水直到 commit, 用于 commit 更新预测器内容
boost::dynamic_bitset<> history; // 全局分支历史，970 位，推测更新，=s0History
```

这里记录了 FetchStream 大部分关键内容，其实最关键的

1. startPC，预测起始 PC, = s0PC, 各子预测器预测 PC
2. predTaken 到 predFTBEntry, 当各预测器生成完 finalPred 之后，填入预测内容
3. resolvedBranches 由 resolve/commit 路径累积实际 CFI，不再从预测字段伪造 actual result
4. update entry 在训练入口临时构造，输入是 prediction-time meta/history snapshot 加 actual branch set
5. resolved: 出现三种 squash(control/noncontrol/trap) resolved 为 true，并保存 squash 信息

这里旧版本曾有两组很类似的数据结构，分别保存 prediction summary 和 execution summary。当前更新协议应直接消费 prediction-time meta/history snapshot 加 `resolvedBranches` actual branch set，而不是在 `FetchTarget` 里二选一推导 actual 结果。

```cpp
    std::vector<ResolvedBranch> resolvedBranches;
    BranchUpdateContext ctx =
        makeBaseBranchUpdateContext(stream);
```

本质上对应两个阶段：

1. 预测阶段，resolved=0, ftb 内容生成 fsq 的 pred 信息，再传递给 ftq 对应内容（ftb->fsq->ftq)
2. resolve/commit 阶段，后端把真实 CFI 写入 `resolvedBranches`。训练时再由 `resolvedBranches` 和预测期 meta/history 构造 direction/target update entries；不要从预测字段反推 actual 结果

相当于 ftq 内容是 fsq 内容的压缩版，动态切换到对应版本上。

这里由于遗留代码，容易造成很多代码误解，删除 fsq 遗留代码需要之后完成！

#### FTQ entry

上一节讲到，FSQ + FTQ 共同构成了 RTL 中 FTQ 的作用，存储 fetchBlock 并一直到其中指令全部 commit 之后才删除对应的 entry, GEM5 的 FTQ entry 是 FSQ 中原本没存储的东西，是一个增量，每个 FSQ entry 对应一个 FTQ entry, 两者合并对应 RTL 的一个 FTQ entry.

```cpp
using FTQ = std::map<FetchTargetId, FtqEntry>;  // <id, FtqEntry>，map 字典，有 64 项
FTQ ftq;    // <id, FtqEntry>，map 字典，有 64 项，用 fetchDemandTargetId 索引
unsigned ftqSize;
FetchTargetId ftqId{0};  // this is a queue ptr for ftq itself
```

这里 FTQ 和 FSQ 一样，都是 map 组织，用 ftqId 来索引 entry, 注意，对于同一个 entry，ftqId = fsqId - 1, 因为 fsqId 从 1 开始计数，ftqId 从 0 开始，例如--debug-flags=DecoupleBP 输出结果

> Set instruction 1542 with stream id 112, fetch id 111
>

这说明这个指令同时来自 fsqId 112 和 ftqId 111

```cpp
struct FtqEntry
{
Addr startPC;  // 块的开始 pc，对应 fsq entry 的 startPC
Addr endPC;    // fallThrough pc, 对应 fsq entry predEndPC
Addr takenPC;  // 当是 taken 分支时，takenPC 是控制 PC, 当是未命中时，takenPC=0
// 对应 fsq_entry pred(exe)BranchInfo.pc

bool taken;  // 中间是否有 taken 分支
Addr target;
FetchStreamId fsqID;  // fsq 的 id, 用于 ftq 索引到对应的 Fsq!
```

当其中有 taken 分支时候，索引 fetchBlock = [startPC, takenPC]

没有 taken 分支， = [startPC, endPC], endPC = startPC + 32 (32 为预测块宽度)

可以看到 FtqEntry 中相对 FSQEntry 内容少了挺多，也可以看做是 FSQEntry 缩减版，很多内容等价于 fsq 内容

```cpp
DecoupledBPUWithFTB::setTakenEntryWithStream(const FetchStream &stream_entry, FtqEntry &ftq_entry)
{
    ftq_entry.taken = true;
    ftq_entry.takenPC = stream_entry.predBranchInfo.pc;
    ftq_entry.endPC = stream_entry.predEndPC;
    ftq_entry.target = stream_entry.predBranchInfo.target;
    ftq_entry.inLoop = stream_entry.fromLoopBuffer;
    ftq_entry.iter = stream_entry.isDouble ? 2 : stream_entry.fromLoopBuffer ? 1 : 0;
    ftq_entry.isExit = stream_entry.isExit;
    ftq_entry.loopEndPC = stream_entry.getBranchInfo().getEnd();
}
```

这里能看出来，ftq 的 takenPC, endPC, target,  几乎等于 fsq 的 predBranchInfo.pc, predEndPC, predTaken

建议结合 Debugflags 来共同查看其中内容，例如

```cpp
    void printFetchTarget(const FtqEntry &e, const char *when)
    {
        DPRINTFR(DecoupleBP,
                 "%s:: %#lx - [%#lx, %#lx) --> %#lx, taken: %d, fsqID: %lu, loop: %d, iter: %d, exit: %d\n",
                 when, e.startPC, e.takenPC, e.endPC, e.target, e.taken,
                 e.fsqID, e.inLoop, e.iter, e.isExit);
    }
```

打印了 ftq entry 具体内容，可以编写小测试，--debug-flags=DecoupleBP, 会在 decoupledPredict 函数调用时候打印当前 ftq 内容，如下

```cpp
looking up pc 0x80000130
Responsing fetch with:: 0x80000130 - [0, 0x80000150) --> 0, taken: 0, fsqID: 1962, loop: 0, iter: 0, exit: 0
Responsing fetch with:: 0x80000130 - [0x80000134, 0x80000150) --> 0x8000013a, taken: 1, fsqID: 1966, loop: 0, iter: 0, exit: 0
```

分别表示 startPC=80000130 开始的两个 ftq entry, 第一个预测不跳，takenPC=0, endPC=130+0x20 为默认 fallThrough; 第二个预测 taken, takenPC=134, endPC=150, taken taget = 13a

#### FTBEntry

FTB 存储分支预测的跳转地址，同时也存储方向信息，是 DecoupledBPUWithFTB 的核心模块，同时提供给 FullFTBPrediction 作为最终预测的一部分

这里的含义和 RTL FTB 基本一致，核心内容就是 tag + 2 个 slots + valid + fallThruAddr

```cpp
typedef struct FTBEntry
{
Addr tag = 0;   // FTB 块的 tag，由 FetchStream 的 start pc 计算
std::vector<FTBSlot> slots;     // 分支槽，最多两条分支（NT, T/NT)
Addr fallThruAddr;              // FTB 块的 fallThru 地址
bool valid = false; } 
```

```cpp
// FTBSlot 分支槽，继承 BranchInfo
typedef struct FTBSlot : BranchInfo
{
bool valid;     // 分支有效
int ctr;  // 2 位饱和计数器，只有 uFTB 使用！
}FTBSlot;

typedef struct BranchInfo {  // 核心就是 pc + target
Addr pc;  // 分支 PC
Addr target;  // 分支目标地址
bool isCond;  // 条件跳转
bool isIndirect;  // 间接跳转
bool isCall;  // 调用
bool isReturn;  // 返回
uint8_t size; // 指令长度
}BranchInfo
```

每个 slot 包含分支的跳转方向和目标地址

为了 uFTB 预测方向，还包含 2bit 饱和计数器。

#### FullFTBPrediction

对应 RTL 的 FullBranchPrediction，就是 3 级预测器，每个阶段的预测结果都存在这里，最终选出 finalPred

```cpp
std::vector<FullFTBPrediction> predsOfEachStage{};  // 3 级，每个阶段的预测结果
FullFTBPrediction finalPred;  // 最终预测结果，从 predsOfEachStage 中选择最准确的
```

```cpp
typedef struct FullFTBPrediction
{
Addr bbStart;  // 块的开始 pc
FTBEntry ftbEntry; // for FTB，保存整个 FTB，核心！
std::vector<bool> condTakens; // 用于条件分支预测器，两个条件分支是否 taken，TAGE 提供，uftb 结果由 2bit 饱和计数器提供

Addr indirectTarget; // for indirect predictor  间接跳转目标地址，ITTAGE 提供
Addr returnTarget; // for RAS，返回地址

bool valid; // hit
unsigned predSource; // 预测来源，哪一级预测的
boost::dynamic_bitset<> history;  // 历史
```

预测结果的核心内容是 ftbEntry, 其他包括分支跳转方向，历史等

这里的内容由每一级预测器填写，每个预测器填写内容不同

例如 uFTB 需要填写 ftbEntry（包括了分支信息和跳转地址），还需要填写 condTakens 预测是否跳转

FTB 只用填写 ftbEntry

TAGE 只用填写 condTakens 作为方向信息；ITTAGE/RAS 分别填写 indirectTarget 和 returnTarget

> 注意每个预测器填入不同级的预测结果中
>
> s0 的 predsOfEachStage[0] 只有 uFTB 填写
>
> s1  的 predsOfEachStage[1] 有 FTB、RAS、TAGE 填写，每个填写内容不应该重叠
>
> s2 的 predsOfEachStage[2] 由 ITTAGE、RAS 填写
>
> 最终 3 级预测得到 finalPred
>

#### 总结

这些数据结构整体关系可以看做

每个预测器都会每拍生成预测结果，其中 FTB/uFTB 生成最核心的 FTBEntry, 然后其他预测器按需填入对应的方向或者别的信息，共同生成每一级的 FullFTBPrediction, 最后 3 选 1 得到最终的 FullFTBPrediction（finalPred）；

下一拍会根据 finalPred 结果生成一项 FSQEntry 放入 FSQ 中

再下一拍会用 FSQEntry 生成一个 FTQEntry 放入 FTQ 中

最后 Fetch 函数会拿出 FTQEntry 来从 ICache 取指

> FsqId 会一直流水到 commit 阶段，直到 commit 后会提交给 Fetch 阶段用于更新各个预测器，更新后才删除对应的 FSQEntry 和 FTQEntry
>

对应的数据也是如此流动，例如

ftb.fallThrough -> finalPred.getFallThrougn -> fsq.predEndPC -> ftq.endPC

对应的关键数据结构中，部分数据变化对应如下

| **ftb entry** | **finalPred/fullPred** | **fsq entry/FetchStream** | **ftq entry** |
| --- | --- | --- | --- |
|  | bbStart | startPC | startPC |
| slots[0,1] | ftb_entry | predBranchInfo.pc <br/>= taken_slots | takenPC |
| fallThroughAddr | | predBranchInfo.target | target |
| | condTakens[0,1] | predTaken | taken |
| | | predEndPC=fallTrhoughAddr<br/>or startPC + 32 | endPC |

### 关键函数

#### 子预测器函数

每个预测器都会继承 TImedBaseFTBPredictor, 关键函数都继承它，并可能 override

```cpp
class TimedBaseFTBPredictor: public SimObject
{
// make predictions, record in stage preds 每拍做预测，记录在 stagePreds 中
virtual void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history, std::vector<FullFTBPrediction> &stagePreds) {}
// 获取预测元数据，相当于 checkpoints, 存储预测时候的状态，预测错误时候回滚，指令提交时候验证
virtual std::shared_ptr<void> getPredictionMeta() { return nullptr; }   
// 推测更新历史，只有 uRAS 实现！其他预测器不会推测更新！
virtual void specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) {}  
// // squash 后恢复历史，tage/RAS 实现，并更新 s0history
virtual void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) {}  
virtual void update(const FetchStream &entry) {}  // 用 commit stream 更新预测器内容，准确的！
// do some statistics on a per-branch and per-predictor basis
virtual void commitBranch(const FetchStream &entry, const DynInstPtr &inst) {}  // commit 阶段统计分支数据
```

其中最关键的就是两个函数！

1. putPCHistory: 每拍做预测
2. update: 指令提交后更新预测器内容

例如 FTB/BTB 类 target predictor: 会在 putPCHistory 时候查表，并填入每一级的
FullFTBPrediction/stage prediction 和 meta 信息；update 时应直接消费真实
branch target/taken 信息，局部构造需要写表的 target entry。这样 update 的数据
来源是 actual branch set，而不是复用预测 entry 去“猜”训练 entry。

```cpp
// 当 commit/resolve stream 时候更新预测器
void DecoupledBPUWithFTB::update(unsigned stream_id, ThreadID tid) {
    auto update_branches = makeUpdateBranchPrefix(stream.resolvedBranches);
    auto ctx = makeBaseBranchUpdateContext(stream);
    auto update_entries = buildTargetUpdateEntries(..., update_branches, ...);
    DPRINTF(DecoupleBP, "update each component\n");
    for (int i = 0; i < numComponents; ++i) {
        components[i]->update(..., ctx, update_entries, update_branches);
    }
}
```

#### 整体预测器函数 BPU

核心的就是 tick 函数，其他重要函数都在其中调用，tick 表示每拍会调用的函数

DecoupledBPUWithFTB 的 tick 函数在 Fetch::tick() 函数最后调用

```cpp
void
DecoupledBPUWithFTB::tick()
{
    if (!receivedPred && numOverrideBubbles == 0 && sentPCHist) { // 没有收到预测结果，没有气泡，并且发送了 PC 历史
        generateFinalPredAndCreateBubbles();  // 用上一拍预测结果生成最终预测结果 finalPred，产生气泡
    }
    if (!squashing) {  // 没有后端冲刷
        tryEnqFetchTarget();    // 尝试入队到 FetchTarget 中，用上一拍 FSQ entry 生成一个 FTQ 条目存入 FTQ
        tryEnqFetchStream();    // 用 finalPred 结果生成 FSQentry 存入 FSQ, 本质上调用 processNewPrediction
    } 

    if (!receivedPred && !streamQueueFull()) { // 没有收到预测结果，并且 FSQ 没有满，开始 BP 预测！
        if (!enableLoopBuffer || (enableLoopBuffer && !lb.isActive())) {
            // put startAddr in preds
            for (int i = 0; i < numStages; i++) {  // 3 级预测器都更新 s0PC = 查询 fetchBlock 地址
                predsOfEachStage[i].bbStart = s0PC;
            }
            for (int i = 0; i < numComponents; i++) {
                components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);
                // 调用各个组件的 putPCHistory 方法分支预测，预测结果记录在 predsOfEachStage 中
                // 后面的同级预测器可能会覆盖 s1/s2 的 predsOfEachStage 内容
            }
        } 
    }
}
```

这里是控制逻辑比较复杂的地方，为了方便说明，只保留了核心函数的调用，涉及到 4 个关键函数

按照逻辑先后关系

1. components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);  调用各个组件的 putPCHistory 方法做出分支预测，预测结果记录在 predsOfEachStage 中
2. generateFinalPredAndCreateBubbles();  // 用上一拍 3 级预测结果生成最终预测结果 finalPred，产生气泡
3. tryEnqFetchStream();    // 用 finalPred 结果生成 FSQentry 存入 FSQ, 本质上调用 processNewPrediction
4. tryEnqFetchTarget();    // 尝试入队到 FetchTarget 中，用上一拍 FSQ entry 生成一个 FTQ 条目存入 FTQ

当然看到代码中调用顺序并不是这样的，其中通过 receivedPred, numOverrideBubbles 等各个控制信号来控制打拍方式等。以初始化举例，0,1,2,3 分别表示 tick

0  fetch squashing, start=0x80000000, 各组件调用 putPCHistory

1  generateFinalPredAndCreateBubbles , tryEnqFetchStream ;insert fsq 1, 同时推测更新预测器

2  tryEnqFetchTarget, get ftq 2 from fsq, predict new

3 fetch squash

4 fetch 用 ftq

整体流程类似下表

> 注意这里默认还没有 override bubble; 这个 fsq0，ftq0 并不表示实际的 fsqid=0, 只是便于说明是第 0 个预测结果
>

| <font style="color:black;">tick 0</font> | <font style="color:black;">生成预测 0</font> | | | |
| --- | --- | --- | --- | --- |
| <font style="color:black;">tick 1</font> | <font style="color:black;">生成预测 1</font> | <font style="color:black;">get finalPred0, 生成 fsq0</font> | | |
| <font style="color:black;">tick 2</font> | <font style="color:black;">生成预测 2</font> | <font style="color:black;">get finalPred1, 生成 fsq1</font> | <font style="color:black;">生成 ftq0</font> | |
| <font style="color:black;">tick 3</font> | | <font style="color:black;">…</font> | <font style="color:black;">生成 ftq1</font> | <font style="color:black;">fetch 可以用 ftq0 取指</font> |
| <font style="color:black;">tick 4</font> | | | <font style="color:black;">…</font> | <font style="color:black;">…</font> |

额外强调下选择最终预测和生成气泡逻辑

```cpp
void DecoupledBPUWithFTB::generateFinalPredAndCreateBubbles(){
         // choose the most accurate prediction，选择各级最准确的预测（最后一级）
        FullFTBPrediction *chosen = &predsOfEachStage[0];
        // 从最后一级开始往前找，选择第一个有效的预测 chosen，只看 valid, 可能会出现 uftb, ftb 同时 valid。
        for (int i = (int) numStages - 1; i >= 0; i--) {
            if (predsOfEachStage[i].valid) {
                chosen = &predsOfEachStage[i];
                DPRINTF(Override, "choose stage %d.\n", i);
                break;
            }
        }
        finalPred = *chosen;
        // calculate bubbles
        unsigned first_hit_stage = 0; // 找到第一个预测命中的级别
        while (first_hit_stage < numStages-1) { // 从前往后找，第一个和 chosen 匹配的级别，作为有效预测
            if (predsOfEachStage[first_hit_stage].match(*chosen)) {
                break;
            }
            first_hit_stage++;
        }
        // generate bubbles
        numOverrideBubbles = first_hit_stage;   // 泡泡数和最终预测来源
        // assign pred source
        finalPred.predSource = first_hit_stage;
        receivedPred = true;    // 收到预测结果，用于跳过下一拍 tick()
}
```

先从后往前找到 valid 的预测结果作为最终预测，然后从前往后找前面的预测结果是否和最终预测一致，如果一致就用前面的预测结果。同时计算泡泡，s0 = 0 bubble, s1 = 1 bubble.

#### Interaction with Fetch

##### detailed explaination of interface with Fetch

其他和 Fetch 交互的关键函数还包括

```cpp
// fetch 函数检查下一条指令是否 taken, 是否要继续译码
std::pair<bool, bool> decoupledPredict();

// redirect the stream，各个重定向（分支、非分支，异常）
void controlSquash();
void nonControlSquash();
void trapSquash();

// commit stream 后更新 BPU, 删除 fsqId 对应的 fsq entry，统计数据；
// 组件训练由 explicit actual branch set 控制。
void update(unsigned fsqID, ThreadID tid);

// 提交分支指令，统计计数！先统计整体的，再统计各个预测器的
void commitBranch(const DynInstPtr &inst, bool miss);
// 通知 branch predictor 指令已提交一条普通指令，每 10 万条 inst 统计一次
void notifyInstCommit(const DynInstPtr &inst);
```

在后文 Fetch 函数那里会讲解何时调用**decoupledPredict 这个关键函数**

后文重定向章节讲解调用各类 Squash 和 update 函数

##### FTQ enqState, readState

主要靠如下 3 个关键数据结构，都在 FTQ 中定义的！后文把他们简称为 enqState, deqState, demandID

```cpp
class FetchTargetQueue
{
FetchTargetEnqState fetchTargetEnqState;  // 入队状态
FetchTargetReadState supplyFetchTargetState;  // 供应 fetch 目标状态
// The demanded fetch target ID to send to fetch  需求状态，下一个要处理的 fetch 目标
FetchTargetId fetchDemandTargetId{0};  // 发送给 fetch 的需求 fetch 目标 ID，新 target
} 

// enqState 定义
struct FetchTargetEnqState
{
Addr pc;  // 入队 pc
FetchStreamId streamId;  // fsq 的 id
FetchTargetId nextEnqTargetId;  // ftq 的 id
};  // 入队状态，bp 写入 ftq，相当于 head
// deqState 定义
struct FetchTargetReadState
{
bool valid;  // 有效
FetchTargetId targetId;  // ftq id
FtqEntry *entry;  // ftq 的 entry，里面包含 startPC 等
};  // 出队状态，o3 在 fetch 阶段读取 ftq，相当于 tail
```

##### FTQ enqueue and dequeue

这里 BPU -> FSQ -> FTQ -> Fetch

对于 FTQ 来说，FSQ 是生产者，通过 EnqState 来写入 FTQ，每次写入一整个 entry

Fetch 是消费者，通过 ReadState/DeqState 来读取 FTQ 内容，读取 id 为 DemandId, 每次读取 entry 中一条指令，当指令超过这个 entry 时候就把这个 entry 完全读出并出队

先看生产者入队，对应 tryEnqFetchTarget(), 如下为核心内容：

pc 首先设置为 0x80000000, 在 tryEnqFetchTarget 函数中，从 FSQ 中的指令流中入队到 FTQ 中。先通过 enqState 中存储的 fsqId 找到对应 FSQ entry, 然后生成对应的 FTQ entry, 最后入队到 FTQ 中

    1. 更新 enqState.pc, 如果 taken, 设置为 target, 否则设置为 endPC=startPC+0x20
    2. enqState.streamId++; nextEnqTargetId+1; 

##### decoupledPredict

再看消费者出队，对应 fetch 调用 decoupledPredict 函数

初始化 demandID = targetID = 0, ftqEntry 就是第一个 entry[0x80000000, 0x80000020]

直到 fetch 调用 decoupledPredict 一直从中这个区间中取指令，读取 deqState.entry 作为当前 FTQ entry,

并决定下个取指 npc(taken or not)，当 npc 超过这个区间，或者出现了分支预测 taken 的情况，run_out_of_this_entry=true, 调用 finishCurrentFetchTarget() 函数

```cpp
FetchTargetQueue::finishCurrentFetchTarget()
{
    ++fetchDemandTargetId;  // 更新需求 id，下一个要处理的 fetch 目标直接 +1，对应 ftqid
    ftq.erase(supplyFetchTargetState.targetId);  // 删除供应 id 对应的 ftq 条目
    supplyFetchTargetState.valid = false;  // 设置供应状态无效
    supplyFetchTargetState.entry = nullptr;  // 设置供应 entry 为空
```

> 注意 ftq 删除逻辑和 fsq 不同，fsq 在其中指令都 commit 后才删除，ftq 在预测块不需要这个 entry 后立刻删除
>

##### trySupplyFetchWithTarget

接下来在 fetch:tick() 同一拍的最后，调用 trySupplyFetchWithTarget()，最终调用到 ftq 的同名函数

```cpp
FetchTargetQueue::trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &in_loop)
{
    // 当供应状态无效或供应 id 不等于需求 id 时，需要从 FTQ 中获取新 target
    if (!supplyFetchTargetState.valid ||
        supplyFetchTargetState.targetId != fetchDemandTargetId) {  
        auto it = ftq.find(fetchDemandTargetId);  // 查找需求 id 对应的 ftq 条目
        if (it != ftq.end()) {  // 找到
            supplyFetchTargetState.valid = true;  // 设置供应状态有效
            supplyFetchTargetState.targetId = fetchDemandTargetId;  // 设置供应 id
            supplyFetchTargetState.entry = &(it->second);  // 写入 ftq entry
        } else {  // 没找到
            return false; // 等之后生成
        }
    }
```

这里根据 demandId 查找到 ftq 中新的一项（不跳转就是下一项，跳转就可能是后面的项，如果找不到就重新生成 FSQ, FTQ entry 再使用), 并写入其中内容

#### 重定向导致 squash

包含 controlSquash, nonControlSquash, trapSquash , 内容差不多

```cpp
DecoupledBPUWithFTB::controlSquash(){
    // 删除大于 squash_stream_id 的所有 fsq entry, 都在错误路径上
    squashStreamAfter(stream_id);
    // 更新 tage 历史
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, stream, real_shamt, real_taken);
    }
    histShiftIn(real_shamt, real_taken, s0History);
    historyManager.squash(stream_id, real_shamt, real_taken, actual_branch);
    // 清空 FTQ
    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id,real_target);
}
```

### Fetch

#### fetch process overview

gem5 的 fetch 函数由于需要支持多指令集，所以在 fetch 阶段同时也 decode 了，而其 decode 阶段没做什么

整体上从 ftq 中取出一项，把 startPC 通过 ITLB 翻译，发送给 ICache, 取出来连续两行，放入 fetchBuffer 中，然后对其二进制译码，更新 npc, 如果 npc 还在 ftq entry 中，就不断译码。译码之后的指令放入 fetchQueue 中，类似 RTL 的 InstBuffer, 只是其中是译码完的指令了。

#### fetch members, timebuffer etc

```cpp
ThreadStatus fetchStatus[MaxThreads];  
// 每个线程在 fetch 阶段：run, 空闲，挤出，阻塞，取值中，等待，静默等，icache 等响应/重发/完成
TimeBuffer<TimeStruct> *timeBuffer; // fetch 和其他流水线的通道
TimeBuffer<TimeStruct>::wire fromDecode;  fromRename, fromIEW, fromComit;
TimeBuffer<FetchStruct>::wire toDecode;   // 写给 decode

uint8_t *fetchBuffer[MaxThreads];   // 存储从 icache 获得的指令二进制

std::deque<DynInstPtr> fetchQueue[MaxThreads];    // 对应 InstBuffer, 每拍存入最多 16 条指令，取出最多 8 条给 decode
```

> 注意 cpu.hh 中还定义了一个同名的 fetchQueue, 是一个 timeBuffer, 和上面的区分！
>
> TimeBuffer<FetchStruct> fetchQueue;   // fetch to decode 传递信息，内容为 FetchStruct，传递指令
>

#### fetch pseudocode and comment

```cpp
Fetch::tick(){
    checkSignalsAndUpdate(tid); // 检查当前状态并更新状态
    fetch() {  // 核心逻辑在 fetch 函数
        if (fetchStatus[tid] == Running) {
            fetchCacheLine(fetch_addr, tid, this_pc.instAddr());    
            // 读取这一行的 cache，如果跨行，需要读取两行
            // 生成对应的 Cache_req, 并 TLB 翻译
            cpu->mmu->translateTiming();
            Fetch::finishTranslation()  // tlb 翻译后调用此函数
                icachePort.sendTimingReq(data_pkt) // 地址翻译后发送 icache 请求
                // recvTimingResp()
                Fetch::processCacheCompletion() // icache 返回数据
            memcpy(fetchBuffer[tid], pkt->getConstPtr<uint8_t>(), fetchBufferSize);     // 把 pkt 内容存入到 fetchBuffer 中
            }

            // 回到 fetch 函数，已经是很多拍之后了
            // 之后都是 ICache complete 执行
            ++fetchStats.cycles;

            下一个块A说明
                }

        fetch() 函数结束后
        最后从fetchQueue中发送指令给decode
            }

```

**这里是****<font style="color:#DF2A3F;">块 A</font>**

```cpp
这里继续在fetch() 函数中，
// 巨大 while() 循环
while (numInst < fetchWidth...){
    memcpy(dec_ptr->moreBytesPtr(), 
        fetchBuffer[tid] + blk_offset * instSize, instSize);    
    // 把 fetchBuffer 二进制搬到 decode 内部 buffer 中，这里 instSize=4, 一条一条译码
    decoder[tid]->moreBytes(this_pc, fetch_addr);   // 是否需要更多字节
        if (dec_ptr->needMoreBytes()) {     // 能取就继续取下一条
            blk_offset++;} // 更新 blk_offset

        do {
            staticInst = dec_ptr->decode(this_pc);      // 译码出具体静态指令了！
            DynInstPtr instruction = buildInst(
            tid, staticInst, curMacroop, this_pc, *next_pc, true);  
            // 得到 DynInst, 并压入 instBuffer/fetchQueue！

            predictedBranch |= lookupAndUpdateNextPC(instruction, *next_pc);  
            // 检查是否分支跳转，用 FTQ 值更新 next_pc

        } while(numInst< fetchWidth && fetchQueue.size) 
            // 另一个巨大 do_while 内层循环
            // 译码完一条指令后由于 dec_ptr->instReady() 退出这个内层循环

            } // 当把 fetchBuffer 中指令用完或者其他限制后，退出外层循环
```

如果没有跳转，每拍最多译码 32byte, 16 条指令，但是由于 decode 宽度限制，每拍最多发送 8 条 inst 给 decode

从 FTQ 中取出值更新 pc 逻辑!!!

```cpp
fetch::tick()
    fetch()
        巨大 while() 循环
            lookupAndUpdateNextPC（instruction, *next_pc）// 检查是否分支跳转，用 FTQ 值更新 pc
                dbpftb->decoupledPredict(next_pc)  // 在这个函数内同时从 FTQ 出队，用 new target 更新 next_pc!!!
                    target_to_fetch = fetchTargetQueue.getTarget(); // 获取当前的 FTQ 条目，也就是消费者出队
                    if(taken) {
                        rtarget.pc(target_to_fetch.target);  // 更新 pc=target
                        set(pc, *target); // 更新 pc = next_pc = target = rtarget = new target
                    } else {// 如果预测 not taken，顺序执行下一条指令，npc += 4
                        inst->advancePC(*target); // target = next_pc = pc += 4
                        if (target->instAddr() >= end) {    // 如果当前指令地址大于等于基本块结束地址，说明已经运行到基本块结束
                            run_out_of_this_entry = true;
                            }
                    }
                if (run_out_of_this_entry) { // 当前 FTQ 条目使用完毕，需要把这个 entry 从 ftq 出队
                    fetchTargetQueue.finishCurrentFetchTarget();  // 出队，当前供应结束
                }


```

*pc->this_pc->next_pc, 传入给 lookup

next_pc->target=>rtarget

整体嵌套还是很深的！，需要仔细梳理才行。优先把代码折叠挺好！

最后 decoupledPredict 会修改 next_pc, 其实就是全局的 fetch.pc, 然后从新的 pc 开始去取指

### 重定向相关

目前 GEM5 还没有实现 predecode, 预译码发现的错误（主要是 jal 这类跳转目标不一致）会在 decode 阶段检查，然后传递给 fetch 更新

```cpp
Decode::tick()
    decode()
        decodeInsts()
while(){  // 检查每条指令
        if (inst->readPredTaken() && !inst->isControl()) {    
            // 预测 taken，但都不是 control flow insts（cfi 指令）
            squash(inst, inst->threadNumber);
        }
        // 对于无条件直接跳转
        if (inst->isDirectCtrl() &&
            (inst->isUncondCtrl() || inst->readPredTaken())）{
            // 检查预测目标是否正确
            if (*target != inst->readPredTarg()) {   // 预测目标和实际目标不一致
                squash(inst, inst->threadNumber);
            }
        }
    }

squash 函数会设置toFetch->decodeInfo[tid].squash = true;
最后fetch 函数会调用dbpftb->controlSquash() 来更新
最终调用各个预测器的recoverHist() 并对FTQ Squash
```

如果在执行过程中发现了跳转方向和预测方向不一致，会传递给 commit 阶段去冲刷

> 之后可以优化到执行阶段去冲刷，即便在错误路径上也不会污染太多！
>

```cpp
// iew.cc 中
if (inst->mispredicted()）{
    squashDueToBranch(inst, tid);  // 冲刷分支预测错误指令
    execWB->squash[tid] = true; // 给 commit squash 信号拉高
}

bool
mispredicted()
{
    std::unique_ptr<PCStateBase> next_pc(pc->clone()); // 当前 pc
    staticInst->advancePC(*next_pc); // pc = npc
    // next_pc = npc = 指令 execute() 就确定了条件分支的跳转目标，存放在 npc 中
    DPRINTF(DecoupleBP, "check misprediction next pc=%s and pred pc=%s\n",
        *next_pc, *predPC);
    return *next_pc != *predPC; // 预测 pc != 执行后的 npc, 错误
}
```

```cpp
// commit.cc 中
if (fromIEW->squash()){
    // 这里虽然名字叫 toIEW, 实际上是 to fetch/decode 阶段内容都在这里
    toIEW->commitInfo[tid].squash = true;
    toIEW->commitInfo[tid].mispredictInst =
        fromIEW->mispredictInst[tid];  // 分支预测错误指令来自 IEW
}
```

最后 fetch 在最开始就检查是否有重定向或者正常提交

```cpp
Fetch::checkSignalsAndUpdate(ThreadID tid){
    if (fromCommit->commitInfo[tid].squash) {   //有 commit squash 信号，也就是 toIEW 设置的
        dbpftb->controlSquash();
    } else if (fromCommit->commitInfo[tid].doneSeqNum) { // 如果 commit 完成，则更新 branch predictor
        dbpftb->update(fromCommit->commitInfo[tid].doneFsqId, tid); 
        // 用 commit stream 更新预测器
    }

    if (fromDecode->decodeInfo[tid].squash) { // decode squash 信号
        dbpftb->controlSquash();
    }

}
```

最后再看看 commit 正常提交指令这里更新 fsqid 对应的 fsq entry

```cpp
// 正常提交，设置 commit 完成序号为最年轻指令序号
toIEW->commitInfo[tid].doneSeqNum = head_inst->seqNum;
if (head_inst->getFsqId() > 1) {
    toIEW->commitInfo[tid].doneFsqId = head_inst->getFsqId() - 1;  
    // 指令对应的 fsq id - 1; 等当前 fsq 指令都提交才更新这个 fsq
}
```

注意这里 doneFsqId 为当前指令对应 fsqid - 1; 意思是这个 fsqid 中所有指令都 commit 后，等到下个 fsqid 中第一个指令 commit 后，才更新之前的 fsq entry，也就是更新 doneFsqId 这个 entry

### FSQ、FTQ 生命周期

在 GEM5 动态指令类中存储了 fsqId, ftqId

```cpp
class DynInst {
/** fsqId and ftqId are used for squashing and committing */
/** The fetch stream queue ID of the instruction. */
unsigned fsqId;
/** The fetch target queue ID of the instruction. */
unsigned ftqId;
}
```

在 fetch 阶段 buildInst() 函数中设置，在块 A 中能找到 buildInst()  调用时间，从静态指令生成动态指令

```cpp
Fetch::buildInst(){
    instruction->setFsqId(dbpftb->getSupplyingStreamId());
    instruction->setFtqId(dbpftb->getSupplyingTargetId());
    fetchQueue[tid].push_back(instruction);     // 当前指令加入 fetchQueue 中
}
```

在 iew(issue, execute, write) 和 commit 阶段读取当前指令的 fsqId, ftqId，当指令提交或者需要重定向 squash 时候，把对应的 id 发送给 fetch 阶段，fetch 阶段再通过获取到的 id 来刷新 fsq, ftq 等

###

### GEM5 pc

在 fetch 中经常发现 pc, 这里的 pc 是一个复杂结构体

类继承关系：

Serializable<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>PCStateBase<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>PCStateWithNext<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>SimplePCState<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>UPCState<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>RiscvISA::PCState

其中 PCStateBase 只包含_pc (当前 pc),_upc(微码相关，不用管)

PCStateWithNext 才添加 _npc 表示 next pc

本质上通过虚函数指针来实现，例如 fetch 中都使用 PCStateBase

可以通过 auto &rtarget = target->as<GenericISA::PCStateWithNext>();  （static_cast）来把他作为子类更新

GEM5 有一个全局的 pc,  并且保持 fetch.pc = cpu->State = commit.pc

同时当译码完成得到 dynInst 后会把 pc 存储在 dynInst 中随指令流动，每条 inst 自带一个 pc

inst->advance(*pc), 会把 pc->_pc = npc，npc 根据当前指令的长度（2byte or 4byte）来更新全局 pc

```cpp
    // Advance the PC.
    void advance() override
    {
        this->_pc = this->_npc;
        this->_npc += InstWidth;
    }
```

### 常用调试技巧

1. debug-flags = FTB, DecoupleBP, DecoupleBPProbe, FTBTage, Fetch 等
2. 关注某个 tick，例如--debug-break=569430  在某个 tick 停下，或者 gdb 运行过程中，call schedBreak(3000) 。一般可以先打印关注的 trace, 找到或者怀疑某些地方不合理，然后在那个 tick 附近停下
3. 还可以 gdb 条件断点 if inst.sn = 30, 也就是关注第 30 个指令，if startAddr = 0x80000000
4. 还有 watchpoints, 之前追踪某个 npc 修改不知道在哪里更新的，最后 watch 那条指令中 pc 的 npc 地址

> <font style="color:rgb(59, 59, 59);background-color:rgb(242, 242, 242);">watch *&((gem5::RiscvISA::PCState*)instruction.data->pc)->_npc</font>
>
> <font style="color:rgb(59, 59, 59);background-color:rgb(242, 242, 242);">发现实在 execute 函数中更新的</font>
>

<font style="color:rgb(59, 59, 59);background-color:rgb(242, 242, 242);">调试配置可以参考</font>[vscode 配置代码跳转和调试](https://bosc.yuque.com/yny0gi/sggyey/xuobc516y876q5n6)

更好的办法是自己编写小测试，首先理解小测试的行为，然后根据小测试来 gdb 调试，打印 debug-flags 来理解 gem5 代码的行为。这里附上两个我自己阅读代码时候添加的小测试

[ftb 小测试](https://bosc.yuque.com/yny0gi/sggyey/yp8uo2nyyhto8zhx)[tage 测试](https://bosc.yuque.com/yny0gi/sggyey/vx45467ek01pwobr)

更多测试程序放在[https://github.com/OpenXiangShan/nexus-am/tree/frontendTest](https://github.com/OpenXiangShan/nexus-am/tree/frontendTest)

### 前端中文注释分支

由于中文注释不方便 push 到主线 xs-dev 上，所以我自己维护了一个阅读分支，在 24 年 10 月就和主线分离了，但考虑到前端部分 gem5 基本没有太大修改，就一直在这个分支上添加注释，方便我阅读理解代码。

**注意：大部分注释由 cursor AI 编辑器添加，不保证正确性，只是方便阅读理解！**

分支在： [https://github.com/OpenXiangShan/GEM5/tree/reading](https://github.com/OpenXiangShan/GEM5/tree/reading)
