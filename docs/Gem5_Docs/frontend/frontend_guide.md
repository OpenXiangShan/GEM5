# gem5 前端代码导读

### 整体架构
参考如下内容

[总体架构 - XiangShan 官方文档](https://docs.xiangshan.cc/zh-cn/latest/frontend/overview/)

[https://github.com/OpenXiangShan/XiangShan-Design-Doc](https://github.com/OpenXiangShan/XiangShan-Design-Doc) 这个设计文档

解耦前端整体架构

![](images/1733192628347-c025d3bd-73a3-479a-9d20-00a22969e633.svg)

BPU 内部架构

![](images/1733192616906-dc9fcf8c-e7d8-4257-bff9-7a2a5c6d6b43.svg)





**GEM5整体工作流程**

bpu -> fsq -> ftq -> Fetch(ICache) -> Decode -> exe -> commit

其中fsq = ftq, 基本作用一样，只是fsq 遗留代码

RTL 中描述FTQ 职责

1. 暂存BPU 预测取指目标，供给给IFU 取指令
2. 暂存BPU 预测器预测信息，当指令提交后用这些信息更新预测器训练，需要维护指令从预测到提交的完整生命周期。后端需要pc, 从ftq 读取。

事实上在GEM5 中，这些职责基本由FSQ、FTQ 共同完成，后文讲解



### 关键数据结构
#### BPU 整体结构
整个预测器叫做DecoupledBPUWithFTB， 核心是BPU, 是和前端解耦的，是以FTB 为关键内容的。

继承GEM5 原本的BPredUnit 预测器结构，但基本不使用其中的函数了

```cpp
class DecoupledBPUWithFTB : public BPredUnit {
    FetchTargetQueue fetchTargetQueue;  // FTQ
    // BP -> FSQ -> FTQ -> Fetch unit
    std::map<FetchStreamId, FetchStream> fetchStreamQueue;  
    // FSQ: 完整指令流，直到commit才从FSQ中移除
    // 5个预测器组件
    DefaultFTB *uftb{};
    DefaultFTB *ftb{};
    FTBTAGE *tage{};
    FTBITTAGE *ittage{};
    ftb_pred::uRAS *uras{};
    std::vector<TimedBaseFTBPredictor*> components{};   // 所有预测器（5个）组件的vector
    std::vector<FullFTBPrediction> predsOfEachStage{};  // 每个阶段的预测结果
    bool receivedPred{false};   // 是否收到BPU预测结果，generateFinalPredAndCreateBubbles()中写入true,tryEnqFetchStream()中写入false

    Addr s0PC;  // 当前PC、预测PC
    boost::dynamic_bitset<> s0History; // 全局分支历史，970位， 推测更新
    FullFTBPrediction finalPred;  // 最终预测结果， 从predsOfEachStage中选择最准确的
}
```

内容主要包含FTQ、FSQ、预测结果，全局历史等

接下来会详细介绍关键组件



#### FSQ entry = FetchStream
这里FSQ 作用和FTQ 作用一样，只是GEM5 第一代预测器是stream based的，是遗留代码，但是承担了大部分FTQ 的工作，第二代预测器是FTQ/FTB based， 新加的一部分就放在了FTQ entry 中

多个（kmhv2 设置64个）FetchStream 组成了fetchStreamQueue， 用FetchStreamId/FsqId 来索引。

```cpp
// BP -> FSQ -> FTQ -> Fetch unit
std::map<FetchStreamId, FetchStream> fetchStreamQueue;  
// FSQ: 完整指令流，可能产生多个FTQ条目，直到commit才从FSQ中移除
unsigned fetchStreamQueueSize;
FetchStreamId fsqId{1};  // 索引ID
```

单个FSQ entry 定义如下

```cpp
struct FetchStream
{
Addr startPC;  // FetchStream的start pc， [startPC, predEndPC)，为s0PC

// 以下几个内容都由finalPred 最终预测填入
bool predTaken;  // 预测跳转
Addr predEndPC;  // 预测的流结束pc（fall through pc）
BranchInfo predBranchInfo;  // 预测跳转的最后一条分支信息
bool isHit;  // 是有效预测， finalPred.valid有效
bool falseHit;  // 是否假命中？
FTBEntry predFTBEntry;  // 预测的FTB条目

// for commit, write at redirect or fetch 重定向信息！很多在squash 之后写入
bool exeTaken;  // exe阶段分支确实跳转
BranchInfo exeBranchInfo;  // 实际跳转的分支信息，区别predBranchInfo

FTBEntry updateFTBEntry;   // 新的，要写入FTB的FTB条目, ftb->update()写入
bool updateIsOldEntry;  // 是否更新为旧的FTB条目

bool resolved;  // squash后变为true, if resolved, 使用exeBranchInfo+exeTaken

// 重定向相关元信息, commit设置并传给BPU
int squashType;  // squash类型， ctrl(分支redirect) or trap or other
Addr squashPC;  // squash的pc
int squashSource;  // squash来源, decode or commit

unsigned predSource;  // 预测来源

// prediction metas
std::vector<std::shared_ptr<void>> predMetas;  // 预测元数据， 每个组件内容不同，记录预测时候信息，向下流水直到commit, 用于commit更新预测器内容
boost::dynamic_bitset<> history; // 全局分支历史，970位， 推测更新, =s0History
```

这里记录了FetchStream 大部分关键内容，其实最关键的

1. startPC， 预测起始PC, = s0PC, 各子预测器预测PC
2. predTaken 到predFTBEntry, 当各预测器生成完finalPred 之后，填入预测内容
3. exeTaken/exeBranchInfo, 默认为predTaken, 如果出现重定向controlSquash，设为真正taken方向
4. updateFTBEntry：调用getAndSetNewFTBEntry之后暂存下，然后ftb->update() 用它来更新ftb entry
5. resolved: 出现三种squash(control/noncontrol/trap) resolved为true, 使用exeTaken信息，并保存squash信息

这里有两组很类似的数据结构，分别是pred开头和exe 开头，内容包括（predEndPC, predTarget, predTaken， exe 同理） 然后当把这些内容传递给FTQ 时候，根据resolved 来二选一传递过去（选pred信息or exe信息）

```cpp
    BranchInfo getBranchInfo() const { return resolved ? exeBranchInfo : predBranchInfo; }
    Addr getControlPC() const { return getBranchInfo().pc; }
    Addr getEndPC() const { return getBranchInfo().getEnd(); } // FIXME: should be end of squash inst when non-control squash of trap squash
    Addr getTaken() const { return resolved ? exeTaken : predTaken; }
    Addr getTakenTarget() const { return getBranchInfo().target; }
```

本质上对应两个阶段：

1. 预测阶段，resolved=0, ftb内容生成fsq 的pred 信息，再传递给ftq 对应内容（ftb->fsq->ftq)
2. 重定向阶段，resolved=1, fsq exe的准确内容，传递给ftq对应内容，用于重新生成新的ftb 项信息(fsq->ftq->new ftb) （其实在getAndSetNewFTBEntry生成新的ftb 时候，直接用的fsq内容, 没用过ftq内容，因为重定向时候，对应的ftq 内容可能已经不存在了，只有fsq 还一直存着）

相当于ftq 内容是fsq 内容的压缩版，动态切换到对应版本上。

这里由于遗留代码，容易造成很多代码误解，删除fsq 遗留代码需要之后完成！



#### FTQ entry
上一节讲到，FSQ + FTQ 共同构成了RTL 中FTQ 的作用，存储fetchBlock 并一直到其中指令全部commit之后才删除对应的entry, GEM5 的FTQ entry 是FSQ 中原本没存储的东西，是一个增量，每个FSQ entry 对应一个FTQ entry, 两者合并对应RTL 的一个FTQ entry.

```cpp
using FTQ = std::map<FetchTargetId, FtqEntry>;  // <id, FtqEntry>， map 字典，有64项
FTQ ftq;    // <id, FtqEntry>， map 字典，有64项, 用fetchDemandTargetId索引
unsigned ftqSize;
FetchTargetId ftqId{0};  // this is a queue ptr for ftq itself
```

这里FTQ 和FSQ 一样，都是map 组织，用ftqId 来索引entry, 注意，对于同一个entry， ftqId = fsqId - 1, 因为fsqId 从1开始计数，ftqId 从0 开始，例如--debug-flags=DecoupleBP 输出结果

> Set instruction 1542 with stream id 112, fetch id 111
>

这说明这个指令同时来自fsqId 112 和ftqId 111

```cpp
struct FtqEntry
{
Addr startPC;  // 块的开始pc，对应fsq entry 的startPC
Addr endPC;    // fallThrough pc, 对应fsq entry predEndPC
Addr takenPC;  // 当是taken分支时，takenPC是控制PC, 当是未命中时，takenPC=0
// 对应fsq_entry pred(exe)BranchInfo.pc

bool taken;		// 中间是否有taken 分支
Addr target;
FetchStreamId fsqID;  // fsq的id, 用于ftq 索引到对应的Fsq!
```

当其中有taken 分支时候，索引fetchBlock = [startPC, takenPC]

没有taken 分支， = [startPC, endPC], endPC = startPC + 32 (32 为预测块宽度) 

可以看到FtqEntry 中相对FSQEntry 内容少了挺多, 也可以看做是FSQEntry缩减版, 很多内容等价于fsq 内容

```cpp
DecoupledBPUWithFTB::setTakenEntryWithStream(const FetchStream &stream_entry, FtqEntry &ftq_entry)
{
    ftq_entry.taken = true;
    ftq_entry.takenPC = stream_entry.getControlPC();
    ftq_entry.endPC = stream_entry.predEndPC;
    ftq_entry.target = stream_entry.getTakenTarget();
    ftq_entry.inLoop = stream_entry.fromLoopBuffer;
    ftq_entry.iter = stream_entry.isDouble ? 2 : stream_entry.fromLoopBuffer ? 1 : 0;
    ftq_entry.isExit = stream_entry.isExit;
    ftq_entry.loopEndPC = stream_entry.getBranchInfo().getEnd();
}
```

这里能看出来，ftq 的takenPC, endPC, target,  几乎等于fsq 的predBranchInfo.pc, predEndPC, predTaken



建议结合Debugflags 来共同查看其中内容，例如

```cpp
    void printFetchTarget(const FtqEntry &e, const char *when)
    {
        DPRINTFR(DecoupleBP,
                 "%s:: %#lx - [%#lx, %#lx) --> %#lx, taken: %d, fsqID: %lu, loop: %d, iter: %d, exit: %d\n",
                 when, e.startPC, e.takenPC, e.endPC, e.target, e.taken,
                 e.fsqID, e.inLoop, e.iter, e.isExit);
    }
```

打印了ftq entry 具体内容，可以编写小测试，--debug-flags=DecoupleBP, 会在decoupledPredict 函数调用时候打印当前ftq 内容，如下

```cpp
looking up pc 0x80000130
Responsing fetch with:: 0x80000130 - [0, 0x80000150) --> 0, taken: 0, fsqID: 1962, loop: 0, iter: 0, exit: 0
Responsing fetch with:: 0x80000130 - [0x80000134, 0x80000150) --> 0x8000013a, taken: 1, fsqID: 1966, loop: 0, iter: 0, exit: 0
```

分别表示startPC=80000130开始的两个ftq entry, 第一个预测不跳，takenPC=0, endPC=130+0x20为默认fallThrough; 第二个预测taken, takenPC=134, endPC=150, taken taget = 13a

#### FTBEntry
FTB 存储分支预测的跳转地址，同时也存储方向信息，是DecoupledBPUWithFTB的核心模块，同时提供给FullFTBPrediction 作为最终预测的一部分

这里的含义和RTL FTB 基本一致, 核心内容就是tag + 2个slots + valid + fallThruAddr

```cpp
typedef struct FTBEntry
{
Addr tag = 0;   // FTB块的tag， 由FetchStream的start pc 计算
std::vector<FTBSlot> slots;     // 分支槽，最多两条分支（NT, T/NT)
Addr fallThruAddr;              // FTB块的fallThru地址
bool valid = false; } 
```

```cpp
// FTBSlot 分支槽，继承BranchInfo
typedef struct FTBSlot : BranchInfo
{
bool valid;     // 分支有效
bool alwaysTaken;  // 总是跳转, 如果分支一直taken, = 1
int ctr;  // 2位饱和计数器， 只有uFTB 使用！
}FTBSlot;

typedef struct BranchInfo {  // 核心就是pc + target
Addr pc;  // 分支PC
Addr target;  // 分支目标地址
bool isCond;  // 条件跳转
bool isIndirect;  // 间接跳转
bool isCall;  // 调用
bool isReturn;  // 返回
uint8_t size; // 指令长度
}BranchInfo
```

每个slot 包含分支的跳转方向和目标地址

为了uFTB预测方向，还包含2bit 饱和计数器。

#### FullFTBPrediction
对应RTL 的FullBranchPrediction， 就是3级预测器， 每个阶段的预测结果都存在这里， 最终选出finalPred

```cpp
std::vector<FullFTBPrediction> predsOfEachStage{};  // 3级，每个阶段的预测结果
FullFTBPrediction finalPred;  // 最终预测结果， 从predsOfEachStage中选择最准确的
```

```cpp
typedef struct FullFTBPrediction
{
Addr bbStart;  // 块的开始pc
FTBEntry ftbEntry; // for FTB， 保存整个FTB，核心！
std::vector<bool> condTakens; // 用于条件分支预测器，两个条件分支是否taken， TAGE提供， uftb结果由2bit 饱和计数器提供

Addr indirectTarget; // for indirect predictor  间接跳转目标地址,ITTAGE提供
Addr returnTarget; // for RAS， 返回地址

bool valid; // hit
unsigned predSource; // 预测来源，哪一级预测的
boost::dynamic_bitset<> history;  // 历史
```

预测结果的核心内容是ftbEntry, 其他包括分支跳转方向，历史等

这里的内容由每一级预测器填写，每个预测器填写内容不同

例如uFTB 需要填写ftbEntry （包括了分支信息和跳转地址），还需要填写condTakens 预测是否跳转

FTB 只用填写ftbEntry

TAGE 只用填写condTakens 作为方向信息；ITTAGE/RAS 分别填写indirectTarget 和returnTarget

> 注意每个预测器填入不同级的预测结果中
>
> s0 的predsOfEachStage[0] 只有uFTB 填写
>
> s1  的predsOfEachStage[1] 有FTB、RAS、TAGE 填写，每个填写内容不应该重叠
>
> s2 的predsOfEachStage[2] 由ITTAGE、RAS 填写
>
> 最终3级预测得到finalPred
>

#### 总结
这些数据结构整体关系可以看做

每个预测器都会每拍生成预测结果，其中FTB/uFTB 生成最核心的FTBEntry, 然后其他预测器按需填入对应的方向或者别的信息，共同生成每一级的FullFTBPrediction, 最后3选1得到最终的FullFTBPrediction（finalPred）；

下一拍会根据finalPred 结果生成一项FSQEntry 放入FSQ 中

再下一拍会用FSQEntry生成一个FTQEntry 放入FTQ中

最后Fetch 函数会拿出FTQEntry 来从ICache取指

> FsqId会一直流水到commit 阶段，直到commit后会提交给Fetch阶段用于更新各个预测器， 更新后才删除对应的FSQEntry 和FTQEntry
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
每个预测器都会继承TImedBaseFTBPredictor, 关键函数都继承它，并可能override

```cpp
class TimedBaseFTBPredictor: public SimObject
{
// make predictions, record in stage preds 每拍做预测，记录在stagePreds中
virtual void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history, std::vector<FullFTBPrediction> &stagePreds) {}
// 获取预测元数据，相当于checkpoints, 存储预测时候的状态，预测错误时候回滚，指令提交时候验证
virtual std::shared_ptr<void> getPredictionMeta() { return nullptr; }   
// 推测更新历史, 只有uRAS实现！ 其他预测器不会推测更新！
virtual void specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) {}  
// // squash后恢复历史, tage/RAS实现，并更新s0history
virtual void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) {}  
virtual void update(const FetchStream &entry) {}  // 用commit stream更新预测器内容, 准确的！
// do some statistics on a per-branch and per-predictor basis
virtual void commitBranch(const FetchStream &entry, const DynInstPtr &inst) {}  // commit阶段统计分支数据
```

其中最关键的就是两个函数！

1. putPCHistory: 每拍做预测
2. update: 指令提交后更新预测器内容

例如FTB: 会在putPCHIstory 时候查找FTB 项，并填入到每一级的FullFTBPrediction stagePreds 中，并写入meta信息； 在update 时候决定是否要插入新的FTB entry 还是更新已有的entry

> FTB 还有个特殊函数getAndSetNewFTBEntry， 会在整个预测器update 时候调用生成一个新的FTB 项，之后再调用各子预测器的update函数
>

```cpp
// 当commit stream时候 更新预测器
void DecoupledBPUWithFTB::update(unsigned stream_id, ThreadID tid) {
    ftb->getAndSetNewFTBEntry(stream);  // 生成新的ftb项或更新原本已有的ftb
    // 接下来ftb->update()写入新项
    DPRINTF(DecoupleBP, "update each component\n");
    for (int i = 0; i < numComponents; ++i) {
        components[i]->update(stream);  // 每个组件更新自己内容!!!!
    }
}
```



#### 整体预测器函数 BPU
核心的就是tick函数，其他重要函数都在其中调用， tick 表示每拍会调用的函数

DecoupledBPUWithFTB 的tick 函数在Fetch::tick() 函数最后调用

```cpp
void
DecoupledBPUWithFTB::tick()
{
    if (!receivedPred && numOverrideBubbles == 0 && sentPCHist) { // 没有收到预测结果，没有气泡，并且发送了PC历史
        generateFinalPredAndCreateBubbles();  // 用上一拍预测结果生成最终预测结果finalPred，产生气泡
    }
    if (!squashing) {  // 没有后端冲刷
        tryEnqFetchTarget();    // 尝试入队到FetchTarget中, 用上一拍FSQ entry 生成一个FTQ条目存入FTQ
        tryEnqFetchStream();    // 用finalPred结果生成FSQentry存入FSQ, 本质上调用makeNewPrediction
    } 

    if (!receivedPred && !streamQueueFull()) { // 没有收到预测结果，并且FSQ没有满， 开始BP预测！
        if (!enableLoopBuffer || (enableLoopBuffer && !lb.isActive())) {
            // put startAddr in preds
            for (int i = 0; i < numStages; i++) {  // 3级预测器都更新s0PC = 查询fetchBlock地址
                predsOfEachStage[i].bbStart = s0PC;
            }
            for (int i = 0; i < numComponents; i++) {
                components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);  
                // 调用各个组件的putPCHistory方法分支预测， 预测结果记录在predsOfEachStage中
                //后面的同级预测器可能会覆盖s1/s2的predsOfEachStage内容
            }
        } 
    }
}
```

这里是控制逻辑比较复杂的地方，为了方便说明，只保留了核心函数的调用，涉及到4个关键函数

按照逻辑先后关系

1. components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);  调用各个组件的putPCHistory方法做出分支预测， 预测结果记录在predsOfEachStage中
2. generateFinalPredAndCreateBubbles();  // 用上一拍3级预测结果生成最终预测结果finalPred，产生气泡
3. tryEnqFetchStream();    // 用finalPred结果生成FSQentry存入FSQ, 本质上调用makeNewPrediction
4. tryEnqFetchTarget();    // 尝试入队到FetchTarget中, 用上一拍FSQ entry 生成一个FTQ条目存入FTQ

当然看到代码中调用顺序并不是这样的，其中通过receivedPred, numOverrideBubbles 等各个控制信号来控制打拍方式等。以初始化举例， 0,1,2,3 分别表示tick

0  fetch squashing, start=0x80000000, 各组件调用putPCHistory

1  generateFinalPredAndCreateBubbles , tryEnqFetchStream ;insert fsq 1, 同时推测更新预测器

2  tryEnqFetchTarget, get ftq 2 from fsq, predict new

3 fetch squash

4 fetch 用ftq 

整体流程类似下表

> 注意这里默认还没有override bubble; 这个fsq0，ftq0并不表示实际的fsqid=0, 只是便于说明是第0个预测结果
>

| <font style="color:black;">tick 0</font> | <font style="color:black;">生成预测0</font> | | | |
| --- | --- | --- | --- | --- |
| <font style="color:black;">tick 1</font> | <font style="color:black;">生成预测1</font> | <font style="color:black;">get finalPred0, 生成fsq0</font> | | |
| <font style="color:black;">tick 2</font> | <font style="color:black;">生成预测2</font> | <font style="color:black;">get finalPred1, 生成fsq1</font> | <font style="color:black;">生成ftq0</font> | |
| <font style="color:black;">tick 3</font> | | <font style="color:black;">…</font> | <font style="color:black;">生成ftq1</font> | <font style="color:black;">fetch 可以用ftq0取指</font> |
| <font style="color:black;">tick 4</font> | | | <font style="color:black;">…</font> | <font style="color:black;">…</font> |


额外强调下选择最终预测和生成气泡逻辑

```cpp
void DecoupledBPUWithFTB::generateFinalPredAndCreateBubbles(){
         // choose the most accurate prediction， 选择各级最准确的预测（最后一级）
        FullFTBPrediction *chosen = &predsOfEachStage[0];
        // 从最后一级开始往前找，选择第一个有效的预测chosen，只看valid, 可能会出现uftb, ftb 同时valid。
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
        while (first_hit_stage < numStages-1) { // 从前往后找，第一个和chosen匹配的级别，作为有效预测
            if (predsOfEachStage[first_hit_stage].match(*chosen)) {
                break;
            }
            first_hit_stage++;
        }
        // generate bubbles
        numOverrideBubbles = first_hit_stage;   // 泡泡数和最终预测来源
        // assign pred source
        finalPred.predSource = first_hit_stage;
        receivedPred = true;    // 收到预测结果，用于跳过下一拍tick()
}
```

先从后往前找到valid 的预测结果作为最终预测，然后从前往后找前面的预测结果是否和最终预测一致，如果一致就用前面的预测结果。同时计算泡泡，s0 = 0 bubble, s1 = 1 bubble.


#### Interaction with Fetch

##### detailed explaination of interface with Fetch
其他和Fetch交互的关键函数还包括

```cpp
// fetch 函数检查下一条指令是否taken, 是否要继续译码
std::pair<bool, bool> decoupledPredict();

// redirect the stream， 各个重定向（分支、 非分支， 异常）
void controlSquash();
void nonControlSquash();
void trapSquash();

// commit stream后更新BPU, 删除fsqId对应的fsq entry，统计数据，当ftbHit or exeTaken时候，调用每个组件update()
void update(unsigned fsqID, ThreadID tid);

// 提交分支指令，统计计数！先统计整体的，再统计各个预测器的
void commitBranch(const DynInstPtr &inst, bool miss);
// 通知branch predictor指令已提交一条普通指令, 每10万条inst 统计一次
void notifyInstCommit(const DynInstPtr &inst);
```

在后文Fetch 函数那里会讲解何时调用**decoupledPredict 这个关键函数**

后文重定向章节讲解调用各类Squash 和update 函数



##### FTQ enqState, readState
主要靠如下3个关键数据结构， 都在FTQ 中定义的！后文把他们简称为enqState, deqState, demandID

```cpp
class FetchTargetQueue
{
FetchTargetEnqState fetchTargetEnqState;  // 入队状态
FetchTargetReadState supplyFetchTargetState;  // 供应fetch目标状态
// The demanded fetch target ID to send to fetch  需求状态，下一个要处理的fetch目标
FetchTargetId fetchDemandTargetId{0};  // 发送给fetch的需求fetch目标ID，新target
} 

// enqState 定义
struct FetchTargetEnqState
{
Addr pc; 	// 入队pc
FetchStreamId streamId;  // fsq的id
FetchTargetId nextEnqTargetId;  // ftq的id
};  // 入队状态, bp 写入ftq，相当于head
// deqState 定义
struct FetchTargetReadState
{
bool valid;  // 有效
FetchTargetId targetId;  // ftq id
FtqEntry *entry;  // ftq的entry， 里面包含startPC等
};  // 出队状态, o3 在fetch阶段读取ftq，相当于tail
```

##### FTQ enqueue and dequeue

这里BPU -> FSQ -> FTQ -> Fetch

对于FTQ 来说，FSQ 是生产者，通过EnqState 来写入FTQ，每次写入一整个entry

Fetch 是消费者, 通过ReadState/DeqState 来读取FTQ内容，读取id 为DemandId, 每次读取entry 中一条指令，当指令超过这个entry 时候就把这个entry 完全读出并出队

先看生产者入队，对应tryEnqFetchTarget(), 如下为核心内容：

pc 首先设置为0x80000000, 在tryEnqFetchTarget 函数中，从FSQ中的指令流中入队到FTQ中。 先通过enqState 中存储的fsqId 找到对应FSQ entry, 然后生成对应的FTQ entry, 最后入队到FTQ中

    1. 更新enqState.pc, 如果taken, 设置为target, 否则设置为endPC=startPC+0x20
    2. enqState.streamId++; nextEnqTargetId+1; 


##### decoupledPredict
再看消费者出队， 对应fetch调用decoupledPredict函数

初始化demandID = targetID = 0, ftqEntry就是第一个entry[0x80000000, 0x80000020]

直到fetch 调用decoupledPredict一直从中这个区间中取指令，读取deqState.entry 作为当前FTQ entry,

并决定下个取指npc(taken or not)， 当npc 超过这个区间，或者出现了分支预测taken 的情况，run_out_of_this_entry=true, 调用finishCurrentFetchTarget()函数

```cpp
FetchTargetQueue::finishCurrentFetchTarget()
{
    ++fetchDemandTargetId;  // 更新需求id，下一个要处理的fetch目标直接+1，对应ftqid
    ftq.erase(supplyFetchTargetState.targetId);  // 删除供应id对应的ftq条目
    supplyFetchTargetState.valid = false;  // 设置供应状态无效
    supplyFetchTargetState.entry = nullptr;  // 设置供应entry为空
```

> 注意ftq 删除逻辑和fsq 不同，fsq 在其中指令都commit 后才删除，ftq 在预测块不需要这个entry 后立刻删除
>

##### trySupplyFetchWithTarget
接下来在fetch:tick() 同一拍的最后，调用trySupplyFetchWithTarget()，最终调用到ftq 的同名函数

```cpp
FetchTargetQueue::trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &in_loop)
{
    // 当供应状态无效或供应id不等于需求id时,需要从FTQ中获取新target
    if (!supplyFetchTargetState.valid ||
        supplyFetchTargetState.targetId != fetchDemandTargetId) {  
        auto it = ftq.find(fetchDemandTargetId);  // 查找需求id对应的ftq条目
        if (it != ftq.end()) {  // 找到
            supplyFetchTargetState.valid = true;  // 设置供应状态有效
            supplyFetchTargetState.targetId = fetchDemandTargetId;  // 设置供应id
            supplyFetchTargetState.entry = &(it->second);  // 写入ftq entry
        } else {  // 没找到
            return false; // 等之后生成
        }
    }
```

这里根据demandId 查找到ftq 中新的一项（不跳转就是下一项，跳转就可能是后面的项，如果找不到就重新生成FSQ, FTQ entry 再使用), 并写入其中内容



#### 重定向导致squash
包含controlSquash, nonControlSquash, trapSquash , 内容差不多

```cpp
DecoupledBPUWithFTB::controlSquash(){
    // 删除大于squash_stream_id的所有fsq entry, 都在错误路径上
    squashStreamAfter(stream_id);
    // 更新tage 历史
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, stream, real_shamt, real_taken);
    }
    histShiftIn(real_shamt, real_taken, s0History);
    historyManager.squash(stream_id, real_shamt, real_taken, stream.exeBranchInfo);
    // 清空FTQ
    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id,real_target);
}
```

### Fetch
#### fetch process overview
gem5 的fetch 函数由于需要支持多指令集，所以在fetch阶段同时也decode了，而其decode阶段没做什么

整体上从ftq 中取出一项，把startPC通过ITLB翻译，发送给ICache, 取出来连续两行，放入fetchBuffer中， 然后对其二进制译码，更新npc, 如果npc 还在ftq entry 中，就不断译码。 译码之后的指令放入fetchQueue中，类似RTL 的InstBuffer, 只是其中是译码完的指令了。

#### fetch members, timebuffer etc
```cpp
ThreadStatus fetchStatus[MaxThreads];  
// 每个线程在fetch阶段：run, 空闲，挤出，阻塞，取值中，等待，静默等，icache等响应/重发/完成
TimeBuffer<TimeStruct> *timeBuffer; // fetch 和其他流水线的通道
TimeBuffer<TimeStruct>::wire fromDecode;  fromRename, fromIEW, fromComit;
TimeBuffer<FetchStruct>::wire toDecode;   // 写给decode

uint8_t *fetchBuffer[MaxThreads];   // 存储从icache获得的指令二进制

std::deque<DynInstPtr> fetchQueue[MaxThreads];    // 对应InstBuffer, 每拍存入最多16条指令，取出最多8条给decode
```

> 注意cpu.hh 中还定义了一个同名的fetchQueue, 是一个timeBuffer, 和上面的区分！
>
> TimeBuffer<FetchStruct> fetchQueue;   // fetch to decode 传递信息，内容为FetchStruct，传递指令
>

#### fetch pseudocode and comment
```cpp
Fetch::tick(){
    checkSignalsAndUpdate(tid); // 检查当前状态并更新状态
    fetch() {  // 核心逻辑在fetch函数
        if (fetchStatus[tid] == Running) {
            fetchCacheLine(fetch_addr, tid, this_pc.instAddr());    
            // 读取这一行的cache，如果跨行，需要读取两行
            // 生成对应的Cache_req, 并TLB 翻译
            cpu->mmu->translateTiming();
            Fetch::finishTranslation() 	// tlb 翻译后调用此函数
                icachePort.sendTimingReq(data_pkt) // 地址翻译后发送icache 请求
                // recvTimingResp()
                Fetch::processCacheCompletion() // icache返回数据
            memcpy(fetchBuffer[tid], pkt->getConstPtr<uint8_t>(), fetchBufferSize);     // 把pkt内容存入到fetchBuffer中
            }

            // 回到fetch函数， 已经是很多拍之后了
            // 之后都是ICache complete执行
            ++fetchStats.cycles;

            下一个块A说明
                }

        fetch() 函数结束后
        最后从fetchQueue中发送指令给decode
            }

```

**这里是****<font style="color:#DF2A3F;">块A</font>**

```cpp
这里继续在fetch() 函数中，
// 巨大while() 循环
while (numInst < fetchWidth...){
    memcpy(dec_ptr->moreBytesPtr(), 
        fetchBuffer[tid] + blk_offset * instSize, instSize);    
    // 把fetchBuffer二进制搬到decode内部buffer中，这里instSize=4, 一条一条译码
    decoder[tid]->moreBytes(this_pc, fetch_addr);   // 是否需要更多字节
        if (dec_ptr->needMoreBytes()) {     // 能取就继续取下一条
            blk_offset++;} // 更新blk_offset

        do {
            staticInst = dec_ptr->decode(this_pc);      // 译码出具体静态指令了！
            DynInstPtr instruction = buildInst(
            tid, staticInst, curMacroop, this_pc, *next_pc, true);  
            // 得到DynInst, 并压入instBuffer/fetchQueue！

            predictedBranch |= lookupAndUpdateNextPC(instruction, *next_pc);  
            // 检查是否分支跳转，用FTQ值更新next_pc

        } while(numInst< fetchWidth && fetchQueue.size) 
            // 另一个巨大do_while内层循环
            // 译码完一条指令后由于dec_ptr->instReady() 退出这个内层循环

            } // 当把fetchBuffer 中指令用完或者其他限制后，退出外层循环
```

如果没有跳转，每拍最多译码32byte, 16条指令，但是由于decode宽度限制，每拍最多发送8条inst 给decode



从FTQ中取出值更新pc 逻辑!!!

```cpp
fetch::tick()
    fetch()
        巨大 while() 循环
            lookupAndUpdateNextPC（instruction, *next_pc）// 检查是否分支跳转，用FTQ值更新pc
                dbpftb->decoupledPredict(next_pc)		// 在这个函数内同时从FTQ 出队，用new target 更新next_pc!!!
                    target_to_fetch = fetchTargetQueue.getTarget(); // 获取当前的FTQ条目, 也就是消费者出队
                    if(taken) {
                        rtarget.pc(target_to_fetch.target); 	// 更新pc=target
                        set(pc, *target); // 更新pc = next_pc = target = rtarget = new target
                    } else {// 如果预测not taken,顺序执行下一条指令, npc += 4
                        inst->advancePC(*target); // target = next_pc = pc += 4
                        if (target->instAddr() >= end) {    // 如果当前指令地址大于等于基本块结束地址，说明已经运行到基本块结束
                            run_out_of_this_entry = true;
                            }
                    }
                if (run_out_of_this_entry) { // 当前FTQ条目使用完毕,需要把这个entry从ftq出队
                    fetchTargetQueue.finishCurrentFetchTarget();  // 出队，当前供应结束
                }


```

*pc->this_pc->next_pc, 传入给lookup

next_pc->target=>rtarget

整体嵌套还是很深的！，需要仔细梳理才行。优先把代码折叠挺好！

最后decoupledPredict 会修改next_pc, 其实就是全局的fetch.pc, 然后从新的pc 开始去取指



### 重定向相关
目前GEM5 还没有实现predecode, 预译码发现的错误（主要是jal这类跳转目标不一致）会在decode 阶段检查，然后传递给fetch 更新

```cpp
Decode::tick()
    decode()
        decodeInsts()
while(){  // 检查每条指令
        if (inst->readPredTaken() && !inst->isControl()) {    
            // 预测taken，但都不是control flow insts（cfi指令）
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

如果在执行过程中发现了跳转方向和预测方向不一致，会传递给commit 阶段去冲刷

> 之后可以优化到执行阶段去冲刷，即便在错误路径上也不会污染太多！
>

```cpp
// iew.cc 中
if (inst->mispredicted()）{
    squashDueToBranch(inst, tid);  // 冲刷分支预测错误指令
    execWB->squash[tid] = true; // 给commit squash信号拉高
}

bool
mispredicted()
{
    std::unique_ptr<PCStateBase> next_pc(pc->clone()); // 当前pc
    staticInst->advancePC(*next_pc);	// pc = npc
    // next_pc = npc = 指令execute()就确定了条件分支的跳转目标，存放在npc 中
    DPRINTF(DecoupleBP, "check misprediction next pc=%s and pred pc=%s\n",
        *next_pc, *predPC);
    return *next_pc != *predPC;	// 预测pc != 执行后的npc, 错误
}
```

```cpp
// commit.cc 中
if (fromIEW->squash()){
    // 这里虽然名字叫toIEW, 实际上是to fetch/decode 阶段内容都在这里
    toIEW->commitInfo[tid].squash = true;
    toIEW->commitInfo[tid].mispredictInst =
        fromIEW->mispredictInst[tid];  // 分支预测错误指令来自IEW
}
```

最后fetch 在最开始就检查是否有重定向或者正常提交

```cpp
Fetch::checkSignalsAndUpdate(ThreadID tid){
    if (fromCommit->commitInfo[tid].squash) {   //有commit squash信号，也就是toIEW设置的
        dbpftb->controlSquash();
    } else if (fromCommit->commitInfo[tid].doneSeqNum) { // 如果commit完成，则更新branch predictor
        dbpftb->update(fromCommit->commitInfo[tid].doneFsqId, tid); 
        // 用commit stream更新预测器
    }

    if (fromDecode->decodeInfo[tid].squash) { // decode squash 信号
        dbpftb->controlSquash();
    }

}
```

最后再看看commit正常提交指令这里更新fsqid 对应的fsq entry

```cpp
// 正常提交，设置commit完成序号为最年轻指令序号
toIEW->commitInfo[tid].doneSeqNum = head_inst->seqNum;
if (head_inst->getFsqId() > 1) {
    toIEW->commitInfo[tid].doneFsqId = head_inst->getFsqId() - 1;  
    // 指令对应的fsq id - 1; 等当前fsq指令都提交才更新这个fsq
}
```

注意这里doneFsqId 为当前指令对应fsqid - 1; 意思是这个fsqid 中所有指令都commit后，等到下个fsqid 中第一个指令commit后，才更新之前的fsq entry，也就是更新doneFsqId 这个entry



### FSQ、FTQ 生命周期
在GEM5 动态指令类中存储了fsqId, ftqId

```cpp
class DynInst {
/** fsqId and ftqId are used for squashing and committing */
/** The fetch stream queue ID of the instruction. */
unsigned fsqId;
/** The fetch target queue ID of the instruction. */
unsigned ftqId;
}
```

在fetch 阶段 buildInst() 函数中设置, 在块A 中能找到buildInst()  调用时间，从静态指令生成动态指令

```cpp
Fetch::buildInst(){
    instruction->setFsqId(dbpftb->getSupplyingStreamId());
    instruction->setFtqId(dbpftb->getSupplyingTargetId());
    fetchQueue[tid].push_back(instruction);     // 当前指令加入fetchQueue中
}
```

在iew(issue, execute, write) 和commit阶段读取当前指令的fsqId, ftqId，当指令提交或者需要重定向squash 时候，把对应的id 发送给fetch 阶段，fetch 阶段再通过获取到的id 来刷新fsq, ftq等

### 
### GEM5 pc
在fetch 中经常发现pc, 这里的pc 是一个复杂结构体

类继承关系：

Serializable<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>PCStateBase<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>PCStateWithNext<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>SimplePCState<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>UPCState<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>RiscvISA::PCState

其中PCStateBase 只包含_pc (当前pc),  _upc(微码相关，不用管)

PCStateWithNext 才添加 _npc 表示next pc

本质上通过虚函数指针来实现, 例如fetch 中都使用PCStateBase

可以通过 auto &rtarget = target->as<GenericISA::PCStateWithNext>();  （static_cast）来把他作为子类更新

GEM5 有一个全局的pc,  并且保持fetch.pc = cpu->State = commit.pc

同时当译码完成得到dynInst 后会把pc 存储在dynInst 中随指令流动， 每条inst 自带一个pc

inst->advance(*pc), 会把pc->_pc = npc， npc 根据当前指令的长度（2byte or 4byte）来更新全局pc

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
2. 关注某个tick，例如--debug-break=569430  在某个tick 停下，或者gdb 运行过程中，call schedBreak(3000) 。一般可以先打印关注的trace, 找到或者怀疑某些地方不合理，然后在那个tick 附近停下
3. 还可以gdb 条件断点 if inst.sn = 30, 也就是关注第30个指令， if startAddr = 0x80000000
4. 还有watchpoints, 之前追踪某个npc 修改不知道在哪里更新的，最后watch 那条指令中pc 的npc地址

> <font style="color:rgb(59, 59, 59);background-color:rgb(242, 242, 242);">watch *&((gem5::RiscvISA::PCState*)instruction.data->pc)->_npc</font>
>
> <font style="color:rgb(59, 59, 59);background-color:rgb(242, 242, 242);">发现实在execute 函数中更新的</font>
>

<font style="color:rgb(59, 59, 59);background-color:rgb(242, 242, 242);">调试配置可以参考</font>[vscode 配置代码跳转和调试](https://bosc.yuque.com/yny0gi/sggyey/xuobc516y876q5n6)

更好的办法是自己编写小测试，首先理解小测试的行为，然后根据小测试来gdb 调试，打印debug-flags 来理解gem5 代码的行为。这里附上两个我自己阅读代码时候添加的小测试

[ftb小测试](https://bosc.yuque.com/yny0gi/sggyey/yp8uo2nyyhto8zhx)[tage测试](https://bosc.yuque.com/yny0gi/sggyey/vx45467ek01pwobr)

更多测试程序放在[https://github.com/OpenXiangShan/nexus-am/tree/frontendTest](https://github.com/OpenXiangShan/nexus-am/tree/frontendTest)

### 前端中文注释分支
由于中文注释不方便push 到主线xs-dev 上，所以我自己维护了一个阅读分支，在24年10月就和主线分离了，但考虑到前端部分gem5 基本没有太大修改，就一直在这个分支上添加注释，方便我阅读理解代码。

**注意：大部分注释由cursor AI 编辑器添加，不保证正确性，只是方便阅读理解！**

分支在： [https://github.com/OpenXiangShan/GEM5/tree/reading](https://github.com/OpenXiangShan/GEM5/tree/reading)



